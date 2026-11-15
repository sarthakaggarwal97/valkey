#!/usr/bin/env python3
"""Deterministic test documents for the JSON engine.

As a module: the generators check_engine.py uses.
As a script:  gen_docs.py corpus <dir> <n>   writes, for engine_bench, <dir>/{session,profile,
              order}.jsonl with n documents each, big.jsonl with n/10, and the single
              documents obj1000, records100k, emb_full, emb_short, ints100k, str10k.

The session, profile and order shapes and the edge-case lists come from the
measurement prototype (json-m0/gen.py, json-m0/proto/gen_edge.py), extended."""
import json, os, random, struct, sys

WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november "
         "oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu").split()

def compact(o): return json.dumps(o, separators=(",", ":"))
def f32(x): return struct.unpack('f', struct.pack('f', x))[0]
def words(r, n): return " ".join(r.choice(WORDS) for _ in range(n))
def hexs(r, n): return "".join(r.choice("0123456789abcdef") for _ in range(n))

# ---------------------------------------------------------------- record shapes

def session(r, i):
    return {"sid": hexs(r, 32), "uid": r.randrange(1, 10**7),
            "created": "2026-11-%02dT%02d:%02d:%02dZ" % (r.randrange(1, 29), r.randrange(24), r.randrange(60), r.randrange(60)),
            "expires": "2026-12-%02dT00:00:00Z" % r.randrange(1, 29), "ip": "%d.%d.%d.%d" % tuple(r.randrange(256) for _ in range(4)),
            "ua": "Mozilla/5.0 (" + words(r, 4) + ") " + words(r, 3), "roles": r.sample(["user", "admin", "ops", "billing"], r.randrange(1, 3)),
            "flags": {"mfa": r.random() < .5, "beta": r.random() < .2}, "cart_items": r.randrange(0, 12),
            "last_path": "/" + "/".join(r.choice(WORDS) for _ in range(r.randrange(1, 4)))}

def profile(r, i, min_age=18):
    return {"id": i, "name": words(r, 2).title(), "email": r.choice(WORDS) + str(r.randrange(1000)) + "@example.com", "age": r.randrange(min_age, 90),
            "score": round(r.uniform(0, 100), 2), "active": r.random() < .8,
            "address": {"street": "%d %s St" % (r.randrange(1, 9999), r.choice(WORDS).title()), "city": r.choice(WORDS).title(),
                        "state": r.choice(["WA", "CA", "NY", "TX"]), "zip": "%05d" % r.randrange(100000),
                        "geo": {"lat": round(r.uniform(-90, 90), 6), "lon": round(r.uniform(-180, 180), 6)}},
            "tags": r.sample(WORDS, r.randrange(3, 7)),
            "prefs": {"theme": r.choice(["dark", "light"]), "lang": r.choice(["en", "de", "fr", "ja"]),
                      "notify": {"email": True, "sms": r.random() < .3, "push": r.random() < .6}},
            "bio": words(r, r.randrange(15, 60))}

def order(r, i):
    items = [{"sku": "SKU-" + hexs(r, 8).upper(), "name": words(r, 3), "qty": r.randrange(1, 9), "price": round(r.uniform(1, 500), 2)}
             for _ in range(r.randrange(3, 9))]
    sub = round(sum(x["qty"] * x["price"] for x in items), 2)
    return {"order_id": "ORD-%09d" % i, "customer_id": r.randrange(1, 10**7), "status": r.choice(["new", "paid", "shipped", "delivered"]),
            "ts": 1790000000 + r.randrange(10**6), "items": items,
            "totals": {"subtotal": sub, "tax": round(sub * 0.0825, 2), "shipping": 4.99, "total": round(sub * 1.0825 + 4.99, 2)},
            "ship_to": {"name": words(r, 2).title(), "line1": "%d %s Ave" % (r.randrange(1, 9999), r.choice(WORDS).title()),
                        "city": r.choice(WORDS).title(), "zip": "%05d" % r.randrange(100000)},
            "notes": words(r, r.randrange(0, 25))}

SHAPES = {"session": session, "profile": profile, "order": order}

def big(n, seed=1, target=4730):
    """The prototype's 3.8 KB document (json-m0/proto/gen_big.py): a profile, then string members extra_NN of nine
    words until the text reaches `target` bytes, then tail_tags, tail_note and tail_count."""
    r = random.Random(seed)
    out = []
    for i in range(n):
        o = profile(r, i, 32) # An age that takes a 2-byte field, so that the in-place writes measured fit.
        tail = {"tail_tags": r.sample(WORDS, 4), "tail_note": words(r, 3), "tail_count": r.randrange(1000, 10**6)}
        tail_len = len(compact(tail)) - 1
        k = 0
        while True:
            room = target - (len(compact(o)) + tail_len) - len(',"extra_00":""')
            if room < 8: break
            o["extra_%02d" % k] = words(r, 9)[:room]
            k += 1
        o.update(tail)
        out.append(compact(o))
    return out

def spine_docs():
    """Single documents that are spine containers: an object of 1,000 integer members, and the single-document
    shapes of the gate corpus exactly as json-m0/gen.py writes them with seed 1 (each from a fresh generator)."""
    def fresh(): return random.Random(1)
    r = fresh()
    docs = {"obj1000": compact({"field_%04d" % i: i * 7 for i in range(1000)}),
            "records100k": compact([{"id": i, "name": "item-%d" % i, "price": round(r.uniform(1, 500), 2), "qty": r.randrange(1, 9),
                                     "tags": r.sample(WORDS, 2)} for i in range(100000)])}
    r = fresh(); docs["emb_full"] = "[" + ",".join(repr(f32(r.gauss(0, 0.05))) for _ in range(1536)) + "]"
    r = fresh(); docs["emb_short"] = "[" + ",".join("%.6f" % r.gauss(0, 0.05) for _ in range(1536)) + "]"
    r = fresh(); docs["ints100k"] = compact([r.randrange(0, 2**31) for _ in range(100000)])
    r = fresh(); docs["str10k"] = compact([hexs(r, r.randrange(8, 14)) for _ in range(10000)])
    return docs

def records(shape, n, seed=1):
    r = random.Random(seed)
    return [compact(SHAPES[shape](r, i)) for i in range(n)]

def singles(scale):
    """The single-document shapes of the prototype corpus, `scale` times smaller."""
    r = random.Random(1)
    n = 100000 // scale
    return [
        compact([{"id": i, "name": "item-%d" % i, "price": round(r.uniform(1, 500), 2), "qty": r.randrange(1, 9), "tags": r.sample(WORDS, 2)} for i in range(n)]),
        "[" + ",".join(repr(f32(r.gauss(0, 0.05))) for _ in range(1536)) + "]",
        "[" + ",".join("%.6f" % r.gauss(0, 0.05) for _ in range(1536)) + "]",
        compact([r.randrange(0, 2**31) for _ in range(n)]),
        compact([hexs(r, r.randrange(8, 14)) for _ in range(10000 // scale + 300)]),
        compact({"field_%03d" % i: i * 7 for i in range(100)}),
    ]

# ---------------------------------------------------------------- numbers (design 5.4)

NUMBERS = [
    # integers: every packed width, the inline word range, int64, beyond
    '0', '-0', '1', '9', '10', '31', '32', '-1', '127', '128', '-128', '-129', '255', '256', '32767', '32768', '-32768', '-32769',
    '2147483647', '2147483648', '-2147483648', '-2147483649', '36028797018963967', '36028797018963968', '-36028797018963968',
    '-36028797018963969', '1152921504606846975', '1152921504606846976', '-1152921504606846976', '-1152921504606846977',
    '9223372036854775807', '9223372036854775808', '-9223372036854775808', '-9223372036854775809', '18446744073709551615',
    '18446744073709551616', '99999999999999999999', '100000000000000000000', '123456789012345678901234567890', '-123456789012345678901234567890',
    '1' * 308, '0.' + '1' * 300,
    # canonical doubles and their non-canonical spellings
    '0.0', '-0.0', '0.00', '-0.00', '0.000', '1.0', '1.00', '-1.0', '1.5', '1.50', '1.500', '0.1', '0.10', '98.6', '19.90', '19.9', '100.0',
    '100.00', '100000.0', '3.14159', '12.50', '4.35', '0.30000000000000004', '123456789.125', '0.5', '0.25', '-0.125',
    # the plain / exponent thresholds: 1e-6 and 1e21
    '0.000001', '0.0000001', '0.00000123', '0.000001234567890123', '0.0000012345678901234', '1e-6', '1e-7', '1E-7', '1e-07', '1.5e-7',
    '1.0e-7', '100000000000000000000.0', '999999999999999900000.0', '123456789012345680000.0', '123456789012345678901.0',
    '1000000000000000000000.0', '1e21', '1e+21', '1E+21', '1.5e+21', '1.5e21', '1.50e+21', '1e22', '1e+22', '10000000000000000.0',
    '1e5', '1E5', '1e+5', '1.0e5', '1.0E3', '1e0', '0e0', '0E0', '-0e0', '0.0e0', '-1E-2', '1e-5', '2.5e-7', '1e-400', '5e-325',
    # 15, 16, 17 and more significant digits
    '123456789012345.6', '1234567890123456.7', '0.1234567890123456', '0.12345678901234567', '0.123456789012345678',
    '1.2345678901234567', '1.23456789012345678', '9007199254740993.0', '9007199254740992.0', '0.1000000000000000055511151231257827',
    '5.436332503450103e-05', '5.436332503450103e-5', '-9.668080747360364e-05', '2.2250738585072014e-308', '2.225073858507201e-308',
    '5e-324', '4.9e-324', '4.9406564584124654e-324', '1.7976931348623157e308', '1.7976931348623157e+308', '1.7976931348623157E+308',
    '1.7976931348623158e+308', '1.5e300', '1.5e+300', '1e-300', '1e+300',
    # near powers of ten
    '0.09999999999999999', '0.1', '0.10000000000000002', '0.9999999999999999', '1.0000000000000002', '9.999999999999998',
    '10.000000000000002', '99999999999999.98', '999999999999999.9', '1000000000000000.1', '0.000009999999999999999', '0.0000010000000000000002',
    '999999999999999900000.0', '999999999999999868928.0',
]

def decimal_texts():
    """Decimals (design 5.4) and their neighbours: every scale from 1 to past the limit of 31, 15 to 19 digits, zeros
    after the point, values at each width the digits are stored in, digits around 2^53 (where dividing by the power of
    ten stops being exact), 2^61 / 22 (the most a spine word holds, with a scale up to 22) and 10^18, negative values
    and the negative zeros that are not decimals."""
    out = []
    for m in (2**53 - 1, 2**53, 2**53 + 1, 2**61 // 22 - 1, 2**61 // 22, 10**17 - 1, 10**17, 10**18 - 1, 10**18, 10**18 + 1,
              123456789012345678, 1234567890123456789, 12345678901234567, 1234567890123456):
        digits = str(m)
        for k in (1, 2, 9, 15, 16, 17, 18, 19, 22, 23, 30, 31, 32):
            text = digits[:-k] + "." + digits[-k:] if k < len(digits) else "0." + "0" * (k - len(digits)) + digits
            out += [text, "-" + text]
    for k in range(17, 35):
        out += ["1." + "5" * k, "0." + "0" * (k - 1) + "1", "-0." + "0" * (k - 1) + "7", "0." + "0" * k, "-0." + "0" * k, "0." + "9" * k]
    for k in range(1, 17):
        out += ["1." + "5" * k, "0." + "0" * (k - 1) + "1", "-0." + "0" * (k - 1) + "7", "0." + "0" * k, "-0." + "0" * k,
                "12." + "3" * k, "-98765." + "4" * k, "0." + "9" * k]
    out += ["1.27", "1.28", "-1.28", "-1.29", "327.67", "327.68", "-327.68", "-327.69", "21474836.47", "21474836.48", "-21474836.48",
            "-21474836.49", "9999999999999.99", "99999999999999.9", "999999999999999.9", "99999999999999.99", "-9999999999999.99",
            "0.999999999999999", "0.9999999999999999", "12345678901234.5", "1234567890123.45", "123456789012345.6", "0.000000000000001",
            "0.0000000000000001", "100.0", "100.00", "19.90", "0.05", "98.6", "1.0", "0.10", "10.0", "5.00", "0.5", "0.25", "0.125",
            "1.10", "2.2", "3.300", "0.07", "-0.07", "127.0", "128.0", "-128.0", "-129.0", "3276.7", "3276.8", "214748364.7", "214748364.8"]
    return out

def divide(text):
    """The value of a decimal as dividing its digits by the power of ten gives it: two roundings when the digits are
    beyond 2^53 or the power beyond 10^22."""
    digits, k = int(text.replace(".", "")), len(text.split(".")[1])
    return float(digits) / float(10 ** k)

def ulp_off_decimals(n, seed=11):
    """Decimals of 16 to 18 digits whose value is one unit in the last place away from what the division gives."""
    r, out = random.Random(seed), []
    while len(out) < n:
        digits, k = str(r.randrange(10 ** 15, 10 ** 18)), r.randint(1, 31)
        text = digits[:-k] + "." + digits[-k:] if k < len(digits) else "0." + "0" * (k - len(digits)) + digits
        if divide(text) != float(text): out.append(text if r.random() < .8 else "-" + text)
    return out

def wide_decimal_docs():
    """Decimals of 16 to 18 digits where they are stored differently: alone, as members, in arrays small and large,
    with one scale and with many, beside 15-digit ones, and with one element that the array's kind cannot hold."""
    r = random.Random(12)
    off = ulp_off_decimals(400)
    docs = list(off) + ['{"v":%s,"w":[%s,"s"]}' % (t, t) for t in off[:100]]
    for n in (1, 2, 3, 100, 128, 129, 130, 300, 1536):
        full = [repr(r.gauss(0, 0.05) * 1.0000001) for _ in range(n)]
        full = [t for t in full if "e" not in t]
        docs.append("[" + ",".join(full) + "]")                                                   # many scales
        docs.append("[" + ",".join(full + ["0.300000000000000004"]) + "]")                         # one that a word does not hold
        docs.append("[" + ",".join(full + ["0.5", "7.25"]) + "]")
        docs.append("[" + ",".join(full + ["3"]) + "]")
        docs.append("[" + ",".join("%.17f" % r.uniform(-9, 9) for _ in range(n)) + "]")            # one scale, 17 or 18 digits
        docs.append("[" + ",".join("%.17f" % r.choice([r.uniform(-9, 9), r.uniform(-.01, .01)]) for _ in range(n)) + "]")  # with 15-digit ones
        docs.append("[" + ",".join("%.16f" % r.uniform(-99, 99) for _ in range(n)) + "]")
        docs.append("[" + ",".join(r.choice(off) for _ in range(n)) + "]")
        docs.append("{" + ",".join('"m%d":%s' % (i, t) for i, t in enumerate(full)) + "}")
    return docs

def literal_array_docs():
    """Arrays of only null, false and true, for the array kind that holds them a byte each: at sizes around the packed
    caps, pure, with one other element, and nested."""
    r, docs = random.Random(14), []
    for n in (1, 2, 3, 4, 5, 15, 16, 17, 127, 128, 129, 130, 255, 256, 257, 2000, 70000):
        lits = [r.choice(["null", "true", "false"]) for _ in range(n)]
        docs.append("[" + ",".join(lits) + "]")
        docs.append("[" + ",".join(["null"] * n) + "]")
        docs.append("[" + ",".join(["true", "false"] * n) + "]")
        if n <= 2000:
            for other in ("0", '"x"', "[]", "0.5"):
                docs.append("[" + ",".join(lits[:n // 2] + [other] + lits[n // 2:]) + "]")
            docs.append('{"flags":[' + ",".join(lits) + '],"more":[[' + ",".join(lits) + "],[" + ",".join(lits[::-1]) + "]]}")
            docs.append("{" + ",".join('"f%d":%s' % (i, t) for i, t in enumerate(lits)) + "}")
    return docs

def boundary_doubles():
    """Doubles around the range a spine word holds inline, [2^-511, 2^513)."""
    out = []
    for e in (-1074, -1022, -513, -512, -511, -510, -1, 0, 1, 511, 512, 513, 514, 1023):
        for m in (1.0, 1.5, 1.9999999999999998):
            out.append(repr(m * 2.0 ** e) if e > -1070 else repr(2.0 ** e))
            out.append(repr(-(m * 2.0 ** e)) if e > -1070 else repr(-(2.0 ** e)))
    return out

def number_docs():
    """Every boundary number as a root, a member, and an element of each kind of array."""
    nums = [n for n in NUMBERS + decimal_texts() + boundary_doubles() if n]
    docs = list(nums)
    docs += ['{"n":%s}' % n for n in nums]
    docs += ['[%s]' % n for n in nums]
    docs += ['[%s,%s]' % (n, n) for n in nums]
    docs += ['[1,%s]' % n for n in nums] + ['[%s,"s"]' % n for n in nums] + ['[0.5,%s]' % n for n in nums]
    docs += ['[0.50,%s]' % n for n in nums] + ['[%s,0.25,0.75]' % n for n in nums] + ['[%s,%s,1.5]' % (n, n) for n in nums]
    # typed arrays that are long enough to leave the packed form under the default caps
    docs += ['[' + ','.join([n] * 130) + ']' for n in nums[::7]]
    docs.append('[' + ','.join(nums) + ']')
    docs.append('{' + ','.join('"k%d":%s' % (i, n) for i, n in enumerate(nums)) + '}')
    return docs

def num_text(r, style):
    if style == 0: return str(r.randrange(-300, 70000))
    if style == 1: return "%.2f" % r.uniform(-50, 50)
    if style == 2: return "%.6f" % r.gauss(0, 0.05)
    if style == 3: return repr(r.uniform(-1e3, 1e3))
    if style == 4: return repr(round(r.uniform(-90, 90), r.randrange(0, 8)))
    if style == 5: return r.choice(NUMBERS)
    if style == 6: return "%.*f" % (r.randrange(1, 17), r.uniform(-1e4, 1e4))
    if style == 7: return str(r.randrange(-2**63, 2**63))
    return repr(f32(r.gauss(0, 0.05)))

def array_docs(n_docs, seed=11):
    """Arrays of every number style, pure and mixed: typed, fixed-k and plain, at every size class."""
    r = random.Random(seed)
    docs = []
    for i in range(n_docs):
        parts = []
        for name in "abcd":
            style = r.randrange(9)
            n = r.choice([0, 1, 2, 3, 8, 40, 130, 300])
            if r.random() < .25: parts.append('"%s":[%s]' % (name, ",".join(num_text(r, r.randrange(9)) for _ in range(n))))
            else: parts.append('"%s":[%s]' % (name, ",".join(num_text(r, style) for _ in range(n))))
        parts.append('"t":[%s]' % ",".join('"%s"' % ("w" * r.randrange(0, 12)) for _ in range(r.choice([0, 2, 5]))))
        parts.append('"n":%s' % num_text(r, r.randrange(9)))
        parts.append('"o":{"s":"%s","in":[%s],"tt":["x","y"]}' % ("p" * r.choice([0, 5, 60, 64, 150, 180, 200, 230]), ",".join(num_text(r, 1) for _ in range(r.choice([0, 3])))))
        parts.append('"s":"%s"' % ("q" * r.choice([0, 3, 40, 63, 64, 120])))
        docs.append("{" + ",".join(parts) + "}" if i % 3 else "[" + ",".join(p.split(":", 1)[1] for p in parts) + "]")
    return docs

# ---------------------------------------------------------------- strings

def string_docs():
    docs = ['""', '"a"', '"abcdefg"', '"abcdefgh"', r'"esc \" \\ \/ \b \f \n \r \t A é € 😀 \u0000 end"', '"café € \U0001F600"',
            r'"\u001f\u007f"', r'"\ud83d\ude00\uD83D\uDE00"', r'"\u00e9\u20ac\uffff\ud7ff\ue000"', r'"\/"', '"/"', '"\x7f"', r'"\u0022\u005c\u002f"',
            r'"\u005C"', '"\u0080\u07ff\u0800\uffff\U00010000\U0010ffff"', r'"\uDBFF\uDFFF"', '"tab\\tnl\\nq\\"bs\\\\"']
    # every control character and every two-character escape, alone and in the middle of a long string
    for cp in list(range(0x20)) + [0x22, 0x5c, 0x2f, 0x7f]:
        docs.append('"\\u%04x"' % cp)
        docs.append('"%s\\u%04X%s"' % ("a" * 70, cp, "b" * 70))
    # every printable ASCII byte raw (except the two that must be escaped)
    docs.append('"' + "".join(chr(c) for c in range(0x20, 0x7f) if c not in (0x22, 0x5c)) + '"')
    # lengths around every header boundary: inline word, short string tag, 8-bit and 16-bit lengths
    for n in (1, 6, 7, 8, 9, 62, 63, 64, 65, 254, 255, 256, 257, 4000, 65534, 65535, 65536, 65537, 70000):
        for ch in ("x", "é", '\\n'):
            s = '"' + ch * n + '"'
            docs += [s, '[' + s + ']', '{"k":' + s + '}', '[' + s + ',' + s + ',1]']
    # the same for member names
    for n in (0, 1, 7, 8, 63, 64, 126, 127, 128, 255, 256, 300, 70000):
        docs.append('{"%s":1,"%s":{"%s":[]}}' % ("k" * n, "j" * n, "k" * n))
    docs.append('{"":1,"a\\nb":2,"é":3,"a\\u0062":4,"ab":5,"\\u0000":6,"\\"":7}')
    return docs

def string_array_docs():
    """Arrays of short strings, for the array kind that holds strings of at most 15 bytes inline: lengths 0, 7, 8,
    15 and 16, escapes and non-ASCII, at sizes around the packed caps, pure and with one element that does not fit."""
    pool = ["", "a", "abcdefg", "abcdefgh", "0123456789abcde", "0123456789abcdef", "\u00e9" * 7, "\u00e9" * 8, "a\"b\\c\nd!", "q\"\\\n\t\u0001x",
            "\U0001F600" * 3, "\U0001F600" * 4, "x" * 15, "/", "\x7f" * 9]
    sets = [pool[:3], pool[:5], pool[3:5], [pool[4]], pool[:6], [pool[6], pool[3]], [pool[6], pool[7]], [pool[8], pool[9]], [pool[10], pool[2]],
            [pool[10], pool[11]], [pool[12], pool[13], pool[14]], pool]
    r = random.Random(3)
    docs = []
    for n in (1, 2, 4, 5, 127, 128, 129, 130, 300, 3000):
        for kinds in sets:
            arr = [r.choice(kinds) for _ in range(n)]
            docs.append(json.dumps(arr, separators=(",", ":"), ensure_ascii=r.random() < .5))
            if n in (5, 129, 300):
                for other in (7, None, 1.5, "s" * 16, [], {"k": "0123456789"}):
                    mixed = list(arr)
                    mixed[r.randrange(n)] = other
                    docs.append(json.dumps(mixed, separators=(",", ":"), ensure_ascii=False))
                docs.append(json.dumps({"ids": arr, "n": n, "more": [arr[:3], arr]}, separators=(",", ":"), ensure_ascii=False))
    return docs

INVALID_UTF8 = [b'\xff', b'\xc0\x80', b'\xed\xa0\x80', b'\x80', b'\xc3', b'\xe2\x82', b'\xf0\x9f\x98', b'\xf4\x90\x80\x80', b'\xf8\x88\x80\x80\x80',
                b'\xe0\x80\x80', b'\xf0\x80\x80\x80', b'\xc1\xbf', b'\xfe', b'abc\xffdef', b'\xc3\xa9\xff\xc3\xa9', b'x' * 70 + b'\xff' + b'y' * 70]

def invalid_utf8_docs():
    """Documents (as bytes) that hold invalid UTF-8 in a string or a member name: valid JSON apart from that."""
    docs = []
    for bad in INVALID_UTF8:
        docs += [b'"' + bad + b'"', b'["a",{"k":"' + bad + b'"}]', b'{"' + bad + b'":1}', b'"\\n' + bad + b'\\t"']
    return docs

# ---------------------------------------------------------------- structure

def length_docs():
    """A string, a member name and number text of every length from 0 to 140, each also as the last bytes of the
    text and of the node it is stored in. The driver gives the engine exact-size copies, so under the address
    sanitizer any copy that reads or writes a byte too many is caught, whatever its length."""
    docs = []
    for n in range(0, 141):
        docs += ['"' + "x" * n + '"', '["' + "y" * n + '"]', '[{"' + "k" * n + '":1},{"' + "k" * n + '":"' + "v" * n + '"}]',
                 "0." + "1" * (n + 1), "[1," + "9" * (n + 1) + "]", '["\\n' + "z" * n + '"]']
    return docs

def deep(n, kind):
    if kind == "a": return "[" * n + "]" * n
    if kind == "o": return '{"a":' * n + "1" + "}" * n
    if kind == "m": return '[{"a":' * n + "[]" + "}]" * n
    return "[" * n + '"leaf"' + "]" * n

def deep_docs():
    """Deeply nested documents. Compact and canonical: the expected output is the input."""
    docs = []
    for n in (1, 2, 3, 15, 16, 17, 31, 32, 33, 100, 1000, 5000, 100000):
        for kind in "aoms":
            if n == 100000 and kind == "m": continue
            docs.append(deep(n, kind))
    docs.append("[" * 2000 + ",".join(["[1,2,{\"a\":[true]}]"] * 3) + "]" * 2000)
    return docs

def caps_docs():
    """Containers right at the limits of the packed form and of each field width."""
    docs = []
    counts = (0, 1, 2, 126, 127, 128, 129, 130, 254, 255, 256, 257, 65534, 65535, 65536, 65537)
    for n in counts:
        docs.append(compact(list(range(n))))                  # typed, widening with n
        docs.append("[" + ",".join(["null"] * n) + "]")       # plain
        docs.append("[" + ",".join(["0.5"] * n) + "]")        # typed doubles
        docs.append("[" + ",".join(["0.50"] * n) + "]")       # typed doubles, fixed format
        docs.append("[" + ",".join(['"s"'] * n) + "]")
    # exactly at the largest bytes cap (65535), and one past it
    for n in (65529, 65530, 65531): docs.append("[" + ",".join(["null"] * n) + "]")
    for n in (65531, 65532, 65533): docs.append("[" + ",".join(["7"] * n) + "]")
    for n in (0, 1, 2, 126, 127, 128, 129, 130, 253, 254, 255, 256, 257, 300, 65534, 65535, 65536, 70000):
        docs.append("{" + ",".join('"m%d":%d' % (i, i) for i in range(n)) + "}")
    # one string in an array: the packed size crosses 255 (size field width) and the bytes cap at 4096 and at 65535
    for n in list(range(245, 260)) + list(range(4080, 4100)) + list(range(65520, 65540)):
        docs.append('["%s"]' % ("s" * n))
        docs.append('{"k":"%s"}' % ("s" * n))
        docs.append('[[1,2,"%s"],{"k":"%s"}]' % ("s" * n, "t" * (n // 2)))
    # packed children of a spine container, and typed arrays inside it
    docs.append(compact([{"id": i, "v": [i, i + 1], "s": "x" * (i % 70)} for i in range(400)]))
    docs.append(compact({"k%d" % i: {"n": i, "s": "str%d" % i, "l": [i, i + 1]} for i in range(600)}))
    docs.append(compact({"k%d" % i: "v" * 40 for i in range(300)}))
    docs.append(compact([[i] * 200 for i in range(40)]))
    docs.append(compact([[i * 0.5] * 200 for i in range(40)]))
    docs.append(compact([list(range(-70000, 70000, 7)), [i * 0.5 for i in range(1000)], ["s%d" % i for i in range(1000)]]))
    # key IDs that take 2 and 3 varint bytes, used inside small packed objects of the same document
    big = "{" + ",".join('"u%d":%d' % (i, i) for i in range(20000)) + "}"
    docs.append('{"big":%s,"small":{"u19999":1,"u16500":{"u200":2,"u127":3,"u128":4},"u0":[{"u16383":5},{"u16384":6}]}}' % big)
    return docs

EDGE = [
    '\ufeff1', '\ufeff {"a":["\ufeff"]}', '\ufeff[]',  # One byte order mark before the text is skipped.
    'null', 'true', 'false', '[]', '{}', '[[]]', '[{}]', '{"a":{}}', '{"a":[]}', '[[[[[[[[[[[[1]]]]]]]]]]]]', '[[],[],{},{}]', '{"a":[],"b":{},"c":[[]]}',
    '[1,2,3]', '[1,-2,300]', '[1,2,70000]', '[1,2,5000000000]', '[1,2,18446744073709551615]', '[1.5,2.5]', '[1,2.5]', '[1.5,"a"]',
    '[0.5,0.25,-0.125]', '[0.1,0.5]', '[-0.0,0.0,1.0]', '[1e300,1e-300,5e-324]', '[null,true,false]', '["a","bcdefghij",""]',
    '[0.50,0.25]', '[0.50,0.2]', '[0.50,1]', '[0.50,1,"x"]', '[0.50,[0.25,0.5],{"a":0.75}]', '[1.10,2.20,3.30]', '[1.10,2.20,3.3]',
    '[0.000000,0.000001]', '[-0.00,0.00]', '[0.10,12345678901234.50]', '[0.10,123456789012345.50]', '[0.5,1e-7]', '[0.50,1e-7]',
    '[0.50,9223372036854775808]', '[100.0,2.0]', '[100.00,2.00]', '[0.123456789012345,0.100000000000000]', '[0.1234567890123456,0.1000000000000000]',
    ' { "a" : 1 , "b" : [ 1 , 2 ] , "c" : { "d" : null } } ', '\t[\n1,\r\n2 ]\n', ' \n\r\t1\t\r\n ', '[ ]', '{ }', '[\n]',
    '{"a":1,"b":2,"a":3}', '{"a":1,"a":2,"a":3}', '{"a":{"x":1},"b":2,"a":[1,2,3],"c":{"a":1,"a":{"q":[{"z":1,"z":2}]}}}',
    '{"k":"v","k":{"deep":{"deeper":"x"}},"k":"final","z":1}', '{"a":{"b":1},"a":{"c":2},"a":{"b":3}}', '{"a\\u0062":1,"ab":2}',
    '{"x":[{"d":1,"d":2}],"x":[{"d":3,"e":4,"d":5}]}',
    compact({"k%d" % i: i for i in range(40)}),
    compact(list(range(-70000, 70000, 7))), compact([i * 0.1 for i in range(1000)]),
    compact([[i, "x" * (i % 20), {"a": i}] for i in range(500)]),
    compact([2**62, -2**62, 2**60, 2**60 - 1, -2**60, -2**60 - 1, 2**63 - 1, -2**63, 2**64 - 1, 1e308, 1e-308, 1.5, 0.0, -0.0, 2.0**-600, 2.0**600, 3.0**-200] * 10),
    compact(["%07d" % i for i in range(200)] + ["12345678", "x" * 300, "é" * 3, "a\"b", "tab\t"]),
    '{"dup":' + json.dumps({"k%d" % (i % 37): i for i in range(20)}) + ',' + ','.join('"d%d":%d' % (i % 50, i) for i in range(200)) + '}',
    '{' + ','.join('"d%d":{"v":%d,"w":[%d]}' % (i % 150, i, i) for i in range(400)) + '}',
    '{' + ','.join('"e%d":%d' % (i % 300, i) for i in range(1000)) + '}',
    '{' + ','.join('"f%d":%d' % (i * 7919 % 5000, i) for i in range(12000)) + '}',
]

def random_docs(n, seed=7):
    r = random.Random(seed)
    budget = [0]
    def rnd(depth):
        budget[0] -= 1
        k = r.random()
        if depth > 5 or k < 0.35 or budget[0] <= 0:
            c = r.randrange(9)
            if c == 0: return None
            if c == 1: return r.random() < .5
            if c == 2: return r.randrange(-40, 40)
            if c == 3: return r.randrange(-2**63, 2**64)
            if c == 4: return r.choice([0.5, 1.25, -3.75, 1e100, 1e-7, 123.456, 0.1, 2.0**r.randrange(-1074, 1023)])
            if c == 5: return r.uniform(-1e6, 1e6)
            if c == 6: return "".join(r.choice("abc\"\\\n\u00e9\u20ac xyz") for _ in range(r.randrange(0, 12)))
            if c == 7: return "s" * r.randrange(0, 400)
            return r.randrange(-2**31, 2**31)
        if k < 0.6:
            c = r.randrange(4); m = r.choice([0, 1, 3, 10, 200])
            if c == 0: return [r.randrange(-2**r.randrange(1, 62), 2**r.randrange(1, 62)) for _ in range(m)]
            if c == 1: return [r.choice([0.5, 0.25, 1e10, 3.0]) if r.random() < .7 else r.random() for _ in range(m)]
            return [rnd(depth + 1) for _ in range(r.choice([0, 1, 2, 5, 20]))]
        return {r.choice(["a", "b", "key", "longer_key_name", "k%d" % r.randrange(300), "\u00e9", "q\"q"]): rnd(depth + 1)
                for _ in range(r.choice([0, 1, 2, 5, 12, 40, 200]))}
    docs = []
    for _ in range(n):
        budget[0] = 3000
        docs.append(json.dumps(rnd(0), separators=(",", ":"), ensure_ascii=r.random() < .5))
    # documents with duplicate member names at several levels (text built by hand)
    def dup_text(depth):
        budget[0] = 30
        if depth > 3 or r.random() < .3: return compact(rnd(4))
        if r.random() < .3: return "[" + ",".join(dup_text(depth + 1) for _ in range(r.randrange(0, 4))) + "]"
        m = r.choice([2, 5, 20, 60, 300]) if depth == 0 else r.choice([1, 2, 5]); pool = r.choice([2, 4, 30, 400])
        return "{" + ",".join('"m%d":%s' % (r.randrange(pool), dup_text(depth + 1)) for _ in range(m)) + "}"
    for _ in range(n * 3 // 8): docs.append(dup_text(0))
    return docs

# ---------------------------------------------------------------- rejected texts

# (text as latin-1, expected error). Every one is rejected from an ordinary client.
REJECT = [(t, "SYNTAX") for t in [
    '', ' ', '[', ']', '{', '}', '[1,]', '[,1]', '{"a":1,}', '{"a"}', '{"a":}', '{a:1}', '[1 2]', '01', '-', '1.', '.5', '1e', '1e+', '+1', '-01', '1.e5',
    '0x10', '1.5.2', '--1', '"abc', '"\\x"', '"\\u12"', '"\\u12g4"', '"\\ud800"', '"\\udc00"', '"\\ud800\\u0041"', '"\\ud800\\ud800"', '"a\tb"', '"a\nb"',
    '"\x00"', '"\x1f"', 'tru', 'nul', 'falsee', 'True', 'NULL', '1 2', '[1]]', '{"a":1}}', 'nan', 'NaN', 'Infinity', '-Infinity', '[1,2', '{"a":1', '"\\',
    '{"a":1 "b":2}', '{"a" 1}', '{1:2}', '[1,,2]', '{"k1":{"k2":{"k3":[1,2,{"k4":1,"k5":}]}}}', '{"a":{"b":1},"c":{"d":2},"e":[{"f":1},{"g":]}',
    "'a'", '[1}', '{"a":1]', '\xef\xbb\xbf', '\xef\xbb\xbf\xef\xbb\xbf1', ' \xef\xbb\xbf1', '1\xef\xbb\xbf', '[\xef\xbb\xbf1]', '\xef\xbb1', '1,', ',', ':', '{"a":1,"a"}', '{"a":1,"b":2,"a":[}',
    '{"a":1,"b":[1,2,{"c":"\x01"}]}',
]] + [(t, "NUM_RANGE") for t in [
    '1e400', '-1e400', '[1e999]', '{"a":1.8e308}', '1' * 400, '-' + '1' * 400, '1.7976931348623159e308', '2e308', '1' + '0' * 309,
    '1' * 310 + '.5', '[0.5,1e309]',
]] + [(t, "UTF8") for t in [
    '"\xff"', '"\xc0\x80"', '"\xed\xa0\x80"', '"\x80"', '"\xc3"', '"\xe2\x82"', '"\xf0\x9f\x98"', '"\xf4\x90\x80\x80"', '"\xf8\x88\x80\x80\x80"',
    '{"\xff":1}', '["ok","\xe0\x80\x80"]', '"abcdefgh\xc1\xbf"', '"\\n\xfe"', '"' + 'x' * 100 + '\xff"',
]]

def write_lines(path, lines):
    with open(path, "wb") as f:
        for l in lines:
            f.write((l if isinstance(l, bytes) else l.encode("utf-8")) + b"\n")

if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] != "corpus":
        sys.exit(__doc__)
    out, n = sys.argv[2], int(sys.argv[3])
    os.makedirs(out, exist_ok=True)
    for shape in SHAPES:
        write_lines(os.path.join(out, shape + ".jsonl"), records(shape, n))
    write_lines(os.path.join(out, "big.jsonl"), big(max(n // 10, 1)))
    for name, doc in spine_docs().items():
        write_lines(os.path.join(out, name + ".jsonl"), [doc])
