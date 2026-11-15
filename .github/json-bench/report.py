#!/usr/bin/env python3
"""Compare benchmark results: report.py BASE.json[,BASE2.json...] NEW.json[,NEW2.json...]

Several files per side are repeat runs: the median is used for each figure.
Prints Markdown."""
import json, statistics, sys


def load(arg):
    return [json.load(open(p)) for p in arg.split(",")]


def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def fmt(v, digits=2):
    if v is None:
        return "n/a"
    if v >= 1000:
        return "{:,.0f}".format(v)
    return ("%." + str(digits) + "f") % v


def main():
    base, new = load(sys.argv[1]), load(sys.argv[2])
    bname, nname = base[0]["name"], new[0]["name"]
    print("## Memory\n")
    print("Bytes per key. `used_memory` is the growth of the server's `used_memory` divided by the number of keys, "
          "so it includes the key name and the keyspace entry. "
          "For the single large documents it is `MEMORY USAGE` of the key.\n")
    print("| Shape | JSON text | %s `used_memory` | %s `used_memory` | Ratio | %s `JSON.DEBUG MEMORY` | %s `JSON.DEBUG MEMORY` | Ratio |"
          % (bname, nname, bname, nname))
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for i, m in enumerate(base[0]["memory"]):
        # For the single big documents the used_memory delta is disturbed by client
        # buffers shrinking, so MEMORY USAGE of the key is reported instead.
        field = "used_memory_per_key" if m["docs"] >= 1000 and m["text_bytes_avg"] < 4096 else "memory_usage_avg"
        bu = med([r["memory"][i][field] for r in base])
        nu = med([r["memory"][i][field] for r in new])
        bd = med([r["memory"][i]["json_debug_memory_avg"] for r in base])
        nd = med([r["memory"][i]["json_debug_memory_avg"] for r in new])
        print("| %s | %s | %s | %s | %.0f%% | %s | %s | %.0f%% |" % (
            m["shape"], fmt(m["text_bytes_avg"], 0), fmt(bu, 0), fmt(nu, 0), 100 * nu / bu, fmt(bd, 0), fmt(nd, 0),
            100 * nd / bd))

    print("\n## Time per command\n")
    print("Server-side microseconds per call (`INFO commandstats`), median of %d and %d runs.\n" % (len(base), len(new)))
    print("| Operation | Command | %s µs | %s µs | Speed-up | %s p99 µs | %s p99 µs | Errors |" % (bname, nname, bname, nname))
    print("|---|---|---:|---:|---:|---:|---:|---|")
    nops = {o["op"]: i for i, o in enumerate(new[0]["ops"])}
    ratios = []
    for i, o in enumerate(base[0]["ops"]):
        if o["op"] not in nops:
            continue
        j = nops[o["op"]]
        b = med([r["ops"][i]["usec_per_call"] for r in base])
        n = med([r["ops"][j]["usec_per_call"] for r in new])
        bp = med([r["ops"][i]["p99"] for r in base])
        np_ = med([r["ops"][j]["p99"] for r in new])
        errs = sum(r["ops"][i]["errors"] for r in base), sum(r["ops"][j]["errors"] for r in new)
        ratio = b / n if b and n else None
        if ratio and not o["op"].startswith("core:"):
            ratios.append(ratio)
        print("| %s | `%s` | %s | %s | %s | %s | %s | %s |" % (
            o["op"], o["cmd"], fmt(b), fmt(n), ("%.2f×" % ratio) if ratio else "n/a", fmt(bp, 1), fmt(np_, 1),
            "" if errs == (0, 0) else "%d / %d" % errs))
    if ratios:
        capped = [min(r, 10.0) for r in ratios]
        print("\nOver the %d JSON operations (the `core:` rows are the same server code on both sides and are "
              "left out): median speed-up %.2f×, geometric mean %.2f× with any single ratio capped at 10×, "
              "range %.2f× to %.2f×. Slower than %s on %d." % (
                  len(ratios), statistics.median(ratios), statistics.geometric_mean(capped), min(ratios),
                  max(ratios), bname, sum(1 for r in ratios if r < 1.0)))

    print("\n## Save, load, free\n")
    print("| | %s | %s |" % (bname, nname))
    print("|---|---:|---:|")
    for k, label in (("save_sec", "`SAVE`, seconds"), ("load_sec", "Load (`DEBUG RELOAD NOSAVE`), seconds"),
                     ("flushall_sync_sec", "`FLUSHALL SYNC`, seconds"), ("rdb_bytes", "RDB file, bytes"),
                     ("keys", "Keys")):
        b = med([r["persist"].get(k) for r in base])
        n = med([r["persist"].get(k) for r in new])
        print("| %s | %s | %s |" % (label, fmt(b, 3), fmt(n, 3)))


main()
