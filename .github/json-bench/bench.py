#!/usr/bin/env python3
"""Benchmark one valkey-server with JSON (external module or built in).

Starts a private server (unix socket only, no TCP), loads the corpus, and
records for each operation the server-side time per call (INFO commandstats)
and latency percentiles (INFO latencystats), plus memory per key and save and
load times. No third-party packages.

  bench.py --server BIN [--module SO] --name NAME --out FILE.json
           [--server-cpu N] [--client-cpu N] [--scale 1.0] [--only REGEX]
"""
import argparse, json, os, random, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))


# ----------------------------------------------------------------------------
# Minimal RESP2 client
# ----------------------------------------------------------------------------
class Conn:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.buf = b""
        self.pos = 0

    @staticmethod
    def enc(*args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            if not isinstance(a, bytes):
                a = str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        return b"".join(out)

    def _fill(self):
        d = self.s.recv(1 << 20)
        if not d:
            raise ConnectionError("server closed the connection")
        if self.pos:
            self.buf = self.buf[self.pos:]
            self.pos = 0
        self.buf += d

    def _line(self):
        while True:
            i = self.buf.find(b"\r\n", self.pos)
            if i >= 0:
                line = self.buf[self.pos:i]
                self.pos = i + 2
                return line
            self._fill()

    def read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return RuntimeError(rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) - self.pos < n + 2:
                self._fill()
            v = self.buf[self.pos:self.pos + n]
            self.pos += n + 2
            return v
        if t == b"*":
            n = int(rest)
            return None if n < 0 else [self.read() for _ in range(n)]
        raise ValueError("bad reply: %r" % line)

    def cmd(self, *args):
        self.s.sendall(self.enc(*args))
        r = self.read()
        if isinstance(r, RuntimeError):
            raise r
        return r

    def pipeline(self, encoded, count):
        """Sends pre-encoded commands and reads `count` replies. Returns the
        number of error replies and one sample error."""
        self.s.sendall(encoded)
        errors, sample = 0, None
        for _ in range(count):
            r = self.read()
            if isinstance(r, RuntimeError):
                errors += 1
                sample = sample or str(r)
        return errors, sample


def info(c, section):
    out = {}
    for line in c.cmd("INFO", section).decode().splitlines():
        if ":" in line and not line.startswith("#"):
            k, v = line.split(":", 1)
            out[k] = v
    return out


def cmdstat(c, name):
    """usec_per_call and calls for one command, name matched without case."""
    for k, v in info(c, "commandstats").items():
        if k.lower() == "cmdstat_" + name.lower():
            f = dict(x.split("=") for x in v.split(","))
            return float(f["usec_per_call"]), int(f["calls"]), int(f.get("failed_calls", 0))
    return None, 0, 0


def latstat(c, name):
    for k, v in info(c, "latencystats").items():
        if k.lower() == "latency_percentiles_usec_" + name.lower():
            return {x.split("=")[0]: float(x.split("=")[1]) for x in v.split(",")}
    return {}


# ----------------------------------------------------------------------------
# Server
# ----------------------------------------------------------------------------
class Server:
    def __init__(self, binary, module, cpu):
        self.dir = tempfile.mkdtemp(prefix="jb.", dir=os.environ.get("BENCH_TMP", "/dev/shm" if os.path.isdir("/dev/shm") else None))
        os.chmod(self.dir, 0o700)
        binary = os.path.abspath(binary)
        module = os.path.abspath(module) if module else None
        args = [binary, "--port", "0", "--unixsocket", "./s.sock", "--unixsocketperm", "700", "--save", "",
                "--appendonly", "no", "--dir", ".", "--logfile", "./log", "--enable-debug-command", "yes",
                "--latency-tracking", "yes", "--latency-tracking-info-percentiles", "50 99 99.9"]
        if module:
            args += ["--loadmodule", module]
        if cpu is not None and shutil.which("taskset"):
            args = ["taskset", "-c", str(cpu)] + args
        self.p = subprocess.Popen(args, cwd=self.dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock = os.path.join(self.dir, "s.sock")
        for _ in range(200):
            if os.path.exists(self.sock):
                break
            if self.p.poll() is not None:
                raise RuntimeError("server exited: " + open(os.path.join(self.dir, "log")).read()[-2000:])
            time.sleep(0.05)
        self.c = Conn(self.sock)
        self.c.cmd("PING")

    def stop(self):
        try:
            self.c.cmd("SHUTDOWN", "NOSAVE")
        except Exception:
            pass
        try:
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


# ----------------------------------------------------------------------------
# Corpus
# ----------------------------------------------------------------------------
def gen(shape, n=1, seed=1):
    out = subprocess.run([sys.executable, os.path.join(HERE, "gen.py"), shape, str(n), str(seed)],
                         check=True, capture_output=True).stdout
    return [x for x in out.split(b"\n") if x]


def load(c, prefix, docs, copies=1):
    batch, n = [], 0
    for k in range(copies):
        for i, d in enumerate(docs):
            batch.append(Conn.enc("JSON.SET", "%s:%d" % (prefix, k * len(docs) + i), ".", d))
            if len(batch) == 500:
                e, s = c.pipeline(b"".join(batch), len(batch))
                assert e == 0, s
                batch = []
    if batch:
        e, s = c.pipeline(b"".join(batch), len(batch))
        assert e == 0, s


def used_memory(c):
    return int(info(c, "memory")["used_memory"])


# ----------------------------------------------------------------------------
# Operations. Each is (name, stat command, iterations, generator(i, rnd) -> list
# of commands). Only the command named in `stat` is reported.
# ----------------------------------------------------------------------------
def run_op(c, name, stat, iters, gen_cmds, pipeline_depth=100):
    rnd = random.Random(7)
    c.cmd("CONFIG", "RESETSTAT")
    sent, errors, sample = 0, 0, None
    t0 = time.perf_counter()
    i = 0
    while i < iters:
        chunk, count = [], 0
        while count < pipeline_depth and i < iters:
            for cmd in gen_cmds(i, rnd):
                chunk.append(Conn.enc(*cmd))
                count += 1
            i += 1
        e, s = c.pipeline(b"".join(chunk), count)
        errors += e
        sample = sample or s
        sent += count
    wall = time.perf_counter() - t0
    usec, calls, failed = cmdstat(c, stat)
    lat = latstat(c, stat)
    return {"op": name, "cmd": stat, "usec_per_call": usec, "calls": calls, "failed": failed, "errors": errors,
            "error_sample": sample, "p50": lat.get("p50"), "p99": lat.get("p99"), "p99.9": lat.get("p99.9"),
            "client_cmds_per_sec": round(sent / wall)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--module")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--server-cpu", help="CPU list for taskset, e.g. 62,63: one core for the main thread and one for background threads")
    ap.add_argument("--client-cpu", type=int)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--only")
    a = ap.parse_args()
    if a.client_cpu is not None and hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {a.client_cpu})

    K = max(1000, int(100000 * a.scale))  # keys per small shape
    N = max(2000, int(200000 * a.scale))  # iterations per small operation
    res = {"name": a.name, "server": a.server, "module": a.module, "keys_per_shape": K, "memory": [], "ops": [],
           "persist": {}}
    small = {s: gen(s, K) for s in ("session", "profile", "order")}
    big = {s: gen(s)[0] for s in ("records100k", "emb_full", "emb_short", "ints100k", "str10k", "obj100")}

    srv = Server(a.server, a.module, a.server_cpu)
    c = srv.c
    try:
        res["version"] = info(c, "server").get("valkey_version")
        res["mem_allocator"] = info(c, "memory").get("mem_allocator")

        # ---- memory -------------------------------------------------------
        for shape in ("session", "profile", "order"):
            c.cmd("FLUSHALL", "SYNC")
            m0 = used_memory(c)
            load(c, "m", small[shape])
            m1 = used_memory(c)
            sample = [int(c.cmd("JSON.DEBUG", "MEMORY", "m:%d" % i)) for i in range(0, K, max(1, K // 2000))]
            usage = [int(c.cmd("MEMORY", "USAGE", "m:%d" % i)) for i in range(0, K, max(1, K // 2000))]
            res["memory"].append({"shape": shape, "docs": K, "text_bytes_avg": sum(map(len, small[shape])) / K,
                                  "used_memory_per_key": (m1 - m0) / K,
                                  "json_debug_memory_avg": sum(sample) / len(sample),
                                  "memory_usage_avg": sum(usage) / len(usage)})
        for shape, doc in big.items():
            c.cmd("FLUSHALL", "SYNC")
            copies = 20 if len(doc) > 100000 else 2000
            m0 = used_memory(c)
            load(c, "m", [doc], copies)
            m1 = used_memory(c)
            res["memory"].append({"shape": shape, "docs": copies, "text_bytes_avg": len(doc),
                                  "used_memory_per_key": (m1 - m0) / copies,
                                  "json_debug_memory_avg": int(c.cmd("JSON.DEBUG", "MEMORY", "m:0")),
                                  "memory_usage_avg": int(c.cmd("MEMORY", "USAGE", "m:0"))})
        c.cmd("FLUSHALL", "SYNC")

        # ---- data for the operations ---------------------------------------
        load(c, "s", small["session"])
        load(c, "p", small["profile"])
        load(c, "o", small["order"])
        for shape, doc in big.items():
            c.cmd("JSON.SET", "big:" + shape, ".", doc)
        # The same profile data as plain strings and as hashes of the top-level
        # scalar fields, to put the JSON figures next to core commands.
        batch = []
        for i, d in enumerate(small["profile"]):
            o = json.loads(d)
            batch.append(Conn.enc("SET", "str:%d" % i, d))
            batch.append(Conn.enc("HSET", "h:%d" % i, "id", o["id"], "name", o["name"], "email", o["email"],
                                  "age", o["age"], "score", o["score"], "active", int(o["active"]), "bio", o["bio"],
                                  "city", o["address"]["city"], "zip", o["address"]["zip"]))
            if len(batch) >= 400 or i == K - 1:
                e, sm = c.pipeline(b"".join(batch), len(batch))
                assert e == 0, sm
                batch = []
        res["dataset_used_memory"] = used_memory(c)
        names = [b'"Al"', b'"Charlie Delta Echo"', b'"Bo Li"', b'"Maximilian Alexander Rhodes"', b'"Sam"']
        rec = b'{"id":1,"name":"item-x","price":9.5,"qty":2,"tags":["a","b"]}'
        P = small["profile"]
        S = small["session"]
        O = small["order"]

        def key(pfx, rnd):
            return "%s:%d" % (pfx, rnd.randrange(K))

        ops = [
            # whole documents
            ("set session (replace)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("s", r), ".", S[i % K])]),
            ("set profile (replace)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("p", r), ".", P[i % K])]),
            ("set order (replace)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("o", r), ".", O[i % K])]),
            ("get session", "JSON.GET", N, lambda i, r: [("JSON.GET", key("s", r))]),
            ("get profile", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r))]),
            ("get order", "JSON.GET", N, lambda i, r: [("JSON.GET", key("o", r))]),
            ("get profile, formatted", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r), "INDENT", "  ", "NEWLINE", "\n", "SPACE", " ")]),
            ("del key + set profile", "JSON.DEL", N // 2, lambda i, r: [("JSON.DEL", "p:%d" % (i % K)), ("JSON.SET", "p:%d" % (i % K), ".", P[i % K])]),
            # single-path reads
            ("get .address.city (legacy)", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r), ".address.city")]),
            ("get $.address.city", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r), "$.address.city")]),
            ("get $.prefs.notify.push", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r), "$.prefs.notify.push")]),
            ("get $.bio (last member)", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r), "$.bio")]),
            ("get 3 paths", "JSON.GET", N, lambda i, r: [("JSON.GET", key("p", r), "$.name", "$.age", "$.address.geo.lat")]),
            ("get $.items[1].price", "JSON.GET", N, lambda i, r: [("JSON.GET", key("o", r), "$.items[1].price")]),
            ("type $.age", "JSON.TYPE", N, lambda i, r: [("JSON.TYPE", key("p", r), "$.age")]),
            ("strlen $.bio", "JSON.STRLEN", N, lambda i, r: [("JSON.STRLEN", key("p", r), "$.bio")]),
            ("objlen $.address", "JSON.OBJLEN", N, lambda i, r: [("JSON.OBJLEN", key("p", r), "$.address")]),
            ("objkeys $.prefs", "JSON.OBJKEYS", N, lambda i, r: [("JSON.OBJKEYS", key("p", r), "$.prefs")]),
            ("arrlen $.tags", "JSON.ARRLEN", N, lambda i, r: [("JSON.ARRLEN", key("p", r), "$.tags")]),
            ("arrindex $.tags", "JSON.ARRINDEX", N, lambda i, r: [("JSON.ARRINDEX", key("p", r), "$.tags", '"kilo"')]),
            ("resp $.address", "JSON.RESP", N, lambda i, r: [("JSON.RESP", key("p", r), "$.address")]),
            ("mget 10 keys $.name", "JSON.MGET", N // 5, lambda i, r: [("JSON.MGET",) + tuple(key("p", r) for _ in range(10)) + ("$.name",)]),
            # multi-match reads
            ("get $.items[*].price", "JSON.GET", N, lambda i, r: [("JSON.GET", key("o", r), "$.items[*].price")]),
            ("get $..price", "JSON.GET", N, lambda i, r: [("JSON.GET", key("o", r), "$..price")]),
            ("get $.items[?(@.qty>4)].sku", "JSON.GET", N, lambda i, r: [("JSON.GET", key("o", r), "$.items[?(@.qty>4)].sku")]),
            # single-path writes
            ("numincrby $.age 1", "JSON.NUMINCRBY", N, lambda i, r: [("JSON.NUMINCRBY", key("p", r), "$.age", 1), ("JSON.NUMINCRBY", key("p", r), "$.age", -1)]),
            ("numincrby $.score 0.25", "JSON.NUMINCRBY", N, lambda i, r: [("JSON.NUMINCRBY", key("p", r), "$.score", "0.25"), ("JSON.NUMINCRBY", key("p", r), "$.score", "-0.25")]),
            ("nummultby $.age 1", "JSON.NUMMULTBY", N, lambda i, r: [("JSON.NUMMULTBY", key("p", r), "$.age", 1)]),
            ("set $.age (same width)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("p", r), "$.age", 18 + i % 70)]),
            ("set $.name (length changes)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("p", r), "$.name", names[i % 5])]),
            ("set $.address.city (nested)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("p", r), "$.address.city", names[i % 5])]),
            ("set $.address (object)", "JSON.SET", N, lambda i, r: [("JSON.SET", key("p", r), "$.address", b'{"street":"1 Main St","city":"Kilo","zip":"00001"}')]),
            ("toggle $.active", "JSON.TOGGLE", N, lambda i, r: [("JSON.TOGGLE", key("p", r), "$.active")]),
            ("strappend $.name", "JSON.STRAPPEND", N // 2, lambda i, r: [("JSON.STRAPPEND", "p:%d" % (i % K), "$.name", '"xy"'), ("JSON.SET", "p:%d" % (i % K), "$.name", names[i % 5])]),
            ("add member $.extra", "JSON.SET", N // 2, lambda i, r: [("JSON.SET", "p:%d" % (i % K), "$.extra", '"value"'), ("JSON.DEL", "p:%d" % (i % K), "$.extra")]),
            ("del member $.extra", "JSON.DEL", N // 2, lambda i, r: [("JSON.SET", "p:%d" % (i % K), "$.extra", '"value"'), ("JSON.DEL", "p:%d" % (i % K), "$.extra")]),
            ("arrappend $.tags", "JSON.ARRAPPEND", N // 2, lambda i, r: [("JSON.ARRAPPEND", "p:%d" % (i % K), "$.tags", '"zulu2"'), ("JSON.ARRPOP", "p:%d" % (i % K), "$.tags")]),
            ("arrpop $.tags", "JSON.ARRPOP", N // 2, lambda i, r: [("JSON.ARRAPPEND", "p:%d" % (i % K), "$.tags", '"zulu2"'), ("JSON.ARRPOP", "p:%d" % (i % K), "$.tags")]),
            ("arrinsert $.tags 0", "JSON.ARRINSERT", N // 2, lambda i, r: [("JSON.ARRINSERT", "p:%d" % (i % K), "$.tags", 0, '"first"'), ("JSON.ARRPOP", "p:%d" % (i % K), "$.tags", 0)]),
            ("arrtrim $.tags", "JSON.ARRTRIM", N // 2, lambda i, r: [("JSON.ARRAPPEND", "p:%d" % (i % K), "$.tags", '"a"', '"b"'), ("JSON.ARRTRIM", "p:%d" % (i % K), "$.tags", 0, -3)]),
            ("merge $.prefs", "JSON.MERGE", N, lambda i, r: [("JSON.MERGE", key("p", r), "$.prefs", b'{"theme":"dark","lang":"en"}')]),
            ("mset 5 keys $.age", "JSON.MSET", N // 5, lambda i, r: [("JSON.MSET",) + sum(((key("p", r), "$.age", 30) for _ in range(5)), ())]),
            # multi-match writes
            ("numincrby $.items[*].qty", "JSON.NUMINCRBY", N, lambda i, r: [("JSON.NUMINCRBY", key("o", r), "$.items[*].qty", 1), ("JSON.NUMINCRBY", key("o", r), "$.items[*].qty", -1)]),
            ("set $.items[*].price", "JSON.SET", N, lambda i, r: [("JSON.SET", key("o", r), "$.items[*].price", "19.99")]),
            # one object with 100 members
            ("obj100: get $.field_050", "JSON.GET", N, lambda i, r: [("JSON.GET", "big:obj100", "$.field_050")]),
            ("obj100: get $.field_099", "JSON.GET", N, lambda i, r: [("JSON.GET", "big:obj100", "$.field_099")]),
            ("obj100: numincrby $.field_099", "JSON.NUMINCRBY", N, lambda i, r: [("JSON.NUMINCRBY", "big:obj100", "$.field_099", 1)]),
            # 100,000 records in one array (8 MB of text)
            ("records: set whole", "JSON.SET", 20, lambda i, r: [("JSON.SET", "big:tmp", ".", big["records100k"])]),
            ("records: get whole", "JSON.GET", 20, lambda i, r: [("JSON.GET", "big:records100k")]),
            ("records: get $[50000].name", "JSON.GET", N, lambda i, r: [("JSON.GET", "big:records100k", "$[%d].name" % r.randrange(100000))]),
            ("records: numincrby $[n].qty", "JSON.NUMINCRBY", N, lambda i, r: [("JSON.NUMINCRBY", "big:records100k", "$[%d].qty" % r.randrange(100000), 1)]),
            ("records: set $[n].name", "JSON.SET", N // 4, lambda i, r: [("JSON.SET", "big:records100k", "$[%d].name" % r.randrange(100000), names[i % 5])]),
            ("records: arrappend + arrpop", "JSON.ARRAPPEND", N // 4, lambda i, r: [("JSON.ARRAPPEND", "big:records100k", "$", rec), ("JSON.ARRPOP", "big:records100k", "$")]),
            ("records: arrlen $", "JSON.ARRLEN", N, lambda i, r: [("JSON.ARRLEN", "big:records100k", "$")]),
            ("records: get $[*].id", "JSON.GET", 20, lambda i, r: [("JSON.GET", "big:records100k", "$[*].id")]),
            ("records: get $..price", "JSON.GET", 20, lambda i, r: [("JSON.GET", "big:records100k", "$..price")]),
            ("records: filter price>499", "JSON.GET", 20, lambda i, r: [("JSON.GET", "big:records100k", "$[?(@.price>499)].id")]),
            ("records: del key (free)", "JSON.DEL", 20, lambda i, r: [("JSON.SET", "big:tmp", ".", big["records100k"]), ("JSON.DEL", "big:tmp")]),
            # arrays of numbers and strings
            ("emb_full: set", "JSON.SET", 2000, lambda i, r: [("JSON.SET", "big:tmp2", ".", big["emb_full"])]),
            ("emb_full: get", "JSON.GET", 2000, lambda i, r: [("JSON.GET", "big:emb_full")]),
            ("emb_full: get $[768]", "JSON.GET", N, lambda i, r: [("JSON.GET", "big:emb_full", "$[768]")]),
            ("emb_short: set", "JSON.SET", 2000, lambda i, r: [("JSON.SET", "big:tmp3", ".", big["emb_short"])]),
            ("emb_short: get", "JSON.GET", 2000, lambda i, r: [("JSON.GET", "big:emb_short")]),
            ("ints100k: set", "JSON.SET", 50, lambda i, r: [("JSON.SET", "big:tmp4", ".", big["ints100k"])]),
            ("ints100k: get", "JSON.GET", 50, lambda i, r: [("JSON.GET", "big:ints100k")]),
            ("ints100k: get $[n]", "JSON.GET", N, lambda i, r: [("JSON.GET", "big:ints100k", "$[%d]" % r.randrange(100000))]),
            ("ints100k: numincrby $[n]", "JSON.NUMINCRBY", N, lambda i, r: [("JSON.NUMINCRBY", "big:ints100k", "$[%d]" % r.randrange(100000), 1)]),
            ("str10k: set", "JSON.SET", 500, lambda i, r: [("JSON.SET", "big:tmp5", ".", big["str10k"])]),
            ("str10k: get", "JSON.GET", 500, lambda i, r: [("JSON.GET", "big:str10k")]),
        ]
        ops += [
            ("core: GET string (596 B)", "get", N, lambda i, r: [("GET", key("str", r))]),
            ("core: SET string (596 B)", "set", N, lambda i, r: [("SET", key("str", r), P[i % K])]),
            ("core: HGET name", "hget", N, lambda i, r: [("HGET", key("h", r), "name")]),
            ("core: HSET name", "hset", N, lambda i, r: [("HSET", key("h", r), "name", names[i % 5])]),
            ("core: HINCRBY age", "hincrby", N, lambda i, r: [("HINCRBY", key("h", r), "age", 1)]),
            ("core: HGETALL (9 fields)", "hgetall", N, lambda i, r: [("HGETALL", key("h", r))]),
            ("core: STRLEN", "strlen", N, lambda i, r: [("STRLEN", key("str", r))]),
        ]
        for name, stat, iters, g in ops:
            if a.only and not re.search(a.only, name):
                continue
            # Warm up, then measure.
            run_op(c, name, stat, max(10, iters // 20), g)
            r = run_op(c, name, stat, iters, g)
            res["ops"].append(r)
            print("%-34s %-14s %9s us/call  p99 %7s  calls %8d  errors %d %s" % (
                name, stat, r["usec_per_call"], r["p99"], r["calls"], r["errors"], r["error_sample"] or ""), flush=True)

        # ---- persistence -----------------------------------------------------
        if not a.only:
            t = time.perf_counter(); c.cmd("SAVE"); res["persist"]["save_sec"] = time.perf_counter() - t
            res["persist"]["rdb_bytes"] = os.path.getsize(os.path.join(srv.dir, "dump.rdb"))
            t = time.perf_counter(); c.cmd("DEBUG", "RELOAD", "NOSAVE"); res["persist"]["load_sec"] = time.perf_counter() - t
            res["persist"]["keys"] = int(c.cmd("DBSIZE"))
            t = time.perf_counter(); c.cmd("FLUSHALL", "SYNC"); res["persist"]["flushall_sync_sec"] = time.perf_counter() - t
            print("persist", res["persist"], flush=True)
    finally:
        srv.stop()
    json.dump(res, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
