#!/usr/bin/env python3
#
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#
"""Differential test of json_num.c against Python.

    check_num.py [--scale S] [--jobs N] path/to/json_num_test

Python's repr(float) gives the shortest digits that round-trip, the closest to
the true value among those, and float(str) is correctly rounded for text of any
length. This script feeds the filter modes of json_num_test and checks that

  * jsonDoublePrint(d) is exactly the digits of repr(d) in the layout that
    json_num.h prescribes,
  * jsonDoubleParse(text) has the bits of float(text),
  * jsonDoublePrintFixed(d, frac) is what the definition in json_num.h says,
    worked out here from repr(d),
  * jsonDoubleFromDecimal(m, e, negative) has the bits of float("<m>e<e>").

--scale multiplies the size of every random corpus (default 1.0: about 33
million doubles printed, 15 million texts parsed, 4 million fixed prints,
6 million decimals converted).
"""

import argparse
import math
import os
import random
import struct
import subprocess
import sys
from decimal import Decimal, getcontext
from multiprocessing import Pool

INF = math.inf
BIN = None
getcontext().prec = 3000  # enough for the exact value of any double or midpoint

# --- What json_num.h says, derived from repr() -------------------------------


def shortest(d):
    """(negative, digits, n): d is 0.DIGITS * 10^n with the digits of repr(d),
    no leading or trailing zeros. Zero gives empty digits."""
    r = repr(d)
    neg = r[0] == "-"
    if neg:
        r = r[1:]
    e = 0
    if "e" in r:
        r, e = r.split("e")
        e = int(e)
    ip, _, fp = r.partition(".")
    digits = (ip + fp).lstrip("0")
    n = len(ip) + e - (len(ip) + len(fp) - len(digits))
    return neg, digits.rstrip("0"), n


def canonical(d):
    neg, digits, n = shortest(d)
    k = len(digits)
    if k == 0:
        s = "0.0"
    elif k <= n <= 21:
        s = digits + "0" * (n - k) + ".0"
    elif 0 < n <= 21:
        s = digits[:n] + "." + digits[n:]
    elif -6 < n <= 0:
        s = "0." + "0" * -n + digits
    else:
        e = n - 1
        s = digits[0] + ("." + digits[1:] if k > 1 else "") + ("e-" if e < 0 else "e+") + str(abs(e))
    return "-" + s if neg else s


def fixed(d, frac):
    """Expected text of jsonDoublePrintFixed, '' for a 0 return."""
    if not 1 <= frac <= 15 or not math.isfinite(d):
        return ""
    neg, digits, n = shortest(d)
    k = len(digits)
    if k == 0:
        s = "0." + "0" * frac
    else:
        if abs(d) >= 1e15:
            return ""
        if max(0, k - n) > frac:  # the shortest digits have more than frac fractional digits
            return ""
        if n > 0:
            ip, fp = (digits + "0" * (n - k))[:n], digits[n:]
        else:
            ip, fp = "0", "0" * -n + digits
        s = ip + "." + fp + "0" * (frac - len(fp))
        if len((ip + fp + "0" * (frac - len(fp))).lstrip("0")) > 15:  # significant digits of the result
            return ""
        assert float(s) == abs(d) and len(s.split(".")[1]) == frac
    return "-" + s if neg else s


# --- Running the C side ------------------------------------------------------


def run(mode, data):
    p = subprocess.run([BIN, mode], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.stdout, p.returncode, p.stderr.decode(errors="replace")


def check_print(name, doubles):
    doubles = [d for d in doubles if math.isfinite(d)]
    out, rc, err = run("print", struct.pack("<%dd" % len(doubles), *doubles))
    got = out.decode().split("\n")[:-1]
    fails = []
    if rc != 0 or len(got) != len(doubles):
        fails.append("%s: print mode exit %d, %d of %d lines: %s" % (name, rc, len(got), len(doubles), err[:2000]))
    for d, g in zip(doubles, got):
        if g != canonical(d):
            fails.append("%s: print %r (%s): got %s want %s" % (name, d, d.hex(), g, canonical(d)))
            if len(fails) > 10:
                break
    return name, len(doubles), fails, 0


def check_parse(name, texts):
    out, rc, err = run("parse", ("\n".join(texts) + "\n").encode())
    want = struct.pack("<%dd" % len(texts), *map(float, texts))
    fails = []
    if rc != 0 or len(out) != len(want):
        fails.append("%s: parse mode exit %d, %d of %d bytes: %s" % (name, rc, len(out), len(want), err[:2000]))
    elif out != want:
        for i, t in enumerate(texts):
            if out[8 * i : 8 * i + 8] != want[8 * i : 8 * i + 8]:
                g = struct.unpack("<d", out[8 * i : 8 * i + 8])[0]
                fails.append(
                    "%s: parse %s: got %r (%s) want %r (%s)" % (name, t[:120], g, g.hex(), float(t), float(t).hex())
                )
                if len(fails) > 10:
                    break
    return name, len(texts), fails, 0


def check_fixed(name, records):
    data = b"".join(struct.pack("<di", d, f) for d, f in records)
    out, rc, err = run("fixed", data)
    got = out.decode().split("\n")[:-1]
    fails = []
    if rc != 0 or len(got) != len(records):
        fails.append("%s: fixed mode exit %d, %d of %d lines: %s" % (name, rc, len(got), len(records), err[:2000]))
    zero = 0
    for (d, f), g in zip(records, got):
        zero += g == ""
        if g != fixed(d, f):
            fails.append("%s: fixed(%r, %d): got '%s' want '%s'" % (name, d, f, g, fixed(d, f)))
            if len(fails) > 10:
                break
    return name, len(records), fails, zero


# --- Corpora of doubles ------------------------------------------------------


def nextafter(d, to):
    return math.nextafter(d, to)


def around(d, ulps):
    out, lo, hi = [d], d, d
    for _ in range(ulps):
        lo, hi = nextafter(lo, -INF), nextafter(hi, INF)
        out += [lo, hi]
    return out


def random_bits(seed, n):
    """Uniform 64-bit patterns: every binary exponent equally likely."""
    return struct.unpack("<%dd" % n, random.Random(seed).randbytes(8 * n))


def subnormals(seed, n):
    rnd = random.Random(seed)
    return [
        struct.unpack("<d", struct.pack("<Q", rnd.getrandbits(rnd.randint(1, 52)) | rnd.getrandbits(1) << 63))[0]
        for _ in range(n)
    ]


def special_doubles():
    out = [0.0, -0.0, 5e-324, 1.7976931348623157e308, 2.2250738585072014e-308, 2.225073858507201e-308]
    for e in range(-1074, 1024):  # every power of two and its neighbours
        out += around(math.ldexp(1.0, e), 2)
    for e in range(-323, 309):  # every power of ten and its neighbours
        out += around(float("1e%d" % e), 2)
    for c in (1e-7, 1e-6, 1e-5, 1e-4, 1.0, 1e15, 1e16, 1e17, 1e20, 1e21, 1e22, 1e23, 2.0**53, 2.0**63, 2.0**64):
        out += around(c, 2000)  # where the layout or the algorithm changes
    out += around(1.7976931348623157e308, 2000) + around(2.2250738585072014e-308, 2000)
    out += [i * 5e-324 for i in range(1, 30000)]  # the smallest subnormals
    for base in (2**53, 2**54, 2**55, 2**60, 2**63, 2**64, 10**16, 10**17, 10**18, 10**19, 10**20, 10**21):
        out += [float(base + i) for i in range(-3000, 3000)]  # 9007199254740993-style integers
    out += [float(9007199254740993), float(9007199254740995), float(18014398509481985), float(123456789012345678)]
    return out + [-d for d in out]


def layout_doubles(seed, n):
    """Random values around the plain/exponent switches and with every length."""
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        lo, hi = rnd.choice(((-9, -4), (18, 24), (-2, 2), (13, 18), (-25, 25)))
        d = rnd.uniform(1, 10) * 10.0 ** rnd.randint(lo, hi)
        if rnd.random() < 0.5:
            d = float("%.*g" % (rnd.randint(1, 17), d))
        out.append(-d if rnd.random() < 0.3 else d)
    return out


def whole_doubles(seed, n):
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        v = rnd.getrandbits(rnd.randint(1, 70))
        if rnd.random() < 0.4:
            v = rnd.randrange(1, 10 ** rnd.randint(1, 6)) * 10 ** rnd.randint(0, 20)
        out.append(float(-v if rnd.random() < 0.3 else v))
    return out


def decimal_text(rnd):
    """A decimal of 1 to 17 significant digits, the way prices, coordinates and
    embedding components are written."""
    nd = rnd.randint(1, 17)
    digits = str(rnd.randrange(10 ** (nd - 1), 10**nd))
    style = rnd.random()
    if style < 0.35:
        point = rnd.randint(0, nd)
        t = (digits[:point] or "0") + ("." + digits[point:] if point < nd else "")
    elif style < 0.6:
        t = "0." + "0" * rnd.randint(0, 4) + digits  # embedding component
    elif style < 0.8:
        t = "%d.%02d" % (rnd.randrange(10 ** rnd.randint(1, 9)), rnd.randrange(100))  # price
    else:
        t = digits[0] + ("." + digits[1:] if nd > 1 else "") + "e%d" % rnd.randint(-330, 308)
    return "-" + t if rnd.random() < 0.25 else t


def decimal_texts(seed, n):
    rnd = random.Random(seed)
    return [decimal_text(rnd) for _ in range(n)]


# --- Corpora of text ---------------------------------------------------------


def repr_texts(seed, n):
    """Random doubles in the notations other writers use."""
    rnd = random.Random(seed)
    out = []
    for d in random_bits(seed, n):
        if not math.isfinite(d):
            continue
        style = rnd.randrange(6)
        if style == 0:
            t = repr(d)
        elif style == 1:
            t = "%.17g" % d
        elif style == 2:
            t = "%.*e" % (rnd.randint(0, 30), d)
        elif style == 3:
            t = "%.*E" % (rnd.randint(14, 18), d)
        elif style == 4:
            t = str(Decimal(d)) if rnd.random() < 0.03 else "%.16g" % d
        else:
            t = "%.*g" % (rnd.randint(1, 15), d)
        out.append(t)
    return out


def digit_string(rnd, nd):
    return str(rnd.randrange(1, 10)) + "".join(rnd.choices("0123456789", k=nd - 1))


def random_texts(seed, n, lengths):
    """Random digit strings of the given lengths, with the decimal point and
    the exponent placed so that most values are finite and nonzero."""
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        nd = rnd.choice(lengths)
        digits = digit_string(rnd, nd)
        style = rnd.randrange(5)
        if style == 0:  # integer with exponent
            t = digits + "e%d" % (rnd.randint(-345, 310) - nd)
        elif style == 1:  # D.DDD with exponent
            t = digits[0] + "." + digits[1:] + rnd.choice("eE") + rnd.choice(("", "+", "-")) + str(rnd.randint(0, 330))
        elif style == 2:  # plain, point anywhere
            point = rnd.randint(1, nd)
            t = digits[:point] + ("." + digits[point:] if point < nd else "")
        elif style == 3:  # leading zeros after the point, zero-padded exponent
            t = (
                "0."
                + "0" * rnd.randint(0, 40)
                + digits
                + "e%s%0*d" % (rnd.choice(("", "+", "-")), rnd.randint(1, 6), rnd.randint(0, 40))
            )
        else:  # trailing zeros, point inside
            t = digits + "0" * rnd.randint(0, 40)
            point = rnd.randint(1, len(t))
            t = t[:point] + "." + t[point:] + "0" * rnd.randint(1, 5) + "e%d" % rnd.randint(-340, 300)
        out.append("-" + t if rnd.random() < 0.2 else t)
    return out


def with_exponent(mantissa, e, rnd):
    """Writes the integer digit string `mantissa` times 10^e in one of the forms
    JSON allows."""
    style = rnd.randrange(3)
    if style == 0:
        return mantissa + "e%d" % e
    if style == 1:
        return mantissa[0] + "." + mantissa[1:] + "0E%+d" % (e + len(mantissa) - 1)
    if e < 0 and -e < len(mantissa) + 400:  # plain notation
        if -e < len(mantissa):
            return mantissa[:e] + "." + mantissa[e:]
        return "0." + "0" * (-e - len(mantissa)) + mantissa
    return mantissa + ("0" * e if 0 <= e < 400 else "e%d" % e)


def halfway_texts(seed, n):
    """The exact midpoint of two adjacent doubles, where rounding is a tie, and
    the same value moved up or down by very little."""
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        if i % 4 == 0:
            d = struct.unpack("<d", struct.pack("<Q", rnd.getrandbits(52)))[0]  # subnormal
        elif i % 4 == 1:
            d = float(rnd.getrandbits(rnd.randint(53, 64)))  # 9007199254740993-style integer
        elif i % 4 == 2:
            d = rnd.uniform(1, 10) * 10.0 ** rnd.randint(-30, 30)
        else:
            d = abs(struct.unpack("<d", struct.pack("<Q", rnd.getrandbits(63)))[0])
        if not math.isfinite(d):
            continue
        up = nextafter(d, INF)
        if up == INF:
            continue
        mid = (Decimal(d) + Decimal(up)) / 2
        for value in (mid, Decimal(d)) if i % 8 == 0 else (mid,):
            _, digits, e = value.as_tuple()
            m = "".join(map(str, digits)).lstrip("0")
            if not m:
                continue
            out.append(with_exponent(m, e, rnd))  # the tie itself
            for pad in (0, rnd.randint(1, 40), 800 - len(m) + rnd.randint(-3, 3), rnd.randint(800, 1500)):
                pad = max(pad, 0)
                out.append(with_exponent(m + "0" * pad + "1", e - pad - 1, rnd))  # just above
                below = str(int(m) - 1)
                if below != "0":
                    out.append(with_exponent(below + "9" * (pad + 1), e - pad - 1, rnd))  # just below
    return out


def edge_texts():
    max_mid = (Decimal(1.7976931348623157e308) + Decimal(2) ** 1024) / 2  # rounds to infinity
    min_mid = Decimal(2) ** -1075  # rounds to zero
    out = ["0", "-0", "0.0", "-0.0", "0e0", "0E+0", "0e-0", "0.000", "-0.000e-999", "0e999999999999999999999"]
    out += [
        "1",
        "-1",
        "10",
        "1.0",
        "1e0",
        "1E0",
        "1e+0",
        "1e-0",
        "1e00000000000000000000001",
        "1.5",
        "0.5",
        "0.1",
        "0.2",
        "0.3",
    ]
    out += (
        ["1e%d" % e for e in range(-400, 400)]
        + ["1E+%d" % e for e in range(400)]
        + ["9e%d" % e for e in range(-400, 400)]
    )
    out += [
        "9007199254740993",
        "9007199254740993.0",
        "9007199254740992.9999999999",
        "9007199254740993.0000000000000000000001",
    ]
    out += [
        "9007199254740995",
        "9007199254740994.99999999999999999999",
        "18014398509481985",
        "18014398509481986",
        "18014398509481987",
    ]
    out += ["%d" % (2**53 + i) for i in range(-50, 200)] + ["%d.5" % (2**52 + i) for i in range(-50, 200)]
    out += ["%d" % (2**64 + i * 1024) for i in range(-50, 200)] + ["%d" % (10**19 + i) for i in range(-50, 200)]
    out += [
        "5e-324",
        "4.9e-324",
        "4.9406564584124654e-324",
        "2.4703282292062327e-324",
        "2.4703282292062328e-324",
        "3e-324",
        "2e-324",
        "1e-324",
    ]
    out += [
        "7.4109846876186981e-324",
        "7.4109846876186982e-324",
        "2.2250738585072011e-308",
        "2.2250738585072012e-308",
        "2.2250738585072014e-308",
    ]
    out += [
        "1.7976931348623157e308",
        "1.7976931348623158e308",
        "1.7976931348623159e308",
        "1.797693134862315807e308",
        "1.797693134862315808e308",
    ]
    out += [
        "17976931348623157e292",
        "17976931348623158079e289",
        "17976931348623158080e289",
        "1.8e308",
        "2e308",
        "1e309",
        "-1e309",
    ]
    out += [
        "1e999999999999999999999",
        "-1e999999999999999999999",
        "1e-999999999999999999999",
        "123456789e-999999999999999999999",
    ]
    out += ["0." + "0" * z + "1" for z in range(0, 420, 7)] + ["1" + "0" * z for z in range(0, 420, 7)]
    out += ["0." + "0" * z + "17976931348623157e" + str(z + 309) for z in range(0, 420, 7)]
    out += ["1" + "0" * z + "e-" + str(z) for z in range(0, 900, 13)] + [
        "0." + "0" * z + "1e" + str(z + 1) for z in range(0, 900, 13)
    ]
    out += ["1" + "0" * z + ".0e-" + str(z + 320) for z in range(0, 900, 13)] + [
        "0." + "0" * z + "9e" + str(z + 309) for z in range(0, 900, 13)
    ]
    out += ["0." + "0" * 5000 + "1e5001", "1" + "0" * 5000 + "e-5000", "1" + "0" * 5000, "0." + "0" * 5000 + "1"]
    for mid in (max_mid, min_mid):
        _, digits, e = mid.as_tuple()
        m = "".join(map(str, digits))
        below = str(int(m) - 1)
        for pad in (0, 1, 30, 800, 2000):
            out += [
                m + "0" * pad + "e%d" % (e - pad),
                m + "0" * pad + "1e%d" % (e - pad - 1),
                below + "9" * (pad + 1) + "e%d" % (e - pad - 1),
            ]
    out += [
        "2.2250738585072012e-308",
        "2.2250738585072011360574097967091319759348195463516456480234261097248222220210769455165295239081350879141491589130396211068700864386945946455276572074078206217433799881410632673292535522868813721490129811224514518898490572223072852551331557550159143974763979834118019993239625482890171070818506906306666559949382757725720157630626906633326475653000092458883164330377797918696120494973903778297049050510806099407302629371289589500035837999672072543043602840788957717961509455167482434710307026091446215722898802581825451803257070188608721131280795122334262883686223215037756666225039825343359745688844239002654981983854879482922068947216898310996983658468140228542433306603398508864458040010349339704275671864433837704860378616227717385456230658746790140867233276367187499e-308",
    ]
    return out + ["-" + t for t in out[::3] if t[0] != "-"]


# --- Fixed -------------------------------------------------------------------


def fixed_records(seed, n):
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        frac = rnd.randint(1, 15)
        whole = rnd.randrange(10 ** rnd.randint(0, 16)) if rnd.random() < 0.8 else 0
        t = "%d.%0*d" % (whole, frac, rnd.randrange(10**frac) if rnd.random() < 0.9 else 0)
        d = float("-" + t if rnd.random() < 0.2 else t)
        style = rnd.random()
        if style < 0.5:
            out.append((d, frac))  # as written
        elif style < 0.8:
            out.append((d, rnd.randint(1, 15)))  # too few or more digits than written
        elif style < 0.9:
            out.append((d, frac + rnd.choice((-1, 1))))
        elif style < 0.95:
            out.append((d, rnd.choice((0, -1, 16, 17, 100, -(2**31)))))
        else:
            out.append((d * 10.0 ** rnd.randint(-20, 5), rnd.randint(1, 15)))
    return out


def fixed_specials():
    ds = [
        0.0,
        -0.0,
        INF,
        -INF,
        math.nan,
        5e-324,
        1.7976931348623157e308,
        0.5,
        0.1,
        0.3,
        0.1 + 0.2,
        1e15,
        1e14,
        1e-15,
        1e-16,
    ]
    ds += around(1e15, 50) + around(1e14, 50) + around(1.0, 50) + around(1e-15, 50) + around(1e-6, 50)
    ds += [float(10**i) for i in range(23)] + [float(10**i - 1) for i in range(18)] + [10.0**-i for i in range(1, 20)]
    ds += [float(i) for i in range(1000)] + [i / 8 for i in range(1000)] + [i / 1000 for i in range(1000)]
    ds += [d for d in struct.unpack("<20000d", random.Random(7).randbytes(160000))]
    return [(s * d, f) for d in ds for f in range(0, 17) for s in (1.0, -1.0)]


# --- jsonDoubleFromDecimal ---------------------------------------------------


def check_decimal(name, records):
    """records: (mantissa, exp10, negative). Python's float() of the number written out is the reference."""
    data = b"".join(struct.pack("<Qii", m, e, neg) for m, e, neg in records)
    out, rc, err = run("decimal", data)
    want = struct.pack("<%dd" % len(records), *(float("%s%de%d" % ("-" if neg else "", m, e)) for m, e, neg in records))
    fails = []
    if rc != 0 or len(out) != len(want):
        fails.append("%s: decimal mode exit %d, %d of %d bytes: %s" % (name, rc, len(out), len(want), err[:2000]))
    elif out != want:
        for i, (m, e, neg) in enumerate(records):
            if out[8 * i : 8 * i + 8] != want[8 * i : 8 * i + 8]:
                g = struct.unpack("<d", out[8 * i : 8 * i + 8])[0]
                fails.append("%s: decimal %d * 10^%d negative=%d: got %r (%s)" % (name, m, e, neg, g, g.hex()))
                if len(fails) > 10:
                    break
    return name, len(records), fails, 0


def decimal_random(seed, n):
    """Mantissas of 1 to 19 digits, and any 64-bit one, with exponents over the whole range of doubles and past it."""
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        m = rnd.getrandbits(64) if i % 16 == 0 else rnd.randrange(10 ** rnd.randint(0, 19))
        span = rnd.choice(((-30, 30), (-30, 30), (-345, 310), (-400, 400)))
        out.append((m, rnd.randint(*span), rnd.random() < 0.2))
    return out


def decimal_fast_path(seed, n):
    """Both sides of every limit of the one-operation path: mantissas around 2^53, exponents around -22 and 22."""
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        m = 2**53 + rnd.randint(-3, 3) if i % 3 == 0 else rnd.getrandbits(rnd.randint(50, 56))
        e = rnd.choice((-24, -23, -22, -21, 21, 22, 23, 24)) if i % 2 == 0 else rnd.randint(-22, 22)
        out.append((m, e, rnd.random() < 0.2))
    return out


def decimal_halfway(seed, n):
    """Exact midpoints of two adjacent doubles that have at most 19 digits, and the numbers next to them."""
    rnd = random.Random(seed)
    out = []
    while len(out) < n:
        # Doubles in [2^(53-j), 2^(54-j)) are 2^-j apart for j >= 0 and 2^-j is a whole number for j < 0.
        j = rnd.randint(-10, 12)
        k = rnd.randrange(2**52, 2**53)
        mid = Decimal(2 * k + 1) / Decimal(2) ** (j + 1) if j >= 0 else Decimal((2 * k + 1) * 2 ** (-j - 1))
        _, digits, e = mid.as_tuple()
        m = int("".join(map(str, digits)))
        if m >= 10**19:
            continue
        pad = rnd.randint(0, 19 - len(str(m)))
        m, e = m * 10**pad, e - pad
        for near in (m, m - 1, m + 1):
            if 0 < near < 2**64:
                out.append((near, e, rnd.random() < 0.2))
    return out


def decimal_edges():
    out = [(0, 0, 0), (0, 5, 1), (0, -400, 0), (1, 400, 0), (1, 309, 0), (1, 308, 0), (2**64 - 1, 308, 1), (2**64 - 1, -343, 0)]
    for m in (1, 2, 4, 5, 9, 17976931348623157, 17976931348623158, 17976931348623159, 2**53, 2**63, 2**64 - 1, 10**19 - 1):
        for e in list(range(-345, -300)) + list(range(285, 312)):
            out.append((m, e, 0))
    # Around the smallest subnormal and half of it, and around the largest double.
    for m in range(2460, 2480):
        out.append((m, -327, 0))
    for m in range(4930, 4950):
        out.append((m, -327, 1))
    return out


# --- Driver ------------------------------------------------------------------

M = 1000000


def task(spec):
    kind, name, fn, args = spec
    data = fn(*args)
    if kind == "print":
        return check_print(name, data)
    if kind == "print-text":
        return check_print(name, list(map(float, data)))
    if kind == "parse":
        return check_parse(name, data)
    if kind == "decimal":
        return check_decimal(name, data)
    return check_fixed(name, data)


def specs(scale):
    def n(count):
        return max(1000, int(count * scale))

    out = []
    for i in range(max(1, round(24 * scale))):
        out.append(("print", "print random bit patterns", random_bits, (1000 + i, M if scale >= 1 / 24 else n(24 * M))))
    out.append(("print", "print powers of 2 and 10, boundaries, neighbours", special_doubles, ()))
    for i in range(2):
        out.append(("print", "print subnormals", subnormals, (2000 + i, n(M))))
        out.append(("print", "print around layout switches", layout_doubles, (2100 + i, n(M))))
        out.append(("print", "print whole values", whole_doubles, (2200 + i, n(M))))
    for i in range(3):
        out.append(("print-text", "print 1-17 digit decimals", decimal_texts, (2300 + i, n(M))))

    for i in range(4):
        out.append(("parse", "parse random doubles as %r/%g/%e text", repr_texts, (3000 + i, n(M))))
        out.append(("parse", "parse 1-17 digit decimals", decimal_texts, (3100 + i, n(M))))
        out.append(("parse", "parse 1-19 digit strings", random_texts, (3200 + i, n(M), list(range(1, 20)))))
    for i in range(2):
        out.append(("parse", "parse 20-40 digit strings", random_texts, (3300 + i, n(M), list(range(20, 41)))))
    out.append(("parse", "parse 30 digit strings", random_texts, (3400, n(300000), [30])))
    out.append(("parse", "parse 100 digit strings", random_texts, (3401, n(300000), [100])))
    out.append(("parse", "parse 800 digit strings", random_texts, (3402, n(100000), [800])))
    out.append(
        (
            "parse",
            "parse 760-810 and 1000-3000 digit strings",
            random_texts,
            (3403, n(60000), list(range(760, 811)) + [1000, 2000, 3000]),
        )
    )
    for i in range(4):
        out.append(("parse", "parse halfway cases and their neighbours", halfway_texts, (3500 + i, n(30000))))
    out.append(("parse", "parse edge cases", edge_texts, ()))

    for i in range(4):
        out.append(("decimal", "decimal random mantissa and exponent", decimal_random, (5000 + i, n(M))))
    for i in range(2):
        out.append(("decimal", "decimal around the limits of the fast path", decimal_fast_path, (5100 + i, n(M // 2))))
        out.append(("decimal", "decimal halfway cases and their neighbours", decimal_halfway, (5200 + i, n(M // 2))))
    out.append(("decimal", "decimal edge cases", decimal_edges, ()))

    for i in range(4):
        out.append(("fixed", "fixed random decimals, 1-15 fraction digits", fixed_records, (4000 + i, n(M))))
    out.append(("fixed", "fixed special values, every frac 0..16", fixed_specials, ()))
    return out


def set_binary(path):
    global BIN
    BIN = path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()
    totals, zeros, failures = {}, {}, []
    with Pool(args.jobs, set_binary, (os.path.abspath(args.binary),)) as pool:
        for name, count, fails, zero in pool.imap_unordered(task, specs(args.scale)):
            totals[name] = totals.get(name, 0) + count
            zeros[name] = zeros.get(name, 0) + zero
            failures += fails
    for name in sorted(totals):
        print("%12d  %s%s" % (totals[name], name, " (%d returned 0)" % zeros[name] if zeros[name] else ""))
    for kind in ("print", "parse", "fixed", "decimal"):
        print("%12d  total %s" % (sum(c for k, c in totals.items() if k.startswith(kind)), kind))
    if failures:
        print("\n".join(failures[:60]))
        sys.exit("check_num: %d FAILURES (%s)" % (len(failures), args.binary))
    print("check_num: ok (%s)" % args.binary)


if __name__ == "__main__":
    main()
