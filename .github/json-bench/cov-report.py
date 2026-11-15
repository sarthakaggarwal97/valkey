#!/usr/bin/env python3
"""Line and function coverage of src/modules/json/json_*.c from lcov .info files.

    cov-report.py LABEL=FILE.info [LABEL=FILE.info ...]

Records of the same source file (one per test binary) are merged. Prints a
Markdown table with one column pair per input, then the functions that no
input covered.
"""
import os
import re
import sys

WANT = re.compile(r"/src/modules/json/(json_[a-z_]+\.c)$")


def load(path):
    lines, funcs = {}, {}  # file -> {line: hits}, file -> {name: hits}
    src = None
    with open(path) as f:
        for raw in f:
            r = raw.strip()
            if r.startswith("SF:"):
                m = WANT.search(os.path.normpath(r[3:]))
                src = m.group(1) if m else None
                if src:
                    lines.setdefault(src, {})
                    funcs.setdefault(src, {})
            elif src is None:
                continue
            elif r.startswith("DA:"):
                ln, hits = r[3:].split(",")[:2]
                d = lines[src]
                d[int(ln)] = d.get(int(ln), 0) + int(hits)
            elif r.startswith("FN:"):
                name = r[3:].split(",", 1)[1]
                funcs[src].setdefault(name, 0)
            elif r.startswith("FNDA:"):
                hits, name = r[5:].split(",", 1)
                funcs[src][name] = funcs[src].get(name, 0) + int(hits)
    return lines, funcs


def pct(a, b):
    return f"{100.0 * a / b:5.1f}% ({a}/{b})" if b else "-"


def main():
    sets = []
    for arg in sys.argv[1:]:
        label, path = arg.split("=", 1)
        sets.append((label, *load(path)))
    files = sorted({f for _, lines, _ in sets for f in lines})
    head = "| file |" + "".join(f" {l} lines | {l} functions |" for l, _, _ in sets)
    print(head)
    print("|---|" + "---|---|" * len(sets))
    tot = [[0, 0, 0, 0] for _ in sets]
    for f in files:
        row = f"| {f} |"
        for i, (_, lines, funcs) in enumerate(sets):
            d, fn = lines.get(f, {}), funcs.get(f, {})
            lh, lf = sum(1 for h in d.values() if h), len(d)
            fh, ff = sum(1 for h in fn.values() if h), len(fn)
            for j, v in enumerate((lh, lf, fh, ff)):
                tot[i][j] += v
            row += f" {pct(lh, lf)} | {pct(fh, ff)} |"
        print(row)
    print("| **total** |" + "".join(f" {pct(t[0], t[1])} | {pct(t[2], t[3])} |" for t in tot))
    print()
    _, _, last = sets[-1]
    print(f"Functions with zero coverage in '{sets[-1][0]}':")
    print()
    for f in files:
        zero = sorted(n for n, h in last.get(f, {}).items() if h == 0)
        for n in zero:
            print(f"- {f}: `{n}`")
    print()


if __name__ == "__main__":
    main()
