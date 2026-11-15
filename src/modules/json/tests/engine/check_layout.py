#!/usr/bin/env python3
"""Runs layout_test (plain and ASan+UBSan) and checks the key table's hash against SipHash-1-2 as its reference
defines it, the message and key read as little-endian words, on whatever byte order the binary was built for.

usage: check_layout.py <build-dir>"""
import os, subprocess, sys

def rotl(x, b): return ((x << b) | (x >> (64 - b))) & 0xFFFFFFFFFFFFFFFF

def sipround(v):
    v0, v1, v2, v3 = v
    M = 0xFFFFFFFFFFFFFFFF
    v0 = (v0 + v1) & M; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32)
    v2 = (v2 + v3) & M; v3 = rotl(v3, 16); v3 ^= v2
    v0 = (v0 + v3) & M; v3 = rotl(v3, 21); v3 ^= v0
    v2 = (v2 + v1) & M; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32)
    return [v0, v1, v2, v3]

def siphash12(key, msg):
    k0, k1 = int.from_bytes(key[:8], "little"), int.from_bytes(key[8:], "little")
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]
    n = len(msg) - len(msg) % 8
    for i in range(0, n, 8):
        m = int.from_bytes(msg[i:i + 8], "little")
        v[3] ^= m; v = sipround(v); v[0] ^= m
    b = (len(msg) << 56) & 0xFFFFFFFFFFFFFFFF | int.from_bytes(msg[n:], "little")
    v[3] ^= b; v = sipround(v); v[0] ^= b
    v[2] ^= 0xFF
    v = sipround(sipround(v))
    return v[0] ^ v[1] ^ v[2] ^ v[3]

def main():
    build = sys.argv[1]
    bad = 0
    for name in ("layout_test", "layout_test_san"):
        binary = os.path.join(build, name)
        p = subprocess.run([binary, "check"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sys.stdout.write("%s: %s" % (name, p.stdout.decode()))
        if p.returncode: bad += 1; sys.stdout.write(p.stderr.decode())
        p = subprocess.run([binary, "hash"], stdout=subprocess.PIPE)
        key = bytes(0xA0 + i * 3 for i in range(16))
        lines = p.stdout.decode().split("\n")[:-1]
        wrong = 0
        for line in lines:
            n, got = line.split()
            msg = bytes((i * 37 + int(n) + 1) & 0xFF for i in range(int(n)))
            h = siphash12(key, msg)
            if int(got, 16) != (h ^ (h >> 32)) & 0xFFFFFFFF: wrong += 1
        print("%s: key hash against SipHash-1-2: %d of %d lengths match" % (name, len(lines) - wrong, len(lines)))
        if wrong or len(lines) != 65: bad += 1
    sys.exit(1 if bad else 0)

main()
