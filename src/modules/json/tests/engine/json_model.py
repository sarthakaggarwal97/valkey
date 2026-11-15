#!/usr/bin/env python3
"""The Python model the engine tests compare against: an independent reading
of JSON text, of design 5.4 (number kinds, canonical text, array formats),
of the module's writer (escaping, INDENT / NEWLINE / SPACE layout) and of its
value comparison. Used by check_engine.py and check_mutate.py."""
import decimal, json, math, re

class Num(str):
    """A number token, kept as its source text."""

def bad_constant(name):
    raise ValueError(name)

def parse(text):
    """Python's own parser. A dict keeps a repeated name at its first position with its last value, which is the rule
    under test. One byte order mark before the text is skipped, as the module does."""
    if text.startswith("\ufeff"): text = text[1:]
    return json.loads(text, parse_int=Num, parse_float=Num, parse_constant=bad_constant)

def decode(raw):
    return raw.decode("utf-8", "surrogateescape")

def encode(text):
    return text.encode("utf-8", "surrogateescape")

ESC = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f", "\n": "\\n", "\r": "\\r", "\t": "\\t"}
ESC_RE = re.compile(r'["\\\x00-\x1f]')

def quote(s):
    """The module's writer: control characters, quote and backslash are escaped, upper-case hex, everything else is copied."""
    return '"' + ESC_RE.sub(lambda m: ESC.get(m.group(), "\\u%04X" % ord(m.group())), s) + '"'

def scalar_text(v):
    if v is None: return "null"
    if v is True: return "true"
    if v is False: return "false"
    if isinstance(v, Num): return "0" if v == "-0" else str(v)
    return quote(v)

def serialize(root, indent="", newline="", space=""):
    """The module's JSON.GET layout. Not recursive."""
    out, stack = [], []
    def put(v):
        if isinstance(v, (dict, list)):
            is_obj = isinstance(v, dict)
            if not v:
                out.append("{}" if is_obj else "[]")
            else:
                out.append("{" if is_obj else "[")
                stack.append([iter(v.items()) if is_obj else iter(v), is_obj, True])
        else:
            out.append(scalar_text(v))
    put(root)
    while stack:
        frame = stack[-1]
        try:
            item = next(frame[0])
        except StopIteration:
            stack.pop()
            out.append(newline + indent * len(stack) + ("}" if frame[1] else "]"))
            continue
        out.append(("" if frame[2] else ",") + newline + indent * len(stack))
        frame[2] = False
        if frame[1]:
            out.append(quote(item[0]) + ":" + space)
            item = item[1]
        put(item)
    return "".join(out)

NUM_RE = re.compile(r"(-?)(0|[1-9][0-9]*)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?\Z")

def shortest_digits(a):
    """(digits, n) for a positive double: its shortest digits, and n such that the value is 0.DIGITS * 10^n."""
    sign, digits, exp = decimal.Decimal(repr(a)).as_tuple()
    digits = list(digits)
    while len(digits) > 1 and digits[-1] == 0:
        digits.pop(); exp += 1
    ds = "".join(map(str, digits))
    return ds, len(ds) + exp

def canonical(d):
    """Canonical text of a double (design 5.4), from Python's shortest repr."""
    if d == 0: return "-0.0" if math.copysign(1, d) < 0 else "0.0"
    ds, n = shortest_digits(abs(d))
    neg = "-" if d < 0 else ""
    if 0 < n <= 21:
        if len(ds) <= n: return neg + ds + "0" * (n - len(ds)) + ".0"
        return neg + ds[:n] + "." + ds[n:]
    if -6 < n <= 0: return neg + "0." + "0" * -n + ds
    e = n - 1
    return neg + ds[0] + ("." + ds[1:] if len(ds) > 1 else "") + "e" + ("-" if e < 0 else "+") + str(abs(e))

DEC_SCALE_MAX = 31
DEC_RE = re.compile(r"(-?)(0|[1-9][0-9]*)\.([0-9]{1,%d})\Z" % DEC_SCALE_MAX)

def is_decimal(t):
    """A decimal (design 5.4): plain notation with 1 to 31 fraction digits whose digits, read as one integer M,
    are below 10^18, and which is not a negative zero. The engine holds it as M and the scale."""
    m = DEC_RE.match(t)
    if not m: return False
    digits = int(m.group(2) + m.group(3))
    return digits < 10**18 and not (m.group(1) and digits == 0)

def analyze(t):
    """The kind the read functions report for a number: 'i' an integer, 'd' a double (its text is the canonical
    text of its value and it is not a decimal), 'r' anything else: a decimal or text kept as it is."""
    neg, ip, fp, ex = NUM_RE.match(t).groups()
    if fp is None and ex is None: return "i" if -2**63 <= int(t) < 2**63 else "r"
    if is_decimal(t): return "r"
    d = float(t)
    return "d" if math.isfinite(d) and canonical(d) == t else "r"

class Mismatch(Exception):
    pass

def need(cond, *what):
    if not cond: raise Mismatch(" ".join(str(w)[:200] for w in what))

def match(c, v):
    """Compares the binary's dump `c` with the parsed value `v`. Returns (depth, fields) of v."""
    if v is None or v is True or v is False:
        need(c is v, "scalar", c, v)
        return 0, 1
    if isinstance(v, Num):
        kind = analyze(v)
        need(isinstance(c, dict) and kind in c, "number kind", c, v, kind)
        need(c[kind] == scalar_text(v), "number text", c, v)
        if kind == "i": need(int(c["v"]) == int(v), "int value", c, v)
        else: need(float(c["v"]) == float(v), "double value", c, v)
        return 0, 1
    if isinstance(v, str):
        need(isinstance(c, dict) and c.get("s") == encode(v).hex(), "string", c, v)
        return 0, 1
    is_obj = isinstance(v, dict)
    need(isinstance(c, dict) and ("o" if is_obj else "a") in c, "container kind", c)
    depth, fields = 1, 1
    if is_obj:
        flat = c["o"]
        need(len(flat) == 2 * len(v), "member count", len(flat), len(v))
        for i, (name, child) in enumerate(v.items()):
            need(flat[2 * i] == encode(name).hex(), "member name", flat[2 * i], name)
            d, f = match(flat[2 * i + 1], child)
            depth = max(depth, d + 1); fields += f
    else:
        need(len(c["a"]) == len(v), "element count", len(c["a"]), len(v))
        for i, child in enumerate(v):
            d, f = match(c["a"][i], child)
            depth = max(depth, d + 1); fields += f
    need(c["n"] == len(v), "count", c["n"], len(v))
    need(c["dp"] == depth, "depth", c["dp"], depth)
    need(c["f"] == fields, "fields", c["f"], fields)
    return depth, fields

INT_RE = re.compile(r"-?(0|[1-9][0-9]*)\Z")

def num_view(t):
    """The module's view of a number: an integer when the text is one within int64, else the text read as a double."""
    if INT_RE.match(t) and -2**63 <= int(t) < 2**63: return ("int", int(t), float(int(t)))
    return ("dbl", None, float(t))

def equal(a, b):
    """GenericValue::operator== of the module."""
    na, nb = isinstance(a, Num), isinstance(b, Num)
    if na or nb:
        if not (na and nb): return False
        x, y = num_view(a), num_view(b)
        return x[1] == y[1] if x[0] == y[0] == "int" else x[2] == y[2]
    if type(a) is not type(b): return False
    if isinstance(a, list): return len(a) == len(b) and all(equal(p, q) for p, q in zip(a, b))
    if isinstance(a, dict): return len(a) == len(b) and all(k in b and equal(a[k], b[k]) for k in a)
    return a == b

