#!/usr/bin/env python3
"""Gate-corpus generator for the native JSON design (deterministic).
Usage: gen.py <shape> [n] [seed]  -> one JSON document per line on stdout.
Shapes: session profile order (many small docs); records100k emb_full emb_short
ints100k str10k obj100 (one document each)."""
import json, random, struct, sys

def f32(x): return struct.unpack('f', struct.pack('f', x))[0]
WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november "
         "oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu").split()
def words(r, n): return " ".join(r.choice(WORDS) for _ in range(n))
def hexs(r, n): return "".join(r.choice("0123456789abcdef") for _ in range(n))

def session(r, i):
    return {"sid": hexs(r, 32), "uid": r.randrange(1, 10**7), "created": "2026-11-%02dT%02d:%02d:%02dZ" % (r.randrange(1, 29), r.randrange(24), r.randrange(60), r.randrange(60)),
            "expires": "2026-12-%02dT00:00:00Z" % r.randrange(1, 29), "ip": "%d.%d.%d.%d" % tuple(r.randrange(256) for _ in range(4)),
            "ua": "Mozilla/5.0 (" + words(r, 4) + ") " + words(r, 3), "roles": r.sample(["user", "admin", "ops", "billing"], r.randrange(1, 3)),
            "flags": {"mfa": r.random() < .5, "beta": r.random() < .2}, "cart_items": r.randrange(0, 12), "last_path": "/" + "/".join(r.choice(WORDS) for _ in range(r.randrange(1, 4)))}

def profile(r, i):
    return {"id": i, "name": words(r, 2).title(), "email": r.choice(WORDS) + str(r.randrange(1000)) + "@example.com", "age": r.randrange(18, 90),
            "score": round(r.uniform(0, 100), 2), "active": r.random() < .8,
            "address": {"street": "%d %s St" % (r.randrange(1, 9999), r.choice(WORDS).title()), "city": r.choice(WORDS).title(), "state": r.choice(["WA", "CA", "NY", "TX"]),
                        "zip": "%05d" % r.randrange(100000), "geo": {"lat": round(r.uniform(-90, 90), 6), "lon": round(r.uniform(-180, 180), 6)}},
            "tags": r.sample(WORDS, r.randrange(3, 7)), "prefs": {"theme": r.choice(["dark", "light"]), "lang": r.choice(["en", "de", "fr", "ja"]),
                                                                 "notify": {"email": True, "sms": r.random() < .3, "push": r.random() < .6}},
            "bio": words(r, r.randrange(15, 60))}

def order(r, i):
    items = [{"sku": "SKU-" + hexs(r, 8).upper(), "name": words(r, 3), "qty": r.randrange(1, 9), "price": round(r.uniform(1, 500), 2)} for _ in range(r.randrange(3, 9))]
    sub = round(sum(x["qty"] * x["price"] for x in items), 2)
    return {"order_id": "ORD-%09d" % i, "customer_id": r.randrange(1, 10**7), "status": r.choice(["new", "paid", "shipped", "delivered"]), "ts": 1790000000 + r.randrange(10**6),
            "items": items, "totals": {"subtotal": sub, "tax": round(sub * 0.0825, 2), "shipping": 4.99, "total": round(sub * 1.0825 + 4.99, 2)},
            "ship_to": {"name": words(r, 2).title(), "line1": "%d %s Ave" % (r.randrange(1, 9999), r.choice(WORDS).title()), "city": r.choice(WORDS).title(), "zip": "%05d" % r.randrange(100000)},
            "notes": words(r, r.randrange(0, 25))}

def main():
    shape = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 1; seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    r = random.Random(seed); out = sys.stdout; d = lambda o: json.dumps(o, separators=(",", ":"))
    if shape in ("session", "profile", "order"):
        fn = globals()[shape]
        for i in range(n): out.write(d(fn(r, i)) + "\n")
    elif shape == "records100k":
        out.write(d([{"id": i, "name": "item-%d" % i, "price": round(r.uniform(1, 500), 2), "qty": r.randrange(1, 9), "tags": r.sample(WORDS, 2)} for i in range(100000)]) + "\n")
    elif shape == "emb_full":   # float32 values written at full double precision, as a float32 model serialised through double
        out.write("[" + ",".join(repr(f32(r.gauss(0, 0.05))) for _ in range(1536)) + "]\n")
    elif shape == "emb_short":  # 6 decimals, at most 9 characters
        out.write("[" + ",".join("%.6f" % r.gauss(0, 0.05) for _ in range(1536)) + "]\n")
    elif shape == "ints100k":
        out.write(d([r.randrange(0, 2**31) for _ in range(100000)]) + "\n")
    elif shape == "str10k":
        out.write(d([hexs(r, r.randrange(8, 14)) for _ in range(10000)]) + "\n")
    elif shape == "obj100":
        out.write(d({"field_%03d" % i: i * 7 for i in range(100)}) + "\n")
    else: sys.exit("unknown shape")
main()
