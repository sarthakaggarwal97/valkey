#!/usr/bin/env python3
"""Profile one command on a private server with perf (user-space samples only).

  prof.py --server BIN [--module SO] [--secs 8] [--top 45] [--callgraph] [--shape profile] CMD ARG...

In the arguments, {k} is a random key of the loaded shape, {doc} a document of
that shape, {big:NAME} the big document key (records100k, emb_full, ...)."""
import argparse, os, random, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--module")
    ap.add_argument("--secs", type=int, default=8)
    ap.add_argument("--top", type=int, default=45)
    ap.add_argument("--callgraph", action="store_true")
    ap.add_argument("--shape", default="profile")
    ap.add_argument("--keys", type=int, default=20000)
    ap.add_argument("--server-cpu", default="62,63")
    ap.add_argument("--client-cpu", type=int, default=60)
    ap.add_argument("cmd", nargs="+")
    a = ap.parse_args()
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {a.client_cpu})
    docs = bench.gen(a.shape, a.keys)
    srv = bench.Server(a.server, a.module, a.server_cpu)
    try:
        c = srv.c
        bench.load(c, "k", docs)
        for arg in a.cmd:
            if arg.startswith("{big:"):
                name = arg[5:-1]
                c.cmd("JSON.SET", "big:" + name, ".", bench.gen(name)[0])
        stop = threading.Event()

        def fill(i, r):
            out = []
            for arg in a.cmd:
                if arg == "{k}":
                    out.append("k:%d" % r.randrange(a.keys))
                elif arg == "{doc}":
                    out.append(docs[i % a.keys])
                elif arg.startswith("{big:"):
                    out.append("big:" + arg[5:-1])
                else:
                    out.append(arg)
            return out

        def drive():
            c2 = bench.Conn(srv.sock)
            r = random.Random(3)
            i = 0
            while not stop.is_set():
                chunk = []
                for _ in range(100):
                    chunk.append(bench.Conn.enc(*fill(i, r)))
                    i += 1
                c2.pipeline(b"".join(chunk), 100)

        t = threading.Thread(target=drive, daemon=True)
        t.start()
        time.sleep(1.0)
        c.cmd("CONFIG", "RESETSTAT")
        data = os.path.join(srv.dir, "perf.data")
        # With taskset in front, the server is the child of the Popen pid.
        pid = srv.p.pid
        kids = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout.split()
        if kids:
            pid = int(kids[0])
        rec = ["perf", "record", "-q", "-e", os.environ.get("PERF_EVENT", "cycles:u"), "-F", "2999", "-p", str(pid), "-o", data]
        if a.callgraph:
            rec += ["--call-graph", "fp"]
        subprocess.run(rec + ["--", "sleep", str(a.secs)], check=False)
        usec, calls, failed = bench.cmdstat(c, a.cmd[0])
        stop.set()
        t.join(timeout=5)
        print("command: %s   usec_per_call %s   calls %d   failed %d" % (" ".join(x if len(x) < 40 else x[:37] + "..." for x in a.cmd), usec, calls, failed))
        rep = ["perf", "report", "-i", data, "--stdio", "--percent-limit", "0.7"]
        rep += ["--children"] if a.callgraph else ["--no-children"]
        if a.callgraph:
            rep += ["-g", "none"]
        out = subprocess.run(rep, capture_output=True, text=True).stdout
        lines = [l for l in out.splitlines() if l.strip() and not l.startswith("#")]
        print("\n".join(l[:150] for l in lines[:a.top]))
    finally:
        srv.stop()


main()
