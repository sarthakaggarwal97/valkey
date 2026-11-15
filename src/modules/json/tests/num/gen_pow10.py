#!/usr/bin/env python3
#
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#
"""Generates and verifies the power-of-ten table of src/modules/json/json_num.c.

    gen_pow10.py            print the table rows
    gen_pow10.py --check F  verify that the table inside C file F is this table
    gen_pow10.py --update F rewrite the table inside C file F in place

Everything here is exact integer / rational arithmetic. Besides emitting the
table, every run re-proves the facts json_num.c relies on (see verify()).

Table. For JSON_POW10_MIN <= e <= JSON_POW10_MAX the entry is the 128-bit
integer g(e) = ceil(10^e * 2^(127 - L)), with L = floor(log2(10^e)), stored as
{high 64 bits, low 64 bits}. So 2^127 <= g(e) < 2^128, and with
beta = 10^e * 2^(127 - L) exactly, beta <= g(e) < beta + 1. The entries for
0 <= e <= JSON_POW10_EXACT_MAX are exact (g = beta).
"""

import re
import sys
from fractions import Fraction
from math import floor, log2

POW10_MIN = -342  # parser: smallest e with (10^19 - 1) * 10^e not rounding to zero for sure
POW10_MAX = 324  # printer: -k for the smallest double (k = -324)
POW10_EXACT_MAX = 55  # 5^55 < 2^128 <= 5^56

BEGIN = "/* BEGIN GENERATED TABLE (tests/num/gen_pow10.py) */"
END = "/* END GENERATED TABLE */"

Q_MIN, Q_MAX = -1074, 971  # a finite double is c * 2^q with Q_MIN <= q <= Q_MAX, c < 2^53


def floor_log2_pow10(e):
    """floor(log2(10^e)), exact."""
    if e >= 0:
        return (10**e).bit_length() - 1
    return -(10**-e).bit_length()  # 10^-e is never a power of two


def entry(e):
    shift = 127 - floor_log2_pow10(e)
    if e >= 0:
        g = 10**e << shift if shift >= 0 else -((-(10**e)) >> -shift)
    else:
        g = -((-(1 << shift)) // 10**-e)
    assert 1 << 127 <= g < 1 << 128, e
    return g


def table():
    return [entry(e) for e in range(POW10_MIN, POW10_MAX + 1)]


def rows():
    out = []
    for e, g in zip(range(POW10_MIN, POW10_MAX + 1), table()):
        out.append("    {0x%016X, 0x%016X}, /* 1e%d */" % (g >> 64, g & (2**64 - 1), e))
    return out


# --- The C formulas, transcribed. Python's >> on negative ints is an arithmetic shift. ---


def c_floor_log2_pow10(e):
    return (e * 217706) >> 16


def c_floor_log10_pow2(e):
    return (e * 1262611) >> 22


def c_floor_log10_three_quarters_pow2(e):
    return (e * 1262611 - 524031) >> 22


def floor_log10(x):
    """floor(log10(x)) for a positive Fraction, exact."""
    k = len(str(x.numerator)) - len(str(x.denominator))
    while Fraction(10) ** k > x:
        k -= 1
    while Fraction(10) ** (k + 1) <= x:
        k += 1
    return k


def one_sided_minima(alpha, limit):
    """For a rational alpha whose denominator exceeds `limit`, returns
    (min frac(n*alpha), min 1 - frac(n*alpha)) over 1 <= n <= limit.

    By the three-distance theorem the successive record lows and record highs
    of frac(n*alpha) are produced by the subtractive Euclidean algorithm on
    (lo, up) = (frac(alpha), 1 - frac(alpha)): the next record is at
    n = n_lo + n_up and replaces the larger of lo, up by their difference."""
    assert alpha.denominator > limit
    lo = alpha - floor(alpha)
    up = 1 - lo
    n_lo = n_up = 1
    while True:
        if lo > up:
            t = min(-(-lo // up) - 1, (limit - n_lo) // n_up)
            if t <= 0:
                break
            lo -= t * up
            n_lo += t * n_up
        elif up > lo:
            t = min(-(-up // lo) - 1, (limit - n_up) // n_lo)
            if t <= 0:
                break
            up -= t * lo
            n_up += t * n_lo
        else:
            break  # lo == up: (n_lo + n_up) * alpha is an integer, and n_lo + n_up > limit
    return lo, up


def self_test_one_sided_minima():
    import random

    rnd = random.Random(1)
    for _ in range(2000):
        b = rnd.randint(2, 400)
        alpha = Fraction(rnd.randint(1, 3 * b), b)
        limit = rnd.randint(1, alpha.denominator - 1) if alpha.denominator > 1 else 0
        if limit == 0:
            continue
        fr = [n * alpha - floor(n * alpha) for n in range(1, limit + 1)]
        assert one_sided_minima(alpha, limit) == (min(fr), min(1 - f for f in fr))


def verify():
    """Re-proves what json_num.c assumes. Raises AssertionError otherwise."""
    tab = table()

    # 1. The exponent formula used with the table, and exactness of 10^0..10^55.
    for e in range(POW10_MIN, POW10_MAX + 1):
        assert c_floor_log2_pow10(e) == floor_log2_pow10(e), e
        beta = Fraction(10) ** e * Fraction(2) ** (127 - floor_log2_pow10(e))
        g = tab[e - POW10_MIN]
        assert beta <= g < beta + 1, e
        assert (g == beta) == (0 <= e <= POW10_EXACT_MAX), e

    # 2. The printer's logarithm formulas, table index range and shift range.
    for q in range(Q_MIN, Q_MAX + 1):
        assert c_floor_log10_pow2(q) == floor_log10(Fraction(2) ** q), q
        assert c_floor_log10_three_quarters_pow2(q) == floor_log10(Fraction(3, 4) * Fraction(2) ** q), q
        for k in (c_floor_log10_pow2(q), c_floor_log10_three_quarters_pow2(q)):
            assert POW10_MIN <= -k <= POW10_MAX
            assert 1 <= q + c_floor_log2_pow10(-k) + 1 <= 4

    # 3. The printer's sticky bit (roundToOdd in json_num.c).
    #
    # For a double c * 2^q and its decimal exponent k the printer needs, for
    # cp in {4c - 2 or 4c - 1, 4c, 4c + 2}, the value x = cp * 2^q / 10^k
    # rounded to odd: floor(x), with the low bit forced to 1 when x is not an
    # integer. It computes P = (cp << h) * g(-k), a 192-bit integer for which
    # P = x * 2^128 + delta with 0 <= delta < cp << h < 2^59, and takes
    # floor(P / 2^128), with sticky = (P mod 2^128 >= 2^59). That is right when
    #     x is an integer, or 2^-69 <= frac(x) and 1 - frac(x) > 2^-69.
    # Regular case: cp is even and below 2^55, so cp = 2n with n < 2^54.
    eps = Fraction(1, 2**69)
    worst_lo = worst_up = Fraction(1)
    for q in range(Q_MIN, Q_MAX + 1):
        alpha = 2 * Fraction(2) ** q / Fraction(10) ** c_floor_log10_pow2(q)
        if alpha.denominator == 1:
            continue  # x is always an integer
        if alpha.denominator <= 2**54:
            lo = up = Fraction(1, alpha.denominator)  # frac(x) is a multiple of this
        else:
            lo, up = one_sided_minima(alpha, 2**54)
        assert lo >= eps and up > eps, q
        worst_lo, worst_up = min(worst_lo, lo), min(worst_up, up)
    # Irregular case (c = 2^52, lower neighbour half as far): three exact values.
    for q in range(Q_MIN + 1, Q_MAX + 1):
        alpha = Fraction(2) ** q / Fraction(10) ** c_floor_log10_three_quarters_pow2(q)
        for cp in (2**54 - 1, 2**54, 2**54 + 2):
            f = cp * alpha - floor(cp * alpha)
            assert f == 0 or (f >= eps and 1 - f > eps), q
    # (The parser needs no such bound: it falls back whenever the product is
    # within 2^64 of a rounding boundary, see decimalToBits in json_num.c.)
    return worst_lo, worst_up


def split_file(path):
    with open(path) as f:
        text = f.read()
    head, rest = text.split(BEGIN + "\n", 1)
    body, tail = rest.split(END, 1)
    return head, body, tail


def main(argv):
    self_test_one_sided_minima()
    lo, up = verify()
    if len(argv) == 1:
        print("\n".join(rows()))
    elif len(argv) == 3 and argv[1] == "--check":
        _, body, _ = split_file(argv[2])
        pairs = re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)\s*\}", body)
        got = [(int(high, 16) << 64) | int(low, 16) for high, low in pairs]
        if got != table():
            sys.exit("%s: table differs from gen_pow10.py" % argv[2])
        print("pow10 table: %d entries match; formulas and bounds verified" % len(got))
        print("  min frac(x) = 2^%.2f, min 1 - frac(x) = 2^%.2f (need 2^-69)" % (log2(lo), log2(up)))
    elif len(argv) == 3 and argv[1] == "--update":
        head, _, tail = split_file(argv[2])
        with open(argv[2], "w") as f:
            f.write(head + BEGIN + "\n" + "\n".join(rows()) + "\n    " + END + tail)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
