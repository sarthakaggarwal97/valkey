#!/usr/bin/env python3
"""Model-based test of the engine's change functions and of defrag.

usage: check_mutate.py <build-dir> <full|quick>

Generates scripts: a document and a random sequence of changes that are valid
for it (jsonSet, jsonSetInt, jsonSetDouble, jsonSetNumText, jsonSetBool,
jsonDelete, jsonObjectAdd, jsonArrayInsert, jsonArrayTrim, jsonStringAppend),
mixed with defrag runs (move everything, move a random half, stop every k
steps and resume after later changes) and with copies. The driver applies
them under several settings of the packed caps, including 0 (everything is a
spine node) and 65535, and prints the document after every step. This script
makes the same changes to a Python value and compares, after every step:

  - the serialised text, byte for byte;
  - the dump of every read function (as in check_engine.py), including the
    kind of every number, which depends on nothing but its text.

The driver checks after every step that the tracked memory total equals a
walk of the document's allocations and that the text parses back to an equal
document, on every copy and at the end that freeing gives back exactly the
tracked bytes and nodes, and at exit that no key and no byte is leaked.

The number of changes made by each primitive (in place, splice, re-emit, slot
write) is collected per kind of change, and the run fails if a combination
that can occur was never exercised."""
import json, os, random, subprocess, sys, threading

import gen_docs as G
from json_model import *

BUILD, MODE = sys.argv[1], sys.argv[2]
QUICK = MODE == "quick"
DATA = os.path.join(BUILD, "data")
BIN = os.path.join(BUILD, "engine_test")
SAN = os.path.join(BUILD, "engine_test_san")
CAPS = [(4096, 128), (64, 4), (16, 1), (0, 0), (65535, 65535)]
RS, US = "\x1e", "\x1f"

# Which primitives each kind of change can use. Removing never re-emits (a shrinking node always fits its fields)
# and cannot be in place. Adding cannot be in place.
POSSIBLE = {
    "S": ("in_place", "splice", "reemit", "slot"), "I": ("in_place", "splice", "reemit", "slot"),
    "D": ("in_place", "splice", "reemit", "slot"), "N": ("in_place", "splice", "reemit", "slot"),
    "B": ("in_place", "splice", "reemit", "slot"), "X": ("splice", "slot"), "T": ("splice", "slot"),
    "A": ("splice", "reemit", "slot"), "R": ("splice", "reemit", "slot"), "P": ("splice", "reemit", "slot"),
    "M": ("in_place", "splice", "reemit", "slot"),
}
NAMES = {"S": "jsonSet", "I": "jsonSetInt", "D": "jsonSetDouble", "N": "jsonSetNumText", "B": "jsonSetBool", "X": "jsonDelete",
         "A": "jsonObjectAdd", "R": "jsonArrayInsert", "T": "jsonArrayTrim", "P": "jsonStringAppend",
         "M": "add (jsonRefDouble, jsonSetDouble)"}

# ---------------------------------------------------------------- the model of a document under change

def build(v):
    """A parsed value as the model holds it."""
    if isinstance(v, dict): return {k: build(x) for k, x in v.items()}
    if isinstance(v, list): return [build(x) for x in v]
    if isinstance(v, Num) and v == "-0": return Num("0")
    return v

def value(text):
    return build(parse(text))

def step_text(step):
    return str(step) if isinstance(step, int) else "k" + step.encode("utf-8", "surrogateescape").hex()

def path_text(path):
    return "/".join(step_text(s) for s in path)

def lookup(root, path):
    for s in path: root = root[s]
    return root

def apply(root, op):
    """Makes one change to the model. Returns the root."""
    code, path = op[0], op[1] if len(op) > 1 else None
    if code in "FC": return root
    if code in "SIDNB":
        if code == "S": new = value(op[2])
        elif code == "I": new = Num(str(op[2]))
        elif code == "N": new = Num("0" if op[2] == "-0" else op[2])
        elif code == "B": new = bool(op[2])
        else: new = Num(canonical(op[2]))  # A computed double gets its canonical text, wherever it is written.
        if not path: return new
        lookup(root, path[:-1])[path[-1]] = new
    elif code == "M":
        # Arithmetic as a command does it: the value read as a double, the sum written as a computed double.
        new = Num(canonical(float(lookup(root, path)) + op[2]))
        if not path: return new
        lookup(root, path[:-1])[path[-1]] = new
    elif code == "X": del lookup(root, path[:-1])[path[-1]]
    elif code == "A": lookup(root, path)[op[2]] = value(op[3])
    elif code == "R": lookup(root, path)[op[2]:op[2]] = [value(t) for t in op[3]]
    elif code == "T":
        arr = lookup(root, path)
        arr[:] = arr[op[2]:op[2] + op[3]]
    elif code == "P":
        new = lookup(root, path) + op[2]
        if not path: return new
        lookup(root, path[:-1])[path[-1]] = new
    return root

def op_text(op):
    code = op[0]
    if code == "C": return "C"
    if code == "F": return US.join(["F", op[1], str(op[2])])
    fields = [code, path_text(op[1])]
    if code in "SN": fields.append(op[2])
    elif code == "I": fields.append(str(op[2]))
    elif code in "DM": fields.append(repr(op[2]))
    elif code == "B": fields.append("1" if op[2] else "0")
    elif code == "A": fields += [op[2].encode("utf-8").hex(), op[3]]
    elif code == "R": fields += [str(op[2])] + list(op[3])
    elif code == "T": fields += [str(op[2]), str(op[3])]
    elif code == "P": fields.append(op[2].encode("utf-8").hex())
    return US.join(fields)

# ---------------------------------------------------------------- generating changes

INTS = [0, 1, 7, 31, 32, -1, 127, 128, -129, 32767, 32768, 2**31 - 1, 2**31, -2**31 - 1, 2**40, 2**60 - 1, 2**60, -2**60 - 1, 2**63 - 1, -2**63]
DOUBLES = [0.0, -0.0, 0.5, 1.5, 2.25, 3.0, 0.1, 19.9, 0.25, 0.75, 100.0, 1e21, 1e-7, 123.456, 0.123, 1.005, 12345678.9, 2.0**600, 2.0**-600,
           0.1 + 0.2, 1e15, 123456789012345.6, 0.000001, 5e-324, 1.7976931348623157e308, 99.99, 0.01, 7.0, 2.5e-7,
           # 16 and 17 digits in plain notation: decimals. Whole values of 17 and 18 digits: a decimal and a double.
           1.2345678901234567, 43591.01031600654, 0.0005396780371665955, -0.05647293105721474, 1e16, 1e17, 9007199254740993.0, 0.000001234567890123]
DELTAS = [0.1, -0.1, 1.0, 0.01, 1e-9, 0.000001, 12345.678, -0.5, 1e10, 0.0, 0.30000000000000004, -1e-17]
TEXTS = ["1.27", "1.28", "327.68", "21474836.48", "0.000001", "0.0000001", "-0.00", "99999999999999.9", "999999999999999.9", "0.50", "1.50", "19.90", "0.25", "100.00", "0.000", "1e5", "1E-7", "1.0", "-0", "-0.0", "12345678901234567890", "0.1234567890123456789",
         # 16 to 19 digits, scales at and beyond the limit, digits and scales around what a word holds
         "0.30000000000000004", "43591.010316006538", "0.0005396780371665955", "123456789012345678.5", "0.123456789012345678",
         "0.0000000000000000000000000000012", "0.00000000000000000000000000000012", "999999999999999999.9", "-0.104811045873349724",
         "0.104811045873349725", "0.0000099999999999999999", "0.00000099999999999999999", "9007199254740993.5", "0.9007199254740993",
         "1.0e3", "4.35", "0.10", "3", "-17", "9223372036854775808", "0.5", "2.5", "1e400"[:3], "123456789012345680000.0", "0.123456"]
STRINGS = ["", "a", "abcdefg", "abcdefgh", "0123456789abcde", "0123456789abcdef", "\u00e9" * 7, "x" * 63, "x" * 64, "y" * 255, "y" * 256, "z" * 300, "é€😀", "q\"\\\n\t", "w" * 5000, "v" * 70000, "/", "\x7f"]
KEYS = ["a", "b", "id", "name", "é", "", "q\"q", "k" * 300] + ["n%d" % i for i in range(400)]

def rand_scalar_text(r):
    c = r.randrange(8)
    if c == 0: return r.choice(["null", "true", "false"])
    if c == 1: return str(r.choice(INTS))
    if c == 2: return str(r.randrange(-50, 50))
    if c == 3: return repr(r.choice(DOUBLES))
    if c == 4: return r.choice(TEXTS)
    if c == 5: return "%.2f" % r.uniform(-99, 99)
    return json.dumps(r.choice(STRINGS[:11]) if r.random() < .9 else r.choice(STRINGS), ensure_ascii=False)

def rand_value_text(r, depth=0):
    c = r.random()
    if depth > 2 or c < 0.6: return rand_scalar_text(r)
    if c < 0.68: return "[" + ",".join(str(r.choice(INTS[:9] if r.random() < .7 else INTS)) for _ in range(r.choice([0, 1, 3, 8, 127, 128, 129, 300]))) + "]"
    if c < 0.72:
        style = r.choice(["%.2f", "%.2f", "%.6f", "%.3f", "%r", None])
        n = r.choice([1, 2, 5, 40, 130])
        if style: return "[" + ",".join(style % r.uniform(-9, r.choice([9, 400, 3e6])) for _ in range(n)) + "]"
        return "[" + ",".join(r.choice(["%.2f", "%.1f", "%d"]) % r.uniform(-9, 9) for _ in range(n)) + "]"
    if c < 0.76: return json.dumps([r.choice(STRINGS[:7]) for _ in range(r.choice([1, 2, 5, 40, 130]))], ensure_ascii=False)
    if c < 0.87: return "[" + ",".join(rand_value_text(r, depth + 1) for _ in range(r.choice([0, 1, 2, 4, 9]))) + "]"
    names = r.sample(KEYS, r.choice([0, 1, 2, 3, 5, 12] if r.random() < .93 else [127, 128, 129, 200]))
    return "{" + ",".join(json.dumps(k, ensure_ascii=False) + ":" + rand_value_text(r, depth + 1) for k in names) + "}"

def descend(r, root):
    """A random walk from the root: every (path, value) on the way."""
    out, path, v = [((), root)], (), root
    while isinstance(v, (dict, list)) and v and r.random() < 0.85:
        step = r.choice(list(v)) if isinstance(v, dict) else r.randrange(len(v))
        path, v = path + (step,), v[step]
        out.append((path, v))
    return out

def gen_op(r, root, bias=None):
    nodes = descend(r, root) + descend(r, root) + descend(r, root)
    def pick(pred):
        found = [n for n in nodes if pred(*n)]
        return r.choice(found) if found else None
    code = bias or r.choices("SIDNBXARTPFCM", weights=[14, 9, 6, 6, 4, 13, 12, 14, 5, 7, 5, 2, 5])[0]
    if code == "C": return ("C",)
    if code == "F": return ("F", r.choice("arss"), r.choice([1, 1, 2, 3, 7, 50]))
    if code == "X":
        n = pick(lambda p, v: len(p) > 0)
        if n: return ("X", n[0])
    if code == "A":
        n = pick(lambda p, v: isinstance(v, dict))
        if n:
            free = [k for k in r.sample(KEYS, 12) if k not in n[1]]
            if free: return ("A", n[0], free[0], rand_value_text(r))
    if code == "R":
        n = pick(lambda p, v: isinstance(v, list))
        if n:
            at = r.choice([0, len(n[1]), r.randrange(len(n[1]) + 1)])
            return ("R", n[0], at, [rand_value_text(r) for _ in range(r.choice([1, 1, 1, 2, 3, 10]))])
    if code == "T":
        n = pick(lambda p, v: isinstance(v, list))
        if n:
            size = len(n[1])
            start = r.choice([0, 0, r.randrange(size + 1)])
            return ("T", n[0], start, r.choice([size, max(size - start - 1, 0), r.randrange(size + 2), 0]))
    if code == "M":
        n = pick(lambda p, v: isinstance(v, Num))
        if n: return ("M", n[0], r.choice(DELTAS))
    if code == "P":
        n = pick(lambda p, v: type(v) is str)
        if n: return ("P", n[0], r.choice(["a", "abcdefgh", "é", "\n", "x" * 60, "y" * 200, "\"", "z" * 4100]))
    # Replacing the whole document ends the interesting part of a script: do it rarely.
    inner = [n for n in nodes if n[0]]
    n = r.choice(inner) if inner and r.random() < .97 else r.choice(nodes)
    if code == "I": return ("I", n[0], r.choice(INTS) if r.random() < .5 else r.randrange(-40, 40))
    if code == "D": return ("D", n[0], r.choice(DOUBLES) if r.random() < .6 else round(r.uniform(-50, 50), r.randrange(0, 5)))
    if code == "N": return ("N", n[0], r.choice(TEXTS) if r.random() < .7 else repr(r.uniform(-1e6, 1e6)))
    if code == "B": return ("B", n[0], r.random() < .5)
    return ("S", n[0], rand_value_text(r))

def gen_script(r, doc_text, nops, bias=None):
    root = value(doc_text)
    ops = []
    for _ in range(nops):
        op = gen_op(r, root, r.choice(bias) if bias and r.random() < .8 else None)
        root = apply(root, op)
        ops.append(op)
    return doc_text, ops

def start_docs(r):
    """Documents to change: every shape of the generator that is small enough to replay quickly, and containers
    sitting right at each cap and field width, so that changes cross them."""
    docs = []
    for shape in G.SHAPES: docs += G.records(shape, 30 if QUICK else 150, seed=9)
    docs += [d for d in G.array_docs(40 if QUICK else 200, seed=5) if len(d) < 6000]
    docs += [d for d in G.EDGE if len(d) < 3000]
    docs += [d for d in G.random_docs(60 if QUICK else 300, seed=13) if len(d) < 6000]
    docs += r.sample([d for d in G.number_docs() if len(d) < 400], 60 if QUICK else 300)
    docs += ['"abcdefg"', '"' + "s" * 250 + '"', "7", "0.5", "null", "[]", "{}", "[0.50,0.25]", "[1,2,3]", "[0.5,1.5]", '{"a":[0.50,1.25,3]}']
    edge = []
    for n in (1, 3, 4, 5, 126, 127, 128, 129, 130, 254, 255, 256, 257):
        edge.append(G.compact(list(range(n))))
        edge.append("[" + ",".join(["null"] * n) + "]")
        edge.append("[" + ",".join(["0.50"] * n) + "]")
        edge.append("{" + ",".join('"m%d":%d' % (i, i) for i in range(n)) + "}")
        edge.append('{"in":{"arr":' + G.compact(["s%d" % i for i in range(n)]) + ',"z":1},"tail":[1,2,3]}')
    for n in (1, 2, 4, 5, 126, 127, 128, 129, 130, 300):
        edge.append(json.dumps(["id-%07d" % i for i in range(n)]))                       # strings of 10 bytes
        edge.append(json.dumps([("%x" % (i * 7919)) * (1 + i % 3) for i in range(n)]))  # mixed lengths, some over 15
        edge.append("[" + ",".join("%.2f" % (i * 0.37 - 20) for i in range(n)) + "]")    # decimals, one scale
        edge.append("[" + ",".join("%.6f" % (i * 0.000731 - 0.04) for i in range(n)) + "]")
        edge.append("[" + ",".join(("%.2f" if i % 5 else "%.3f") % (i * 1.5) for i in range(n)) + "]")  # two scales
        edge.append("[" + ",".join("%.17f" % (i * 0.0037 - 0.2) for i in range(n)) + "]")             # 15 to 17 digits, one scale
        edge.append("[" + ",".join(repr((i + 7) * 0.000731 * 1.0000001) for i in range(n)) + "]")  # full precision, many scales
        edge.append("[" + ",".join("%d.%017d" % (i % 10, i * 7919) for i in range(n)) + "]")           # 18 digits when not below 1
        edge.append('{"prices":[' + ",".join("%.2f" % (i * 123.45) for i in range(n)) + '],"ids":' + json.dumps(["k%014d" % i for i in range(n)]) + "}")
    for n in (5, 8, 9, 40, 50, 57, 58, 59, 240, 247, 248, 249, 250, 4070, 4080, 4085, 4088):
        edge.append('["%s"]' % ("s" * n))
        edge.append('{"k":"%s","n":1}' % ("s" * n))
        edge.append('[[1,2,"%s"],{"k":[true,"%s"]}]' % ("s" * n, "t" * (n // 2)))
    edge.append(G.compact([{"id": i, "v": [i, i + 1], "s": "x" * (i % 70)} for i in range(200)]))
    edge.append(G.compact({"k%d" % i: {"n": i, "s": "str%d" % i, "l": [i, i + 1]} for i in range(300)}))
    edge.append(G.compact(list(range(-3000, 3000, 7))))
    edge.append(G.compact([i * 0.5 for i in range(700)]))
    return docs, edge

def scripts():
    r = random.Random(77)
    docs, edge = start_docs(r)
    out = []
    for d in docs: out.append(gen_script(r, d, r.choice([8, 20, 35])))
    for d in edge:
        out.append(gen_script(r, d, 30))
        # Mostly adding and removing, so that containers at a cap cross it; with copies in between, which bring a
        # container that became a spine node back to the packed form.
        out.append(gen_script(r, d, 40, bias="ARXXRAATPC"))
        out.append(gen_script(r, d, 40, bias="SIDNBM"))
    return out

# ---------------------------------------------------------------- running and comparing

failures = []
counts = {}
compared = [0, 0]

def fail(msg):
    failures.append(msg)
    if len(failures) <= 12: print("  FAIL " + msg[:600], flush=True)

def run(binary, caps, lines, tag):
    path = os.path.join(DATA, "mutate.hex")
    with open(path, "wb") as f:
        for l in lines: f.write(l.encode("utf-8", "surrogateescape").hex().encode() + b"\n")
    p = subprocess.run([binary, "-x", "-b", str(caps[0]), "-e", str(caps[1]), "mutate", path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        fail("%s: driver failed: %s" % (tag, p.stderr.decode(errors="replace")[-500:]))
        return None
    return p.stdout.split(b"\n")[:-1]

def check_first(batch, out, tag):
    """Replays every script on the model, in step with the driver's output."""
    it = iter(out)
    for doc, ops in batch:
        root = value(doc)
        for i in range(len(ops) + 1):
            if i: root = apply(root, ops[i - 1])
            line = next(it)
            text, dump = line[1:].split(b"\t")
            what = "%s: %r after %d changes, last %r" % (tag, doc[:60], i, ops[i - 1] if i else None)
            if bytes.fromhex(text.decode()) != encode(serialize(root)):
                fail(what + ": text " + repr(bytes.fromhex(text.decode())[:200]) + " want " + repr(serialize(root)[:200]))
                compared[1] += 1
                break
            if dump != b"-":
                try:
                    match(json.loads(dump), root)
                except (Mismatch, ValueError, KeyError, TypeError) as e:
                    fail(what + ": dump: " + str(e))
                    compared[1] += 1
                    break
            compared[0] += 1
        else:
            if next(it) != b".": fail(tag + ": script end missing")
            continue
        for line in it:
            if line == b".": break

def check_same(out, first, tag):
    """Another build or cap setting must print exactly what the first run printed."""
    a = [l for l in out if not l.startswith(b"#")]
    b = [l for l in first if not l.startswith(b"#")]
    if len(a) != len(b):
        fail("%s: %d lines, expected %d" % (tag, len(a), len(b)))
        return
    bad = [i for i in range(len(a)) if a[i] != b[i]]
    compared[0] += len(a) - len(bad) - a.count(b".")
    compared[1] += len(bad)
    if bad: fail("%s: line %d differs from the first run: %r / %r" % (tag, bad[0], a[bad[0]][:200], b[bad[0]][:200]))

def add_counts(out):
    for l in out:
        if not l.startswith(b"#"): continue
        fields = l.decode().split()
        row = counts.setdefault(fields[0][1], {})
        for f in fields[1:]:
            k, v = f.split("=")
            row[k] = row.get(k, 0) + int(v)

def main():
    os.makedirs(DATA, exist_ok=True)
    all_scripts = scripts()
    print("%d scripts, %d changes" % (len(all_scripts), sum(len(ops) for _, ops in all_scripts)), flush=True)
    sizes = []
    for doc, ops in all_scripts:
        root = value(doc)
        for op in ops: root = apply(root, op)
        sizes.append(len(serialize(root)))
    print("documents at the end of their script: median %d bytes, largest %d" % (sorted(sizes)[len(sizes) // 2], max(sizes)), flush=True)
    lines = [RS.join([doc] + [op_text(op) for op in ops]) for doc, ops in all_scripts]
    first = None
    for binary in (BIN, SAN):
        # The sanitizer build runs every script in quick mode and every third one otherwise.
        pick = range(len(lines)) if binary == BIN or QUICK else range(0, len(lines), 3)
        for caps in CAPS:
            tag = "%s cb=%d ce=%d" % ("san" if binary == SAN else "O2", caps[0], caps[1])
            out = run(binary, caps, [lines[i] for i in pick], tag)
            if out is None: continue
            add_counts(out)
            if first is None:
                first = out
                check_first(all_scripts, out, tag)
            elif binary == BIN:
                check_same(out, first, tag)
            else:
                # Compare with the same scripts of the first run.
                want, it = [], iter([l for l in first if not l.startswith(b"#")])
                for i in range(len(lines)):
                    chunk = []
                    for l in it:
                        chunk.append(l)
                        if l == b".": break
                    if i in pick: want += chunk
                check_same(out, want, tag)
            print("  %-28s done" % tag, flush=True)

    print("\nchanges by primitive, all runs:")
    print("  %-18s %10s %10s %10s %10s" % ("", "in place", "splice", "re-emit", "slot"))
    for code in "SIDNBXARTPM":
        row = counts.get(code, {})
        print("  %-18s %10d %10d %10d %10d" % (NAMES[code], row.get("in_place", 0), row.get("splice", 0), row.get("reemit", 0), row.get("slot", 0)))
        for prim in ("in_place", "splice", "reemit", "slot"):
            if prim in POSSIBLE[code] and not row.get(prim, 0): fail("%s never used %s" % (NAMES[code], prim))
            if prim not in POSSIBLE[code] and row.get(prim, 0): fail("%s used %s, which should not be possible" % (NAMES[code], prim))
    print("TOTAL steps compared: pass=%d fail=%d" % (compared[0], compared[1]))
    for f in failures[:20]: print("FAILED:", f[:800])
    status.append(1 if failures else 0)

sys.setrecursionlimit(20000)
threading.stack_size(512 * 1024 * 1024)
status = []
t = threading.Thread(target=main)
t.start()
t.join()
sys.exit(status[0] if status else 1)
