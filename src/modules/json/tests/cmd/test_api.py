#!/usr/bin/env python3
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
"""Test of the C functions the JSON module exports to other modules (json_api.h).

The test module apimod/apimod.so declares the five functions as undefined
symbols and exposes commands that call them. It is loaded into

  reference   valkey-server-ref --loadmodule libjson --loadmodule apimod.so
  candidate   src/valkey-server --loadmodule apimod.so   (JSON is built in)

and every command is sent to both. The replies must be identical. A few
replies are also asserted outright, so the documented behaviour is checked
even where the two servers agree by accident. If the reference cannot load
the test module, only those assertions run, on the candidate.

Both servers listen on a unix socket only, in a mode-700 directory under
tests/cmd/run/. Usage: test_api.py [-v]
"""

import os
import shutil
import subprocess
import sys

import test_cmd as T
from test_cmd import NIL, OK

HERE = os.path.dirname(os.path.abspath(__file__))
APIMOD_DIR = os.path.join(HERE, "apimod")
APIMOD = os.path.join(APIMOD_DIR, "apimod.so")

DOC = (
    '{"s":"str","i":5,"d":19.90,"t":true,"z":null,"a":[1,"two",[3],{"four":4}],"e":[],"o":{"x":{"y":[]},"s":"deep"},'
    '"u":"\\u00e9\\u0000\\n","big":123456789012345678901234567890}'
)

DOC_TEXT = DOC.replace("\\u00e9", "\u00e9").encode("utf-8")  # What JSON.GET replies: only control characters stay escaped.

# (command, expected candidate reply or None to only compare with the reference)
STEPS = [
    (("JSON.SET", "k", ".", DOC), OK),
    (("JSON.SET", "scalar", ".", "42"), OK),
    (("SET", "str", "plain"), OK),
    (("LPUSH", "list", "x"), 1),
    # is_json_key and is_json_key2
    (("APIMOD.ISKEY", "k"), [1, 1]),
    (("APIMOD.ISKEY", "scalar"), [1, 1]),
    (("APIMOD.ISKEY", "str"), [0, 0]),
    (("APIMOD.ISKEY", "list"), [0, 0]),
    (("APIMOD.ISKEY", "nokey"), [0, 0]),
    # get_json_value_type: the first value's type, -1 for any failure
    (("APIMOD.TYPE", "k", "."), [0, b"object", 6]),
    (("APIMOD.TYPE", "k", "$"), [0, b"object", 6]),
    (("APIMOD.TYPE", "k", ".s"), [0, b"string", 6]),
    (("APIMOD.TYPE", "k", "$.i"), [0, b"integer", 7]),
    (("APIMOD.TYPE", "k", ".d"), [0, b"number", 6]),
    (("APIMOD.TYPE", "k", ".big"), [0, b"number", 6]),
    (("APIMOD.TYPE", "k", "$.t"), [0, b"boolean", 7]),
    (("APIMOD.TYPE", "k", ".z"), [0, b"null", 4]),
    (("APIMOD.TYPE", "k", ".a"), [0, b"array", 5]),
    (("APIMOD.TYPE", "k", "$.a[*]"), [0, b"integer", 7]),  # several values: the first
    (("APIMOD.TYPE", "k", ".a[*]"), [0, b"integer", 7]),
    (("APIMOD.TYPE", "k", "$..s"), [0, b"string", 6]),
    (("APIMOD.TYPE", "k", "$.a[-1,1]"), [0, b"object", 6]),
    (("APIMOD.TYPE", "k", ".nothing"), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", "$.nothing"), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", "$.e[*]"), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", ".a[9]"), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", "$.a[9]"), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", "$["), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", ".["), [-1, NIL, 0]),
    (("APIMOD.TYPE", "k", ""), None),
    (("APIMOD.TYPE", "scalar", "."), [0, b"integer", 7]),
    (("APIMOD.TYPE", "nokey", "."), [-1, NIL, 0]),
    (("APIMOD.TYPE", "str", "."), [-1, NIL, 0]),
    (("APIMOD.TYPE", "list", "$"), [-1, NIL, 0]),
    # get_json_value: what JSON.GET replies for the path
    (("APIMOD.VALUE", "k", "."), [0, DOC_TEXT, len(DOC_TEXT)]),
    (("APIMOD.VALUE", "k", "$"), [0, b"[" + DOC_TEXT + b"]", len(DOC_TEXT) + 2]),
    (("APIMOD.VALUE", "k", ".s"), [0, b'"str"', 5]),
    (("APIMOD.VALUE", "k", "$.s"), [0, b'["str"]', 7]),
    (("APIMOD.VALUE", "k", ".d"), [0, b"19.90", 5]),
    (("APIMOD.VALUE", "k", ".u"), [0, b'"\xc3\xa9\\u0000\\n"', 12]),
    (("APIMOD.VALUE", "k", ".a"), [0, b'[1,"two",[3],{"four":4}]', 24]),
    (("APIMOD.VALUE", "k", ".a[*]"), [0, b"1", 1]),  # legacy, several values: the first
    (("APIMOD.VALUE", "k", "$.a[*]"), [0, b'[1,"two",[3],{"four":4}]', 24]),  # JSONPath: all of them
    (("APIMOD.VALUE", "k", "$..s"), [0, b'["str","deep"]', 14]),
    (("APIMOD.VALUE", "k", "$.nothing"), [0, b"[]", 2]),  # JSONPath, no match: success with []
    (("APIMOD.VALUE", "k", "$.e[*]"), [0, b"[]", 2]),
    (("APIMOD.VALUE", "k", "$.a[9]"), [0, b"[]", 2]),
    (("APIMOD.VALUE", "k", ".nothing"), [-1, NIL, 0]),  # legacy, no match: failure
    (("APIMOD.VALUE", "k", ".a[9]"), [-1, NIL, 0]),
    (("APIMOD.VALUE", "k", ".e[*]"), [-1, NIL, 0]),
    (("APIMOD.VALUE", "k", "$["), [-1, NIL, 0]),
    (("APIMOD.VALUE", "k", ".["), [-1, NIL, 0]),
    (("APIMOD.VALUE", "k", "$.a[x]"), [-1, NIL, 0]),
    (("APIMOD.VALUE", "k", ""), None),
    (("APIMOD.VALUE", "scalar", "$"), [0, b"[42]", 4]),
    (("APIMOD.VALUE", "nokey", "."), [-1, NIL, 0]),
    (("APIMOD.VALUE", "str", "$"), [-1, NIL, 0]),
    # get_json_values_and_types: three paths at once, failures in place
    (
        ("APIMOD.MULTI", "k", ".s", "$.a[*]", ".i"),
        [0, [b'"str"', b'[1,"two",[3],{"four":4}]', b"5"], [5, 24, 1], [b"string", b"integer", b"integer"], [6, 7, 7]],
    ),
    (
        ("APIMOD.MULTI", "k", ".nothing", "$.nothing", "$["),
        [0, [NIL, b"[]", NIL], [0, 2, 0], [NIL, NIL, NIL], [0, 0, 0]],
    ),
    (
        ("APIMOD.MULTI", "k", "$.o.x", ".a[9]", ".a[*]", "$..s", ".z"),
        [
            0,
            [b'[{"y":[]}]', NIL, b"1", b'["str","deep"]', b"null"],
            [10, 0, 1, 14, 4],
            [b"object", NIL, b"integer", b"string", b"null"],
            [6, 0, 7, 6, 4],
        ],
    ),
    (("APIMOD.MULTI", "k", "."), [0, [DOC_TEXT], [len(DOC_TEXT)], [b"object"], [6]]),
    (("APIMOD.MULTI", "nokey", ".a", ".b", ".c"), [-1, NIL, NIL, NIL, NIL]),
    (("APIMOD.MULTI", "str", "$", ".", "$.a"), [-1, NIL, NIL, NIL, NIL]),
    (("APIMOD.VALUES", "k", ".s", "$.i", ".nothing"), [0, [b'"str"', b"[5]", NIL], [5, 3, 0]]),
    (("APIMOD.VALUES", "list", ".s", "$.i", ".nothing"), [-1, NIL, NIL]),
    # The functions read: the document and the server are as before.
    (("JSON.GET", "k"), DOC_TEXT),
    (("PING",), T.Simple(b"PONG")),
]


def main():
    verbose = "-v" in sys.argv[1:]
    r = subprocess.run(["make", "-C", APIMOD_DIR], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(APIMOD):
        print("cannot build apimod.so:\n%s%s" % (r.stdout, r.stderr))
        return 2
    os.makedirs(T.RUN_ROOT, mode=0o700, exist_ok=True)
    os.chmod(T.RUN_ROOT, 0o700)
    rundir = os.path.join(T.RUN_ROOT, "a%d" % os.getpid())
    os.makedirs(rundir, mode=0o700)
    servers, failures = [], 0
    try:
        new = T.Server("candidate", T.NEW_SERVER, os.path.join(rundir, "new"), ["--loadmodule", APIMOD])
        servers.append(new)
        if not new.wait_ready():
            print("the candidate did not start with apimod.so loaded. Log:\n%s" % new.log_tail())
            return 1
        # The server opens a module with RTLD_LOCAL, which on macOS keeps the JSON
        # module's symbols out of the flat namespace the test module is resolved
        # in. Inserting the JSON library into the reference process first makes
        # them visible. --loadmodule then finds the same image already loaded.
        env = {"DYLD_INSERT_LIBRARIES": T.REF_MODULE} if sys.platform == "darwin" else None
        ref = T.Server("reference", T.REF_SERVER, os.path.join(rundir, "ref"),
                       ["--loadmodule", T.REF_MODULE, "--loadmodule", APIMOD], env=env)  # fmt: skip
        servers.append(ref)
        have_ref = ref.wait_ready()
        if not have_ref:
            print("NOTE: the reference could not load apimod.so next to the JSON module, so the replies are")
            print("      only checked against the expected values, on the candidate. Its log ends:")
            print("      " + ref.log_tail(4).replace("\n", "\n      "))
        cn = new.connect()
        cr = ref.connect() if have_ref else None
        compared = asserted = 0
        for cmd, expected in STEPS:
            got = cn.cmd(*cmd)
            want = cr.cmd(*cmd) if cr else None
            ok = True
            if expected is not None:
                asserted += 1
                ok = expected == got
            if cr is not None:
                compared += 1
                ok = ok and got == want
            if verbose or not ok:
                print("  %s %s" % ("ok  " if ok else "FAIL", T.show(cmd)[:100]))
                if not ok or verbose:
                    print("       candidate: %r" % (got,))
                    if cr is not None:
                        print("       reference: %r" % (want,))
                    if expected is not None:
                        print("       expected : %r" % (expected,))
            failures += 0 if ok else 1
        print("%d commands: %d compared with the reference, %d asserted, %d failed"
              % (len(STEPS), compared, asserted, failures))  # fmt: skip
    finally:
        for s in servers:
            s.stop()
        if not failures:
            shutil.rmtree(rundir, ignore_errors=True)
        else:
            print("run directory kept: %s" % rundir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
