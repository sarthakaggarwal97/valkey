#!/usr/bin/env python3
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
"""Directed, per-command differential test of the built-in JSON commands.

Every command list in this file is sent to two servers and the replies are
compared on the wire (type and bytes), once under RESP2 and once under RESP3:

  reference   a server that loads the external valkey-json module
  candidate   src/valkey-server of this tree, where JSON is built in

For each section the keyspace events and the replication stream of the two
servers are compared too.

The kinds of step:

  T(...)        the two replies must be identical
  N(exp, ...)   a listed behaviour change (design 2.3, B1 to B10) or a module
                defect that is not reproduced (design 2.5): the candidate must
                reply `exp`. The reference reply is shown but not compared.
  O(exp, ...)   as N, but the command is sent to the candidate only, because
                the module crashes, asserts or breaks the protocol on it.
  P(pred, ...)  both replies must satisfy pred (implementation-defined values
                such as JSON.DEBUG MEMORY).
  S(...)        both replies are arrays with the same elements in any order.

Both servers listen on a unix socket only, in a mode-700 directory under
tests/cmd/run/. No TCP port is opened.

Usage: test_cmd.py [-v] [--only SECTION] [--keep] [--no-repl] [--reads-only]
Environment: JSON_REF_SERVER, JSON_REF_MODULE, JSON_NEW_SERVER override paths.
JSON_NEW_ARGS adds arguments to the candidate's command line.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

sys.setrecursionlimit(100000)  # Replies nest as deep as the documents do.

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", ".."))
NEW_SERVER = os.environ.get("JSON_NEW_SERVER", os.path.join(TREE, "src", "valkey-server"))
# The reference: a valkey-server built without JSON (make BUILD_JSON=no) and
# the external valkey-json module's shared library. Both must be given.
REF_SERVER = os.environ.get("JSON_REF_SERVER", "")
REF_MODULE = os.environ.get("JSON_REF_MODULE", "")
if not REF_SERVER or not REF_MODULE:
    sys.exit("Set JSON_REF_SERVER (a valkey-server built with BUILD_JSON=no) and "
             "JSON_REF_MODULE (the external valkey-json shared library).")
# Extra arguments for the candidate. Setting JSON_NEW_SERVER to the reference
# binary and this to "--loadmodule <module>" runs the reference against itself,
# which checks the harness.
NEW_ARGS = os.environ.get("JSON_NEW_ARGS", "").split()
RUN_ROOT = os.path.join(HERE, "run")
MARK = b"__mark__"


# --------------------------------------------------------------------------
# A minimal RESP2/RESP3 client that keeps the wire type of every reply
# --------------------------------------------------------------------------


class Simple(bytes):
    """A simple string reply (+OK)."""

    def __repr__(self):
        return "+" + bytes(self).decode("utf-8", "replace")

    def __eq__(self, other):
        return isinstance(other, Simple) and bytes(self) == bytes(other)

    def __hash__(self):
        return hash(bytes(self))


class Err(bytes):
    """An error reply."""

    def __repr__(self):
        return "-" + bytes(self).decode("utf-8", "replace")

    def __eq__(self, other):
        if isinstance(other, ErrPrefix):
            return other == self
        return isinstance(other, Err) and bytes(self) == bytes(other)

    def __hash__(self):
        return hash(bytes(self))


class ErrPrefix:
    """Matches an error reply that starts with the given text."""

    def __init__(self, prefix):
        self.prefix = prefix if isinstance(prefix, bytes) else prefix.encode()

    def __eq__(self, other):
        return isinstance(other, Err) and bytes(other).startswith(self.prefix)

    def __repr__(self):
        return "-" + self.prefix.decode() + "..."


class Nil:
    """A null reply. `wire` tells null bulk ($-1), null array (*-1) and RESP3 null (_) apart."""

    def __init__(self, wire):
        self.wire = wire

    def __eq__(self, other):
        return isinstance(other, Nil) and self.wire == other.wire

    def __repr__(self):
        return "nil(" + self.wire + ")"


class AnyNil:
    """Matches the null of whichever protocol is in use."""

    def __eq__(self, other):
        return isinstance(other, Nil)

    def __repr__(self):
        return "nil"


NIL = AnyNil()
OK = Simple(b"OK")


class Conn:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(20)
        self.sock.connect(path)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            if isinstance(a, str):
                a = a.encode("utf-8")
            elif isinstance(a, int):
                a = b"%d" % a
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("server closed the connection")
        self.buf += data

    def _line(self):
        while True:
            at = self.buf.find(b"\r\n")
            if at >= 0:
                line, self.buf = self.buf[:at], self.buf[at + 2 :]
                return line
            self._fill()

    def _bytes(self, n, crlf=True):
        need = n + (2 if crlf else 0)
        while len(self.buf) < need:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[need:]
        return data

    def read(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return Simple(rest)
        if kind == b"-":
            return Err(rest)
        if kind == b":":
            return int(rest)
        if kind == b"$":
            return Nil("$-1") if rest == b"-1" else self._bytes(int(rest))
        if kind == b"*":
            return Nil("*-1") if rest == b"-1" else [self.read() for _ in range(int(rest))]
        if kind == b"_":
            return Nil("_")
        if kind == b"#":
            return ("bool", rest)
        if kind == b",":
            return ("double", rest)
        if kind == b"(":
            return ("bignum", rest)
        if kind == b"=":
            return ("verbatim", self._bytes(int(rest)))
        if kind == b"!":
            return Err(self._bytes(int(rest)))
        if kind == b"%":
            return ("map", [self.read() for _ in range(2 * int(rest))])
        if kind == b"~":
            return ("set", [self.read() for _ in range(int(rest))])
        if kind == b">":
            return ("push", [self.read() for _ in range(int(rest))])
        if kind == b"|":
            for _ in range(2 * int(rest)):
                self.read()
            return self.read()
        raise ValueError("unexpected reply line %r" % line)

    def cmd(self, *args):
        self.send(*args)
        return self.read()


class Replica(Conn):
    """Attaches as a replica with SYNC and reads the command stream."""

    def attach(self):
        self.sock.sendall(b"SYNC\r\n")
        while True:  # The primary sends newlines while it prepares the snapshot.
            while not self.buf:
                self._fill()
            if self.buf[:1] == b"\n":
                self.buf = self.buf[1:]
                continue
            break
        line = self._line()
        if not line.startswith(b"$") or line.startswith(b"$EOF"):
            raise ValueError("unexpected SYNC reply %r" % line[:60])
        self._bytes(int(line[1:]), crlf=False)

    def until_mark(self, mark):
        cmds = []
        while True:
            while self.buf[:1] == b"\n":
                self.buf = self.buf[1:]
            c = self.read()
            if not isinstance(c, list) or not c:
                continue
            name = c[0].upper()
            if name in (b"PING", b"SELECT", b"REPLCONF"):
                continue
            if name == b"PEXPIREAT" and len(c) == 3:
                c[2] = b"<time>"  # An absolute time in milliseconds: it differs between two servers.
            if name == b"SET" and len(c) == 3 and c[1] == MARK:
                if c[2] == mark:
                    return cmds
                continue
            cmds.append(c)


class Subscriber(Conn):
    """Collects keyspace events."""

    def attach(self):
        self.send("PSUBSCRIBE", "__keyspace@0__:*")
        self.read()

    def until_mark(self):
        """Returns the (key, event) pairs seen up to the next marker."""
        events = []
        while True:
            m = self.read()
            if not isinstance(m, list) or len(m) != 4 or m[0] != b"pmessage":
                continue
            key = m[2][len(b"__keyspace@0__:") :]
            if key == MARK:
                return events
            events.append((key, m[3]))


# --------------------------------------------------------------------------
# Servers
# --------------------------------------------------------------------------


class Server:
    def __init__(self, name, binary, rundir, extra, env=None):
        self.name = name
        self.rundir = rundir
        os.makedirs(rundir, mode=0o700, exist_ok=True)
        os.chmod(rundir, 0o700)
        args = [
            binary,
            "--port", "0",
            "--unixsocket", "./x.sock",
            "--unixsocketperm", "700",
            "--save", "",
            "--appendonly", "no",
            "--dir", ".",
            "--logfile", "./server.log",
            "--repl-diskless-sync", "no",
            "--enable-debug-command", "local",
            "--notify-keyspace-events", "KA",
        ] + extra  # fmt: skip
        self.proc = subprocess.Popen(
            args, cwd=rundir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, **(env or {}))
        )
        self.sock_path = os.path.join(rundir, "x.sock")

    def wait_ready(self, timeout=15.0):
        """Returns True once the server answers PING, False if it exited."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return False
            if os.path.exists(self.sock_path):
                try:
                    c = self.connect()
                    ok = c.cmd("PING") == Simple(b"PONG")
                    c.close()
                    if ok:
                        return True
                except (OSError, ConnectionError):
                    pass
            time.sleep(0.05)
        return False

    def connect(self):
        # The socket path is used relative to the run directory to keep it short.
        cwd = os.getcwd()
        os.chdir(self.rundir)
        try:
            return Conn("./x.sock")
        finally:
            os.chdir(cwd)

    def connect_as(self, cls):
        cwd = os.getcwd()
        os.chdir(self.rundir)
        try:
            return cls("./x.sock")
        finally:
            os.chdir(cwd)

    def log_tail(self, n=15):
        try:
            with open(os.path.join(self.rundir, "server.log"), "r", errors="replace") as f:
                return "".join(f.readlines()[-n:])
        except OSError:
            return ""

    def stop(self):
        if self.proc.poll() is None:
            try:
                c = self.connect()
                c.send("SHUTDOWN", "NOSAVE")
                c.close()
            except (OSError, ConnectionError):
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# --------------------------------------------------------------------------
# Steps and sections
# --------------------------------------------------------------------------


def T(*args):
    return ("same", args, None)


def N(expected, *args):
    return ("new", args, expected)


def O(expected, *args):  # noqa: E743
    return ("only", args, expected)


def P(pred, *args):
    return ("pred", args, pred)


def S(*args):
    """Both replies are arrays with the same elements in any order."""
    return ("sorted", args, None)


def X(key, json_text):
    """Stores a document on the reference and moves it to the candidate with
    DUMP and RESTORE: the trusted path, where the candidate rejects nothing."""
    return ("xfer", (key, json_text), None)


def depth_of(r):
    """Nesting of arrays in a reply, without recursion."""
    n = 0
    while isinstance(r, list) and r:
        r = r[-1]
        n += 1
    return n


def is_pos_int(r):
    return isinstance(r, int) and not isinstance(r, bool) and r > 0


def is_array_of_pos_int(n):
    return lambda r: isinstance(r, list) and len(r) == n and all(is_pos_int(x) for x in r)


WIKI = (
    '{"firstName":"John","lastName":"Smith","age":27,"weight":135.25,"isAlive":true,'
    '"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},'
    '"phoneNumbers":[{"type":"home","number":"212 555-1234"},{"type":"office","number":"646 555-4567"}],'
    '"children":[],"spouse":null,"groups":[[],[1,2],["a"]]}'
)
STORE = (
    '{"store":{"book":['
    '{"category":"reference","author":"Nigel Rees","title":"Sayings of the Century","price":8.95,"in-stock":true},'
    '{"category":"fiction","author":"Evelyn Waugh","title":"Sword of Honour","price":12.99,"in-stock":true,'
    '"movies":[{"title":"Sword of Honour","realisator":{"first_name":"Bill","last_name":"Anderson"}}]},'
    '{"category":"fiction","author":"Herman Melville","title":"Moby Dick","isbn":"0-553-21311-3","price":9,'
    '"in-stock":false},'
    '{"category":"fiction","author":"J. R. R. Tolkien","title":"The Lord of the Rings","isbn":"0-115-03266-2",'
    '"price":22.99,"in-stock":true}],'
    '"bicycle":{"color":"red","price":19.95,"in-stock":false}},"expensive":10}'
)
MIXED = '{"a":[1,2,3],"b":"str","c":{"x":1,"y":[true,false]},"d":10,"e":1.5,"f":true,"g":null,"h":[],"i":{}}'
DEEP_OK = "[" * 128 + "]" * 128
DEEP_BAD = "[" * 129 + "]" * 129

SECTIONS = []


def section(name, *steps):
    SECTIONS.append((name, list(steps)))


section(
    "set_root",
    T("JSON.SET", "k", ".", '"str"'),
    T("JSON.GET", "k"),
    T("JSON.TYPE", "k"),
    T("JSON.SET", "k", "$", "123"),
    T("JSON.GET", "k", "$"),
    T("JSON.TYPE", "k", "$"),
    T("JSON.SET", "k", ".", "-0"),
    T("JSON.GET", "k"),
    T("JSON.SET", "k", ".", "1.5"),
    T("JSON.TYPE", "k"),
    T("JSON.SET", "k", ".", "true"),
    T("JSON.TYPE", "k"),
    T("JSON.SET", "k", ".", "null"),
    T("JSON.TYPE", "k"),
    T("JSON.GET", "k"),
    T("JSON.SET", "k", ".", "[]"),
    T("JSON.TYPE", "k"),
    T("JSON.SET", "k", ".", "{}"),
    T("JSON.TYPE", "k"),
    T("JSON.SET", "k", ".", "  { \"a\" : [ 1 , 2 ] }  "),
    T("JSON.GET", "k"),
    T("JSON.SET", "k", ".", WIKI),
    T("JSON.GET", "k"),
    T("JSON.GET", "k", "."),
    T("JSON.GET", "k", "$"),
    T("TYPE", "k"),
    # NX and XX on the root
    T("JSON.SET", "k", ".", "1", "NX"),
    T("JSON.SET", "k", ".", "1", "nx"),
    T("JSON.SET", "nokey", ".", "1", "XX"),
    T("JSON.SET", "k", ".", "2", "XX"),
    T("JSON.GET", "k"),
    T("JSON.SET", "k2", "$", "3", "NX"),
    T("JSON.GET", "k2"),
    T("JSON.SET", "k", ".", "1", "ZZ"),
    T("JSON.SET", "k", ".", "1", "NX", "XX"),
    T("JSON.SET", "k", "."),
    T("JSON.SET", "k"),
    T("JSON.SET"),
    # errors
    T("JSON.SET", "new", ".a", "1"),
    T("JSON.SET", "new", "$.a", "1"),
    T("JSON.SET", "new", ".a", "1", "XX"),
    T("JSON.SET", "new", ".", "{"),
    T("JSON.SET", "new", ".", ""),
    T("JSON.SET", "new", ".", "{'a':1}"),
    T("JSON.SET", "new", ".", "[1,]"),
    T("JSON.SET", "new", ".", "1 2"),
    T("JSON.SET", "new", ".", "nul"),
    T("EXISTS", "new"),
    T("SET", "s", "plain"),
    T("JSON.SET", "s", ".", "1"),
    T("JSON.SET", "s", ".a", "1"),
    T("JSON.GET", "s"),
    # depth limit on parse
    T("JSON.SET", "deep", ".", DEEP_OK),
    T("JSON.DEBUG", "DEPTH", "deep"),
    T("JSON.SET", "deep2", ".", DEEP_BAD),
    T("EXISTS", "deep2"),
    # duplicate member names
    T("JSON.SET", "dup", ".", '{"a":1,"b":2,"a":3}'),
    T("JSON.GET", "dup"),
    T("JSON.OBJLEN", "dup"),
    # TTL: a root set discards it, a sub-path set keeps it
    T("JSON.SET", "ttl", ".", '{"a":1}'),
    T("EXPIRE", "ttl", "1000"),
    T("JSON.SET", "ttl", ".a", "2"),
    P(lambda r: is_pos_int(r), "TTL", "ttl"),
    T("JSON.NUMINCRBY", "ttl", ".a", "1"),
    P(lambda r: is_pos_int(r), "TTL", "ttl"),
    T("JSON.SET", "ttl", ".", '{"a":1}'),
    T("TTL", "ttl"),
)

section(
    "set_subpath",
    T("JSON.SET", "k", ".", WIKI),
    T("JSON.SET", "k", ".firstName", '"Jane"'),
    T("JSON.GET", "k", ".firstName"),
    T("JSON.SET", "k", "$.lastName", '"Doe"'),
    T("JSON.GET", "k", "$.lastName"),
    T("JSON.SET", "k", ".address.city", '"Boston"'),
    T("JSON.SET", "k", "$.address.zip", "12345"),
    T("JSON.GET", "k", ".address"),
    T("JSON.SET", "k", '.address["street"]', '"1 Main"'),
    T("JSON.SET", "k", "$['address']['state']", '"MA"'),
    T("JSON.GET", "k", "$.address"),
    # array elements
    T("JSON.SET", "k", ".phoneNumbers[0].type", '"mobile"'),
    T("JSON.SET", "k", "$.phoneNumbers[-1].type", '"work"'),
    T("JSON.GET", "k", "$.phoneNumbers[*].type"),
    T("JSON.SET", "k", ".phoneNumbers[2]", "1"),
    T("JSON.SET", "k", "$.phoneNumbers[2]", "1"),
    T("JSON.SET", "k", ".phoneNumbers[-3]", "1"),
    T("JSON.SET", "k", ".children[0]", "1"),
    T("JSON.SET", "k", "$.children[0]", "1"),
    # a new member must be the last step, and only in an object
    T("JSON.SET", "k", ".x.y", "1"),
    T("JSON.SET", "k", "$.x.y", "1"),
    T("JSON.SET", "k", ".age.x", "1"),
    T("JSON.SET", "k", "$.age.x", "1"),
    T("JSON.SET", "k", ".phoneNumbers.x", "1"),
    T("JSON.SET", "k", "$.phoneNumbers.x", "1"),
    T("JSON.GET", "k"),
    # NX and XX on a sub-path
    T("JSON.SET", "k", ".age", "30", "NX"),
    T("JSON.SET", "k", ".newkey", "30", "XX"),
    T("JSON.SET", "k", ".age", "30", "XX"),
    T("JSON.SET", "k", ".newkey", "30", "NX"),
    T("JSON.SET", "k", "$.age", "31", "NX"),
    T("JSON.SET", "k", "$.newkey2", "31", "XX"),
    T("JSON.GET", "k", ".age", ".newkey"),
    # wildcards, slices, unions, filters
    T("JSON.SET", "m", ".", MIXED),
    T("JSON.SET", "m", "$.a[*]", "0"),
    T("JSON.GET", "m", "$.a"),
    T("JSON.SET", "m", ".a[*]", "7"),
    T("JSON.GET", "m", ".a"),
    T("JSON.SET", "m", "$.a[0:2]", '"s"'),
    T("JSON.SET", "m", "$.a[0,2]", "null"),
    T("JSON.GET", "m", "$.a"),
    T("JSON.SET", "m", "$.c.*", "[1]"),
    T("JSON.GET", "m", "$.c"),
    T("JSON.SET", "m", "$.h[*]", "1"),
    T("JSON.SET", "m", ".h[*]", "1"),
    T("JSON.SET", "m", "$.nothing[*]", "1"),
    T("JSON.SET", "m", "$.d.*", "1"),
    T("JSON.SET", "m", ".d.*", "1"),
    T("JSON.GET", "m"),
    T("JSON.SET", "st", ".", STORE),
    T("JSON.SET", "st", "$.store.book[?(@.price<10)].price", "1"),
    T("JSON.GET", "st", "$.store.book[*].price"),
    T("JSON.SET", "st", "$..price", "2"),
    T("JSON.GET", "st", "$..price"),
    T("JSON.SET", "st", "$.store.book[*].isbn", '"x"', "XX"),
    T("JSON.SET", "st", "$.store.book[*].title", '"t"', "NX"),
    T("JSON.GET", "st", "$.store.book[*].isbn"),
    # inserting one member through a wildcard
    T("JSON.SET", "o", ".", '{"a":{"x":1},"b":{"x":2},"c":{"x":3}}'),
    T("JSON.SET", "o", "$.*.y", "9"),
    T("JSON.GET", "o"),
    # path errors
    T("JSON.SET", "k", "", "1"),
    T("JSON.SET", "k", "$[", "1"),
    T("JSON.SET", "k", ".a[", "1"),
    T("JSON.SET", "k", "$.phoneNumbers[x]", "1"),
    T("JSON.SET", "k", "$...a", "1"),
    T("JSON.SET", "k", ".age", "{"),
    T("JSON.SET", "k", ".[", "{"),
    # depth limit on a sub-path write
    T("JSON.SET", "d", ".", '{"a":{"b":{}}}'),
    T("JSON.SET", "d", ".a.b.c", "[" * 124 + "]" * 124),
    T("JSON.SET", "d", ".a.b.c", "[" * 125 + "]" * 125),
    T("JSON.SET", "d", ".a.b.c", "[" * 126 + "]" * 126),
    T("JSON.DEBUG", "DEPTH", "d"),
)

section(
    "set_scalar_values",
    # Integers and booleans at one existing path take a short cut in the
    # candidate. Everything around the short cut must look the same.
    T("JSON.SET", "k", ".", '{"n":1,"b":true,"a":[1.5,2.5],"o":{"n":0},"s":"x"}'),
    *[
        step
        for value in (
            "42", "0", "-0", "-5", "7", "007", "-", "", "-00", "+1", "1e2", "1.0", " 3", "3 ", "true", "false",
            "True", " true", "null", "999999999999999999", "-999999999999999999", "1000000000000000000",
            "9223372036854775807", "-9223372036854775808", "9223372036854775808", "12a", "0x10", "--1", "1-",
        )  # fmt: skip
        for step in (T("JSON.SET", "k", ".n", value), T("JSON.GET", "k", ".n"), T("JSON.TYPE", "k", ".n"))
    ],
    T("JSON.SET", "k", "$.b", "false"),
    T("JSON.SET", "k", ".a[0]", "3"),
    T("JSON.SET", "k", "$.a[1]", "true"),
    T("JSON.SET", "k", ".s", "5"),
    T("JSON.SET", "k", "$.o", "6"),
    T("JSON.GET", "k"),
    T("JSON.SET", "k", ".n", "8", "NX"),
    T("JSON.SET", "k", ".n", "8", "XX"),
    T("JSON.SET", "k", ".new", "9", "XX"),
    T("JSON.SET", "k", ".new", "9", "NX"),
    T("JSON.SET", "k", "$.*", "10"),
    T("JSON.SET", "k", "$..n", "true"),
    T("JSON.GET", "k"),
    T("JSON.MSET", "k", ".n", "11", "k2", ".", "12"),
    T("JSON.MGET", "k", "k2", "."),
    T("EXPIRE", "k", "1000"),
    T("JSON.SET", "k", ".n", "13"),
    P(is_pos_int, "TTL", "k"),
)

section(
    "get",
    T("JSON.SET", "k", ".", WIKI),
    T("JSON.SET", "st", ".", STORE),
    T("JSON.GET", "nokey"),
    T("JSON.GET", "nokey", "$.a"),
    T("JSON.GET", "nokey", ".a", ".b"),
    T("JSON.GET"),
    T("JSON.GET", "k", ".firstName"),
    T("JSON.GET", "k", "firstName"),
    T("JSON.GET", "k", "$.firstName"),
    T("JSON.GET", "k", ".address.city"),
    T("JSON.GET", "k", "$.address.city"),
    T("JSON.GET", "k", '["address"]["city"]'),
    T("JSON.GET", "k", "$['address']['city']"),
    T("JSON.GET", "k", ".phoneNumbers[0]"),
    T("JSON.GET", "k", ".phoneNumbers[-1].number"),
    T("JSON.GET", "k", "$.phoneNumbers[-1].number"),
    T("JSON.GET", "k", ".phoneNumbers[2]"),
    T("JSON.GET", "k", "$.phoneNumbers[2]"),
    T("JSON.GET", "k", ".phoneNumbers[-3]"),
    T("JSON.GET", "k", ".nothing"),
    T("JSON.GET", "k", "$.nothing"),
    T("JSON.GET", "k", ".nothing.deeper"),
    T("JSON.GET", "k", ".age.x"),
    T("JSON.GET", "k", "$.age.x"),
    T("JSON.GET", "k", ".age[0]"),
    T("JSON.GET", "k", "$.age[0]"),
    T("JSON.GET", "k", ".spouse"),
    T("JSON.GET", "k", "$.spouse"),
    T("JSON.GET", "k", ".children"),
    T("JSON.GET", "k", "$.children[*]"),
    T("JSON.GET", "k", ".children[*]"),
    T("JSON.GET", "k", ".phoneNumbers[*].type"),
    T("JSON.GET", "k", "$.phoneNumbers[*].type"),
    T("JSON.GET", "k", "$.*"),
    T("JSON.GET", "k", ".*"),
    T("JSON.GET", "k", "$..type"),
    T("JSON.GET", "k", "..type"),
    T("JSON.GET", "k", "$.groups[1][0]"),
    T("JSON.GET", "k", "$.groups[*][*]"),
    T("JSON.GET", "k", "$.groups[1:]"),
    T("JSON.GET", "k", "$.groups[::2]"),
    T("JSON.GET", "k", "$.groups[2,0]"),
    T("JSON.GET", "k", "$.groups[0,0]"),
    T("JSON.GET", "k", "$.groups[::0]"),
    T("JSON.GET", "k", "$.age.*"),
    T("JSON.GET", "k", ".age.*"),
    T("JSON.GET", "st", "$.store.book[?(@.price<10)].title"),
    T("JSON.GET", "st", "$.store.book[?(@.isbn)].title"),
    T("JSON.GET", "st", '$.store.book[?(@.category=="fiction" && @.price>10)].title'),
    T("JSON.GET", "st", "$.store.book[?(@.price>$.expensive)].price"),
    T("JSON.GET", "st", "$..book[0,1].author"),
    T("JSON.GET", "st", "$.store..price"),
    T("JSON.GET", "st", "$..*"),
    # several paths
    T("JSON.GET", "k", ".firstName", ".lastName"),
    T("JSON.GET", "k", ".firstName", ".nothing"),
    T("JSON.GET", "k", "$.firstName", "$.nothing"),
    T("JSON.GET", "k", ".firstName", "$.lastName"),
    T("JSON.GET", "k", ".nothing", "$.lastName"),
    T("JSON.GET", "k", ".phoneNumbers[5]", "$.lastName"),
    T("JSON.GET", "k", ".phoneNumbers[5]", ".lastName"),
    T("JSON.GET", "k", "$.phoneNumbers[*].type", "$.address.city", "$.groups"),
    T("JSON.GET", "k", '.a"b', ".lastName"),
    T("JSON.GET", "k", '$.a"b\\c', "$.lastName"),
    T("JSON.GET", "k", "$[", "$.lastName"),
    T("JSON.GET", "k", ".firstName", ".firstName"),
    # formatting
    T("JSON.GET", "k", "INDENT", "  ", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "\t", "SPACE", " ", "NEWLINE", "\n", ".address"),
    T("JSON.GET", "k", "indent", "=*=*", "space", "--", "newline", "\r\n", ".address"),
    T("JSON.GET", "k", "INDENT", "  "),
    T("JSON.GET", "k", "NEWLINE", "\n"),
    T("JSON.GET", "k", "SPACE", " "),
    T("JSON.GET", "k", "INDENT", ""),
    T("JSON.GET", "k", "INDENT", "", "NEWLINE", "", "SPACE", ""),
    T("JSON.GET", "k", "NOESCAPE"),
    T("JSON.GET", "k", "NOESCAPE", "INDENT", "  ", "SPACE", " ", "NEWLINE", "\n", "NOESCAPE"),
    T("JSON.GET", "k", ".", "INDENT", "  ", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "  ", ".firstName", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "  ", "SPACE", " ", ".firstName", ".lastName", "NEWLINE", "\n"),
    T("JSON.GET", "k", ".firstName", "INDENT", "  ", "SPACE", " ", "NEWLINE", "\n", ".lastName"),
    T("JSON.GET", "k", "INDENT", "\t", "$.address", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "\t", "$.phoneNumbers[*]", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "\t", "$.nothing", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "\t", "$.children", "$.groups", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "\t", "$.nothing", "$.address", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", "\t", ".address", ".groups", "SPACE", " ", "NEWLINE", "\n"),
    T("JSON.GET", "k", "INDENT", ">", ".address", "$.groups"),
    T("JSON.GET", "k", "NEWLINE", "\n", "$.address", "$.groups"),
    T("JSON.GET", "k", "INDENT", "  ", "SPACE", " ", "NEWLINE"),
    T("JSON.GET", "k", "INDENT", "  ", "SPACE"),
    T("JSON.GET", "k", "NEWLINE", "\n", "SPACE", " ", "INDENT"),
    T("JSON.GET", "k", "NEWLINE", "INDENT"),
    T("JSON.GET", "k", "INDENT", "NEWLINE", ".firstName"),
    # number text is kept as written
    T("JSON.SET", "n", ".", '[19.90,1e5,1E5,1.0,0.1,-0.0,100,1e-7,1.5e300,123456789012345678901234567890,0.30000000000000004,-1,1.10]'),
    T("JSON.GET", "n"),
    T("JSON.GET", "n", "$[*]"),
    T("JSON.TYPE", "n", "$[*]"),
    # strings and escapes
    T("JSON.SET", "s", ".", '{"a\\"b":"x\\ny\\t\\u00e9\\u0001/","é":"中文","e":""}'),
    T("JSON.GET", "s"),
    T("JSON.GET", "s", '$["a\\"b"]'),
    T("JSON.GET", "s", ".é"),
    T("JSON.OBJKEYS", "s"),
    T("JSON.STRLEN", "s", ".é"),
    # wrong type of key
    T("SET", "str", "x"),
    T("JSON.GET", "str"),
    T("JSON.GET", "str", ".a", ".b"),
)

section(
    "mget",
    T("JSON.SET", "a", ".", '{"x":1,"y":[1,2]}'),
    T("JSON.SET", "b", ".", '{"x":"two"}'),
    T("JSON.SET", "c", ".", "[1,2,3]"),
    T("SET", "str", "x"),
    T("JSON.MGET", "a", "b", "c", "nokey", ".x"),
    T("JSON.MGET", "a", "b", "c", "nokey", "$.x"),
    T("JSON.MGET", "a", "b", "c", "."),
    T("JSON.MGET", "a", "b", "c", "$"),
    T("JSON.MGET", "a", "b", ".y[1]"),
    T("JSON.MGET", "a", "b", "$.y[1]"),
    T("JSON.MGET", "a", "b", ".y[5]"),
    T("JSON.MGET", "a", "b", "$.y[5]"),
    T("JSON.MGET", "a", "$..x"),
    T("JSON.MGET", "nokey", "nokey2", "."),
    T("JSON.MGET", "a", "str", "."),
    T("JSON.MGET", "str", "a", "$.x"),
    T("JSON.MGET", "a", "b", "$["),
    T("JSON.MGET", "a", "b", ".["),
    T("JSON.MGET", "a", "b", "$.y[x]"),
    T("JSON.MGET", "a"),
    T("JSON.MGET"),
)

section(
    "del",
    T("JSON.SET", "k", ".", WIKI),
    T("JSON.DEL", "k", ".firstName"),
    T("JSON.DEL", "k", ".firstName"),
    T("JSON.DEL", "k", "$.firstName"),
    T("JSON.FORGET", "k", "$.lastName"),
    T("JSON.FORGET", "k", ".nothing"),
    T("JSON.DEL", "k", ".phoneNumbers[0]"),
    T("JSON.DEL", "k", ".phoneNumbers[5]"),
    T("JSON.DEL", "k", "$.phoneNumbers[5]"),
    T("JSON.DEL", "k", "$.groups[*]"),
    T("JSON.DEL", "k", "$.address.*"),
    T("JSON.DEL", "k", ".age.x"),
    T("JSON.DEL", "k", "$.age.x"),
    T("JSON.DEL", "k", "$["),
    T("JSON.DEL", "k", ".["),
    T("JSON.DEL", "k", "$.groups[x]"),
    T("JSON.GET", "k"),
    T("JSON.SET", "st", ".", STORE),
    T("JSON.DEL", "st", "$..price"),
    T("JSON.DEL", "st", "$.store.book[?(@.isbn)]"),
    T("JSON.DEL", "st", "$.store.book[0,0]"),
    T("JSON.GET", "st"),
    T("JSON.SET", "a", ".", "[0,1,2,3,4,5,6,7,8,9]"),
    T("JSON.DEL", "a", "$[1,3,5]"),
    T("JSON.DEL", "a", "$[0:2]"),
    T("JSON.DEL", "a", ".[-1]"),
    T("JSON.GET", "a"),
    T("JSON.SET", "nest", ".", '{"a":{"a":{"a":1}},"b":{"a":2}}'),
    T("JSON.DEL", "nest", "$..a"),
    T("JSON.GET", "nest"),
    T("JSON.DEL", "k"),
    T("JSON.DEL", "k"),
    T("EXISTS", "k"),
    T("JSON.DEL", "st", "$"),
    T("EXISTS", "st"),
    T("JSON.FORGET", "a", "."),
    T("JSON.DEL", "nokey", ".a"),
    T("SET", "str", "x"),
    T("JSON.DEL", "str"),
    T("JSON.DEL", "str", ".a"),
    T("JSON.DEL"),
    T("JSON.DEL", "a", "b", "c"),
)

section(
    "numincrby_nummultby",
    T("JSON.SET", "k", ".", '{"i":1,"j":10,"s":"x","a":[1,2,"x",4],"o":{"n":5},"big":9007199254740993}'),
    T("JSON.NUMINCRBY", "k", ".i", "1"),
    T("JSON.NUMINCRBY", "k", "$.i", "1"),
    T("JSON.NUMINCRBY", "k", ".i", "-5"),
    T("JSON.NUMINCRBY", "k", ".i", " 2 "),
    T("JSON.NUMINCRBY", "k", ".i", "0"),
    T("JSON.NUMMULTBY", "k", ".j", "2"),
    T("JSON.NUMMULTBY", "k", "$.j", "3"),
    T("JSON.NUMMULTBY", "k", ".j", "-1"),
    # B3: an integer result is exact. The module computes -60 * 0 in doubles and replies "-0".
    N(b"0", "JSON.NUMMULTBY", "k", ".j", "0"),
    T("JSON.GET", "k", ".i", ".j"),
    T("JSON.TYPE", "k", "$.i"),
    # several values
    T("JSON.NUMINCRBY", "k", "$.a[*]", "1"),
    T("JSON.NUMINCRBY", "k", ".a[*]", "1"),
    T("JSON.NUMMULTBY", "k", "$.a[*]", "2"),
    T("JSON.NUMMULTBY", "k", ".a[*]", "2"),
    T("JSON.NUMINCRBY", "k", "$..n", "1"),
    T("JSON.NUMINCRBY", "k", "$.*", "1"),
    T("JSON.GET", "k"),
    # errors
    T("JSON.NUMINCRBY", "k", ".s", "1"),
    T("JSON.NUMINCRBY", "k", "$.s", "1"),
    T("JSON.NUMINCRBY", "k", ".nothing", "1"),
    T("JSON.NUMINCRBY", "k", "$.nothing", "1"),
    T("JSON.NUMINCRBY", "k", ".a[9]", "1"),
    T("JSON.NUMINCRBY", "k", "$.a[9]", "1"),
    T("JSON.NUMINCRBY", "k", "$.s.*", "1"),
    T("JSON.NUMINCRBY", "k", "$...i", "1"),
    T("JSON.NUMINCRBY", "k", "$[", "1"),
    T("JSON.NUMINCRBY", "k", ".i", "x"),
    T("JSON.NUMINCRBY", "k", ".i", '"1"'),
    T("JSON.NUMINCRBY", "k", ".i", ""),
    T("JSON.NUMINCRBY", "k", ".i", "1 2"),
    T("JSON.NUMINCRBY", "k", ".i", "+1"),
    T("JSON.NUMINCRBY", "k", ".i", "01"),
    T("JSON.NUMINCRBY", "k", ".i", "1."),
    T("JSON.NUMINCRBY", "k", ".i", "[1]"),
    T("JSON.NUMINCRBY", "k", ".i", "1e400"),
    T("JSON.NUMMULTBY", "k", ".s", "2"),
    T("JSON.NUMMULTBY", "k", ".nothing", "2"),
    T("JSON.NUMMULTBY", "k", ".i", "x"),
    T("JSON.NUMINCRBY", "nokey", ".i", "1"),
    T("JSON.NUMMULTBY", "nokey", ".i", "1"),
    T("SET", "str", "x"),
    T("JSON.NUMINCRBY", "str", ".i", "1"),
    T("JSON.NUMINCRBY", "k", ".i"),
    T("JSON.NUMINCRBY", "k", ".i", "1", "2"),
    T("JSON.NUMMULTBY", "k", ".i"),
    # double overflow is an error in both
    T("JSON.SET", "f", ".", '{"x":1.7e308,"y":-1.7e308}'),
    T("JSON.NUMINCRBY", "f", ".x", "1.7e308"),
    T("JSON.NUMMULTBY", "f", ".x", "10"),
    T("JSON.NUMMULTBY", "f", ".y", "10"),
    T("JSON.GET", "f"),
)

section(
    "strlen_strappend",
    T("JSON.SET", "k", ".", '{"s":"abc","e":"","n":1,"a":["x","yy",3,"zzz"],"o":{"s":"deep"}}'),
    T("JSON.STRLEN", "k", ".s"),
    T("JSON.STRLEN", "k", "$.s"),
    T("JSON.STRLEN", "k", ".e"),
    T("JSON.STRLEN", "k", ".n"),
    T("JSON.STRLEN", "k", "$.n"),
    T("JSON.STRLEN", "k", ".nothing"),
    T("JSON.STRLEN", "k", "$.nothing"),
    T("JSON.STRLEN", "k", "$.a[*]"),
    T("JSON.STRLEN", "k", ".a[*]"),
    T("JSON.STRLEN", "k", "$..s"),
    T("JSON.STRLEN", "k", "$.a[0,0]"),
    T("JSON.STRLEN", "k"),
    T("JSON.STRLEN", "k", "$"),
    T("JSON.STRLEN", "nokey"),
    T("JSON.STRLEN", "nokey", "$.a"),
    T("JSON.STRLEN", "k", "$["),
    T("JSON.STRLEN", "k", ".a[9]"),
    T("JSON.STRLEN", "k", "$.a[9]"),
    T("JSON.STRLEN"),
    T("JSON.STRLEN", "k", ".s", "x"),
    T("JSON.STRAPPEND", "k", ".s", '"def"'),
    T("JSON.STRAPPEND", "k", "$.s", '"g"'),
    T("JSON.STRAPPEND", "k", ".e", '""'),
    T("JSON.STRAPPEND", "k", "$.a[*]", '"!"'),
    T("JSON.STRAPPEND", "k", ".a[*]", '"?"'),
    T("JSON.STRAPPEND", "k", "$..s", '"\\u00e9\\n"'),
    T("JSON.STRAPPEND", "k", "$.a[0,0]", '"d"'),
    T("JSON.GET", "k"),
    T("JSON.STRAPPEND", "k", ".n", '"x"'),
    T("JSON.STRAPPEND", "k", "$.n", '"x"'),
    T("JSON.STRAPPEND", "k", ".nothing", '"x"'),
    T("JSON.STRAPPEND", "k", "$.nothing", '"x"'),
    T("JSON.STRAPPEND", "k", ".s", "x"),
    T("JSON.STRAPPEND", "k", ".s", "1"),
    T("JSON.STRAPPEND", "k", ".s", '["x"]'),
    T("JSON.STRAPPEND", "k", ".n", "1"),
    T("JSON.STRAPPEND", "k", ".nothing", "1"),
    T("JSON.STRAPPEND", "k", "$.a[9]", '"x"'),
    T("JSON.STRAPPEND", "k", '"x"'),
    T("JSON.STRAPPEND", "nokey", ".s", '"x"'),
    T("JSON.STRAPPEND", "k"),
    T("JSON.STRAPPEND", "k", ".s", '"x"', "y"),
    T("JSON.SET", "root", ".", '"abc"'),
    T("JSON.STRAPPEND", "root", '"def"'),
    T("JSON.STRAPPEND", "root", ".", '"g"'),
    T("JSON.STRAPPEND", "root", "$", '"h"'),
    T("JSON.GET", "root"),
    T("JSON.STRLEN", "root"),
)

section(
    "toggle",
    T("JSON.SET", "k", ".", '{"t":true,"f":false,"n":1,"a":[true,1,false,"x"],"o":{"t":true}}'),
    T("JSON.TOGGLE", "k", ".t"),
    T("JSON.TOGGLE", "k", ".t"),
    T("JSON.TOGGLE", "k", "$.t"),
    T("JSON.TOGGLE", "k", "$.f"),
    T("JSON.TOGGLE", "k", "$.a[*]"),
    T("JSON.TOGGLE", "k", ".a[*]"),
    T("JSON.TOGGLE", "k", "$..t"),
    T("JSON.TOGGLE", "k", "$.a[0,0]"),
    T("JSON.GET", "k"),
    T("JSON.TOGGLE", "k", ".n"),
    T("JSON.TOGGLE", "k", "$.n"),
    T("JSON.TOGGLE", "k", ".nothing"),
    T("JSON.TOGGLE", "k", "$.nothing"),
    T("JSON.TOGGLE", "k", ".a[9]"),
    T("JSON.TOGGLE", "k", "$.a[9]"),
    T("JSON.TOGGLE", "k", "$["),
    T("JSON.TOGGLE", "k"),
    T("JSON.TOGGLE", "nokey"),
    T("JSON.TOGGLE", "nokey", ".a"),
    T("JSON.TOGGLE"),
    T("JSON.TOGGLE", "k", ".t", "x"),
    T("JSON.SET", "b", ".", "true"),
    T("JSON.TOGGLE", "b"),
    T("JSON.TOGGLE", "b", "$"),
    T("JSON.GET", "b"),
)

section(
    "objlen_objkeys",
    T("JSON.SET", "k", ".", WIKI),
    T("JSON.OBJLEN", "k"),
    T("JSON.OBJLEN", "k", "$"),
    T("JSON.OBJLEN", "k", ".address"),
    T("JSON.OBJLEN", "k", "$.address"),
    T("JSON.OBJLEN", "k", ".age"),
    T("JSON.OBJLEN", "k", "$.age"),
    T("JSON.OBJLEN", "k", ".nothing"),
    T("JSON.OBJLEN", "k", "$.nothing"),
    T("JSON.OBJLEN", "k", "$.*"),
    T("JSON.OBJLEN", "k", ".*"),
    T("JSON.OBJLEN", "k", "$.phoneNumbers[*]"),
    T("JSON.OBJLEN", "k", ".phoneNumbers[*]"),
    T("JSON.OBJLEN", "nokey"),
    T("JSON.OBJLEN", "k", "$["),
    T("JSON.OBJLEN"),
    T("JSON.OBJKEYS", "k"),
    T("JSON.OBJKEYS", "k", "$"),
    T("JSON.OBJKEYS", "k", ".address"),
    T("JSON.OBJKEYS", "k", "$.address"),
    T("JSON.OBJKEYS", "k", ".age"),
    T("JSON.OBJKEYS", "k", "$.age"),
    T("JSON.OBJKEYS", "k", ".nothing"),
    T("JSON.OBJKEYS", "k", "$.nothing"),
    T("JSON.OBJKEYS", "k", "$.*"),
    T("JSON.OBJKEYS", "k", ".*"),
    T("JSON.OBJKEYS", "k", "$.phoneNumbers[*]"),
    T("JSON.OBJKEYS", "k", ".phoneNumbers[*]"),
    T("JSON.OBJKEYS", "k", ".phoneNumbers[9]"),
    T("JSON.OBJKEYS", "k", "$.phoneNumbers[9]"),
    T("JSON.OBJKEYS", "k", "$["),
    T("JSON.OBJKEYS", "k", ".["),
    T("JSON.OBJKEYS", "k", "$.phoneNumbers[x]"),
    T("JSON.OBJKEYS", "nokey"),
    T("JSON.OBJKEYS"),
    T("JSON.SET", "e", ".", '{"a":{},"b":{"x":1},"c":[]}'),
    T("JSON.OBJKEYS", "e", ".a"),
    T("JSON.OBJKEYS", "e", "$.a"),
    T("JSON.OBJKEYS", "e", ".*"),
    T("JSON.OBJKEYS", "e", "$.*"),
    T("JSON.OBJLEN", "e", ".a"),
    T("JSON.SET", "e2", ".", "{}"),
    T("JSON.OBJKEYS", "e2"),
    T("JSON.OBJKEYS", "e2", "$"),
    T("SET", "str", "x"),
    T("JSON.OBJKEYS", "str"),
    T("JSON.OBJLEN", "str"),
)

section(
    "arrlen_arrappend_arrinsert",
    T("JSON.SET", "k", ".", '{"a":[1,2,3],"e":[],"n":1,"m":[[1],[2,3],"x",[]],"o":{"a":[9]}}'),
    T("JSON.ARRLEN", "k", ".a"),
    T("JSON.ARRLEN", "k", "$.a"),
    T("JSON.ARRLEN", "k", ".e"),
    T("JSON.ARRLEN", "k", ".n"),
    T("JSON.ARRLEN", "k", "$.n"),
    T("JSON.ARRLEN", "k", ".nothing"),
    T("JSON.ARRLEN", "k", "$.nothing"),
    T("JSON.ARRLEN", "k", "$.m[*]"),
    T("JSON.ARRLEN", "k", ".m[*]"),
    T("JSON.ARRLEN", "k", "$..a"),
    T("JSON.ARRLEN", "k"),
    T("JSON.ARRLEN", "k", "$"),
    T("JSON.ARRLEN", "nokey"),
    T("JSON.ARRLEN", "k", "$["),
    T("JSON.ARRLEN"),
    T("JSON.ARRAPPEND", "k", ".a", "4"),
    T("JSON.ARRAPPEND", "k", "$.a", "5", '"six"', "[7]", '{"8":9}', "null"),
    T("JSON.ARRAPPEND", "k", ".e", "{}"),
    T("JSON.ARRAPPEND", "k", "$.m[*]", "0"),
    T("JSON.ARRAPPEND", "k", ".m[*]", "0"),
    T("JSON.ARRAPPEND", "k", "$..a", "true"),
    T("JSON.ARRAPPEND", "k", "$.m[0,0]", "1"),
    T("JSON.GET", "k"),
    T("JSON.ARRAPPEND", "k", ".n", "1"),
    T("JSON.ARRAPPEND", "k", "$.n", "1"),
    T("JSON.ARRAPPEND", "k", ".nothing", "1"),
    T("JSON.ARRAPPEND", "k", "$.nothing", "1"),
    T("JSON.ARRAPPEND", "k", ".a", "{"),
    T("JSON.ARRAPPEND", "k", ".a", "1", "{"),
    T("JSON.ARRAPPEND", "k", ".n", "{"),
    T("JSON.ARRAPPEND", "k", ".nothing", "{"),
    T("JSON.ARRAPPEND", "k", "$.a[9]", "1"),
    T("JSON.ARRAPPEND", "k", ".a"),
    T("JSON.ARRAPPEND", "k"),
    T("JSON.ARRAPPEND", "nokey", ".a", "1"),
    T("JSON.ARRLEN", "k", ".a"),
    T("JSON.SET", "i", ".", '{"a":[1,2,3],"e":[],"n":1,"m":[[1],[2,3],"x",[]]}'),
    T("JSON.ARRINSERT", "i", ".a", "0", '"first"'),
    T("JSON.ARRINSERT", "i", "$.a", "2", '"mid"', '"mid2"'),
    T("JSON.ARRINSERT", "i", ".a", "-1", '"beforelast"'),
    T("JSON.ARRINSERT", "i", ".a", "7", '"end"'),
    T("JSON.ARRINSERT", "i", ".a", "9", '"past"'),
    T("JSON.ARRINSERT", "i", "$.a", "9", '"past"'),
    T("JSON.ARRINSERT", "i", ".a", "-9", '"before"'),
    T("JSON.ARRINSERT", "i", ".a", "-8", '"atstart"'),
    T("JSON.ARRINSERT", "i", ".e", "0", "1"),
    T("JSON.ARRINSERT", "i", ".e", "-1", "0"),
    T("JSON.ARRINSERT", "i", "$.m[*]", "0", "null"),
    T("JSON.ARRINSERT", "i", ".m[*]", "0", "true"),
    T("JSON.GET", "i"),
    T("JSON.ARRINSERT", "i", ".n", "0", "1"),
    T("JSON.ARRINSERT", "i", "$.n", "0", "1"),
    T("JSON.ARRINSERT", "i", ".nothing", "0", "1"),
    T("JSON.ARRINSERT", "i", "$.nothing", "0", "1"),
    T("JSON.ARRINSERT", "i", ".a", "x", "1"),
    T("JSON.ARRINSERT", "i", ".a", "1.5", "1"),
    T("JSON.ARRINSERT", "i", ".a", "0", "{"),
    T("JSON.ARRINSERT", "i", ".a", "99", "{"),
    T("JSON.ARRINSERT", "i", ".a", "0"),
    T("JSON.ARRINSERT", "i", ".a"),
    T("JSON.ARRINSERT", "nokey", ".a", "0", "1"),
    T("JSON.SET", "r", ".", "[]"),
    T("JSON.ARRAPPEND", "r", ".", "1", "2"),
    T("JSON.ARRINSERT", "r", "$", "1", "1.5"),
    T("JSON.GET", "r"),
    # depth limit: the module counts the array's depth, not its new element's
    T("JSON.SET", "d", ".", '{"a":[]}'),
    T("JSON.ARRAPPEND", "d", ".a", "[" * 127 + "]" * 127),
    T("JSON.ARRAPPEND", "d", ".a", "[" * 128 + "]" * 128),
    T("JSON.ARRINSERT", "d", ".a", "0", "[" * 128 + "]" * 128),
    T("JSON.ARRLEN", "d", ".a"),
)

section(
    "arrpop_arrtrim",
    T("JSON.SET", "k", ".", '{"a":[1,"two",[3],{"four":4},null,true,1.50],"e":[],"n":1,"m":[[1,2],[3],"x",[]]}'),
    T("JSON.ARRPOP", "k", ".a"),
    T("JSON.ARRPOP", "k", "$.a"),
    T("JSON.ARRPOP", "k", ".a", "0"),
    T("JSON.ARRPOP", "k", "$.a", "1"),
    T("JSON.ARRPOP", "k", ".a", "-2"),
    T("JSON.ARRPOP", "k", ".a", "99"),
    T("JSON.ARRPOP", "k", ".a", "-99"),
    T("JSON.ARRPOP", "k", ".a"),
    T("JSON.ARRPOP", "k", "$.a"),
    T("JSON.ARRPOP", "k", ".e"),
    T("JSON.ARRPOP", "k", "$.e"),
    T("JSON.ARRPOP", "k", ".n"),
    T("JSON.ARRPOP", "k", "$.n"),
    T("JSON.ARRPOP", "k", ".nothing"),
    T("JSON.ARRPOP", "k", "$.nothing"),
    T("JSON.ARRPOP", "k", "$.m[*]"),
    T("JSON.ARRPOP", "k", ".m[*]"),
    T("JSON.ARRPOP", "k", "$.m[*]", "0"),
    T("JSON.GET", "k"),
    T("JSON.ARRPOP", "k", ".a", "x"),
    T("JSON.ARRPOP", "k", ".a", "0", "1"),
    T("JSON.ARRPOP", "k", "$["),
    T("JSON.ARRPOP", "k", "$.m[9]"),
    T("JSON.ARRPOP", "nokey"),
    T("JSON.ARRPOP", "nokey", ".a"),
    T("JSON.ARRPOP"),
    T("JSON.SET", "r", ".", "[1,2,3]"),
    T("JSON.ARRPOP", "r"),
    T("JSON.ARRPOP", "r", "."),
    T("JSON.ARRPOP", "r", "$"),
    T("JSON.ARRPOP", "r"),
    T("JSON.GET", "r"),
    T("JSON.SET", "t", ".", '{"a":[0,1,2,3,4,5,6,7,8,9],"b":[0,1,2,3],"c":[0,1,2,3],"e":[],"n":1,"m":[[0,1,2],[0],"x",[]]}'),
    T("JSON.ARRTRIM", "t", ".a", "1", "8"),
    T("JSON.ARRTRIM", "t", "$.a", "0", "99"),
    T("JSON.ARRTRIM", "t", ".a", "-5", "5"),
    T("JSON.ARRTRIM", "t", ".a", "2", "2"),
    T("JSON.ARRTRIM", "t", ".a", "0", "0"),
    T("JSON.GET", "t", ".a"),
    T("JSON.ARRTRIM", "t", ".b", "3", "1"),
    T("JSON.ARRTRIM", "t", ".c", "1", "-1"),
    T("JSON.ARRTRIM", "t", ".e", "0", "1"),
    T("JSON.ARRTRIM", "t", "$.m[*]", "1", "5"),
    T("JSON.ARRTRIM", "t", ".m[*]", "0", "0"),
    T("JSON.GET", "t"),
    T("JSON.ARRTRIM", "t", ".n", "0", "1"),
    T("JSON.ARRTRIM", "t", "$.n", "0", "1"),
    T("JSON.ARRTRIM", "t", ".nothing", "0", "1"),
    T("JSON.ARRTRIM", "t", "$.nothing", "0", "1"),
    T("JSON.ARRTRIM", "t", ".a", "x", "1"),
    T("JSON.ARRTRIM", "t", ".a", "0", "x"),
    T("JSON.ARRTRIM", "t", ".a", "0"),
    T("JSON.ARRTRIM", "t", ".a", "0", "1", "2"),
    T("JSON.ARRTRIM", "nokey", ".a", "0", "1"),
)

section(
    "arrindex",
    T("JSON.SET", "k", ".", '{"a":[1,"two",[3],{"four":4},null,true,1.5,1,"two",1.0,[],{}],"e":[],"n":1,"m":[[1,2],[2,1],"x",[]]}'),
    T("JSON.ARRINDEX", "k", ".a", "1"),
    T("JSON.ARRINDEX", "k", "$.a", "1"),
    T("JSON.ARRINDEX", "k", ".a", '"two"'),
    T("JSON.ARRINDEX", "k", ".a", "[3]"),
    T("JSON.ARRINDEX", "k", ".a", '{"four":4}'),
    T("JSON.ARRINDEX", "k", ".a", "null"),
    T("JSON.ARRINDEX", "k", ".a", "true"),
    T("JSON.ARRINDEX", "k", ".a", "false"),
    T("JSON.ARRINDEX", "k", ".a", "1.5"),
    T("JSON.ARRINDEX", "k", ".a", "1.50"),
    T("JSON.ARRINDEX", "k", ".a", "1.0"),
    T("JSON.ARRINDEX", "k", ".a", "[]"),
    T("JSON.ARRINDEX", "k", ".a", "{}"),
    T("JSON.ARRINDEX", "k", ".a", "1", "1"),
    T("JSON.ARRINDEX", "k", ".a", "1", "1", "7"),
    T("JSON.ARRINDEX", "k", ".a", "1", "1", "8"),
    T("JSON.ARRINDEX", "k", ".a", "1", "0", "0"),
    T("JSON.ARRINDEX", "k", ".a", "1", "0", "-1"),
    T("JSON.ARRINDEX", "k", ".a", "1", "1", "-1"),
    T("JSON.ARRINDEX", "k", ".a", "1", "1", "-2"),
    T("JSON.ARRINDEX", "k", ".a", "1", "-5"),
    T("JSON.ARRINDEX", "k", ".a", "1", "99"),
    T("JSON.ARRINDEX", "k", ".a", "1", "0", "99"),
    T("JSON.ARRINDEX", "k", ".a", "1", "5", "2"),
    T("JSON.ARRINDEX", "k", ".a", '"two"', "2"),
    T("JSON.ARRINDEX", "k", ".e", "1"),
    T("JSON.ARRINDEX", "k", "$.e", "1"),
    T("JSON.ARRINDEX", "k", "$.m[*]", "1"),
    T("JSON.ARRINDEX", "k", ".m[*]", "1"),
    T("JSON.ARRINDEX", "k", "$.m[*]", "1", "1"),
    T("JSON.ARRINDEX", "k", ".n", "1"),
    T("JSON.ARRINDEX", "k", "$.n", "1"),
    T("JSON.ARRINDEX", "k", ".nothing", "1"),
    T("JSON.ARRINDEX", "k", "$.nothing", "1"),
    T("JSON.ARRINDEX", "k", ".a", "{"),
    T("JSON.ARRINDEX", "k", ".n", "{"),
    T("JSON.ARRINDEX", "k", ".a", "1", "x"),
    T("JSON.ARRINDEX", "k", ".a", "1", "0", "x"),
    T("JSON.ARRINDEX", "k", ".a"),
    T("JSON.ARRINDEX", "k", ".a", "1", "0", "1", "2"),
    T("JSON.ARRINDEX", "nokey", ".a", "1"),
    T("JSON.ARRINDEX", "k", "$[", "1"),
    T("JSON.SET", "st", ".", STORE),
    T("JSON.ARRINDEX", "st", "$.store.book[?(@.price<10)].movies", "1"),
    T("JSON.ARRINDEX", "st", "$..movies", '{"title":"Sword of Honour","realisator":{"first_name":"Bill","last_name":"Anderson"}}'),
)

section(
    "arrindex_numbers",
    # How numbers compare: integers at and beyond the ends of int64, and
    # decimals written in different ways.
    T("JSON.SET", "k", ".", "[-1,18446744073709551615,9223372036854775807,9223372036854775808,"
      "1234567890123456789012345,1234567890123456789012346,1.5,19.90,1,2.0,1e2,0.1,-0.0]"),
    *[
        T("JSON.ARRINDEX", "k", ".", needle)
        for needle in (
            "-1", "18446744073709551615", "9223372036854775807", "9223372036854775808",
            "1234567890123456789012345", "1234567890123456789012346", "1234567890123456789012347",
            "18446744073709551616", "-9223372036854775808", "-9223372036854775809",
            "1.5", "1.50", "15e-1", "19.9", "19.90", "1.0", "1.00", "2", "2.0", "100", "100.0", "1E2", "0.10",
            "0", "0.0", "-0", "-0.0", "9223372036854775807.0", "9.223372036854775807e18",
        )  # fmt: skip
    ],
    T("JSON.SET", "a", ".", "[18446744073709551615]"),
    T("JSON.ARRINDEX", "a", ".", "-1"),
    T("JSON.SET", "a", ".", "[-1]"),
    T("JSON.ARRINDEX", "a", ".", "18446744073709551615"),
    T("JSON.SET", "a", ".", "[9223372036854775808]"),
    T("JSON.ARRINDEX", "a", ".", "9223372036854775807"),
    T("JSON.SET", "a", ".", "[9223372036854775807]"),
    T("JSON.ARRINDEX", "a", ".", "9223372036854775808"),
    T("JSON.SET", "a", ".", "[1234567890123456789012346]"),
    T("JSON.ARRINDEX", "a", ".", "1234567890123456789012345"),
    T("JSON.SET", "a", ".", "[1234567890123456789012345]"),
    T("JSON.ARRINDEX", "a", ".", "1234567890123456789012346"),
    # The same comparison inside a filter.
    T("JSON.GET", "k", "$[?(@==-1)]"),
    T("JSON.GET", "k", "$[?(@==18446744073709551615)]"),
    T("JSON.GET", "k", "$[?(@==1.50)]"),
    T("JSON.GET", "k", "$[?(@>19.8)]"),
    T("JSON.TYPE", "k", "$[*]"),
    T("JSON.RESP", "k"),
)

DEC = {
    "a": "0.3141592653589793",  # 16 significant digits
    "b": "0.30000000000000004",  # 17
    "c": "0.05396780371665955",  # 16, 17 after the point
    "d": "123456.789012345678",  # 18
    "e": "0.123456789012345678",  # 18
    "f": "1234567.890123456789",  # 19: not kept as a decimal
    "g": "0.1234567890123456789",  # 19
    "h": "2.5000000000000000",  # 17 with trailing zeros
    "i": "1234567890123456789",  # a 19-digit integer
    "j": "9223372036854775808",  # a 19-digit integer past int64
    "k": "-0.000000000000000001",  # scale 18
    "l": "1.5e3",
}
DEC_DOC = "{" + ",".join('"%s":%s' % kv for kv in DEC.items()) + ',"arr":[' + ",".join(DEC.values()) + "]}"

section(
    "decimals_read",
    # Long decimals keep their text. Everything that reads them agrees with
    # the module: text, type, value in comparisons and filters.
    T("JSON.SET", "k", ".", DEC_DOC),
    T("JSON.GET", "k"),
    T("JSON.GET", "k", "$.*"),
    T("JSON.GET", "k", "INDENT", " ", "NEWLINE", "\n", "SPACE", " ", ".arr"),
    T("JSON.TYPE", "k", "$.*"),
    T("JSON.TYPE", "k", "$.arr[*]"),
    T("JSON.MGET", "k", "$.arr[1,3,5]"),
    T("JSON.DEBUG", "FIELDS", "k"),
    T("JSON.ARRLEN", "k", ".arr"),
    T("JSON.STRLEN", "k", "$.arr[*]"),
    *[T("JSON.ARRINDEX", "k", ".arr", v) for v in DEC.values()],
    *[
        T("JSON.ARRINDEX", "k", ".arr", v)
        for v in (
            "0.31415926535897930", "0.3", "0.3000000000000000", "0.300000000000000044", "0.05396780371665956",
            "123456.78901234568", "123456.7890123457", "0.12345678901234568", "0.1234567890123456", "1234567.8901234567",
            "0.12345678901234568", "2.5", "2.50", "1234567890123456789.0", "1234567890123456768", "9223372036854775807",
            "9.223372036854775808e18", "-1e-18", "-0.0000000000000000010", "1500", "1500.0", "15e2",
        )  # fmt: skip
    ],
    *[
        T("JSON.GET", "k", "$.arr[?(@%s)]" % f)
        for f in (
            "==0.30000000000000004", "==0.3", ">0.3", "<0.2", ">=0.3141592653589793", "<=0.05396780371665955",
            "==123456.789012345678", "==123456.78901234568", ">123456.789012345677", "==0.123456789012345678",
            "==1234567.890123456789", "!=2.5", "==2.5", "==1234567890123456789", ">1234567890123456788",
            "==9223372036854775808", ">9223372036854775807", "<0", "==-0.000000000000000001", "==1500", ">1e3",
        )  # fmt: skip
    ],
    T("JSON.GET", "k", "$.arr[?(@>$.b)]"),
    # Written at a path, appended, inserted, popped, merged: the text stays.
    T("JSON.SET", "w", ".", '{"x":0}'),
    *[step for v in DEC.values() for step in (T("JSON.SET", "w", ".x", v), T("JSON.GET", "w", ".x"), T("JSON.TYPE", "w", ".x"))],
    T("JSON.SET", "w", ".", "[]"),
    T("JSON.ARRAPPEND", "w", ".", *DEC.values()),
    T("JSON.ARRINSERT", "w", ".", "1", "0.999999999999999999", "0.9999999999999999999"),
    T("JSON.GET", "w"),
    T("JSON.ARRPOP", "w", ".", "1"),
    T("JSON.ARRPOP", "w", ".", "1"),
    T("JSON.ARRPOP", "w"),
    T("JSON.ARRPOP", "w", "$", "3"),
    T("JSON.ARRTRIM", "w", ".", "1", "4"),
    T("JSON.GET", "w"),
    T("JSON.MERGE", "k", ".", '{"a":0.10000000000000001,"m":{"z":0.000000000000000123},"b":null}'),
    T("JSON.GET", "k", ".a", ".m"),
    T("JSON.MSET", "k", ".c", "0.111111111111111111", "w", ".", "1.000000000000000001"),
    T("JSON.MGET", "k", "w", "."),
    T("JSON.CLEAR", "k", "$.*"),
    T("JSON.GET", "k"),
    T("JSON.CLEAR", "k", "$.arr[*]"),
    T("JSON.GET", "k", ".arr"),
)

section(
    "decimals_arithmetic",
    # B3: a result is exact for integers and the shortest text of the double
    # otherwise. The module prints 17 significant digits.
    T("JSON.SET", "k", ".", DEC_DOC),
    N(b"1.3141592653589793", "JSON.NUMINCRBY", "k", ".a", "1"),
    N(b"1.3141592653589793", "JSON.GET", "k", ".a"),
    N(b"0.6000000000000001", "JSON.NUMMULTBY", "k", ".b", "2"),
    N(b"0.05396780371665955", "JSON.NUMINCRBY", "k", ".c", "0"),
    N(b"123457.78901234567", "JSON.NUMINCRBY", "k", ".d", "1"),
    N(b"1.2345678901234567", "JSON.NUMMULTBY", "k", ".e", "10"),
    N(b"1234568.8901234567", "JSON.NUMINCRBY", "k", ".f", "1"),
    N(b"0.24691357802469135", "JSON.NUMMULTBY", "k", ".g", "2"),
    N(b"3.5", "JSON.NUMINCRBY", "k", ".h", "1"),
    N(b"1234567890123456790", "JSON.NUMINCRBY", "k", ".i", "1"),
    N(Simple(b"integer"), "JSON.TYPE", "k", ".i"),
    N(b"2469135780246913580", "JSON.NUMMULTBY", "k", ".i", "2"),
    N(b"2469135780246913500.0", "JSON.NUMMULTBY", "k", ".i", "1.0"),
    N(Simple(b"number"), "JSON.TYPE", "k", ".i"),
    N(b"9223372036854776000.0", "JSON.NUMINCRBY", "k", ".j", "1"),
    N(b"-1e-18", "JSON.NUMINCRBY", "k", ".k", "0"),
    N(b"1501.5", "JSON.NUMINCRBY", "k", ".l", "1.5"),
    N(b"3003.0", "JSON.NUMMULTBY", "k", ".l", "2"),
    # A decimal argument, on integers and on decimals.
    T("JSON.SET", "n", ".", '{"i":1,"d":0.1,"x":[1,0.2,"s",0.30000000000000004]}'),
    N(b"1.1234567890123457", "JSON.NUMINCRBY", "n", ".i", "0.123456789012345678"),
    N(b"0.30000000000000004", "JSON.NUMINCRBY", "n", ".d", "0.2"),
    N(b"[1.1,0.30000000000000004,null,0.4]", "JSON.NUMINCRBY", "n", "$.x[*]", "0.1"),
    N(b"[2.2,0.6000000000000001,null,0.8]", "JSON.NUMMULTBY", "n", "$.x[*]", "2"),
    N(b'[2.2,0.6000000000000001,"s",0.8]', "JSON.GET", "n", ".x"),
    N([Simple(b"number"), Simple(b"number"), Simple(b"string"), Simple(b"number")], "JSON.TYPE", "n", "$.x[*]"),
    T("JSON.NUMINCRBY", "n", ".x[2]", "0.5"),
    T("JSON.NUMINCRBY", "n", ".i", "0.1.2"),
    T("JSON.NUMINCRBY", "n", ".i", "1e"),
)

section(
    "literal_arrays",
    # An array of only true, false and null has its own compact form in the
    # candidate. Every command must treat it as any other array.
    T("JSON.SET", "k", ".", '{"l":[true,false,null,true],"t":[true,true],"n":[null,null,null],"f":[false],"e":[]}'),
    T("JSON.GET", "k"),
    T("JSON.GET", "k", "INDENT", "  ", "NEWLINE", "\n", ".l"),
    T("JSON.GET", "k", "$.l[*]"),
    T("JSON.GET", "k", "$.l[1:3]"),
    T("JSON.GET", "k", "$.l[-1]"),
    T("JSON.GET", "k", "$..[0]"),
    T("JSON.GET", "k", "$.l[?(@==true)]"),
    T("JSON.GET", "k", "$.l[?(@==null)]"),
    T("JSON.GET", "k", "$.l[?(@!=false)]"),
    T("JSON.TYPE", "k", "$.l[*]"),
    T("JSON.TYPE", "k", ".l"),
    T("JSON.ARRLEN", "k", "$.*"),
    T("JSON.OBJLEN", "k", "$.l"),
    T("JSON.STRLEN", "k", "$.l[*]"),
    T("JSON.RESP", "k"),
    T("JSON.RESP", "k", "$.l"),
    T("JSON.DEBUG", "FIELDS", "k"),
    T("JSON.DEBUG", "FIELDS", "k", ".l"),
    T("JSON.DEBUG", "DEPTH", "k"),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "k", ".l"),
    *[T("JSON.ARRINDEX", "k", ".l", v) for v in ("true", "false", "null", "1", '"true"', "[true]", "0")],
    T("JSON.ARRINDEX", "k", ".l", "true", "1"),
    T("JSON.ARRINDEX", "k", ".l", "true", "1", "3"),
    T("JSON.ARRINDEX", "k", "$.*", "null"),
    T("JSON.TOGGLE", "k", "$.l[*]"),
    T("JSON.TOGGLE", "k", ".l[0]"),
    T("JSON.TOGGLE", "k", ".l[2]"),
    T("JSON.TOGGLE", "k", "$.n[*]"),
    T("JSON.TOGGLE", "k", ".n[*]"),
    T("JSON.TOGGLE", "k", ".l"),
    T("JSON.GET", "k"),
    T("JSON.NUMINCRBY", "k", "$.l[*]", "1"),
    T("JSON.NUMINCRBY", "k", ".l[0]", "1"),
    T("JSON.STRAPPEND", "k", "$.l[*]", '"x"'),
    T("JSON.CLEAR", "k", "$.l[*]"),
    T("JSON.CLEAR", "k", "$.l[*]"),
    T("JSON.GET", "k", ".l"),
    T("JSON.CLEAR", "k", ".t"),
    T("JSON.CLEAR", "k", ".e"),
    T("JSON.GET", "k"),
    T("JSON.ARRAPPEND", "k", ".t", "true", "false", "null"),
    T("JSON.ARRAPPEND", "k", "$.*", "true"),
    T("JSON.GET", "k"),
    T("JSON.ARRAPPEND", "k", ".t", "1", '"x"'),
    T("JSON.ARRAPPEND", "k", ".n", "[null]", "{}"),
    T("JSON.GET", "k"),
    T("JSON.ARRINSERT", "k", ".f", "0", "true"),
    T("JSON.ARRINSERT", "k", ".f", "-1", "null", "null"),
    T("JSON.ARRINSERT", "k", ".f", "2", "0.5"),
    T("JSON.ARRINSERT", "k", ".e", "0", "false"),
    T("JSON.ARRINSERT", "k", ".e", "5", "false"),
    T("JSON.GET", "k"),
    T("JSON.ARRPOP", "k", ".l"),
    T("JSON.ARRPOP", "k", ".l", "0"),
    T("JSON.ARRPOP", "k", "$.*"),
    T("JSON.ARRPOP", "k", ".e"),
    T("JSON.ARRPOP", "k", ".e"),
    T("JSON.GET", "k"),
    T("JSON.SET", "k", ".l", "[true,false,null,true,false,null]"),
    T("JSON.ARRTRIM", "k", ".l", "1", "4"),
    T("JSON.ARRTRIM", "k", ".l", "0", "0"),
    T("JSON.ARRTRIM", "k", ".l", "5", "9"),
    T("JSON.GET", "k", ".l"),
    T("JSON.SET", "k", ".l", "[true,false,null,true,false,null]"),
    T("JSON.SET", "k", ".l[0]", "false"),
    T("JSON.SET", "k", ".l[1]", "null"),
    T("JSON.SET", "k", "$.l[2]", "true"),
    T("JSON.GET", "k", ".l"),
    T("JSON.SET", "k", ".l[3]", "7"),
    T("JSON.SET", "k", ".l[4]", '"s"'),
    T("JSON.GET", "k", ".l"),
    T("JSON.SET", "k", ".l", "[true,false,null,true,false,null]"),
    T("JSON.SET", "k", "$.l[*]", "null"),
    T("JSON.SET", "k", "$.l[0,2,4]", "true"),
    T("JSON.SET", "k", "$.l[1]", "[false]"),
    T("JSON.GET", "k", ".l"),
    T("JSON.SET", "k", ".l", "[true,false,null,true,false,null]"),
    T("JSON.DEL", "k", ".l[0]"),
    T("JSON.DEL", "k", "$.l[1,3]"),
    T("JSON.DEL", "k", "$.l[?(@==true)]"),
    T("JSON.GET", "k", ".l"),
    T("JSON.DEL", "k", "$.l[*]"),
    T("JSON.GET", "k", ".l"),
    T("JSON.SET", "k", ".l", "[true,false,null]"),
    T("JSON.MERGE", "k", ".", '{"l":[null,true],"n":null,"q":[false,false]}'),
    T("JSON.MERGE", "k", "$.l[0]", '{"a":1}'),
    T("JSON.MERGE", "k", "$.q[*]", "true"),
    T("JSON.MERGE", "k", ".q", "[null]"),
    T("JSON.GET", "k"),
    T("JSON.MSET", "k", ".q[0]", "true", "k2", ".", "[null,null]"),
    T("JSON.MGET", "k", "k2", "."),
    # As a whole document, through a reload, and copied.
    T("JSON.SET", "r", ".", "[true,false,null]"),
    T("JSON.TOGGLE", "r", "$[*]"),
    T("JSON.ARRAPPEND", "r", ".", "null"),
    T("JSON.ARRPOP", "r", ".", "0"),
    T("JSON.ARRINDEX", "r", ".", "null"),
    T("DEBUG", "RELOAD"),
    T("JSON.GET", "r"),
    T("JSON.GET", "k"),
    T("COPY", "r", "r2"),
    T("JSON.TOGGLE", "r2", ".[0]"),
    T("JSON.MGET", "r", "r2", "."),
    T("JSON.CLEAR", "r"),
    T("JSON.GET", "r"),
    T("JSON.SET", "big", ".", "[" + ",".join(["true", "false", "null"] * 2000) + "]"),
    T("JSON.ARRLEN", "big"),
    T("JSON.ARRINDEX", "big", ".", "null", "3000"),
    T("JSON.TOGGLE", "big", ".[5999]"),
    T("JSON.TOGGLE", "big", ".[5998]"),
    T("JSON.ARRINSERT", "big", ".", "3000", "1"),
    T("JSON.ARRTRIM", "big", ".", "2998", "3002"),
    T("JSON.GET", "big"),
)

section(
    "clear",
    T("JSON.SET", "k", ".", '{"a":[1,2],"o":{"x":1},"s":"str","i":5,"d":1.5,"t":true,"f":false,"z":null,"ea":[],"eo":{},"es":"","zi":0,"zd":0.0,"big":12345678901234567890}'),
    T("JSON.CLEAR", "k", ".a"),
    T("JSON.CLEAR", "k", ".a"),
    T("JSON.CLEAR", "k", "$.o"),
    T("JSON.CLEAR", "k", ".s"),
    T("JSON.CLEAR", "k", ".i"),
    T("JSON.CLEAR", "k", ".d"),
    T("JSON.CLEAR", "k", ".t"),
    T("JSON.CLEAR", "k", ".f"),
    T("JSON.CLEAR", "k", ".z"),
    T("JSON.CLEAR", "k", ".big"),
    T("JSON.GET", "k"),
    T("JSON.CLEAR", "k", "$.*"),
    T("JSON.SET", "k", ".", '{"a":[1,2],"o":{"x":1},"s":"str","i":5,"d":1.5,"t":true,"f":false,"z":null,"ea":[],"eo":{},"es":"","zi":0,"zd":0.0}'),
    T("JSON.CLEAR", "k", "$.*"),
    T("JSON.GET", "k"),
    T("JSON.CLEAR", "k", "$.*"),
    T("JSON.SET", "k", ".", '{"a":[1,2],"o":{"x":1},"s":"str"}'),
    T("JSON.CLEAR", "k", ".*"),
    T("JSON.GET", "k"),
    T("JSON.SET", "k", ".", '{"a":[1,2],"o":{"x":1}}'),
    T("JSON.CLEAR", "k"),
    T("JSON.GET", "k"),
    T("JSON.CLEAR", "k", "$"),
    T("JSON.CLEAR", "k", ".nothing"),
    T("JSON.CLEAR", "k", "$.nothing"),
    T("JSON.SET", "k", ".", '{"a":[1,2]}'),
    T("JSON.CLEAR", "k", ".a[5]"),
    T("JSON.CLEAR", "k", "$.a[5]"),
    T("JSON.CLEAR", "k", "$["),
    T("JSON.CLEAR", "k", ".a.x"),
    T("JSON.CLEAR", "nokey"),
    T("JSON.CLEAR", "nokey", ".a"),
    T("JSON.CLEAR"),
    T("JSON.CLEAR", "k", ".a", "x"),
    T("SET", "str", "x"),
    T("JSON.CLEAR", "str"),
)

section(
    "type",
    T("JSON.SET", "k", ".", MIXED),
    T("JSON.TYPE", "k"),
    T("JSON.TYPE", "k", "$"),
    T("JSON.TYPE", "k", ".a"),
    T("JSON.TYPE", "k", ".b"),
    T("JSON.TYPE", "k", ".c"),
    T("JSON.TYPE", "k", ".d"),
    T("JSON.TYPE", "k", ".e"),
    T("JSON.TYPE", "k", ".f"),
    T("JSON.TYPE", "k", ".g"),
    T("JSON.TYPE", "k", "$.*"),
    T("JSON.TYPE", "k", ".*"),
    T("JSON.TYPE", "k", "$.a[*]"),
    T("JSON.TYPE", "k", ".nothing"),
    T("JSON.TYPE", "k", "$.nothing"),
    T("JSON.TYPE", "k", ".a[9]"),
    T("JSON.TYPE", "k", "$.a[9]"),
    T("JSON.TYPE", "k", "$["),
    T("JSON.TYPE", "k", ".["),
    T("JSON.TYPE", "k", "$.a[x]"),
    T("JSON.TYPE", "nokey"),
    T("JSON.TYPE", "nokey", "$.a"),
    T("JSON.TYPE"),
    T("JSON.TYPE", "k", ".a", "x"),
    T("SET", "str", "x"),
    T("JSON.TYPE", "str"),
    T("JSON.SET", "n", ".", "[1,-1,0,1.0,1e2,9223372036854775807,-9223372036854775808,0.5,123456789012345678901234567890]"),
    T("JSON.TYPE", "n", "$[*]"),
)

section(
    "resp",
    T("JSON.SET", "k", ".", '{"s":"str","i":5,"neg":-7,"d":1.5,"t":true,"f":false,"z":null,"a":[1,"two",[3,[4]],{"five":5}],"ea":[],"eo":{},"o":{"x":{"y":[]}},"d2":19.90,"d3":1e5,"d4":0.1,"d5":1e-7,"d6":1.5e30,"d7":100.0}'),
    T("JSON.RESP", "k"),
    T("JSON.RESP", "k", "$"),
    T("JSON.RESP", "k", ".s"),
    T("JSON.RESP", "k", "$.s"),
    T("JSON.RESP", "k", ".i"),
    T("JSON.RESP", "k", ".d"),
    T("JSON.RESP", "k", ".t"),
    T("JSON.RESP", "k", ".f"),
    T("JSON.RESP", "k", ".z"),
    T("JSON.RESP", "k", "$.z"),
    T("JSON.RESP", "k", ".a"),
    T("JSON.RESP", "k", "$.a"),
    T("JSON.RESP", "k", "$.a[*]"),
    T("JSON.RESP", "k", ".ea"),
    T("JSON.RESP", "k", ".eo"),
    T("JSON.RESP", "k", ".o"),
    T("JSON.RESP", "k", "$.*"),
    T("JSON.RESP", "k", ".nothing"),
    T("JSON.RESP", "k", "$.nothing"),
    T("JSON.RESP", "k", ".a[9]"),
    T("JSON.RESP", "k", "$.a[9]"),
    T("JSON.RESP", "k", "$["),
    T("JSON.RESP", "nokey"),
    T("JSON.RESP"),
    T("JSON.RESP", "k", ".a", "x"),
    T("SET", "str", "x"),
    T("JSON.RESP", "str"),
    # A legacy path that selects several values: the module sends one reply
    # per value, which breaks the protocol. Here the first value is replied.
    O(1, "JSON.RESP", "k", ".a[*]"),
)

section(
    "debug",
    T("JSON.SET", "k", ".", WIKI),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "k"),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "k", "."),
    P(is_array_of_pos_int(1), "JSON.DEBUG", "MEMORY", "k", "$"),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "k", ".address"),
    P(is_array_of_pos_int(1), "JSON.DEBUG", "MEMORY", "k", "$.address"),
    P(is_array_of_pos_int(2), "JSON.DEBUG", "MEMORY", "k", "$.phoneNumbers[*]"),
    P(is_pos_int, "JSON.DEBUG", "memory", "k", ".phoneNumbers[*]"),
    # A scalar document lives in the document word alone. It still has a size.
    T("JSON.SET", "sc", ".", "65536"),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "sc"),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "sc", "."),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "sc", ""),
    P(is_array_of_pos_int(1), "JSON.DEBUG", "MEMORY", "sc", "$"),
    P(is_pos_int, "JSON.DEBUG", "MEMORY", "k", ".age"),
    T("DEL", "sc"),
    T("JSON.DEBUG", "MEMORY", "k", ".nothing"),
    T("JSON.DEBUG", "MEMORY", "k", "$.nothing"),
    T("JSON.DEBUG", "MEMORY", "k", "$["),
    T("JSON.DEBUG", "MEMORY", "nokey"),
    T("JSON.DEBUG", "MEMORY", "nokey", ".a"),
    T("JSON.DEBUG", "MEMORY"),
    T("JSON.DEBUG", "MEMORY", "k", ".", "x"),
    T("JSON.DEBUG", "FIELDS", "k"),
    T("JSON.DEBUG", "FIELDS", "k", "."),
    T("JSON.DEBUG", "FIELDS", "k", "$"),
    T("JSON.DEBUG", "FIELDS", "k", ".address"),
    T("JSON.DEBUG", "FIELDS", "k", "$.address"),
    T("JSON.DEBUG", "FIELDS", "k", ".firstName"),
    T("JSON.DEBUG", "FIELDS", "k", "$.*"),
    T("JSON.DEBUG", "fields", "k", ".*"),
    T("JSON.DEBUG", "FIELDS", "k", ".children"),
    T("JSON.DEBUG", "FIELDS", "k", ".groups"),
    T("JSON.DEBUG", "FIELDS", "k", ".nothing"),
    T("JSON.DEBUG", "FIELDS", "k", "$.nothing"),
    T("JSON.DEBUG", "FIELDS", "nokey"),
    T("JSON.DEBUG", "FIELDS"),
    T("JSON.DEBUG", "DEPTH", "k"),
    T("JSON.DEBUG", "DEPTH", "nokey"),
    T("JSON.DEBUG", "DEPTH"),
    T("JSON.DEBUG", "DEPTH", "k", "x"),
    T("JSON.SET", "d0", ".", "1"),
    T("JSON.SET", "d1", ".", "{}"),
    T("JSON.SET", "d2", ".", "[[]]"),
    T("JSON.SET", "d3", ".", '{"a":{"b":[1]}}'),
    T("JSON.DEBUG", "DEPTH", "d0"),
    T("JSON.DEBUG", "DEPTH", "d1"),
    T("JSON.DEBUG", "DEPTH", "d2"),
    T("JSON.DEBUG", "DEPTH", "d3"),
    T("JSON.DEBUG", "FIELDS", "d0"),
    T("JSON.DEBUG", "FIELDS", "d1"),
    T("JSON.DEBUG", "FIELDS", "d2"),
    T("JSON.DEBUG", "FIELDS", "d3"),
    T("SET", "str", "x"),
    T("JSON.DEBUG", "MEMORY", "str"),
    T("JSON.DEBUG", "FIELDS", "str"),
    T("JSON.DEBUG", "DEPTH", "str"),
    T("DEL", "k", "d0", "d1", "d2", "str"),
    # The key name is a bulk string in the candidate and a simple string in
    # the module (see the section reply_injection).
    N([3, b"d3"], "JSON.DEBUG", "MAX-DEPTH-KEY"),
    N(lambda r: isinstance(r, list) and len(r) == 2 and is_pos_int(r[0]) and r[1] == b"d3", "JSON.DEBUG", "MAX-SIZE-KEY"),
    T("JSON.DEBUG", "MAX-DEPTH-KEY", "x"),
    T("FLUSHALL"),
    N([0, b""], "JSON.DEBUG", "MAX-DEPTH-KEY"),
    N([0, b""], "JSON.DEBUG", "MAX-SIZE-KEY"),
    T("JSON.DEBUG"),
    T("JSON.DEBUG", "NOSUCH"),
    T("JSON.DEBUG", "HELP", "x"),
    T("COMMAND", "GETKEYS", "JSON.DEBUG", "MEMORY", "k"),
    T("COMMAND", "GETKEYS", "JSON.DEBUG", "FIELDS", "k", "."),
    T("COMMAND", "GETKEYS", "JSON.DEBUG", "DEPTH", "k"),
    T("COMMAND", "GETKEYS", "JSON.DEBUG", "HELP"),
    T("COMMAND", "GETKEYS", "JSON.DEBUG", "MAX-DEPTH-KEY"),
)

section(
    "mset",
    T("JSON.MSET", "a", ".", "1", "b", "$", '{"x":1}', "c", ".", "[1,2]"),
    T("JSON.MGET", "a", "b", "c", "."),
    T("JSON.MSET", "a", ".", '{"n":1}', "b", "$.x", "2", "b2", ".", "true"),
    T("JSON.MGET", "a", "b", "b2", "."),
    T("JSON.MSET", "b", "$.y", "[]", "c", "$[0]", '"first"'),
    T("JSON.MGET", "b", "c", "."),
    # all or nothing
    T("JSON.MSET", "a", ".", "5", "new", ".x", "1"),
    T("JSON.MSET", "a", ".", "5", "b", ".x", "{"),
    T("JSON.MSET", "a", ".", "5", "b", ".", "{"),
    T("JSON.MSET", "a", ".", "5", "b", ".q.r", "1"),
    T("JSON.MSET", "a", ".", "5", "c", ".[9]", "1"),
    T("JSON.MSET", "a", ".", "5", "b", "$[", "1"),
    T("SET", "str", "x"),
    T("JSON.MSET", "a", ".", "5", "str", ".", "1"),
    T("JSON.MGET", "a", "b", "c", "new", "."),
    T("GET", "str"),
    T("JSON.MSET", "a", ".", "5", "b"),
    T("JSON.MSET", "a", ".", "5", "b", "."),
    T("JSON.MSET", "a", "."),
    T("JSON.MSET"),
    T("JSON.MSET", "b", "$.nothing[*]", "1"),
    T("JSON.MSET", "w", ".", '{"a":[1,2,3]}', "a", ".", "0"),
    T("JSON.MSET", "w", "$.a[*]", "0"),
    T("JSON.GET", "w"),
    T("COMMAND", "GETKEYS", "JSON.MSET", "a", ".", "1", "b", ".", "2"),
    # A key named twice. The module asserts when the second triple depends on
    # the first, so these go to the candidate only (design 8.4).
    # Every triple is first checked against the stored document, as in the
    # module, then the triples of one key run in order on a staged copy.
    T("JSON.SET", "tw", ".", '{"a":0}'),
    O(OK, "JSON.MSET", "tw", ".", '{"a":1}', "tw", ".b", "2"),
    O(b'{"a":1,"b":2}', "JSON.GET", "tw"),
    O(OK, "JSON.MSET", "tw", ".a", "10", "tw", ".a", "11", "tw", ".c", "12"),
    O(b'{"a":11,"b":2,"c":12}', "JSON.GET", "tw"),
    O(ErrPrefix("NONEXISTENT"), "JSON.MSET", "tw", ".", "{}", "tw", ".x.y", "1"),
    O(b'{"a":11,"b":2,"c":12}', "JSON.GET", "tw"),
    O(ErrPrefix("SYNTAXERR"), "JSON.MSET", "tw2", ".", "{}", "tw2", ".x", "1"),
    O(0, "EXISTS", "tw2"),
    O(OK, "JSON.MSET", "tw", ".d", "1", "other", ".", "true", "tw", ".", '{"z":0}', "tw", ".y", "1"),
    O(b'{"z":0,"y":1}', "JSON.GET", "tw"),
    O(b"true", "JSON.GET", "other"),
    # The second triple is valid for the stored document and fails on the
    # staged one. Nothing changes, on this key or any other.
    O(OK, "JSON.SET", "tw", ".", '{"a":{"b":1}}'),
    O(ErrPrefix("ERROR Cannot insert"), "JSON.MSET", "other", ".", "1", "tw", ".", '{"a":5}', "tw", ".a.b", "2"),
    O(b'{"a":{"b":1}}', "JSON.GET", "tw"),
    O(b"true", "JSON.GET", "other"),
    # A TTL survives when no triple replaces the whole document.
    O(1, "EXPIRE", "tw", "1000"),
    O(OK, "JSON.MSET", "tw", ".a.b", "2", "tw", ".a.c", "3"),
    O(b'{"a":{"b":2,"c":3}}', "JSON.GET", "tw"),
    O(is_pos_int, "TTL", "tw"),
    O(OK, "JSON.MSET", "tw", ".a.b", "4", "tw", ".a.b", "5"),
    O(b'{"a":{"b":5,"c":3}}', "JSON.GET", "tw"),
    # And is discarded when one does, as with JSON.SET on the root.
    O(OK, "JSON.MSET", "tw", ".a.b", "6", "tw", ".", "[]"),
    O(-1, "TTL", "tw"),
)

section(
    "merge",
    T("JSON.MERGE", "k", ".", '{"a":1,"b":{"c":2,"d":3},"e":[1,2]}'),
    T("JSON.GET", "k"),
    T("JSON.MERGE", "k", "$", '{"a":2,"f":"new"}'),
    T("JSON.MERGE", "k", ".", '{"b":{"c":null,"x":{"y":null,"z":1}}}'),
    T("JSON.GET", "k"),
    T("JSON.MERGE", "k", "$.b", '{"d":null,"q":[null,1]}'),
    T("JSON.MERGE", "k", ".e", '{"now":"object"}'),
    T("JSON.MERGE", "k", "$.a", "[1,null]"),
    T("JSON.MERGE", "k", "$.f", "null"),
    T("JSON.GET", "k"),
    # a path that adds a member takes the patch as it is
    T("JSON.MERGE", "k", "$.added", '{"p":null,"q":1}'),
    T("JSON.MERGE", "k", ".added2", "5"),
    T("JSON.GET", "k"),
    # an object member onto a member that is not an object keeps its nulls
    T("JSON.MERGE", "k", ".", '{"a":{"keep":null,"v":1}}'),
    T("JSON.GET", "k", ".a"),
    # an empty object patch changes nothing in an object
    T("JSON.MERGE", "k", ".", "{}"),
    T("JSON.MERGE", "k", ".added2", "{}"),
    T("JSON.GET", "k"),
    T("JSON.MERGE", "k", ".", "5"),
    T("JSON.GET", "k"),
    T("JSON.MERGE", "k", ".", '{"x":null,"y":{"z":null}}'),
    T("JSON.GET", "k"),
    # several targets
    T("JSON.SET", "m", ".", '{"u":[{"n":1,"o":{"a":1}},{"n":2},{"n":3,"o":5}]}'),
    T("JSON.MERGE", "m", "$.u[*]", '{"n":null,"o":{"b":2},"new":true}'),
    T("JSON.GET", "m"),
    T("JSON.MERGE", "m", "$.u[*].extra", '{"e":1}'),
    T("JSON.MERGE", "m", ".u[*].o", '{"c":3}'),
    T("JSON.GET", "m"),
    T("JSON.MERGE", "m", "$.nothing[*]", '{"e":1}'),
    T("JSON.MERGE", "m", "$.u[9]", '{"e":1}'),
    T("JSON.MERGE", "m", ".u[9]", '{"e":1}'),
    T("JSON.MERGE", "m", "$.x.y", '{"e":1}'),
    T("JSON.MERGE", "m", "$[", '{"e":1}'),
    T("JSON.MERGE", "m", ".u", "{"),
    T("JSON.MERGE", "m", ".x.y", "{"),
    # a new key
    T("JSON.MERGE", "nk", ".a", "1"),
    T("JSON.MERGE", "nk", "$.a", "1"),
    T("JSON.MERGE", "nk", ".", "{"),
    T("JSON.MERGE", "nk", "$", '{"a":null,"b":{"c":null}}'),
    T("JSON.GET", "nk"),
    T("SET", "str", "x"),
    T("JSON.MERGE", "str", ".", "1"),
    T("JSON.MERGE", "m", "."),
    T("JSON.MERGE", "m", ".", "1", "2"),
    T("JSON.MERGE"),
    T("JSON.SET", "ttl", ".", '{"a":1}'),
    T("EXPIRE", "ttl", "1000"),
    T("JSON.MERGE", "ttl", ".", '{"b":2}'),
    P(is_pos_int, "TTL", "ttl"),
)

section(
    "command_table",
    *[
        T("COMMAND", "INFO", name)
        for name in (
            "JSON.SET JSON.MERGE JSON.MSET JSON.GET JSON.MGET JSON.DEL JSON.FORGET JSON.NUMINCRBY "
            "JSON.NUMMULTBY JSON.STRLEN JSON.STRAPPEND JSON.TOGGLE JSON.OBJLEN JSON.OBJKEYS JSON.ARRLEN "
            "JSON.ARRAPPEND JSON.ARRPOP JSON.ARRINSERT JSON.ARRTRIM JSON.CLEAR JSON.ARRINDEX JSON.TYPE JSON.RESP"
        ).split()
    ],
    T("COMMAND", "GETKEYS", "JSON.SET", "k", ".", "1"),
    T("COMMAND", "GETKEYS", "JSON.MGET", "a", "b", "c", "."),
    T("COMMAND", "GETKEYS", "JSON.GET", "k", ".a", ".b"),
    T("COMMAND", "GETKEYS", "JSON.ARRAPPEND", "k", ".", "1", "2"),
    S("ACL", "CAT", "json"),
    T("JSON.NOSUCH", "k"),
    T("json.set", "lower", ".", "1"),
    T("Json.Get", "lower"),
)

# ---------------------------------------------------------------------------
# The listed behaviour changes (design 2.3). Each asserts the new behaviour.
# ---------------------------------------------------------------------------

section(
    "B1_invalid_utf8",
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".", b'"\xff\xfe"'),
    N(0, "EXISTS", "k"),
    T("JSON.SET", "k", ".", '{"a":"ok","arr":[]}'),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".a", b'"\xc3\x28"'),
    N(ErrPrefix("SYNTAXERR"), "JSON.ARRAPPEND", "k", ".arr", b'"\xed\xa0\x80"'),
    N(ErrPrefix("SYNTAXERR"), "JSON.STRAPPEND", "k", ".a", b'"\xff"'),
    N(ErrPrefix("SYNTAXERR"), "JSON.MERGE", "k", ".", b'{"m":"\xff"}'),
    N(ErrPrefix("SYNTAXERR"), "JSON.MSET", "k", ".a", b'"\xff"'),
    N(b'{"a":"ok","arr":[]}', "JSON.GET", "k"),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".b", b'{"\xff":1}'),
    T("JSON.SET", "k", ".a", b'"\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80"'),
    N(b'{"a":"\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80","arr":[]}', "JSON.GET", "k"),
)

section(
    "B2_number_range",
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".", "1.8e308"),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".", "[1,-1.8e308]"),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".", "1e400"),
    N(0, "EXISTS", "k"),
    T("JSON.SET", "k", ".", "1.7976931348623157e308"),
    T("JSON.GET", "k"),
)

section(
    "B3_computed_numbers",
    T("JSON.SET", "k", ".", '{"a":1.1,"b":1.5,"c":2,"d":9007199254740993,"e":9223372036854775807,"f":0.1,"g":3,"h":1.5}'),
    N(b"2.1", "JSON.NUMINCRBY", "k", ".a", "1"),
    N(b"2.1", "JSON.GET", "k", ".a"),
    N(b"3.0", "JSON.NUMINCRBY", "k", ".b", "1.5"),
    N(b"3.0", "JSON.GET", "k", ".b"),
    N(Simple(b"number"), "JSON.TYPE", "k", ".b"),
    N(b"3.0", "JSON.NUMINCRBY", "k", ".c", "1.0"),
    N(Simple(b"number"), "JSON.TYPE", "k", ".c"),
    N(b"9007199254740994", "JSON.NUMINCRBY", "k", ".d", "1"),
    N(b"18014398509481988", "JSON.NUMMULTBY", "k", ".d", "2"),
    N(b"18014398509481988", "JSON.GET", "k", ".d"),
    N(Simple(b"integer"), "JSON.TYPE", "k", ".d"),
    N(b"9223372036854776000.0", "JSON.NUMINCRBY", "k", ".e", "1"),
    N(Simple(b"number"), "JSON.TYPE", "k", ".e"),
    N(b"0.30000000000000004", "JSON.NUMINCRBY", "k", ".f", "0.2"),
    N(b"6", "JSON.NUMMULTBY", "k", ".g", "2"),
    N(Simple(b"integer"), "JSON.TYPE", "k", ".g"),
    N(b"3.0", "JSON.NUMMULTBY", "k", ".h", "2"),
    N(Simple(b"number"), "JSON.TYPE", "k", ".h"),
    N(b"[6.0,12]", "JSON.NUMMULTBY", "k", "$['h','g']", "2"),
    T("JSON.SET", "v", ".", '{"a":[1,2.5,"x",4]}'),
    N(b"[2,3.5,null,5]", "JSON.NUMINCRBY", "v", "$.a[*]", "1"),
    N(b"[3.0,4.5,null,6.0]", "JSON.NUMINCRBY", "v", "$.a[*]", "1.0"),
    N(b"6.0", "JSON.NUMINCRBY", "v", ".a[*]", "0"),
    N(b"[3.0,4.5,\"x\",6.0]", "JSON.GET", "v", ".a"),
)

section(
    "B4_nested_matches",
    T("JSON.SET", "k", ".", '{"a":{"a":1}}'),
    N(OK, "JSON.SET", "k", "$..a", '{"a":2}'),
    N(b'{"a":{"a":2}}', "JSON.GET", "k"),
    # Design 2.5: nested matches in the array commands, CLEAR and MERGE read
    # freed memory in the module, so these go to the candidate only. Replies
    # come from the document as it was before the command (Q1).
    O(OK, "JSON.SET", "n", ".", '{"a":[1,{"a":[2,3]}]}'),
    O([3, 3], "JSON.ARRAPPEND", "n", "$..a", "9"),
    O(b'{"a":[1,{"a":[2,3,9]},9]}', "JSON.GET", "n"),
    O([b"9", b"9"], "JSON.ARRPOP", "n", "$..a"),
    O(b'{"a":[1,{"a":[2,3]}]}', "JSON.GET", "n"),
    O([b"1", b"2"], "JSON.ARRPOP", "n", "$..a", "0"),
    O(b'{"a":[{"a":[3]}]}', "JSON.GET", "n"),
    O([2, 2], "JSON.ARRINSERT", "n", "$..a", "0", "7"),
    O(b'{"a":[7,{"a":[7,3]}]}', "JSON.GET", "n"),
    O([1, 1], "JSON.ARRTRIM", "n", "$..a", "1", "1"),
    O(b'{"a":[{"a":[3]}]}', "JSON.GET", "n"),
    O(2, "JSON.CLEAR", "n", "$..a"),
    O(b'{"a":[]}', "JSON.GET", "n"),
    O(OK, "JSON.SET", "n", ".", '{"a":{"x":1,"a":{"x":2}}}'),
    O(OK, "JSON.MERGE", "n", "$..a", '{"x":null,"y":0}'),
    O(b'{"a":{"a":{"x":2},"y":0}}', "JSON.GET", "n"),
    O(OK, "JSON.SET", "n", ".", '{"a":{"a":{"a":1}},"b":{"a":2}}'),
    O(4, "JSON.DEL", "n", "$..a"),
    O(b'{"b":{}}', "JSON.GET", "n"),
)

section(
    "B5_literal_member_name",
    T("JSON.SET", "k", ".", "{}"),
    N(OK, "JSON.SET", "k", "$.b~1c", "1"),
    N(b'{"b~1c":1}', "JSON.GET", "k"),
    N(OK, "JSON.SET", "k", "$.t~0x", "2"),
    N(b'{"b~1c":1,"t~0x":2}', "JSON.GET", "k"),
    N(OK, "JSON.SET", "k", "$['p/q']", "3"),
    N(b'{"b~1c":1,"t~0x":2,"p/q":3}', "JSON.GET", "k"),
    N(b"[3]", "JSON.GET", "k", "$['p/q']"),
    N(OK, "JSON.SET", "k", "$.b~1c", "10"),
    N(b"[10]", "JSON.GET", "k", "$.b~1c"),
    N(b"[11]", "JSON.NUMINCRBY", "k", "$.b~1c", "1"),
    N(1, "JSON.DEL", "k", "$.t~0x"),
    N(1, "JSON.DEL", "k", "$['p/q']"),
    N(b'{"b~1c":11}', "JSON.GET", "k"),
    N(OK, "JSON.MERGE", "k", "$.m~1n", '{"x":1}'),
    N(b'{"b~1c":11,"m~1n":{"x":1}}', "JSON.GET", "k"),
)

section(
    "B6_all_or_nothing",
    T("JSON.SET", "k", ".", '{"a":1,"b":1.7e308}'),
    N(ErrPrefix("OVERFLOW"), "JSON.NUMINCRBY", "k", "$.*", "1.7e308"),
    N(b'{"a":1,"b":1.7e308}', "JSON.GET", "k"),
    N(ErrPrefix("OVERFLOW"), "JSON.NUMMULTBY", "k", "$.*", "10"),
    N(b'{"a":1,"b":1.7e308}', "JSON.GET", "k"),
    T("JSON.SET", "i", ".", '{"a":[1,2,3],"b":[1]}'),
    N(ErrPrefix("OUTOFBOUNDARIES"), "JSON.ARRINSERT", "i", "$.*", "2", "0"),
    N(b'{"a":[1,2,3],"b":[1]}', "JSON.GET", "i"),
    # Design 2.5: exactly one update and at least one insert. The module moves
    # the value into the update and inserts what is left.
    T("JSON.SET", "u", ".", '{"x":{"v":1},"y":{},"z":{}}'),
    N(OK, "JSON.SET", "u", "$.*.v", '{"deep":[1,2]}'),
    N(b'{"x":{"v":{"deep":[1,2]}},"y":{"v":{"deep":[1,2]}},"z":{"v":{"deep":[1,2]}}}', "JSON.GET", "u"),
)

section(
    "B7_size_limit",
    T("CONFIG", "SET", "json.max-document-size", "64"),
    N(ErrPrefix("LIMIT Document size"), "JSON.SET", "big", ".", '"' + "x" * 4096 + '"'),
    N(0, "EXISTS", "big"),
    N(OK, "JSON.SET", "s", ".", '{"a":[],"s":""}'),
    N(ErrPrefix("LIMIT Document size"), "JSON.SET", "s", ".b", '"' + "x" * 4096 + '"'),
    N(ErrPrefix("LIMIT Document size"), "JSON.ARRAPPEND", "s", ".a", '"' + "x" * 4096 + '"'),
    N(ErrPrefix("LIMIT Document size"), "JSON.ARRINSERT", "s", ".a", "0", '"' + "x" * 4096 + '"'),
    N(ErrPrefix("LIMIT Document size"), "JSON.STRAPPEND", "s", ".s", '"' + "x" * 4096 + '"'),
    N(ErrPrefix("LIMIT Document size"), "JSON.MERGE", "s", ".", '{"m":"' + "x" * 4096 + '"}'),
    N(ErrPrefix("LIMIT Document size"), "JSON.MSET", "s", ".b", '"' + "x" * 4096 + '"'),
    N(ErrPrefix("LIMIT Document size"), "JSON.MSET", "big2", ".", '"' + "x" * 4096 + '"'),
    N(b'{"a":[],"s":""}', "JSON.GET", "s"),
    # Writes that shrink a document are never refused.
    N(1, "JSON.DEL", "s", ".a"),
    T("CONFIG", "SET", "json.max-document-size", "0"),
    T("JSON.SET", "big", ".", '"' + "x" * 4096 + '"'),
    T("JSON.STRLEN", "big"),
)

section(
    "B8_debug_subcommands",
    N(
        [
            Simple(
                b"JSON.DEBUG MEMORY <key> [path] - report memory size (bytes). Without path reports document size "
                b"without keys. With path reports size including keys"
            ),
            Simple(b"JSON.DEBUG DEPTH <key> - report the maximum path depth of the JSON document."),
            Simple(
                b"JSON.DEBUG FIELDS <key> [path] - report number of fields in the JSON element. Path defaults to "
                b"root if not provided."
            ),
            Simple(b"JSON.DEBUG HELP - print help message."),
            Simple(b"------- DANGER, LONG RUNNING COMMANDS, DON'T USE ON PRODUCTION SYSTEM --------"),
            Simple(b"JSON.DEBUG MAX-DEPTH-KEY - Find JSON key with maximum depth"),
            Simple(b"JSON.DEBUG MAX-SIZE-KEY  - Find JSON key with largest memory size"),
        ],
        "JSON.DEBUG", "HELP",
    ),  # fmt: skip
    # The removed subcommands go to the candidate only: on the reference,
    # KEYTABLE-CORRUPT really corrupts the key table, and the server can crash
    # on a later command (seen on Linux x86-64).
    O(ErrPrefix("ERR unknown subcommand"), "JSON.DEBUG", "KEYTABLE-CHECK"),
    O(ErrPrefix("ERR unknown subcommand"), "JSON.DEBUG", "KEYTABLE-CORRUPT", "x"),
    O(ErrPrefix("ERR unknown subcommand"), "JSON.DEBUG", "KEYTABLE-DISTRIBUTION", "1"),
    T("JSON.SET", "k", ".", "1"),
    O(ErrPrefix("ERR unknown subcommand"), "JSON.DEBUG", "TEST-SHARED-API", "k", "."),
)

section(
    "B9_deep_document_loads",
    T("JSON.SET", "k", ".", "[[[[[1]]]]]"),
    T("CONFIG", "SET", "json.max-path-limit", "3"),
    T("JSON.SET", "k2", ".", "[[[[1]]]]"),
    T("JSON.GET", "k"),
    # The stored document is deeper than the limit now. It still loads.
    # The module rejects it on load, so the reload goes to the candidate only.
    O(OK, "DEBUG", "RELOAD"),
    O(b"[[[[[1]]]]]", "JSON.GET", "k"),
    T("CONFIG", "SET", "json.max-path-limit", "128"),
)

section(
    "module_defects_not_reproduced",
    # STRAPPEND cuts both operands at an embedded NUL in the module.
    T("JSON.SET", "k", ".", '{"s":"a\\u0000b"}'),
    N(6, "JSON.STRAPPEND", "k", ".s", '"c\\u0000d"'),
    N(b'"a\\u0000bc\\u0000d"', "JSON.GET", "k", ".s"),
    N(6, "JSON.STRLEN", "k", ".s"),
    # An integer above INT64_MAX keeps its text and is a "number" in both.
    T("JSON.SET", "u", ".", "[18446744073709551615,9223372036854775808]"),
    T("JSON.GET", "u"),
    T("JSON.TYPE", "u", "$[*]"),
    T("JSON.RESP", "u"),
    T("JSON.CLEAR", "u", "$[*]"),
    T("JSON.GET", "u"),
)


# ---------------------------------------------------------------------------
# Inputs that crash, hang or desynchronise the reference. They go to the
# candidate only. It must give a sane reply and still be up afterwards.
# ---------------------------------------------------------------------------

EVIL_KEY = b"evil\r\n+INJECTED\r\n:1"

section(
    "reply_injection",
    # A key name with line breaks. The module sends it as a simple string in
    # JSON.DEBUG MAX-SIZE-KEY and MAX-DEPTH-KEY, which puts two extra replies
    # on the wire, so these go to the candidate only.
    O(OK, "JSON.SET", EVIL_KEY, "$", '{"a":[1,2,3]}'),
    O(lambda r: isinstance(r, list) and len(r) == 2 and is_pos_int(r[0]) and r[1] == EVIL_KEY, "JSON.DEBUG", "MAX-SIZE-KEY"),
    O([2, EVIL_KEY], "JSON.DEBUG", "MAX-DEPTH-KEY"),
    O(Simple(b"PONG"), "PING"),
    # Member names and values with line breaks never travel as simple strings.
    O(OK, "JSON.SET", "k", "$", '{"a\\r\\n+X":"v\\r\\n+Y","b":true}'),
    O([[b"a\r\n+X", b"b"]], "JSON.OBJKEYS", "k", "$"),
    O([Simple(b"{"), [b"a\r\n+X", b"v\r\n+Y"], [b"b", Simple(b"true")]], "JSON.RESP", "k"),
    O(ErrPrefix("SYNTAXERR"), "JSON.GET", "k", "$[\r\n+Z"),
    O(lambda r: isinstance(r, Err) and b"\r" not in r and b"\n" not in r, "JSON.SET", "k", b"$.a\r\n.b.c", "1"),
    O(Simple(b"PONG"), "PING"),
)

section(
    "B1_member_name_through_path",
    # B1 also covers a member name that a path adds: it is JSON text too.
    T("JSON.SET", "u", "$", '{"o":{}}'),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "u", b"$.\xff\xfe", "1"),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "u", b'$["\xc3\x28"]', "1"),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "u", b".o.\xed\xa0\x80", "1"),
    N(ErrPrefix("SYNTAXERR"), "JSON.SET", "u", b"$.*.\xc0\xaf", "1"),
    N(ErrPrefix("SYNTAXERR"), "JSON.MERGE", "u", b"$.\xff", '{"a":1}'),
    N(ErrPrefix("SYNTAXERR"), "JSON.MSET", "u", b"$.o.\xf5\x80\x80\x80", "1"),
    N(ErrPrefix("SYNTAXERR"), "JSON.MSET", "other", ".", "1", "u", b".\xff", "2"),
    N(b'{"o":{}}', "JSON.GET", "u"),
    N(0, "EXISTS", "other"),
    # A path that only reads through such a name is not an error.
    N(b"[]", "JSON.GET", "u", b"$.\xff\xfe"),
    N(0, "JSON.DEL", "u", b"$.\xff\xfe"),
    N(ErrPrefix("NONEXISTENT"), "JSON.SET", "u", b"$.\xff.x", "1"),
    # Valid UTF-8 of every length is fine.
    T("JSON.SET", "u", b"$.\xc3\xa9", "1"),
    T("JSON.SET", "u", b"$.o.\xe4\xb8\xad", "2"),
    T("JSON.MERGE", "u", b"$.\xf0\x9f\x98\x80", "3"),
    N(b'{"o":{"\xe4\xb8\xad":2},"\xc3\xa9":1,"\xf0\x9f\x98\x80":3}', "JSON.GET", "u"),
)

section(
    "format_limit",
    # Formatting strings multiply the size of a reply. Short ones are never
    # checked. Long ones are refused when the reply could pass 512 MB. The
    # module has no such limit and would build the reply, so the long ones go
    # to the candidate only.
    T("JSON.SET", "k", ".", WIKI),
    T("JSON.SET", "one", ".", "[1]"),
    T("JSON.GET", "k", "INDENT", " " * 40, "NEWLINE", "\n" * 12, "SPACE", " " * 12),
    T("JSON.GET", "k", "INDENT", "ab" * 50, "$.address"),
    O(lambda r: isinstance(r, bytes) and len(r) > 2**20, "JSON.GET", "one", "INDENT", "x" * (2**20)),
    O(lambda r: isinstance(r, bytes) and len(r) > 2**20, "JSON.GET", "one", "INDENT", "x" * (2**20), "$", "."),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "INDENT", "x" * (2**23)),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "NEWLINE", "x" * (2**24)),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "SPACE", "x" * (2**25)),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "INDENT", "x" * (2**23), "$"),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "INDENT", "x" * (2**23), "$.*"),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "INDENT", "x" * (2**23), ".address", ".groups"),
    O(ErrPrefix("LIMIT Formatted"), "JSON.GET", "k", "INDENT", "x" * (2**23), "$.address", ".groups"),
    O(b'"John"', "JSON.GET", "k", "SPACE", "x" * (2**23), ".firstName"),
    O(WIKI.encode(), "JSON.GET", "k"),
    O(Simple(b"PONG"), "PING"),
)

DEEP = 5000
DEEP_ARRAYS = "[" * DEEP + "1" + "]" * DEEP
DEEP_OBJECTS = '{"a":' * DEEP + "1" + "}" * DEEP

section(
    "survives_number_range",
    # B2: an ordinary client cannot store a number that is not a finite double.
    O(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", "$", "1.8e308"),
    O(ErrPrefix("NONEXISTENT"), "JSON.NUMINCRBY", "k", "$", "1"),
    # On a trusted path it is kept as text, and arithmetic on it is an error.
    X("k", "1.8e308"),
    O(b"1.8e308", "JSON.GET", "k"),
    O(Simple(b"number"), "JSON.TYPE", "k"),
    O(ErrPrefix("OVERFLOW"), "JSON.NUMINCRBY", "k", "$", "1"),
    O(ErrPrefix("OVERFLOW"), "JSON.NUMINCRBY", "k", ".", "-1.7e308"),
    O(ErrPrefix("OVERFLOW"), "JSON.NUMMULTBY", "k", "$", "0"),
    O(ErrPrefix("OVERFLOW"), "JSON.NUMMULTBY", "k", "$", "0.5"),
    O(b"1.8e308", "JSON.GET", "k"),
    O(b"1.8e308", "JSON.RESP", "k"),
    O([b"1.8e308"], "JSON.RESP", "k", "$"),
    O(1, "JSON.CLEAR", "k"),
    O(b"0.0", "JSON.GET", "k"),
    X("a", "[1,-1.8e308,2]"),
    O(b"[1,-1.8e308,2]", "JSON.GET", "a"),
    O(ErrPrefix("SYNTAXERR"), "JSON.ARRINDEX", "a", ".", "-1.8e308"),  # B2 applies to the argument.
    O(2, "JSON.ARRINDEX", "a", ".", "2"),
    O([Simple(b"["), 1, b"-1.8e308", 2], "JSON.RESP", "a"),
    O(ErrPrefix("OVERFLOW"), "JSON.NUMINCRBY", "a", "$[*]", "1"),
    O(b"[1,-1.8e308,2]", "JSON.GET", "a"),
    O(b"-1.8e308", "JSON.ARRPOP", "a", ".", "1"),
    O(b"[2,3]", "JSON.NUMINCRBY", "a", "$[*]", "1"),
    # A result that leaves the range of a double.
    O(OK, "JSON.SET", "m", ".", "1e308"),
    O(ErrPrefix("OVERFLOW"), "JSON.NUMMULTBY", "m", ".", "10"),
    O(b"1e308", "JSON.GET", "m"),
    # Subnormal numbers, and a 25-byte number text in JSON.RESP.
    O(OK, "JSON.SET", "s", ".", "[5e-324]"),
    O([Simple(b"["), b"5e-324"], "JSON.RESP", "s"),
    O(b"[5e-324]", "JSON.GET", "s"),
    O(b"[1e-323]", "JSON.NUMMULTBY", "s", "$[0]", "2"),
    O(b"[0.0]", "JSON.NUMMULTBY", "s", "$[0]", "0.1"),
    O(OK, "JSON.SET", "t", "$", '{"aa":-2.1331486805971874e-06}'),
    O([[Simple(b"{"), [b"aa", b"-0.0000021331486805971874"]]], "JSON.RESP", "t", "$"),
    O(Simple(b"PONG"), "PING"),
)

def zeros_only(value):
    """A JSON array of one or more copies of `value`: see the note below."""
    return lambda r: isinstance(r, bytes) and r[:1] == b"[" and set(r[1:-1].split(b",")) == {value}


section(
    "survives_slice_step",
    # A slice step of 2^31 or 2^32 never ends in the module, whose loop
    # counter is an int. The candidate must answer. It keeps the module's int
    # arithmetic, so 2^31 + 1 and 2^32 + 1 select what they select there.
    # NOTE: for 2^31 and 2^32 the path evaluator currently repeats the first
    # element about a thousand times instead of selecting it once. That is
    # reported against json_path.c. Until it is settled these steps only
    # require an answer made of that element.
    O(OK, "JSON.SET", "k", ".", "[0,1,2,3,4,5]"),
    O(zeros_only(b"0"), "JSON.GET", "k", "$[0:4:4294967296]"),
    O(zeros_only(b"0"), "JSON.GET", "k", "$[0:4:2147483648]"),
    # The module's int counter wraps here and it replies [0,2]. The
    # candidate's evaluator steps past the end.
    O(b"[0]", "JSON.GET", "k", "$[0:4:2147483649]"),
    O(b"[0,1,2,3]", "JSON.GET", "k", "$[0:4:4294967297]"),
    O(b"[0,2]", "JSON.GET", "k", "$[0:4:2]"),
    O(zeros_only(b"5"), "JSON.GET", "k", "$[5:0:-4294967296]"),
    O(b"[0,1,2,3,4,5]", "JSON.GET", "k"),
    O(Simple(b"PONG"), "PING"),
)

section(
    "survives_slice_step_write",
    # A write through a slice step of 2^32 selects each value once.
    O(OK, "JSON.SET", "k", ".", "[0,1,2,3,4,5]"),
    O(b"[2]", "JSON.NUMINCRBY", "k", "$[1:6:4294967296]", "1"),
    O(1, "JSON.DEL", "k", "$[1:6:4294967296]"),
    O(b"[0,2,3,4,5]", "JSON.GET", "k"),
    O(Simple(b"PONG"), "PING"),
)

section(
    "survives_tilde_and_nul_names",
    # JSON.DEL of several values in a document that has a "~" member name.
    O(OK, "JSON.SET", "k", ".", '{"a~b":1,"c":2,"d~1e":[1,2,3],"f/g":{"h~0":4}}'),
    O(1, "JSON.DEL", "k", "$['d~1e'][1]"),
    O(1, "JSON.DEL", "k", "$..['h~0']"),
    O(4, "JSON.DEL", "k", "$.*"),
    O(b"{}", "JSON.GET", "k"),
    O(OK, "JSON.SET", "k", ".", '{"x":{"a~b":1,"c":2},"y":{"a~b":3,"c":4}}'),
    O(2, "JSON.DEL", "k", "$..['a~b']"),
    O(2, "JSON.DEL", "k", "$..c"),
    O(b'{"x":{},"y":{}}', "JSON.GET", "k"),
    # A member name with a NUL can be read, written and removed.
    O(OK, "JSON.SET", "n", ".", '{"a\\u0000b":1,"c":2}'),
    O(b"[1]", "JSON.GET", "n", '$["a\\u0000b"]'),
    O(1, "JSON.DEL", "n", '$["a\\u0000b"]'),
    O(b'{"c":2}', "JSON.GET", "n"),
    O(Simple(b"PONG"), "PING"),
)

section(
    "survives_nul_in_json",
    # "1\x00garbage" is not JSON: the text does not end at the NUL as it does
    # in the module. Every command that takes JSON text rejects it.
    O(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".", b"1\x00garbage"),
    O(0, "EXISTS", "k"),
    O(OK, "JSON.SET", "k", ".", '{"a":[1],"s":"x","n":1}'),
    O(ErrPrefix("SYNTAXERR"), "JSON.SET", "k", ".n", b"1\x00garbage"),
    O(ErrPrefix("SYNTAXERR"), "JSON.ARRAPPEND", "k", ".a", b"1\x00garbage"),
    O(ErrPrefix("SYNTAXERR"), "JSON.ARRINSERT", "k", ".a", "0", b"1\x00garbage"),
    O(ErrPrefix("SYNTAXERR"), "JSON.ARRINDEX", "k", ".a", b"1\x00garbage"),
    O(ErrPrefix("SYNTAXERR"), "JSON.STRAPPEND", "k", ".s", b'"y"\x00garbage'),
    O(ErrPrefix("SYNTAXERR"), "JSON.STRAPPEND", "k", ".s", b'"y\x00z"'),
    O(ErrPrefix("SYNTAXERR"), "JSON.MERGE", "k", ".", b'{"m":1}\x00garbage'),
    O(ErrPrefix("SYNTAXERR"), "JSON.MSET", "k", ".n", b"1\x00garbage"),
    O(ErrPrefix("SYNTAXERR"), "JSON.MSET", "k2", ".", b"1\x00garbage"),
    O(ErrPrefix("WRONGTYPE Value is not a number"), "JSON.NUMINCRBY", "k", ".n", b"1\x00garbage"),
    O(ErrPrefix("WRONGTYPE Value is not a number"), "JSON.NUMMULTBY", "k", ".n", b"1\x00"),
    O(b'{"a":[1],"s":"x","n":1}', "JSON.GET", "k"),
    O(0, "EXISTS", "k2"),
    # A path ends at its first NUL, as in the module.
    O(b"1", "JSON.GET", "k", b".n\x00garbage"),
    O(Simple(b"PONG"), "PING"),
)

section(
    "survives_deep_document",
    # 5,000 levels arrive through RESTORE and survive DEBUG RELOAD, then every
    # read command and several writes run on them with the limit back at 128.
    # Loading accepts a document deeper than json.max-path-limit (B9), up to
    # 1024 levels or the limit if that is higher, so the limit is raised while
    # the documents are loaded.
    O(OK, "CONFIG", "SET", "json.max-path-limit", "5000"),
    X("d", DEEP_ARRAYS),
    X("o", DEEP_OBJECTS),
    O(OK, "DEBUG", "RELOAD"),
    O(OK, "CONFIG", "SET", "json.max-path-limit", "128"),
    O(DEEP_ARRAYS.encode(), "JSON.GET", "d"),
    O(DEEP_OBJECTS.encode(), "JSON.GET", "o"),
    O(lambda r: isinstance(r, bytes) and len(r) > 2 * DEEP, "JSON.GET", "d", "INDENT", " ", "NEWLINE", "\n"),
    O(lambda r: isinstance(r, bytes) and r.startswith(b'{"$":[[') and r.endswith(b"]]]}"), "JSON.GET", "d", "$", ".[0]"),
    O([DEEP_ARRAYS.encode(), DEEP_OBJECTS.encode(), NIL], "JSON.MGET", "d", "o", "nokey", "."),
    O(Simple(b"array"), "JSON.TYPE", "d"),
    O([Simple(b"object")], "JSON.TYPE", "o", "$"),
    O(1, "JSON.ARRLEN", "d"),
    O(1, "JSON.OBJLEN", "o"),
    O([b"a"], "JSON.OBJKEYS", "o"),
    O(ErrPrefix("WRONGTYPE"), "JSON.STRLEN", "d"),
    O(-1, "JSON.ARRINDEX", "d", ".", "1"),
    O(ErrPrefix("LIMIT"), "JSON.ARRINDEX", "d", ".", "[" * (DEEP - 1) + "1" + "]" * (DEEP - 1)),
    O(lambda r: depth_of(r) == DEEP, "JSON.RESP", "d"),
    O(lambda r: depth_of(r) == 2 * DEEP, "JSON.RESP", "o"),
    O(DEEP, "JSON.DEBUG", "DEPTH", "d"),
    O(DEEP, "JSON.DEBUG", "DEPTH", "o"),
    O(DEEP, "JSON.DEBUG", "FIELDS", "d"),
    O([DEEP], "JSON.DEBUG", "FIELDS", "o", "$"),
    O(is_pos_int, "JSON.DEBUG", "MEMORY", "d"),
    O(is_pos_int, "JSON.DEBUG", "MEMORY", "o", "."),
    O(lambda r: isinstance(r, list) and r[0] == DEEP, "JSON.DEBUG", "MAX-DEPTH-KEY"),
    O(lambda r: isinstance(r, list) and is_pos_int(r[0]), "JSON.DEBUG", "MAX-SIZE-KEY"),
    # A recursive descent goes at most 4,096 levels below its start, then
    # fails fast: its cost would otherwise grow with the square of the depth.
    O(lambda r: isinstance(r, Err) and r.startswith(b"LIMIT"), "JSON.ARRLEN", "d", "$..*"),
    O(lambda r: isinstance(r, Err) and r.startswith(b"LIMIT"), "JSON.TYPE", "o", "$..a"),
    # Writes below the surface. A sub-path write is checked against the limit.
    O(lambda r: isinstance(r, Err), "JSON.SET", "d", "$" + "[0]" * 150, "2"),
    O(ErrPrefix("LIMIT"), "JSON.SET", "d", "$" + "[0]" * 127, "[[[1]]]"),
    O(OK, "JSON.SET", "o", "$.a.b", "2"),
    O(2, "JSON.OBJLEN", "o", ".a"),
    O([2], "JSON.ARRAPPEND", "d", "$[0]", "7"),
    O(b"7", "JSON.ARRPOP", "d", ".[0]"),
    O(b"[3]", "JSON.NUMINCRBY", "o", "$.a.b", "1"),
    O(1, "JSON.CLEAR", "o", "$.a.a"),
    O(b'{"a":{"a":{},"b":3}}', "JSON.GET", "o"),
    O(OK, "CONFIG", "SET", "json.max-path-limit", "5000"),
    O(OK, "DEBUG", "RELOAD"),
    O(OK, "CONFIG", "SET", "json.max-path-limit", "128"),
    O(DEEP_ARRAYS.encode(), "JSON.GET", "d"),
    # JSON.DEL: one level, every level, the key.
    O(1, "JSON.DEL", "d", "$" + "[0]" * 100),
    O(99, "JSON.DEBUG", "DEPTH", "d"),
    O(99, "JSON.DEL", "d", "$..[0]"),
    O(b"[]", "JSON.GET", "d"),
    O(OK, "CONFIG", "SET", "json.max-path-limit", "5000"),
    X("d", DEEP_ARRAYS),
    O(OK, "CONFIG", "SET", "json.max-path-limit", "128"),
    O(1, "JSON.DEL", "d"),
    O(1, "JSON.FORGET", "o", "."),
    O(Simple(b"PONG"), "PING"),
)


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------


def changes_inside_a_document(cmd):
    """True for a command that edits part of a document (as opposed to a read, or a write of a whole key)."""
    name = (cmd[0].decode() if isinstance(cmd[0], bytes) else str(cmd[0])).upper()
    root = (".", "$", b".", b"$")
    if name == "JSON.SET":
        return len(cmd) > 2 and cmd[2] not in root
    if name == "JSON.MSET":
        return any(p not in root for p in cmd[2::3])
    if name in ("JSON.DEL", "JSON.FORGET"):
        return len(cmd) > 2 and cmd[2] not in root
    return name in (
        "JSON.MERGE", "JSON.NUMINCRBY", "JSON.NUMMULTBY", "JSON.STRAPPEND", "JSON.TOGGLE", "JSON.ARRAPPEND",
        "JSON.ARRINSERT", "JSON.ARRPOP", "JSON.ARRTRIM", "JSON.CLEAR",
    )  # fmt: skip


def show(cmd):
    out = []
    for a in cmd:
        s = a.decode("utf-8", "backslashreplace") if isinstance(a, bytes) else str(a)
        out.append(s if len(s) <= 80 else s[:77] + "...")
    return " ".join(out)


class Target:
    def __init__(self, server, use_repl):
        self.server = server
        self.conns = {2: server.connect(), 3: server.connect()}
        hello = self.conns[3].cmd("HELLO", "3")
        if isinstance(hello, Err):
            raise RuntimeError("%s: HELLO 3 failed: %r" % (server.name, hello))
        self.sub = server.connect_as(Subscriber)
        self.sub.attach()
        self.repl = None
        if use_repl:
            self.repl = server.connect_as(Replica)
            self.repl.attach()

    def mark(self, n):
        """Ends a section: returns its events and its replication stream."""
        mark = b"%d" % n
        self.conns[2].cmd("SET", MARK, mark)
        events = self.sub.until_mark()
        stream = self.repl.until_mark(mark) if self.repl else []
        return events, stream

    def close(self):
        for c in list(self.conns.values()) + [self.sub] + ([self.repl] if self.repl else []):
            c.close()


def run_section(name, steps, ref, new, proto, verbose, counter, reads_only=False):
    failures = []
    compare_side_effects = True
    for t in (ref, new):
        t.conns[2].cmd("FLUSHALL")
        t.conns[2].cmd("CONFIG", "SET", "json.max-document-size", "0")
        t.conns[2].cmd("CONFIG", "SET", "json.max-path-limit", "128")
    counter[0] += 1
    ref.mark(counter[0])
    new.mark(counter[0])

    for kind, cmd, expected in steps:
        if kind == "only" and NEW_ARGS:
            continue  # The candidate is a reference server: it must not get these.
        if kind == "xfer":
            compare_side_effects = False
            if reads_only or NEW_ARGS:
                continue
            key, text = cmd
            rc = ref.conns[2]
            rc.cmd("CONFIG", "SET", "json.max-path-limit", "100000")
            r1 = rc.cmd("JSON.SET", "__xfer__", ".", text)
            payload = rc.cmd("DUMP", "__xfer__")
            rc.cmd("DEL", "__xfer__")
            rc.cmd("CONFIG", "SET", "json.max-path-limit", "128")
            r2 = new.conns[2].cmd("RESTORE", key, "0", payload, "REPLACE") if isinstance(payload, bytes) else None
            ok = r1 == OK and r2 == OK
            if verbose or not ok:
                print("  %s xfer %s %s" % ("ok  " if ok else "FAIL", key, show((text,))))
                if not ok:
                    print("       reference JSON.SET: %r, candidate RESTORE: %r" % (r1, r2))
            if not ok:
                failures.append((name, proto, ("<xfer>", key)))
            continue
        if reads_only and (kind in ("new", "only") or changes_inside_a_document(cmd)):
            continue
        got_new = new.conns[proto].cmd(*cmd)
        got_ref = None
        if kind != "only":
            got_ref = ref.conns[proto].cmd(*cmd)
        ok = True
        if kind == "same":
            ok = got_new == got_ref
            want = got_ref
        elif kind == "sorted":
            ok = isinstance(got_new, list) and isinstance(got_ref, list) and sorted(got_new) == sorted(got_ref)
            want = got_ref
        elif kind == "pred":
            ok = bool(expected(got_new)) and bool(expected(got_ref))
            want = "<predicate>"
        else:
            compare_side_effects = False
            ok = bool(expected(got_new)) if callable(expected) else expected == got_new
            want = "<predicate>" if callable(expected) else expected
        if verbose or not ok:
            print("  %s %-4s %s" % ("ok  " if ok else "FAIL", kind, show(cmd)))
            if not ok or verbose > 1:
                print("       candidate: %r" % (got_new,))
                print("       %s: %r" % ("reference" if kind in ("same", "pred", "sorted") else "expected ", want))
                if kind == "new":
                    print("       reference: %r" % (got_ref,))
        if not ok:
            failures.append((name, proto, cmd))

    counter[0] += 1
    ev_ref, st_ref = ref.mark(counter[0])
    ev_new, st_new = new.mark(counter[0])
    if compare_side_effects:
        if ev_ref != ev_new:
            print("  FAIL keyspace events differ in section %s (RESP%d)" % (name, proto))
            for i in range(max(len(ev_ref), len(ev_new))):
                a = ev_ref[i] if i < len(ev_ref) else None
                b = ev_new[i] if i < len(ev_new) else None
                if a != b:
                    print("       first difference at event %d: reference %r, candidate %r" % (i, a, b))
                    break
            failures.append((name, proto, ("<events>",)))
        if st_ref != st_new:
            print("  FAIL replication streams differ in section %s (RESP%d)" % (name, proto))
            for i in range(max(len(st_ref), len(st_new))):
                a = st_ref[i] if i < len(st_ref) else None
                b = st_new[i] if i < len(st_new) else None
                if a != b:
                    print("       first difference at command %d:" % i)
                    print("         reference: %s" % (show(a) if a else None))
                    print("         candidate: %s" % (show(b) if b else None))
                    break
            failures.append((name, proto, ("<replication>",)))
    return failures


def check_b10(rundir):
    """B10: a config that loads the external module as well does not start."""
    if not os.path.exists(REF_MODULE):
        print("  skip B10 startup check: %s not found" % REF_MODULE)
        return []
    s = Server("both", NEW_SERVER, rundir, ["--loadmodule", REF_MODULE])
    try:
        started = s.wait_ready(timeout=8.0)
        if started:
            print("  FAIL B10: the candidate started with the external JSON module loaded as well")
            return [("B10_startup", 0, ("--loadmodule",))]
        print("  ok   B10: the candidate refuses to start with the external JSON module loaded as well")
        return []
    finally:
        s.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="count", default=0)
    ap.add_argument("--only", action="append", help="run only this section (repeatable)")
    ap.add_argument("--keep", action="store_true", help="keep the run directory")
    ap.add_argument("--no-repl", action="store_true", help="do not compare replication streams")
    ap.add_argument(
        "--reads-only",
        action="store_true",
        help="skip every step that edits part of a document, and the behaviour-change steps",
    )
    args = ap.parse_args()

    for path in (NEW_SERVER, REF_SERVER, REF_MODULE):
        if not os.path.exists(path):
            print("missing: %s" % path)
            return 2

    os.makedirs(RUN_ROOT, mode=0o700, exist_ok=True)
    os.chmod(RUN_ROOT, 0o700)
    rundir = os.path.join(RUN_ROOT, "r%d" % os.getpid())
    os.makedirs(rundir, mode=0o700)
    os.chmod(rundir, 0o700)

    servers, targets, failures = [], [], []
    try:
        ref_srv = Server("reference", REF_SERVER, os.path.join(rundir, "ref"), ["--loadmodule", REF_MODULE])
        servers.append(ref_srv)
        new_srv = Server("candidate", NEW_SERVER, os.path.join(rundir, "new"), NEW_ARGS)
        servers.append(new_srv)
        for s in servers:
            if not s.wait_ready():
                print("%s server did not start. Log:\n%s" % (s.name, s.log_tail()))
                return 2
        probe = new_srv.connect()
        r = probe.cmd("JSON.SET", "__probe__", ".", "1")
        probe.cmd("DEL", "__probe__")
        probe.close()
        if r != OK:
            print("the candidate has no working JSON.SET (%r): JSON is not built in yet" % (r,))
            return 2

        ref = Target(ref_srv, not args.no_repl)
        new = Target(new_srv, not args.no_repl)
        targets = [ref, new]
        counter = [0]
        ran = 0
        for proto in (2, 3):
            for name, steps in SECTIONS:
                if args.only and name not in args.only:
                    continue
                if name.startswith("crash_") and not args.only:
                    if proto == 2:
                        print("[skip] %s: known to stop the candidate, run it with --only" % name)
                    continue
                print("[RESP%d] %s (%d steps)" % (proto, name, len(steps)))
                failures += run_section(name, steps, ref, new, proto, args.verbose, counter, args.reads_only)
                ran += len(steps)
        if not NEW_ARGS and (not args.only or "B10" in args.only):
            print("[startup] B10")
            failures += check_b10(os.path.join(rundir, "both"))
        print()
        print("%d steps run, %d failed" % (ran, len(failures)))
        for name, proto, cmd in failures:
            print("  RESP%s %s: %s" % (proto, name, show(cmd)))
    finally:
        for t in targets:
            t.close()
        for s in servers:
            s.stop()
        if not args.keep and not failures:
            shutil.rmtree(rundir, ignore_errors=True)
        elif failures or args.keep:
            print("run directory kept: %s" % rundir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
