#!/usr/bin/env python3
"""Two extra measurements on one server: real-world JSON files, and many clients.

  realbench.py --server BIN [--module SO] --name NAME --out FILE.json --data DIR
               [--server-cpu 62,63] [--client-cpus 40-59] [--bench valkey-benchmark]

Part 1: every *.json file in DIR is stored as one key. Reports MEMORY USAGE and
JSON.DEBUG MEMORY of the key, and server-side time of JSON.SET and JSON.GET.
Part 2: valkey-benchmark with 50 connections, pipeline 16, 8 threads, over a
unix socket, 100,000 profile documents. Reports requests per second."""
import argparse, glob, json, os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--module")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--data", required=True)
    ap.add_argument("--server-cpu", default="62,63")
    ap.add_argument("--client-cpus", default="40-59")
    ap.add_argument("--bench", required=True)
    a = ap.parse_args()
    res = {"name": a.name, "files": [], "throughput": []}
    srv = bench.Server(a.server, a.module, a.server_cpu)
    c = srv.c
    try:
        # ---- real documents ------------------------------------------------
        for path in sorted(glob.glob(os.path.join(a.data, "*.json"))):
            doc = open(path, "rb").read()
            name = os.path.basename(path)
            c.cmd("FLUSHALL", "SYNC")
            c.cmd("JSON.SET", "doc", ".", doc)
            row = {"file": name, "text_bytes": len(doc), "memory_usage": int(c.cmd("MEMORY", "USAGE", "doc")),
                   "json_debug_memory": int(c.cmd("JSON.DEBUG", "MEMORY", "doc"))}
            iters = max(200, min(100000, 60000000 // max(1, len(doc))))
            for label, stat, cmd in (("set", "JSON.SET", ("JSON.SET", "tmp", ".", doc)),
                                     ("get", "JSON.GET", ("JSON.GET", "doc"))):
                enc = bench.Conn.enc(*cmd)
                for rounds in (max(1, iters // 20), iters):  # warm-up, then measured
                    c.cmd("CONFIG", "RESETSTAT")
                    done = 0
                    while done < rounds:
                        n = min(20 if len(doc) > 50000 else 100, rounds - done)
                        e, s = c.pipeline(enc * n, n)
                        assert e == 0, s
                        done += n
                row[label + "_usec"] = bench.cmdstat(c, stat)[0]
            # The reply must be the same document.
            row["roundtrip_ok"] = json.loads(c.cmd("JSON.GET", "doc")) == json.loads(doc)
            res["files"].append(row)
            print("%-24s text %8d  mem %8d  set %9.2f us  get %9.2f us  roundtrip %s" % (
                name, len(doc), row["memory_usage"], row["set_usec"], row["get_usec"], row["roundtrip_ok"]), flush=True)

        # ---- many clients ----------------------------------------------------
        c.cmd("FLUSHALL", "SYNC")
        docs = bench.gen("profile", 1000)
        doc = docs[7]
        base = [a.bench, "-s", srv.sock, "-c", "50", "-P", "16", "--threads", "8", "-r", "100000", "-q"]
        if shutil.which("taskset"):
            base = ["taskset", "-c", a.client_cpus] + base
        runs = [
            ("JSON.SET whole profile", 3000000, ["JSON.SET", "p:__rand_int__", ".", doc]),
            ("JSON.GET whole profile", 3000000, ["JSON.GET", "p:__rand_int__"]),
            ("JSON.GET $.address.city", 3000000, ["JSON.GET", "p:__rand_int__", "$.address.city"]),
            ("JSON.NUMINCRBY $.age 1", 3000000, ["JSON.NUMINCRBY", "p:__rand_int__", "$.age", "1"]),
            ("JSON.SET $.name", 3000000, ["JSON.SET", "p:__rand_int__", "$.name", '"Charlie Delta Echo"']),
            ("JSON.ARRAPPEND $.tags (1.5M) ", 1500000, ["JSON.ARRAPPEND", "p:__rand_int__", "$.tags", '"x"']),
        ]
        for label, n, cmd in runs:
            best = 0.0
            for attempt in range(2):
                if label.startswith("JSON.ARRAPPEND") and attempt:
                    break  # appends grow the documents: run once
                out = subprocess.run(base + ["-n", str(n)] + cmd, capture_output=True).stdout.decode(errors="replace")
                m = re.findall(r"([0-9.]+) requests per second", out)
                if m:
                    best = max(best, float(m[-1]))
            errs = int(bench.info(c, "stats").get("total_error_replies", 0))
            res["throughput"].append({"op": label.strip(), "rps": best, "total_error_replies": errs})
            print("%-32s %12.0f requests/s   (error replies so far: %d)" % (label, best, errs), flush=True)
        res["keys"] = int(c.cmd("DBSIZE"))
    finally:
        srv.stop()
    json.dump(res, open(a.out, "w"), indent=1)


main()
