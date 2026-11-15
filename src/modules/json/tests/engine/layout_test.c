/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* layout_test.c -- white-box checks of the byte layout of json_engine.c, for
 * the places where it depends on byte order or on the width of a pointer or
 * of size_t. It includes the engine's source to reach its static helpers.
 *
 * usage: layout_test check   the checks; prints "layout: ok"
 *        layout_test hash    key table hashes of a fixed list of names under
 *                            a fixed seed, for check_layout.py to compare
 *                            with SipHash-1-2 as its reference defines it */

#include "../../json_engine.c"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void *plainRealloc(void *p, size_t n) {
    return realloc(p, n);
}

static size_t plainUsable(void *p) {
    (void)p;
    return 0;
}

static const jsonAllocator plain = {malloc, plainRealloc, free, plainUsable};

/* The memory byte of a word that holds its low eight bits. */
static uint8_t lowByteInMemory(const uint64_t *w) {
    return ((const uint8_t *)w)[JSON_BIG_ENDIAN ? 7 : 0];
}

static void checkInlineStrings(void) {
    const uint8_t text[8] = {'a', 'B', 'c', 'D', 'e', 'F', 'g', 'H'};
    for (uint32_t len = 0; len <= W_STR_MAX; len++) {
        for (int flags = 0; flags < 4; flags++) {
            const uint64_t w = emitText(text, len, flags & 1, flags >> 1, NULL);
            EXPECT(wordTag(w) == W_STR);
            EXPECT((lowByteInMemory(&w) & W_TAG_MASK) == W_STR); /* The tag is in the byte the low bits are in. */
            EXPECT(((w >> W_STR_LEN_SHIFT) & W_STR_MAX) == len);
            EXPECT(((w & W_STR_NOESC) != 0) == (flags & 1) && ((w & W_STR_RAWNUM) != 0) == (flags >> 1));
            EXPECT(memcmp(wordStrBytes(&w), text, len) == 0);
            /* The bytes after the string are zero, and none of it is in the tag byte. */
            const uint8_t *b = wordStrBytes(&w);
            for (uint32_t i = len; i < W_STR_MAX; i++) EXPECT(b[i] == 0);
            EXPECT(b + W_STR_MAX <= (const uint8_t *)&w + 8 && (JSON_BIG_ENDIAN ? b == (const uint8_t *)&w : b == (const uint8_t *)&w + 1));
            /* What a reference to it hands out. */
            jsonRef r;
            uint32_t got;
            refFromWord(&r, w, 0);
            const char *s = flags >> 1 ? NULL : jsonRefString(&r, &got);
            if (s) EXPECT(got == len && memcmp(s, text, len) == 0 && (const uint8_t *)s == wordStrBytes(&r.w));
        }
    }
}

static void checkLoads(void) {
    const uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT(loadU64LE(b) == 0x0807060504030201ull);
    EXPECT(loadU64(b) == (JSON_BIG_ENDIAN ? 0x0102030405060708ull : 0x0807060504030201ull));
    uint8_t p[8];
    storeU16(p, 0x0102);
    EXPECT(loadU16(p) == 0x0102 && p[0] == (JSON_BIG_ENDIAN ? 1 : 2));
    storeU32(p, 0x01020304u);
    EXPECT(loadU32(p) == 0x01020304u);
    /* Every integer width of a typed array, with bytes that all differ. */
    static const int64_t vals[] = {0, 1, -1, 0x01, -0x7F, 0x0102, -0x0102, 0x01020304, -0x01020304, 0x0102030405060708ll,
                                   -0x0102030405060708ll, INT64_MIN, INT64_MAX, INT32_MIN, INT16_MIN, INT8_MIN};
    for (uint32_t ek = EK_I8; ek <= EK_D64; ek++) {
        if (ek == EK_F64) continue;
        for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            if (!ekindHoldsInt(ek, vals[i])) continue;
            uint8_t e[9] = {0};
            ekStoreInt(e + 1, ek, vals[i]);
            EXPECT(ekInt(e + 1, ek) == vals[i]);
            uint8_t o[16];
            const uint8_t *end = packInt(o, vals[i]);
            EXPECT(pkInt(o) == vals[i] && pkSkip(o) == end);
            if (ekIsDec(ek)) {
                for (uint32_t scale = 1; scale <= DEC_SCALE_MAX; scale++) {
                    uint32_t got;
                    end = packDecimal(o, vals[i], scale);
                    EXPECT(pkDecimal(o, &got) == vals[i] && got == scale && pkSkip(o) == end);
                }
            }
        }
    }
}

static void checkVarints(void) {
    static const uint32_t vals[] = {1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455, 268435456, UINT32_MAX};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        uint8_t b[8];
        uint32_t got;
        const uint8_t *end = varintPut(b, vals[i]);
        EXPECT((uint32_t)(end - b) == varintSize(vals[i]));
        EXPECT(varintGet(b, &got) == end && got == vals[i]);
    }
}

static void checkWords(void) {
    /* Decimals at the edges of what a word holds, both signs, every scale. */
    static const int64_t ms[] = {0, 1, 0x0102, 0x01020304, (int64_t)W_DEC_MAGNITUDE_MAX, -(int64_t)W_DEC_MAGNITUDE_MAX};
    for (size_t i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
        for (uint32_t scale = 1; scale <= W_DEC_SCALES; scale++) {
            uint32_t got;
            EXPECT(wordDecFits(ms[i], scale));
            const uint64_t w = wordDec(ms[i], scale);
            EXPECT(wordIsDec(w) && wordDecimal(w, &got) == ms[i] && got == scale);
        }
    }
    EXPECT(!wordDecFits((int64_t)W_DEC_MAGNITUDE_MAX + 1, 1) && !wordDecFits(1, W_DEC_SCALES + 1));
    /* Pointers: the low half of the word, the tag bits clear. */
    uint8_t *node = nodeAlloc(BOXNUM_SIZE, NULL);
    EXPECT(((uintptr_t)node & W_TAG_MASK) == 0);
    EXPECT(wordNode(nodeWord(node)) == node && wordTag(nodeWord(node)) == W_PTR);
    EXPECT(UINTPTR_MAX > UINT32_MAX || (nodeWord(node) >> 32) == 0);
    free(node);
    EXPECT(wordKeyId(wordKey(0xFEDCBA98u)) == 0xFEDCBA98u);
}

/* A byte that needs no attention, different at each position. */
static uint8_t plainByte(size_t i) {
    const uint8_t c = (uint8_t)(0x20 + (i * 13) % 0x5F);
    return c == '"' || c == '\\' ? 'q' : c;
}

/* scanPlain stops at the first byte that needs attention, wherever it is in
 * an eight-byte block: compared with a byte loop at every offset. */
static void checkScan(void) {
    static const uint8_t stops[] = {'"', '\\', 0x00, 0x01, 0x1F, 0x80, 0xC3, 0xFF};
    uint8_t buf[48];
    for (size_t k = 0; k < sizeof(stops); k++) {
        for (size_t len = 0; len <= 40; len++) {
            for (size_t at = 0; at <= len; at++) {
                for (int high = 0; high < 2; high++) {
                    memset(buf, 'a', sizeof(buf));
                    for (size_t i = 0; i < len; i++) buf[i] = plainByte(i);
                    if (at < len) buf[at] = stops[k];
                    const uint8_t *want = buf;
                    while (want < buf + len && !(*want == '"' || *want == '\\' || *want < 0x20 || (*want >= 0x80 && high))) want++;
                    EXPECT(scanPlain(buf, buf + len, high) == want);
                }
            }
        }
    }
}

/* The x87 rounding helper: an integer beyond 2^53 is equal as a double to its neighbour. */
static void checkDoubles(void) {
    volatile int64_t a = 9007199254740993ll, b = 9007199254740992ll;
    EXPECT(doubleRound((double)a) == doubleRound((double)b));
    EXPECT(decimalValue(1, 1) == 0.1 && decimalValue(-12345, 2) == -123.45 && decimalValue(1, 22) == 1e-22);
}

/* A size beyond size_t stops the process rather than wrap around. */
static void checkSizes(void) {
    EXPECT(engSize(1234) == 1234);
    EXPECT(sizeSat((uint64_t)SIZE_MAX) == SIZE_MAX);
#if SIZE_MAX < UINT64_MAX
    EXPECT(sizeSat((uint64_t)SIZE_MAX + 1) == SIZE_MAX);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        const struct rlimit none = {0, 0};
        setrlimit(RLIMIT_CORE, &none); /* A sanitizer's core would be all of its shadow memory. */
        signal(SIGILL, SIG_DFL);
        signal(SIGTRAP, SIG_DFL);
        volatile size_t s = engSize((uint64_t)SIZE_MAX + 1);
        (void)s;
        _exit(0);
    }
    int status = 0;
    EXPECT(pid > 0 && waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
    puts("layout: a size past size_t stops the process");
#endif
}

static void printHashes(void) {
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) seed[i] = (uint8_t)(0xA0 + i * 3);
    jsonEngineSetHashSeed(seed);
    uint8_t name[64];
    for (int len = 0; len <= 64; len++) {
        for (int i = 0; i < len; i++) name[i] = (uint8_t)(i * 37 + len + 1);
        printf("%d %08" PRIx32 "\n", len, ktHash(name, (size_t)len));
    }
}

int main(int argc, char **argv) {
    jsonEngineInit(&plain);
    if (argc == 2 && !strcmp(argv[1], "hash")) {
        printHashes();
        return 0;
    }
    if (argc != 2 || strcmp(argv[1], "check")) {
        fprintf(stderr, "usage: layout_test check|hash\n");
        return 2;
    }
    printf("layout: %s-endian, %zu-bit pointers, %zu-bit size_t, FLT_EVAL_METHOD %d\n", JSON_BIG_ENDIAN ? "big" : "little",
           sizeof(void *) * 8, sizeof(size_t) * 8, (int)FLT_EVAL_METHOD);
    checkInlineStrings();
    checkLoads();
    checkVarints();
    checkWords();
    checkScan();
    checkDoubles();
    checkSizes();
    if (failures) {
        fprintf(stderr, "layout: %d FAILURES\n", failures);
        return 1;
    }
    puts("layout: ok");
    return 0;
}
