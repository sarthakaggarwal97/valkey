#!/usr/bin/env python3
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
"""Test of the surface valkey-search uses of the JSON module.

valkey-search itself is not available here. Its use of JSON was read from its
source (github.com/valkey-io/valkey-search, default branch) and is reproduced
by the test module searchmod/searchmod.so:

  What valkey-search does                                   Where
  --------------------------------------------------------  ----------------------------------------
  Detects JSON by ValkeyModule_Call(ctx, "MODULE", "c",     vmsdk/src/module.cc, IsModuleLoaded()
  "LIST") and a module whose "name" is "json". A positive   src/attribute_data_type.cc,
  answer is cached. No version is compared.                 IsJsonModuleSupported()
  Takes ValkeyModule_GetSharedAPI(ctx, "JSON_GetValue") as  src/attribute_data_type.h,
  int (*)(ValkeyModuleKey *key, const char *path,           JsonSharedAPIGetValueFn
  ValkeyModuleString **result). If it is missing it only    src/attribute_data_type.cc
  logs that cluster mode needs valkey-json 1.02 or higher.
  Reads an attribute with json_get(open_key, path, &str).   src/attribute_data_type.cc,
  VALKEYMODULE_ERR means "no such attribute". The string    GetJsonAttribute(), NormalizeJsonAttribute()
  is freed with ValkeyModule_FreeString(nullptr, str).      vmsdk/src/managed_pointers.h
  The text must start with "[" (a `$` path result): the
  brackets and one pair of quotes are removed. Paths are
  the index's identifiers, and "$" for the whole record.    src/query/response_generator.cc
  Without the shared API it calls ValkeyModule_Call(ctx,    src/attribute_data_type.cc
  "JSON.GET", "cc", key, path) and wants a string reply.
  No other JSON command is issued.
  Opens keys with VALKEYMODULE_OPEN_KEY_NOEFFECTS |         src/index_schema.cc,
  VALKEYMODULE_READ. A key is JSON when ValkeyModule_       ProcessKeyspaceNotification()
  KeyType(key) == VALKEYMODULE_KEYTYPE_MODULE. The type     src/attribute_data_type.h, IsProperType()
  name and ValkeyModule_ModuleTypeGetType are not used.
  Subscribes to VALKEYMODULE_NOTIFY_MODULE | GENERIC |      src/attribute_data_type.h,
  EXPIRED | EVICTED. Only "rename_from", "rename_to",       GetValkeyEventTypes()
  "move_from" and "move_to" are told apart by name. On any  src/keyspace_event_manager.cc
  other event the key is read again.
  At the end of its OnLoad it logs "Json supported!" or     src/valkey_search.cc, ValkeySearch::OnLoad()
  "Json not supported!" and loads either way. JSON may
  also be loaded later: the check is repeated until it
  succeeds.

The test module is loaded into

  reference   valkey-server-ref --loadmodule libjson --loadmodule searchmod.so
  candidate   src/valkey-server --loadmodule searchmod.so     (JSON is built in)

Every command goes to both and the replies must be identical. After every
command the keyspace events the module received (type, name, key, whether the
key was a module key, and the document JSON_GetValue gave at that moment) are
compared as well. Then the cases that exist only with the built-in module are
checked on the candidate: the shared API is there when a module of the
configuration loads, and with `json-enabled no` the lookup fails cleanly.

Both servers listen on a unix socket only, in a mode-700 directory under
tests/cmd/run/. Usage: test_search.py [-v]
"""

import os
import shutil
import subprocess
import sys
import time

import test_cmd as T
from test_cmd import OK, ErrPrefix

HERE = os.path.dirname(os.path.abspath(__file__))
MOD_DIR = os.path.join(HERE, "searchmod")
MOD = os.path.join(MOD_DIR, "searchmod.so")

DOC = (
    '{"title":"Dune","year":1965,"price":19.90,"tags":["sf","classic"],"author":{"name":"Frank \\"H\\" Herbert","born":1920},'
    '"vec":[0.1,0.2,0.3],"ok":true,"none":null,"empty":"","u":"\\u00e9\\n","list":[{"n":1},{"n":2}]}'
)

READS = [
    # the whole record, single values of every kind, arrays, several matches
    ("SEARCHMOD.GET", "doc", "$"),
    ("SEARCHMOD.GET", "doc", "$.title"),
    ("SEARCHMOD.GET", "doc", "$.year"),
    ("SEARCHMOD.GET", "doc", "$.price"),
    ("SEARCHMOD.GET", "doc", "$.ok"),
    ("SEARCHMOD.GET", "doc", "$.none"),
    ("SEARCHMOD.GET", "doc", "$.empty"),
    ("SEARCHMOD.GET", "doc", "$.u"),
    ("SEARCHMOD.GET", "doc", "$.tags"),
    ("SEARCHMOD.GET", "doc", "$.tags[0]"),
    ("SEARCHMOD.GET", "doc", "$.tags[*]"),
    ("SEARCHMOD.GET", "doc", "$.vec"),
    ("SEARCHMOD.GET", "doc", "$.author"),
    ("SEARCHMOD.GET", "doc", "$.author.name"),
    ("SEARCHMOD.GET", "doc", "$['author']['born']"),
    ("SEARCHMOD.GET", "doc", "$.list[*].n"),
    ("SEARCHMOD.GET", "doc", "$..n"),
    ("SEARCHMOD.GET", "doc", "$.list[?(@.n>1)]"),
    # missing paths
    ("SEARCHMOD.GET", "doc", "$.missing"),
    ("SEARCHMOD.GET", "doc", "$.tags[9]"),
    ("SEARCHMOD.GET", "doc", "$.title.x"),
    ("SEARCHMOD.GET", "doc", ""),
    # legacy syntax: found, missing, several values
    ("SEARCHMOD.GET", "doc", "."),
    ("SEARCHMOD.GET", "doc", ".title"),
    ("SEARCHMOD.GET", "doc", "title"),
    ("SEARCHMOD.GET", "doc", ".tags"),
    ("SEARCHMOD.GET", "doc", ".tags[*]"),
    ("SEARCHMOD.GET", "doc", ".missing"),
    ("SEARCHMOD.GET", "doc", ".tags[9]"),
    # invalid paths
    ("SEARCHMOD.GET", "doc", "$["),
    ("SEARCHMOD.GET", "doc", ".["),
    ("SEARCHMOD.GET", "doc", "$.tags[x]"),
    # other documents and keys
    ("SEARCHMOD.GET", "scalar", "$"),
    ("SEARCHMOD.GET", "arr", "$"),
    ("SEARCHMOD.GET", "arr", "$[*]"),
    ("SEARCHMOD.GET", "str", "$"),
    ("SEARCHMOD.GET", "hash", "$.a"),
    ("SEARCHMOD.GET", "nokey", "$"),
    ("SEARCHMOD.TYPE", "doc"),
    ("SEARCHMOD.TYPE", "scalar"),
    ("SEARCHMOD.TYPE", "str"),
    ("SEARCHMOD.TYPE", "hash"),
    ("SEARCHMOD.TYPE", "nokey"),
]
# The same reads through JSON.GET, the path valkey-search takes without the shared API.
READS += [("SEARCHMOD.CALLGET",) + c[1:] for c in READS if c[0] == "SEARCHMOD.GET"]

WRITES = [
    # every JSON write command, both syntaxes, and commands that change nothing
    ("JSON.SET", "k", "$", '{"a":1,"s":"x","b":true,"arr":[1,2,3],"o":{"n":1}}'),
    ("JSON.SET", "k", "$.a", "2"),
    ("JSON.SET", "k", ".new", '"v"'),
    ("JSON.SET", "k", "$.a", "3", "NX"),
    ("JSON.SET", "k", "$.zz", "3", "XX"),
    ("JSON.SET", "k", "$.nothing[*]", "1"),
    ("JSON.SET", "k", ".x.y", "1"),
    ("JSON.SET", "k", "$", "{"),
    ("JSON.MSET", "k", "$.m", "1", "k2", "$", "[1,2]"),
    ("JSON.MSET", "k", "$.m", "2", "nokey", "$.a", "1"),
    ("JSON.MERGE", "k", "$", '{"merged":{"x":1},"new":null}'),
    ("JSON.MERGE", "k", ".o", '{"n":null}'),
    ("JSON.MERGE", "k3", "$", '{"a":1}'),
    ("JSON.MERGE", "k", "$.nothing[*]", "1"),
    ("JSON.NUMINCRBY", "k", "$.a", "5"),
    ("JSON.NUMINCRBY", "k", ".a", "-1"),
    ("JSON.NUMINCRBY", "k", "$.nothing", "1"),
    ("JSON.NUMINCRBY", "k", ".nothing", "1"),
    ("JSON.NUMINCRBY", "k", ".s", "1"),
    ("JSON.NUMMULTBY", "k", "$.a", "2"),
    ("JSON.NUMMULTBY", "k", ".a", "3"),
    ("JSON.STRAPPEND", "k", "$.s", '"y"'),
    ("JSON.STRAPPEND", "k", ".s", '"z"'),
    ("JSON.STRAPPEND", "k", "$.nothing", '"z"'),
    ("JSON.TOGGLE", "k", "$.b"),
    ("JSON.TOGGLE", "k", ".b"),
    ("JSON.TOGGLE", "k", "$.nothing"),
    ("JSON.TOGGLE", "k", ".nothing"),
    ("JSON.ARRAPPEND", "k", "$.arr", "4", "5"),
    ("JSON.ARRAPPEND", "k", ".arr", "6"),
    ("JSON.ARRAPPEND", "k", "$.nothing", "6"),
    ("JSON.ARRINSERT", "k", "$.arr", "0", "0"),
    ("JSON.ARRINSERT", "k", ".arr", "1", "0.5"),
    ("JSON.ARRINSERT", "k", ".arr", "99", "0"),
    ("JSON.ARRPOP", "k", "$.arr"),
    ("JSON.ARRPOP", "k", ".arr", "0"),
    ("JSON.ARRPOP", "k", "$.nothing"),
    ("JSON.ARRTRIM", "k", "$.arr", "1", "3"),
    ("JSON.ARRTRIM", "k", ".arr", "0", "1"),
    ("JSON.ARRTRIM", "k", "$.nothing", "0", "1"),
    ("JSON.CLEAR", "k", "$.arr"),
    ("JSON.CLEAR", "k", ".o"),
    ("JSON.CLEAR", "k", "$.nothing"),
    ("JSON.CLEAR", "k", ".nothing"),
    ("JSON.DEL", "k", "$.merged"),
    ("JSON.FORGET", "k", ".new"),
    ("JSON.DEL", "k", "$.nothing"),
    ("JSON.DEL", "k", ".nothing"),
    ("JSON.DEL", "k", "$["),
    ("JSON.FORGET", "k3"),
    ("JSON.DEL", "k2", "."),
    ("JSON.DEL", "nokey"),
    # reads fire nothing
    ("JSON.GET", "k", "$"),
    ("JSON.TYPE", "k", "$.a"),
    # generic commands on a JSON key
    ("EXPIRE", "k", "1000"),
    ("PERSIST", "k"),
    ("RENAME", "k", "k_renamed"),
    ("RENAMENX", "k_renamed", "k"),
    ("COPY", "k", "k_copy"),
    ("MOVE", "k_copy", "1"),
    ("UNLINK", "k_copy"),
    ("JSON.SET", "tmp", "$", "[1]"),
    ("DEL", "tmp"),
    ("JSON.SET", "tmp", "$", "[2]"),
    ("SET", "tmp", "now a string"),
    ("JSON.SET", "tmp", "$", "[3]"),
    ("DEL", "tmp"),
    ("JSON.SET", "ttl", "$", '{"a":1}'),
    ("PEXPIRE", "ttl", "1"),
    ("<sleep>",),
    ("EXISTS", "ttl"),
    ("DEL", "k"),
]


def build():
    r = subprocess.run(["make", "-C", MOD_DIR], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(MOD):
        print("cannot build searchmod.so:\n%s%s" % (r.stdout, r.stderr))
        return False
    return True


def startup_checks(rundir):
    """The cases that exist only with the built-in module. Returns the number of failures."""
    failures = 0

    def check(what, ok, detail=""):
        nonlocal failures
        print("  %s %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else ": " + detail))
        failures += 0 if ok else 1

    # 1. A module of the configuration finds the built-in JSON in its OnLoad.
    s = T.Server("candidate", T.NEW_SERVER, rundir + "/s1", ["--loadmodule", MOD])
    try:
        up = s.wait_ready()
        log = open(os.path.join(s.rundir, "server.log"), errors="replace").read()
        check("built-in JSON is loaded before the modules of the configuration: OnLoad finds JSON_GetValue",
              up and "searchmod: Json supported" in log, s.log_tail(4))  # fmt: skip
        if up:
            c = s.connect()
            st = c.cmd("SEARCHMOD.STATUS")
            check("MODULE LIST names it \"json\" (it reports version %s, the external module 999999; valkey-search "
                  "compares no version)" % (st[2] if isinstance(st, list) else "?"),
                  isinstance(st, list) and st[:2] == [1, 1], repr(st))  # fmt: skip
    finally:
        s.stop()

    # 2. json-enabled no: the lookup fails cleanly. A strict consumer refuses to load and the server says why.
    s = T.Server("candidate", T.NEW_SERVER, rundir + "/s2", ["--json-enabled", "no", "--loadmodule", MOD])
    try:
        up = s.wait_ready(timeout=8)
        deadline = time.time() + 5
        while s.proc.poll() is None and time.time() < deadline:
            time.sleep(0.05)
        log = open(os.path.join(s.rundir, "server.log"), errors="replace").read()
        check("json-enabled no, strict consumer: the server refuses to start and logs the reason, no crash",
              not up and s.proc.poll() is not None and s.proc.returncode > 0 and "Json not supported" in log
              and "crashed by signal" not in log, "up=%r exit=%r\n%s" % (up, s.proc.poll(), s.log_tail(5)))  # fmt: skip
    finally:
        s.stop()

    # 3. json-enabled no, a consumer that loads anyway (what valkey-search does): JSON is reported as unsupported.
    s = T.Server("candidate", T.NEW_SERVER, rundir + "/s3", ["--json-enabled", "no", "--loadmodule", MOD, "lenient"])
    try:
        up = s.wait_ready()
        check("json-enabled no, lenient consumer: the server starts", up, s.log_tail(5))
        if up:
            c = s.connect()
            check("  MODULE LIST has no json and JSON_GetValue is not exported", c.cmd("SEARCHMOD.STATUS")[:2] == [0, 0])
            check("  reading an attribute reports that JSON is not supported",
                  ErrPrefix("ERR The JSON module is not supported") == c.cmd("SEARCHMOD.GET", "k", "$"))  # fmt: skip
            check("  JSON.SET is an unknown command", ErrPrefix("ERR unknown command") == c.cmd("JSON.SET", "k", "$", "1"))
            check("  the server is still up", c.cmd("PING") == T.Simple(b"PONG"))
    finally:
        s.stop()

    # 4. json-enabled no with the external module: it can be loaded in its place, and the consumer finds it.
    s = T.Server("candidate", T.NEW_SERVER, rundir + "/s4",
                 ["--json-enabled", "no", "--loadmodule", T.REF_MODULE, "--loadmodule", MOD])  # fmt: skip
    try:
        up = s.wait_ready()
        check("json-enabled no with the external module loaded instead: the consumer finds that one", up, s.log_tail(5))
        if up:
            c = s.connect()
            st = c.cmd("SEARCHMOD.STATUS")
            check("  its shared API answers", st[:2] == [1, 1] and c.cmd("JSON.SET", "k", "$", '{"a":1}') == OK
                  and c.cmd("SEARCHMOD.GET", "k", "$.a") == [b"[1]", T.Simple(b"value"), b"1"], repr(st))  # fmt: skip
    finally:
        s.stop()
    return failures


def main():
    verbose = "-v" in sys.argv[1:]
    if not build():
        return 2
    os.makedirs(T.RUN_ROOT, mode=0o700, exist_ok=True)
    os.chmod(T.RUN_ROOT, 0o700)
    rundir = os.path.join(T.RUN_ROOT, "s%d" % os.getpid())
    os.makedirs(rundir, mode=0o700)
    servers, failures = [], 0
    try:
        ref = T.Server("reference", T.REF_SERVER, rundir + "/ref", ["--loadmodule", T.REF_MODULE, "--loadmodule", MOD])
        new = T.Server("candidate", T.NEW_SERVER, rundir + "/new", ["--loadmodule", MOD])
        servers = [ref, new]
        for s in servers:
            if not s.wait_ready():
                print("%s did not start with searchmod.so. Log:\n%s" % (s.name, s.log_tail()))
                return 1
        cr, cn = ref.connect(), new.connect()

        # Load-time behaviour.
        sr, sn = cr.cmd("SEARCHMOD.STATUS"), cn.cmd("SEARCHMOD.STATUS")
        ok = sr[:2] == [1, 1] and sn[:2] == [1, 1]
        print("%s load: both find the module \"json\" and JSON_GetValue (version: reference %s, candidate %s)"
              % ("ok  " if ok else "FAIL", sr[2], sn[2]))  # fmt: skip
        failures += 0 if ok else 1

        compared = events = 0

        def both(cmd):
            nonlocal compared, events, failures
            if cmd[0] == "<sleep>":
                time.sleep(0.3)
                return
            a, b = cr.cmd(*cmd), cn.cmd(*cmd)
            ea, eb = cr.cmd("SEARCHMOD.EVENTS"), cn.cmd("SEARCHMOD.EVENTS")
            compared += 1
            events += len(ea)
            ok = a == b and ea == eb
            failures += 0 if ok else 1
            if verbose or not ok:
                print("  %s %s" % ("ok  " if ok else "FAIL", T.show(cmd)[:110]))
                if a != b or verbose:
                    print("       reference: %r\n       candidate: %r" % (a, b))
                if ea != eb or verbose:
                    print("       events reference: %r\n       events candidate: %r" % (ea, eb))

        for cmd in (
            ("JSON.SET", "doc", "$", DOC),
            ("JSON.SET", "scalar", "$", '"just a string"'),
            ("JSON.SET", "arr", "$", '[1,"two",[3]]'),
            ("SET", "str", "plain"),
            ("HSET", "hash", "a", "1"),
        ):
            both(cmd)
        setup_events = events
        for cmd in READS:
            both(cmd)
        n_reads = len(READS)
        for cmd in WRITES:
            both(cmd)
        print("%d reads through JSON_GetValue, JSON.GET and the key type check, %d commands for keyspace events: "
              "%d commands compared, %d events compared, %d failed"
              % (n_reads, len([c for c in WRITES if c[0] != "<sleep>"]), compared, events, failures))  # fmt: skip
        del setup_events
        for s in servers:
            s.stop()
        servers = []
        failures += startup_checks(rundir)
    finally:
        for s in servers:
            s.stop()
        if not failures:
            shutil.rmtree(rundir, ignore_errors=True)
        else:
            print("run directory kept: %s" % rundir)
    print("failures: %d" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
