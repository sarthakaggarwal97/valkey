#!/usr/bin/env python3
"""Differential test of json_engine.c against an independent model in Python.

usage: check_engine.py <build-dir> <full|quick>

Runs build/engine_test and build/engine_test_san (ASan+UBSan) over generated
documents, under several settings of the packed caps, and compares:

  roundtrip  serialised output, compact and formatted, with the text this
             script derives from the input: the same values in the same
             order, a duplicate member name resolved as last value at first
             position, strings escaped as the module's writer escapes them,
             and every number as its source text ("-0" prints "0").
  dump       what every read function returns (type, count, integer value,
             double value, number text, string bytes, member names, depth,
             fields) with this script's own walk of the document, including
             the INT / DOUBLE / RAW classification of design 5.4.
  equals     jsonRefEquals with the module's comparison, modelled here.
  reject     invalid texts are refused with the expected error.

The binary itself checks, for every document, that a copy is equal and
serialises alike, that jsonDocMemUsage is exactly what the document's
allocations hold, and at exit that no key reference and no byte is leaked."""
import json, math, os, random, subprocess, sys, threading

import gen_docs as G

BUILD, MODE = sys.argv[1], sys.argv[2]
QUICK = MODE == "quick"
DATA = os.path.join(BUILD, "data")
BIN = os.path.join(BUILD, "engine_test")
SAN = os.path.join(BUILD, "engine_test_san")
CAPS = [(4096, 128), (64, 4), (16, 1), (65535, 65535)]
# CHECK_ONLY=layout,numbers runs those corpora only, and none of the other checks: for trying one change quickly.
ONLY = [c for c in os.environ.get("CHECK_ONLY", "").split(",") if c]

from json_model import *

# ---------------------------------------------------------------- running

results = []
failures = []

def record(name, cfg, op, npass, nfail, note=""):
    results.append((name, cfg, op, npass, nfail))
    if nfail:
        failures.append("%s %s %s: %d failed %s" % (name, cfg, op, nfail, note))
        print("  FAIL %-12s %-22s %-16s pass=%d fail=%d %s" % (name, cfg, op, npass, nfail, note), flush=True)

def run(binary, args, lines):
    """Runs the driver on `lines` (bytes), given to it in hex. Returns its output lines, or None if it failed."""
    path = os.path.join(DATA, "in.hex")
    with open(path, "wb") as f:
        for l in lines: f.write(l.hex().encode() + b"\n")
    p = subprocess.run([binary, "-x"] + args + [path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        return None, p.stderr.decode(errors="replace")[-600:]
    return p.stdout.split(b"\n")[:-1], ""

def caps_args(caps):
    return ["-b", str(caps[0]), "-e", str(caps[1])]

def cfg_name(binary, caps, extra=""):
    return "%s cb=%d ce=%d%s" % ("san" if binary == SAN else "O2", caps[0], caps[1], extra)

def check_roundtrip(name, binary, caps, lines, expected, extra_args=(), tag=""):
    """expected: for each line the output bytes, or an error name."""
    outs, err = run(binary, caps_args(caps) + list(extra_args) + ["roundtrip"], lines)
    cfg = cfg_name(binary, caps, tag)
    if outs is None or len(outs) != len(lines):
        return record(name, cfg, "roundtrip", 0, len(lines), "driver failed: " + err)
    ok = bad = 0
    note = ""
    for i, (out, want) in enumerate(zip(outs, expected)):
        if isinstance(want, str): good = out.startswith(b"!E " + want.encode() + b" ")
        else: good = not out.startswith(b"!") and bytes.fromhex(out.decode()) == want
        if good: ok += 1
        else:
            bad += 1
            if not note: note = "line %d: in=%r out=%r want=%r" % (i + 1, lines[i][:80], out[:120], want[:60])
    record(name, cfg, "roundtrip", ok, bad, note)

def check_dump(name, binary, caps, lines, values, extra_args=(), tag=""):
    outs, err = run(binary, caps_args(caps) + list(extra_args) + ["dump"], lines)
    cfg = cfg_name(binary, caps, tag)
    if outs is None or len(outs) != len(lines):
        return record(name, cfg, "dump", 0, len(lines), "driver failed: " + err)
    ok = bad = 0
    note = ""
    for i, (out, v) in enumerate(zip(outs, values)):
        try:
            match(json.loads(out), v)
            ok += 1
        except (Mismatch, ValueError, KeyError, TypeError) as e:
            bad += 1
            if not note: note = "line %d: in=%r: %s" % (i + 1, lines[i][:80], e)
    record(name, cfg, "dump", ok, bad, note)

def depth_of(raw):
    """Nesting depth of a JSON text, by counting brackets outside strings."""
    depth = best = 0
    in_str = esc = False
    for ch in raw:
        if in_str:
            if esc: esc = False
            elif ch == 0x5C: esc = True
            elif ch == 0x22: in_str = False
        elif ch == 0x22: in_str = True
        elif ch in (0x5B, 0x7B):
            depth += 1; best = max(best, depth)
        elif ch in (0x5D, 0x7D): depth -= 1
    return best

# ---------------------------------------------------------------- corpora

def corpora():
    """name -> (lines as bytes, 'model' or 'same'). 'same' documents are compact and canonical, so the expected
    output is the input. They are the ones Python's recursive parser cannot take, or that need no model."""
    n = 1000 if QUICK else 20000
    c = {}
    for shape in G.SHAPES: c[shape] = (G.records(shape, n), "model")
    c["singles"] = (G.singles(20 if QUICK else 1), "model")
    c["numbers"] = (G.number_docs(), "model")
    c["arrays"] = (G.array_docs(150 if QUICK else 1200), "model")
    c["strings"] = (G.string_docs(), "model")
    c["lengths"] = (G.length_docs(), "model")
    c["strarrays"] = (G.string_array_docs(), "model")
    c["wide"] = (G.wide_decimal_docs(), "model")
    c["literals"] = (G.literal_array_docs(), "model")
    c["edge"] = (G.EDGE, "model")
    c["random"] = (G.random_docs(100 if QUICK else 600), "model")
    c["caps"] = (G.caps_docs(), "model")
    c["layout"] = (G.layout_docs(), "model")  # Byte order and widths: every encoding, with asymmetric bytes.
    c["deep"] = (G.deep_docs(), "same")
    return {k: ([l if isinstance(l, bytes) else l.encode("utf-8") for l in v[0]], v[1]) for k, v in c.items()}

FORMATS = [("  ", "\n", " "), ("\t", "\r\n", ""), ("", "\n", ""), ("--", "", ""), ("", "", "  "), ("ab", "cd", "ef")]

def format_args(fmt):
    args = []
    for flag, s in zip(("-i", "-n", "-s"), fmt):
        if s: args += [flag, s]
    return args

def mutate(r, v, how):
    """A variant of a parsed value: 'shuffle' is equal to it, the others usually are not."""
    if isinstance(v, dict) and v:
        if how == "shuffle":
            items = [(k, mutate(r, x, how)) for k, x in v.items()]
            r.shuffle(items)
            return dict(items)
        k = r.choice(list(v))
        out = dict(v)
        if how == "drop": del out[k]
        else: out[k] = mutate(r, v[k], how)
        return out
    if isinstance(v, list) and v:
        if how == "shuffle": return [mutate(r, x, how) for x in v]
        i = r.randrange(len(v))
        out = list(v)
        if how == "drop": del out[i]
        else: out[i] = mutate(r, v[i], how)
        return out
    if how in ("shuffle", "drop"): return v
    if isinstance(v, Num):
        kind = analyze(v)[0]
        if how == "respell" and kind == "i" and abs(int(v)) < 2**53: return Num(r.choice([v + ".0", v + "e0", v + ".00", v + "E+0"]))
        if how == "respell" and "e" not in v and "E" not in v and "." in v: return Num(v + r.choice(["0", "00", "e0"]))
        return Num(str(int(float(v)) + 1) if math.isfinite(float(v)) and abs(float(v)) < 1e15 else "7")
    if isinstance(v, str): return v + "x"
    if v is None: return False
    if v is True: return False
    if v is False: return True
    return None

EQUAL_PAIRS = [
    ("1", "1"), ("1", "2"), ("1", "1.0"), ("1", "1e0"), ("1.0", "1.00"), ("0", "-0"), ("0", "-0.0"), ("0.0", "-0.0"), ("0.1", "0.10"),
    ("-1", "18446744073709551615"), ("-2", "18446744073709551614"), ("-1", "18446744073709551614"), ("-9223372036854775808", "9223372036854775808"),
    ("9223372036854775807", "9223372036854775807"), ("9223372036854775808", "9223372036854775808"), ("9223372036854775808", "9223372036854775809"),
    ("9007199254740993", "9007199254740992"), ("9007199254740993", "9007199254740992.0"), ("18446744073709551615", "18446744073709551615.0"),
    ("18446744073709551616", "18446744073709551616"), ("18446744073709551616", "18446744073709551617"),
    ("123456789012345678901234567890", "123456789012345678901234567890.0"), ("1e5", "100000"), ("1e5", "100000.0"), ("1E5", "1e5"),
    ("5e-324", "4.9e-324"), ("1e-400", "0"), ("1e-400", "0.0"), ("1.50", "1.5"), ("19.90", "19.9"), ("19.90", "19.91"),
    ('"a"', '"a"'), ('"a"', '"b"'), ('""', '""'), ('"abcdefgh"', '"abcdefgh"'), ('"abcdefg"', '"abcdefgh"'), ('"1"', "1"), ('"\\u0041"', '"A"'),
    ('"' + "x" * 300 + '"', '"' + "x" * 300 + '"'), ('"' + "x" * 300 + '"', '"' + "x" * 299 + 'y"'),
    ("true", "true"), ("true", "false"), ("null", "null"), ("null", "false"), ("null", "0"), ("true", "1"), ("false", "0"), ("null", '""'),
    ("[]", "[]"), ("{}", "{}"), ("[]", "{}"), ("[]", "null"), ("[]", '""'), ("[1,2]", "[1,2]"), ("[1,2]", "[2,1]"), ("[1,2]", "[1,2,3]"),
    ("[1,2]", "[1.0,2e0]"), ("[0.50,0.25]", "[0.5,0.25]"), ("[0.50,0.25]", "[0.50,0.26]"), ("[1,2,3]", "[1,2,300]"), ("[1,2,3]", '[1,2,"3"]'),
    ("[[]]", "[[]]"), ("[[]]", "[{}]"), ("[[1]]", "[[1,[]]]"), ('{"a":1,"b":2}', '{"b":2,"a":1}'), ('{"a":1}', '{"a":1,"b":2}'), ('{"a":1}', '{"b":1}'),
    ('{"a":1,"a":2}', '{"a":2}'), ('{"a":1,"b":2,"a":3}', '{"b":2,"a":3}'), ('{"a":{"b":[1,{"c":null}]}}', '{"a":{"b":[1,{"c":null}]}}'),
    ('{"a":{"b":[1,{"c":null}]}}', '{"a":{"b":[1,{"c":false}]}}'), ('{"a":[]}', '{"a":{}}'), ('{"never-seen-before":1}', '{"a":1}'),
]

# Numbers of every kind the engine has, at the int64 and uint64 boundaries and at 2^53. Each is compared with each.
NUMBER_KINDS = [
    # JSON_INT
    "0", "1", "-1", "100", "9007199254740991", "9007199254740992", "9007199254740993", "-9007199254740993",
    "9223372036854775806", "9223372036854775807", "-9223372036854775807", "-9223372036854775808",
    # Decimals
    "0.0", "1.0", "-1.0", "1.5", "1.50", "100.00", "9007199254740.99", "0.000000000000001",
    # Decimals of 16 to 18 digits: beyond 2^53, where their value comes from the number library
    "0.30000000000000004", "0.3", "43591.010316006538", "43591.01031600654", "9007199254740993.5", "900719925474099.25",
    "922337203685477580.7", "0.000000000000000000000000000001", "100.0000000000000000",
    # JSON_DOUBLE: canonical text that is not a decimal
    "9007199254740992.0", "90071992547409.92", "922337203685477.5", "9223372036854776000.0", "-9223372036854776000.0", "18446744073709552000.0",
    "1.2345678901234568e+24", "1e-7",
    # Number text: integers beyond int64, within uint64 and beyond it
    "9223372036854775808", "9223372036854775809", "-9223372036854775809", "18446744073709551614", "18446744073709551615",
    "18446744073709551616", "18446744073709551617", "-18446744073709551615", "1234567890123456789012345", "1234567890123456789012346",
    # Number text: fractions and exponents
    "1e0", "-1E0", "0e0", "150e-2", "1e2", "9007199254740993.0", "9.223372036854776e18", "1.8446744073709552e19", "9.007199254740993e15", "9223372036854775807.0", "9223372036854775808e0",
    "9.223372036854775807E18", "-9223372036854775808.0", "18446744073709551615.0", "1.8446744073709551615e19", "1.2345678901234567890123450e24",
]

def number_pairs():
    """Each pair as two roots, as elements of two packed arrays, and as elements of two typed or plain arrays."""
    pairs = []
    for a in NUMBER_KINDS:
        for b in NUMBER_KINDS:
            pairs += [(a, b), ('[%s,"x"]' % a, '[%s,"x"]' % b), ("[%s,%s]" % (a, a), "[%s,%s]" % (b, b))]
    return pairs

def equal_pairs():
    r = random.Random(5)
    pairs = list(EQUAL_PAIRS) + number_pairs()
    docs = G.random_docs(60 if QUICK else 300, seed=21) + G.records("order", 100) + G.array_docs(100, seed=3) + G.caps_docs()[:60]
    for text in docs:
        v = parse(text)
        for how in ("same", "shuffle", "respell", "change", "drop"):
            other = v if how == "same" else mutate(r, v, how)
            pairs.append((text, serialize(other)))
    return pairs

def main():
    os.makedirs(DATA, exist_ok=True)
    binaries = [BIN, SAN]
    for b in binaries:
        p = subprocess.run([b, "unit"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        record("unit", "san" if b == SAN else "O2", "unit", int(p.returncode == 0), int(p.returncode != 0), p.stderr.decode(errors="replace")[-400:])

    all_corpora = corpora()
    for name, (lines, mode) in all_corpora.items():
        if ONLY and name not in ONLY: continue
        print("%-10s %6d documents, %9d bytes" % (name, len(lines), sum(map(len, lines))), flush=True)
        values = [parse(decode(l)) for l in lines] if mode == "model" else None
        compact_out = [encode(serialize(v)) for v in values] if values else lines
        heavy = name in G.SHAPES or name == "singles"
        for binary in binaries:
            # The sanitizer build gets every corpus, but fewer of the large uniform ones.
            cut = len(lines) if binary == BIN or not heavy else max(len(lines) // 10, 6)
            for caps in CAPS:
                check_roundtrip(name, binary, caps, lines[:cut], compact_out[:cut])
                # Trusted input must give the same result as untrusted for valid text.
                if caps == CAPS[0]: check_roundtrip(name, binary, caps, lines[:cut], compact_out[:cut], ["-t"], " trusted")
                if values and (caps in CAPS[:2] or not heavy): check_dump(name, binary, caps, lines[:cut], values[:cut])
        if values:
            sub = len(lines) if not heavy else max(len(lines) // 10, 6)
            for fmt in FORMATS:
                want = [encode(serialize(v, *fmt)) for v in values[:sub]]
                for binary, caps in ((BIN, CAPS[0]), (BIN, CAPS[1]), (SAN, CAPS[2])):
                    check_roundtrip(name, binary, caps, lines[:sub], want, format_args(fmt), " fmt=%r" % (fmt,))
            if name in G.SHAPES:
                # Where the module's format and Python's agree, compare with json.dumps directly.
                want = [json.dumps(json.loads(l), indent="  ", separators=(",", ": "), ensure_ascii=False).encode() for l in lines[:sub]]
                check_roundtrip(name, BIN, CAPS[0], lines[:sub], want, format_args(FORMATS[0]), " json.dumps")

    if ONLY: return finish()

    # Depth limit: a document is refused exactly when it nests deeper than the limit.
    lines = [l for l in all_corpora["deep"][0] if len(l) < 50000] + all_corpora["edge"][0]
    for limit in (1, 2, 16, 32, 128):
        want = ["DEPTH" if depth_of(l) > limit else encode(serialize(parse(decode(l)))) for l in lines]
        for binary in binaries:
            check_roundtrip("depth", binary, CAPS[0], lines, want, ["-d", str(limit)], " max_depth=%d" % limit)

    # Rejected texts. Trusted input skips the UTF-8 and range checks, and then keeps the text.
    lines = [t.encode("latin-1") for t, _ in G.REJECT]
    kinds = [k for _, k in G.REJECT]
    for binary in binaries:
        for caps in CAPS[:2]:
            check_roundtrip("reject", binary, caps, lines, kinds)
            check_roundtrip("reject", binary, caps, lines, [k if k == "SYNTAX" else l for l, k in zip(lines, kinds)], ["-t"], " trusted")
    bad_utf8 = G.invalid_utf8_docs()
    for binary in binaries:
        for caps in CAPS:
            check_roundtrip("bad-utf8", binary, caps, bad_utf8, ["UTF8"] * len(bad_utf8))
            check_roundtrip("bad-utf8", binary, caps, bad_utf8, bad_utf8, ["-t"], " trusted")
            check_dump("bad-utf8", binary, caps, bad_utf8, [parse(decode(l)) for l in bad_utf8], ["-t"], " trusted")

    # Equality, with the two sides emitted under the same and under different caps.
    pairs = equal_pairs()
    lines = [(a + "\t" + b).encode("utf-8") for a, b in pairs]
    want = [b"1" if equal(parse(a), parse(b)) else b"0" for a, b in pairs]
    for binary in binaries:
        for caps, caps2 in ((CAPS[0], CAPS[0]), (CAPS[1], CAPS[1]), (CAPS[0], CAPS[2]), (CAPS[2], CAPS[3])):
            outs, err = run(binary, caps_args(caps) + ["-B", str(caps2[0]), "-E", str(caps2[1]), "equals"], lines)
            cfg = cfg_name(binary, caps, " vs cb=%d ce=%d" % caps2)
            if outs is None or len(outs) != len(lines):
                record("equals", cfg, "equals", 0, len(lines), "driver failed: " + err)
                continue
            bad = [i for i, (o, w) in enumerate(zip(outs, want)) if o != w]
            record("equals", cfg, "equals", len(lines) - len(bad), len(bad), "pair %r got %r" % (pairs[bad[0]], outs[bad[0]]) if bad else "")
    finish()

def finish():
    total_pass = sum(r[3] for r in results)
    total_fail = sum(r[4] for r in results)
    by_op = {}
    for name, cfg, op, npass, nfail in results:
        key = (op, "san" if cfg.startswith("san") else "O2")
        by_op[key] = (by_op.get(key, (0, 0))[0] + npass, by_op.get(key, (0, 0))[1] + nfail)
    print()
    for (op, build), (npass, nfail) in sorted(by_op.items()):
        print("%-10s %-4s pass=%-8d fail=%d" % (op, build, npass, nfail))
    off = G.ulp_off_decimals(400)
    print("wide decimals whose digits divided by the power of ten give a double one unit off: %d of %d checked here, each read as a root, a member and an element" % (sum(G.divide(t.lstrip("-")) != float(t.lstrip("-")) for t in off), len(off)))
    print("TOTAL comparisons: pass=%d fail=%d in %d runs of the driver" % (total_pass, total_fail, len(results)))
    for f in failures[:20]: print("FAILED:", f)
    status.append(1 if total_fail else 0)

# Python's JSON parser and match() recurse on nesting depth: give them room.
sys.setrecursionlimit(20000)
threading.stack_size(512 * 1024 * 1024)
status = []
t = threading.Thread(target=main)
t.start()
t.join()
sys.exit(status[0] if status else 1)
