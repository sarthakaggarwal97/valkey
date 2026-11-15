#!/usr/bin/env python3
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
"""Mixed-version replication and AOF test of the built-in JSON commands.

Two implementations must be able to follow each other:

  reference   a server that loads the external valkey-json module
  candidate   src/valkey-server of this tree, where JSON is built in

Replication, in both directions. The primary gets a varied keyspace, the
replica does a full sync, then the write commands of the directed suite
(test_cmd.py) run on the primary section by section. After each section every
key is compared on the two sides (JSON.GET key $, and DEBUG DIGEST-VALUE where
the text is equal). When a section diverges it is run again command by command
to name the command that caused it, and the difference is classified as one of
the documented deliberate differences or left UNCLASSIFIED.

AOF. An append-only file written by one implementation (as the commands were
received, and again after BGREWRITEAOF) is loaded by the other.

A replica or an AOF load is a trusted path: it must accept whatever the
primary accepted. With the module as primary the stream therefore also has
JSON text that an ordinary client of the candidate would be refused: text cut
at a NUL, invalid UTF-8, numbers outside the range of a double, documents
deeper or larger than the replica's own limits.

Replication needs TCP. The primary listens on 127.0.0.1 only, on a free port,
with protected mode on. Everything else uses unix sockets in a mode-700
directory under tests/cmd/run/. Every server is stopped at the end.

Usage: test_repl.py [-v] [--only a|b|aof]
"""

import json
import os
import shutil
import socket
import sys
import time

import test_cmd as T
from test_cmd import Err, Nil

HERE = os.path.dirname(os.path.abspath(__file__))
VERBOSE = "-v" in sys.argv[1:]

READS = {
    "JSON.GET", "JSON.MGET", "JSON.TYPE", "JSON.STRLEN", "JSON.OBJLEN", "JSON.OBJKEYS", "JSON.ARRLEN", "JSON.ARRINDEX",
    "JSON.RESP", "JSON.DEBUG", "COMMAND", "ACL", "TTL", "EXISTS", "GET", "TYPE", "PING", "DEBUG",
}  # fmt: skip

# Sections of the directed suite that are not replayed here: commands that
# stop or desynchronise the module (it would be the primary or the replica),
# and sections about replies only.
SKIP_SECTIONS = ("survives_", "reply_injection", "format_limit", "command_table", "B8_", "B9_", "B10")


def name_of(cmd):
    return (cmd[0].decode() if isinstance(cmd[0], bytes) else str(cmd[0])).upper()


def stream_sections():
    for name, steps in T.SECTIONS:
        if name.startswith(SKIP_SECTIONS):
            continue
        yield name, [cmd for kind, cmd, _ in steps if kind in ("same", "new", "pred", "sorted")]


def keyspace():
    """Documents of every shape the suites use, for the full sync."""
    docs = {
        "wiki": T.WIKI,
        "store": T.STORE,
        "mixed": T.MIXED,
        "decimals": T.DEC_DOC,
        "deep": T.DEEP_OK,
        "deep_objects": '{"a":' * 127 + "1" + "}" * 127,
        "scalar_int": "42",
        "scalar_str": '"\\u00e9\\u4e2d\\ud83d\\ude00 \\u0000 \\n"',
        "scalar_null": "null",
        "scalar_dec": "19.90",
        "bigints": "[18446744073709551615,-9223372036854775809,1234567890123456789012345,9223372036854775807]",
        "exponents": "[1e5,1E5,1.0E+2,1e-7,1.5e300,-0.0,0e0,2.50e2]",
        "non_ascii": '{"été":"日本語","\U0001f600":["שלום","é"]}',
        "escapes": '{"a\\"b":"x\\\\y\\/z\\b\\f\\n\\r\\t\\u001f\\u007f","":""}',
        "literals": "[true,false,null,true,false,null]",
        "literals_obj": '{"t":[true,true],"n":[null],"f":[false,false,false],"e":[]}',
        "large_ints": "[" + ",".join(str(i * 7919) for i in range(10000)) + "]",
        "large_dec": "[" + ",".join("%d.%06d" % (i % 3, (i * 104729) % 1000000) for i in range(5000)) + "]",
        "large_strings": "[" + ",".join('"s%d"' % i for i in range(5000)) + "]",
        "records": "["
        + ",".join('{"id":%d,"name":"user%d","score":%d.5,"tags":["a","b"],"ok":%s}' % (i, i, i, "true" if i % 2 else "false") for i in range(2000))
        + "]",
        "wide": "{" + ",".join('"k%d":%d' % (i, i) for i in range(3000)) + "}",
        "empty_obj": "{}",
        "empty_arr": "[]",
        "dup_keys": '{"a":1,"b":2,"a":3}',
    }
    return docs


# Commands a module primary accepts and an ordinary client of the candidate
# would be refused. On the replica they arrive on the trusted path.
TRUSTED_STREAM = [
    # text that ends at a NUL
    ("JSON.SET", "nul", ".", b'{"a":1,"s":"x","arr":[1],"n":5}\x00junk'),
    ("JSON.SET", "nul", ".b", b"2\x00junk"),
    ("JSON.SET", "nul", "$.c", b'"str"\x00'),
    ("JSON.ARRAPPEND", "nul", ".arr", b"2\x00x", b'"three"\x00\x00', b"[4]\x00]"),
    ("JSON.ARRINSERT", "nul", ".arr", "0", b"0\x00garbage"),
    ("JSON.STRAPPEND", "nul", ".s", b'"y"\x00"z"'),
    ("JSON.MERGE", "nul", ".", b'{"m":{"k":1}}\x00{'),
    ("JSON.MSET", "nul", ".d", b"4\x00a", "nul2", ".", b"[1,2]\x00,3]"),
    ("JSON.NUMINCRBY", "nul", ".n", b"1\x00garbage"),
    ("JSON.NUMMULTBY", "nul", ".n", b"2\x00.5"),
    # invalid UTF-8 in values and in member names, also added through a path
    ("JSON.SET", "u8", ".", b'{"s":"\xff\xfe","\xff":1,"arr":[]}'),
    ("JSON.SET", "u8", b"$.\xc3\x28", "1"),
    ("JSON.SET", "u8", ".v", b'"\xed\xa0\x80"'),
    ("JSON.ARRAPPEND", "u8", ".arr", b'"\xc0\xaf"', b'{"\xf5":[]}'),
    ("JSON.MERGE", "u8", ".", b'{"m":"\xff"}'),
    ("JSON.MERGE", "u8", b"$.\xfe", '{"a":1}'),
    ("JSON.STRAPPEND", "u8", ".s", b'"\xff"'),
    ("JSON.MSET", "u8", b".\xfd", "1", "u8b", ".", b'["\xff"]'),
    # numbers that are not finite doubles (never read as numbers afterwards:
    # the module stops when it converts one)
    ("JSON.SET", "rng", ".", "[1.8e308,-1.8e308,1]"),
    ("JSON.SET", "rng2", ".", '{"a":1,"arr":[]}'),
    ("JSON.SET", "rng2", ".a", "1.8e308"),
    ("JSON.ARRAPPEND", "rng2", ".arr", "-1.8e309", "2"),
    ("JSON.MERGE", "rng2", ".", '{"m":1e999}'),
]


class Pair:
    """A primary and its replica."""

    def __init__(self, rundir, primary_is_ref):
        port = free_port()
        tcp = ["--port", str(port), "--bind", "127.0.0.1", "-::1", "--protected-mode", "yes"]
        ref_args = ["--loadmodule", T.REF_MODULE]
        if primary_is_ref:
            self.primary = T.Server("reference (primary)", T.REF_SERVER, rundir + "/p", ref_args + tcp)
            self.replica = T.Server("candidate (replica)", T.NEW_SERVER, rundir + "/r", [])
        else:
            self.primary = T.Server("candidate (primary)", T.NEW_SERVER, rundir + "/p", tcp)
            self.replica = T.Server("reference (replica)", T.REF_SERVER, rundir + "/r", ref_args)
        self.port = port
        self.servers = [self.primary, self.replica]
        for s in self.servers:
            if not s.wait_ready():
                raise RuntimeError("%s did not start:\n%s" % (s.name, s.log_tail()))
        self.p = self.primary.connect()
        self.r = self.replica.connect()

    def attach(self):
        self.r.cmd("REPLICAOF", "127.0.0.1", str(self.port))
        deadline = time.time() + 60
        while time.time() < deadline:
            info = self.r.cmd("INFO", "replication")
            if isinstance(info, bytes) and b"master_link_status:up" in info:
                return
            time.sleep(0.05)
        raise RuntimeError("the replica did not finish the full sync:\n%s" % self.replica.log_tail())

    def sync(self):
        n = self.p.cmd("WAIT", "1", "20000")
        if n != 1:
            raise RuntimeError("WAIT replied %r: the replica is gone?\n%s" % (n, self.replica.log_tail()))

    def both(self, *cmd):
        self.p.cmd(*cmd)
        self.r.cmd(*cmd)

    def stop(self):
        for s in self.servers:
            s.stop()


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def snapshot(conn):
    out = {}
    keys = conn.cmd("KEYS", "*")
    for k in keys:
        t = conn.cmd("TYPE", k)
        if t == T.Simple(b"ReJSON-RL"):
            out[k] = conn.cmd("JSON.GET", k, "$")
        else:
            out[k] = ("other", t, conn.cmd("GET", k) if t == T.Simple(b"string") else None)
    return out


def diff_keys(a, b):
    return sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))


def as_numbers(text):
    """The document with every number read as a double, or None if it is not valid UTF-8 JSON."""
    try:
        return json.loads(text.decode("utf-8"), parse_int=float, parse_float=float)
    except (ValueError, UnicodeDecodeError, AttributeError):
        return None


def classify(cmd, reply, before_p, after_p, after_r, key):
    """Names the documented difference a divergence belongs to."""
    name = name_of(cmd)
    args = [a if isinstance(a, bytes) else str(a).encode() for a in cmd[1:]]
    path = args[1] if len(args) > 1 else b""
    vp, vr = after_p.get(key), after_r.get(key)
    if isinstance(reply, Err) and before_p.get(key) != vp:
        return "B6 (the primary failed part-way and kept the partial change)"
    if name in ("JSON.NUMINCRBY", "JSON.NUMMULTBY"):
        np_, nr_ = as_numbers(vp), as_numbers(vr)
        if np_ is not None and np_ == nr_:
            return "B3 (same values, different number text or type)"
        return "B3 (computed numbers: exact integers)"
    if b"~" in path or (b"/" in path and name in ("JSON.SET", "JSON.MSET", "JSON.MERGE", "JSON.DEL", "JSON.FORGET")):
        return "B5 (member name with ~ or /)"
    if b".." in path and name in ("JSON.SET", "JSON.MSET", "JSON.MERGE", "JSON.DEL", "JSON.FORGET", "JSON.CLEAR"):
        return "B4 (nested matches)"
    if name == "JSON.STRAPPEND" and (b"\\u0000" in args[-1] or b"\\u0000" in (before_p.get(key) or b"")):
        return "C (STRAPPEND cuts at an embedded NUL in the module)"
    if name == "JSON.SET" and (b"*" in path or b"," in path or b"?" in path):
        return "C (SET with one update and inserts: the module moves the value)"
    return "UNCLASSIFIED"


def nesting(text):
    """The deepest nesting of arrays and objects in JSON text."""
    depth = deepest = 0
    in_string = escaped = False
    for c in text:
        if in_string:
            if escaped:
                escaped = False
            elif c == 0x5C:
                escaped = True
            elif c == 0x22:
                in_string = False
        elif c == 0x22:
            in_string = True
        elif c in (0x5B, 0x7B):
            depth += 1
            deepest = max(deepest, depth)
        elif c in (0x5D, 0x7D):
            depth -= 1
    return deepest


def fmt_doc(v):
    s = repr(v)
    return s if len(s) <= 150 else s[:147] + "..."


def run_stream(pair, sections, report):
    """Runs sections on the primary and compares. Returns (commands, divergences)."""
    total = 0
    for name, cmds in sections:
        pair.both("CONFIG", "SET", "json.max-document-size", "0")
        pair.both("CONFIG", "SET", "json.max-path-limit", "128")
        pair.p.cmd("FLUSHALL")
        for cmd in cmds:
            if name_of(cmd) == "CONFIG":
                pair.both(*cmd)
            elif name_of(cmd) not in READS:
                pair.p.cmd(*cmd)
                total += 1
        pair.sync()
        if not diff_keys(snapshot(pair.p), snapshot(pair.r)):
            if VERBOSE:
                print("    %-34s same" % name)
            continue
        # Again, command by command, to name what diverges.
        pair.both("CONFIG", "SET", "json.max-document-size", "0")
        pair.both("CONFIG", "SET", "json.max-path-limit", "128")
        pair.p.cmd("FLUSHALL")
        pair.sync()
        before = {}
        found = 0
        for cmd in cmds:
            if name_of(cmd) == "CONFIG":
                pair.both(*cmd)
                continue
            if name_of(cmd) in READS:
                continue
            reply = pair.p.cmd(*cmd)
            pair.sync()
            sp, sr = snapshot(pair.p), snapshot(pair.r)
            for key in diff_keys(sp, sr):
                found += 1
                report.append((name, cmd, reply, key, sp.get(key), sr.get(key), classify(cmd, reply, before, sp, sr, key)))
                # Bring the replica back to the primary's document, so that the
                # next difference is charged to its own command.
                if key in sp and isinstance(sp[key], bytes):
                    pair.p.cmd("JSON.SET", key, ".", pair.p.cmd("JSON.GET", key, "."))
                else:
                    pair.p.cmd("SET", key, "x")  # A DEL of a missing key is not propagated.
                    pair.p.cmd("DEL", key)
            if found:
                pair.sync()
                sp = snapshot(pair.p)
            before = sp
        if VERBOSE or found:
            print("    %-34s %d divergence(s)" % (name, found))
    return total


def digests(pair):
    """DEBUG DIGEST-VALUE of every key whose text is equal on both sides."""
    sp, sr = snapshot(pair.p), snapshot(pair.r)
    # The module stops when it converts a number that is not a finite double,
    # and a digest does: those documents are left out.
    same = [k for k in sp if sp[k] == sr.get(k) and not (isinstance(sp[k], bytes) and (b"e308" in sp[k] or b"e309" in sp[k] or b"e999" in sp[k]))]
    bad = [k for k in same if pair.p.cmd("DEBUG", "DIGEST-VALUE", k) != pair.r.cmd("DEBUG", "DIGEST-VALUE", k)]
    return len(same), bad


def replication(rundir, primary_is_ref):
    title = "(a) module primary, candidate replica" if primary_is_ref else "(b) candidate primary, module replica"
    print("== replication %s" % title)
    pair = Pair(rundir, primary_is_ref)
    report = []
    try:
        for key, text in keyspace().items():
            r = pair.p.cmd("JSON.SET", key, ".", text)
            if r != T.OK:
                raise RuntimeError("could not load %s on the primary: %r" % (key, r))
        if primary_is_ref:
            pair.p.cmd("JSON.SET", "bad_utf8", ".", b'{"s":"\xff\xfe","\xc3\x28":[1]}')
            pair.p.cmd("JSON.SET", "out_of_range", ".", "[1.8e308,-1e999]")
        pair.attach()
        pair.sync()
        sp, sr = snapshot(pair.p), snapshot(pair.r)
        bad = diff_keys(sp, sr)
        print("  full sync: %d keys, %d differ%s" % (len(sp), len(bad), "" if not bad else ": %r" % bad))
        for k in bad:
            report.append(("full sync", ("<RDB>",), None, k, sp.get(k), sr.get(k), "UNCLASSIFIED"))
        n, bad_digest = digests(pair)
        print("  DEBUG DIGEST-VALUE after full sync: %d keys compared, %d differ%s"
              % (n, len(bad_digest), "" if not bad_digest else ": %r" % bad_digest[:8]))  # fmt: skip

        total = run_stream(pair, list(stream_sections()), report)
        print("  stream: %d write commands of the directed suite, %d divergence(s)" % (total, len(report)))

        if primary_is_ref:
            # The trusted path: nothing here may diverge. The replica also has
            # stricter limits than the primary.
            before = len(report)
            pair.p.cmd("FLUSHALL")
            pair.p.cmd("CONFIG", "SET", "json.max-path-limit", "100000")
            pair.r.cmd("CONFIG", "SET", "json.max-path-limit", "16")
            pair.r.cmd("CONFIG", "SET", "json.max-document-size", "64")
            deep = [
                ("JSON.SET", "deep", ".", "[" * 300 + "]" * 300),
                ("JSON.SET", "deep2", ".", '{"a":[]}'),
                ("JSON.ARRAPPEND", "deep2", ".a", "[" * 200 + "1" + "]" * 200),
                ("JSON.ARRINSERT", "deep2", ".a[0]" + "[0]" * 150, "0", '{"x":[[[]]]}'),
                ("JSON.SET", "deep2", ".a[0]" + "[0]" * 150 + "[0].y", "[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]"),
                ("JSON.MERGE", "deep2", ".a[0]" + "[0]" * 150 + "[0]", '{"z":{"z":{"z":{"z":{"z":{"z":{"z":{"z":{"z":{}}}}}}}}}}'),
                ("JSON.MSET", "deep2", ".b", "[" * 100 + "]" * 100, "deep3", ".", '{"big":"' + "x" * 5000 + '"}'),
                ("JSON.SET", "big", ".", '{"s":"","a":[]}'),
                ("JSON.STRAPPEND", "big", ".s", '"' + "y" * 5000 + '"'),
                ("JSON.ARRAPPEND", "big", ".a", '"' + "z" * 5000 + '"'),
            ]
            for cmd in TRUSTED_STREAM + deep:
                reply = pair.p.cmd(*cmd)
                pair.sync()
                sp, sr = snapshot(pair.p), snapshot(pair.r)
                for key in diff_keys(sp, sr):
                    report.append(("trusted path", cmd, reply, key, sp.get(key), sr.get(key), "UNCLASSIFIED"))
                    pair.p.cmd("JSON.SET", key, ".", pair.p.cmd("JSON.GET", key, "."))
                if isinstance(reply, Err) and VERBOSE:
                    print("    (the primary refused %s: %r)" % (T.show(cmd)[:60], reply))
            pair.both("CONFIG", "SET", "json.max-path-limit", "128")
            pair.both("CONFIG", "SET", "json.max-document-size", "0")
            sp = snapshot(pair.p)
            print("  trusted path: %d commands (NUL, invalid UTF-8, number range, depth and size limits), %d keys, "
                  "%d divergence(s)" % (len(TRUSTED_STREAM) + len(deep), len(sp), len(report) - before))  # fmt: skip
            n, bad_digest = digests(pair)
            print("  DEBUG DIGEST-VALUE on those keys: %d compared, %d differ%s"
                  % (n, len(bad_digest), "" if not bad_digest else ": %r" % bad_digest[:8]))  # fmt: skip
    finally:
        pair.stop()
    return report


# --------------------------------------------------------------------------
# AOF
# --------------------------------------------------------------------------

AOF_ARGS = ["--appendonly", "yes", "--aof-use-rdb-preamble", "no", "--appendfsync", "always"]


def wait_rewrite(conn):
    deadline = time.time() + 60
    while time.time() < deadline:
        info = conn.cmd("INFO", "persistence")
        if b"aof_rewrite_in_progress:0" in info and b"aof_rewrite_scheduled:0" in info:
            return
        time.sleep(0.05)
    raise RuntimeError("the AOF rewrite did not finish")


def aof_cross(rundir, writer_is_ref, rewrite):
    """One implementation writes an AOF, the other loads it. Returns (keys, differing keys or None if it did not load)."""
    wdir, ldir = rundir + "/w", rundir + "/l"
    ref_args = ["--loadmodule", T.REF_MODULE]
    writer = T.Server("writer", T.REF_SERVER if writer_is_ref else T.NEW_SERVER, wdir, (ref_args if writer_is_ref else []) + AOF_ARGS)
    try:
        if not writer.wait_ready():
            raise RuntimeError("the writer did not start:\n%s" % writer.log_tail())
        c = writer.connect()
        for key, text in keyspace().items():
            c.cmd("JSON.SET", "ks:" + key, ".", text)
        n = 0
        for name, cmds in stream_sections():
            for cmd in cmds:
                if name_of(cmd) in READS or not name_of(cmd).startswith("JSON.") or len(cmd) < 2:
                    continue
                # One keyspace for the whole stream: prefix the keys with the section.
                cmd = [a if isinstance(a, bytes) else str(a).encode("utf-8") for a in cmd]
                key_args = range(1, len(cmd), 3) if name_of(cmd) == "JSON.MSET" else (1,)
                for i in key_args:
                    cmd[i] = name.encode() + b":" + cmd[i]
                c.cmd(*cmd)
                n += 1
        if writer_is_ref:
            for cmd in TRUSTED_STREAM:
                c.cmd(*cmd)
                n += 1
        if rewrite:
            c.cmd("BGREWRITEAOF")
            wait_rewrite(c)
        written = snapshot(c)
        c.close()
    finally:
        writer.stop()
    os.makedirs(ldir, mode=0o700)
    shutil.copytree(os.path.join(wdir, "appendonlydir"), os.path.join(ldir, "appendonlydir"))
    loader = T.Server("loader", T.NEW_SERVER if writer_is_ref else T.REF_SERVER, ldir, ([] if writer_is_ref else ref_args) + AOF_ARGS)
    try:
        if not loader.wait_ready(timeout=60):
            return n, len(written), None, loader.log_tail(6)
        c = loader.connect()
        loaded = snapshot(c)
        c.close()
        return n, len(written), [(k, written.get(k), loaded.get(k)) for k in diff_keys(written, loaded)], ""
    finally:
        loader.stop()


def aof_deep(rundir):
    """Item: a document deeper than the limit must survive an AOF rewrite and an AOF replay."""
    s = T.Server("candidate", T.NEW_SERVER, rundir, AOF_ARGS)
    failures = []
    try:
        if not s.wait_ready():
            raise RuntimeError("did not start:\n%s" % s.log_tail())
        c = s.connect()
        c.cmd("JSON.SET", "k", "$", "[" * 128 + "]" * 128)
        r = c.cmd("JSON.ARRAPPEND", "k", "$" + "[0]" * 127, "[1]")
        d = c.cmd("JSON.DEBUG", "DEPTH", "k")
        if r != [1] or d != 129:
            failures.append("setup: ARRAPPEND replied %r, depth %r" % (r, d))
        # also a nesting the limit forbade by the time of the reload, and a size limit
        c.cmd("CONFIG", "SET", "json.max-path-limit", "500")
        c.cmd("JSON.SET", "deep", ".", "[" * 400 + "]" * 400)
        c.cmd("JSON.SET", "o", ".", '{"a":{"b":{"c":{}}}}')
        c.cmd("JSON.SET", "o", ".a.b.c.d", "[[[[[[[[[[1]]]]]]]]]]")
        c.cmd("JSON.MERGE", "o", ".a.b", '{"m":{"m":{"m":{"m":{"m":{"m":{}}}}}}}')
        c.cmd("JSON.SET", "big", ".", '"' + "x" * 4000 + '"')
        c.cmd("CONFIG", "SET", "json.max-path-limit", "8")
        c.cmd("CONFIG", "SET", "json.max-document-size", "64")
        want = snapshot(c)
        for label, rewrite in (("replay of the commands", False), ("after BGREWRITEAOF", True)):
            if rewrite:
                c.cmd("BGREWRITEAOF")
                wait_rewrite(c)
            r = c.cmd("DEBUG", "LOADAOF")
            got = snapshot(c)
            bad = diff_keys(want, got)
            if r != T.OK or bad:
                failures.append("%s: DEBUG LOADAOF replied %r, keys that differ: %r" % (label, r, bad))
        c.close()
    finally:
        s.stop()
    return failures


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    for path in (T.NEW_SERVER, T.REF_SERVER, T.REF_MODULE):
        if not os.path.exists(path):
            print("missing: %s" % path)
            return 2
    os.makedirs(T.RUN_ROOT, mode=0o700, exist_ok=True)
    os.chmod(T.RUN_ROOT, 0o700)
    rundir = os.path.join(T.RUN_ROOT, "x%d" % os.getpid())
    os.makedirs(rundir, mode=0o700)
    unclassified = 0
    try:
        for tag, primary_is_ref in (("a", True), ("b", False)):
            if only not in (None, tag):
                continue
            report = replication(os.path.join(rundir, tag), primary_is_ref)
            classes = {}
            for item in report:
                classes.setdefault(item[6], []).append(item)
            for label in sorted(classes):
                items = classes[label]
                print("  %3d  %s" % (len(items), label))
                show = items if (VERBOSE or label == "UNCLASSIFIED") else items[:2]
                for section, cmd, reply, key, vp, vr, _ in show:
                    print("         [%s] %s" % (section, T.show(cmd)[:110]))
                    print("            primary reply %s" % fmt_doc(reply))
                    print("            primary %s" % fmt_doc(vp))
                    print("            replica %s" % fmt_doc(vr))
            unclassified += len(classes.get("UNCLASSIFIED", []))

        if only in (None, "aof"):
            print("== AOF")
            bad = aof_deep(os.path.join(rundir, "aofdeep"))
            print("  candidate, documents deeper and larger than its limits: %s" % ("survive rewrite and replay" if not bad else "FAIL"))
            for b in bad:
                print("     " + b)
            unclassified += len(bad)
            i = 0
            for writer_is_ref in (True, False):
                for rewrite in (True, False):
                    i += 1
                    who = "module writes, candidate loads" if writer_is_ref else "candidate writes, module loads"
                    how = "after BGREWRITEAOF" if rewrite else "commands as received"
                    n, keys, bad, log = aof_cross(os.path.join(rundir, "aof%d" % i), writer_is_ref, rewrite)
                    if bad is None:
                        print("  %s, %s: %d commands, %d keys: THE LOADER DID NOT START" % (who, how, n, keys))
                        print("     " + log.replace("\n", "\n     "))
                        unclassified += 1 if rewrite or writer_is_ref else 0
                        continue
                    print("  %s, %s: %d commands, %d keys, %d differ" % (who, how, n, keys, len(bad)))
                    for k, w, l in bad if (VERBOSE or rewrite) else bad[:0]:
                        # A rewritten AOF holds whole documents. The module refuses one nested
                        # deeper than its limit, as it refuses its own (B9). The array commands
                        # let a document get one level past the limit in both implementations.
                        b9 = not writer_is_ref and l is None and isinstance(w, bytes) and nesting(w) - 1 > 128
                        print("       %r: %s" % (k, "B9 (deeper than the module's limit: it does not load it)" if b9 else "UNCLASSIFIED"))
                        print("          written %s\n          loaded  %s" % (fmt_doc(w), fmt_doc(l)))
                        if rewrite and not b9:
                            unclassified += 1
                    if rewrite:
                        pass
                    elif bad:
                        by = {}
                        for k, w, l in bad:
                            by.setdefault(k.split(b":")[0], 0)
                            by[k.split(b":")[0]] += 1
                        print("       by section: %s" % ", ".join("%s=%d" % (a.decode(), b) for a, b in sorted(by.items())))
    finally:
        shutil.rmtree(rundir, ignore_errors=True)
    print()
    print("unclassified divergences and failures: %d" % unclassified)
    return 1 if unclassified else 0


if __name__ == "__main__":
    sys.exit(main())
