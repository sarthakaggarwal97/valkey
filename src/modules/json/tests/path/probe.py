#!/usr/bin/env python3
"""Ad-hoc probe of the reference server: probe.py DOC PATH [PATH...]
Prints the JSON.GET reply or error for each path. Socket: run/ref/x.sock."""
import sys, os, valkey
r = valkey.Valkey(unix_socket_path=os.path.join(os.path.dirname(os.path.abspath(__file__)), "run/ref/x.sock"))
doc = sys.argv[1]
for p in sys.argv[2:]:
    r.execute_command("JSON.SET", "probe", ".", doc)
    try:
        print(repr(p), "=>", r.execute_command("JSON.GET", "probe", p))
    except Exception as e:
        print(repr(p), "=> ERR", e)
