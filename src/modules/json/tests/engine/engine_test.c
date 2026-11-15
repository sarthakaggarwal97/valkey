/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* engine_test.c -- test driver for json_engine.c.
 *
 * usage: engine_test [options] <op> [file]
 *
 *   roundtrip  For each line of the file (one JSON text): parse, serialise,
 *              print the output in hex, or "!E <error> <offset>".
 *   dump       For each line: print a description of the document obtained
 *              through the read functions only. check_engine.py compares it
 *              with its own walk of the same text.
 *   equals     Each line is two JSON texts separated by a tab. Prints
 *              jsonRefEquals of the two as 0 or 1.
 *   mutate     Each line is a JSON text and a list of changes to make to it
 *              (see opMutate). Prints the document after every change, then
 *              how often each write primitive was used by each kind of change.
 *   unit       Built-in tests of what needs no corpus.
 *
 *   -b N  json.max-packed-bytes        -e N  json.max-packed-entries
 *   -B N, -E N  the same for the second document of `equals`
 *   -t    trusted input                -d N  max_depth
 *   -x    input lines are hex          -i/-n/-s STR  INDENT/NEWLINE/SPACE
 *
 * Every document also goes through self-checks (copy, equality, memory
 * accounting). The run fails unless, at the end, every key reference was
 * released and every byte the engine allocated was freed. */

#include "../../json_engine.h"
#include "../../json_engine_stats.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            fprintf(stderr, "%s:%d: check failed: %s (line %ld)\n", __FILE__, __LINE__, #cond, g_line); \
            exit(1);                                                                                    \
        }                                                                                               \
    } while (0)

static long g_line; /* Input line being processed, for messages. */

/* ----------------------------------------------------------------------------
 * Allocator: exact accounting, and usable size == requested size, so that
 * the sanitizer sees any access past what the engine asked for.
 * ------------------------------------------------------------------------- */

/* 24 bytes on every target, so that a block is aligned to 8 bytes and not to
 * 16 (malloc gives 16): the engine may count on the 8 of the smallest
 * allocator classes, and on nothing more. */
typedef struct blockHeader {
    uint64_t size;
    uint64_t magic;
    uint64_t pad;
} blockHeader;
_Static_assert(sizeof(blockHeader) == 24, "the block header keeps blocks 8 bytes off 16");

#define BLOCK_MAGIC 0x6A736F6E626C6B21ull

static _Atomic size_t live_bytes, live_blocks;

static blockHeader *blockOf(void *p) {
    blockHeader *h = (blockHeader *)p - 1;
    if (h->magic != BLOCK_MAGIC) {
        fprintf(stderr, "allocator: bad block %p\n", p);
        abort();
    }
    return h;
}

static void *testAlloc(size_t n) {
    blockHeader *h = malloc(sizeof(*h) + n);
    if (!h || ((uintptr_t)h & 7)) abort();
    h->size = n;
    h->magic = BLOCK_MAGIC;
    live_bytes += n;
    live_blocks++;
    return h + 1;
}

static void testFree(void *p) {
    blockHeader *h = blockOf(p);
    live_bytes -= h->size;
    live_blocks--;
    h->magic = 0;
    free(h);
}

static void *testRealloc(void *p, size_t n) {
    /* Always moves, so that a stale pointer is caught. */
    void *np = testAlloc(n);
    const size_t old = blockOf(p)->size;
    memcpy(np, p, old < n ? old : n);
    testFree(p);
    return np;
}

static size_t testUsable(void *p) {
    return (size_t)blockOf(p)->size;
}

static const jsonAllocator test_allocator = {testAlloc, testRealloc, testFree, testUsable};

/* ----------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

typedef struct text {
    char *p;
    size_t len, cap;
} text;

static void textAdd(text *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->p = realloc(t->p, t->cap);
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = '\0';
}

static void textAddStr(text *t, const char *s) {
    textAdd(t, s, strlen(s));
}

static void textAddHex(text *t, const char *s, size_t n) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        const char pair[2] = {HEX[(unsigned char)s[i] >> 4], HEX[(unsigned char)s[i] & 15]};
        textAdd(t, pair, 2);
    }
}

static int hexValue(char c) {
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

/* Serialises into `out`, starting from a small stack buffer so that the move
 * to the heap is exercised. */
static void serialize(const jsonRef *r, const jsonFormat *fmt, text *out) {
    char small[48];
    jsonBuf b;
    jsonBufInit(&b, small, sizeof(small));
    jsonSerialize(r, fmt, &b);
    out->len = 0;
    textAdd(out, b.p, b.len);
    jsonBufFree(&b);
}

static const char *errName(jsonErr e) {
    static const char *names[] = {"OK", "SYNTAX", "UTF8", "NUM_RANGE", "DEPTH", "TOO_BIG"};
    return names[e];
}

static struct {
    int trusted, hex_input;
    uint32_t max_depth;
    uint32_t cap_bytes, cap_entries;   /* -b, -e */
    uint32_t cap2_bytes, cap2_entries; /* -B, -E: caps for the second document of `equals`. */
    jsonFormat fmt;
    int has_fmt;
} opt;

static jsonDoc parseDoc(const char *s, size_t n, jsonErr *err, size_t *off) {
    /* An exact-size copy: the sanitizer sees any read past the end. */
    char *copy = malloc(n ? n : 1);
    memcpy(copy, s, n);
    jsonVal *v = jsonValParse(copy, n, opt.trusted ? JSON_PARSE_TRUSTED : 0, opt.max_depth, err, off);
    free(copy);
    return v ? jsonValToDoc(v) : 0;
}

/* ----------------------------------------------------------------------------
 * Self-checks run on every document
 * ------------------------------------------------------------------------- */

static void checkDoc(jsonDoc d) {
    static text a, b;
    jsonRef root, copy_root;
    jsonDocRoot(d, &root);
    const jsonDoc copy = jsonDocCopy(d);
    jsonDocRoot(copy, &copy_root);
    CHECK(jsonRefEquals(&root, &copy_root));
    CHECK(jsonRefEquals(&copy_root, &root));
    serialize(&root, NULL, &a);
    serialize(&copy_root, NULL, &b);
    CHECK(a.len == b.len && memcmp(a.p, b.p, a.len) == 0);

    /* The tracked size is exactly what the document's allocations hold, and
     * the free effort is its number of nodes. */
    const size_t mem = jsonDocMemUsage(copy), effort = jsonDocFreeEffort(copy);
    const int tree = strcmp(jsonDocEncoding(copy), "jtree") == 0;
    CHECK(mem == jsonDocMemUsage(d));
    CHECK(effort == jsonDocFreeEffort(d));
    CHECK(strcmp(jsonDocEncoding(copy), jsonDocEncoding(d)) == 0);
    CHECK(jsonRefMemUsage(&copy_root) == mem);
    const size_t bytes_before = live_bytes, blocks_before = live_blocks;
    jsonDocFree(copy);
    CHECK(bytes_before - live_bytes == mem);
    if (tree && effort > 1) {
        CHECK(blocks_before - live_blocks == effort);
    } else {
        CHECK(effort == 1 && blocks_before - live_blocks <= 1);
    }
}

/* ----------------------------------------------------------------------------
 * dump: the document as the read functions show it
 * ------------------------------------------------------------------------- */

static void checkSame(const jsonRef *a, const jsonRef *b, int deep) {
    static text ta, tb;
    char na[JSON_NUM_BUF], nb[JSON_NUM_BUF];
    uint32_t la, lb;
    const jsonType type = jsonRefType(a);
    CHECK(type == jsonRefType(b));
    CHECK(jsonRefCount(a) == jsonRefCount(b));
    if (type == JSON_INT || type == JSON_DOUBLE || type == JSON_RAWNUM) {
        const char *sa = jsonRefNumText(a, na, &la), *sb = jsonRefNumText(b, nb, &lb);
        CHECK(la == lb && memcmp(sa, sb, la) == 0);
    } else if (type == JSON_STRING) {
        const char *sa = jsonRefString(a, &la), *sb = jsonRefString(b, &lb);
        CHECK(la == lb && memcmp(sa, sb, la) == 0);
    }
    if (!deep) return;
    CHECK(jsonRefEquals(a, b));
    serialize(a, NULL, &ta);
    serialize(b, NULL, &tb);
    CHECK(ta.len == tb.len && memcmp(ta.p, tb.p, ta.len) == 0);
}

/* A value copied out with jsonValFromRef serialises to the same text. */
static void checkCopyOut(const jsonRef *r) {
    static text a, b;
    jsonRef root;
    jsonVal *v = jsonValFromRef(r);
    const uint32_t depth = jsonValDepth(v);
    CHECK(depth == jsonRefDepth(r));
    const jsonDoc d = jsonValToDoc(v);
    jsonDocRoot(d, &root);
    serialize(r, NULL, &a);
    serialize(&root, NULL, &b);
    CHECK(a.len == b.len && memcmp(a.p, b.p, a.len) == 0);
    jsonDocFree(d);
}

static void dumpScalar(const jsonRef *r, text *out) {
    char num[JSON_NUM_BUF], line[128];
    uint32_t len;
    int64_t iv = 0;
    jsonRef tmp;
    jsonIter it;
    const jsonType type = jsonRefType(r);
    CHECK(jsonRefCount(r) == 0 && jsonRefDepth(r) == 0 && jsonRefFields(r) == 1);
    CHECK(!jsonArrayGet(r, 0, &tmp) && !jsonObjectGet(r, "a", 1, &tmp, NULL));
    jsonIterInit(&it, r);
    CHECK(!jsonIterNext(&it, &tmp, NULL, NULL, NULL));
    CHECK(jsonRefInt(r, &iv) == (type == JSON_INT));
    if (type != JSON_STRING) CHECK(jsonRefString(r, &len) == NULL && len == 0);
    switch (type) {
    case JSON_NULL: textAddStr(out, "null"); break;
    case JSON_FALSE: textAddStr(out, "false"); break;
    case JSON_TRUE: textAddStr(out, "true"); break;
    case JSON_INT:
    case JSON_DOUBLE:
    case JSON_RAWNUM: {
        const char *s = jsonRefNumText(r, num, &len);
        textAddStr(out, type == JSON_INT ? "{\"i\":\"" : type == JSON_DOUBLE ? "{\"d\":\""
                                                                             : "{\"r\":\"");
        textAdd(out, s, len);
        if (type == JSON_INT) {
            snprintf(line, sizeof(line), "\",\"v\":\"%" PRId64 "\"}", iv);
        } else {
            snprintf(line, sizeof(line), "\",\"v\":\"%.17g\"}", jsonRefDouble(r));
        }
        textAddStr(out, line);
        break;
    }
    default: {
        const char *s = jsonRefString(r, &len);
        CHECK(s != NULL);
        textAddStr(out, "{\"s\":\"");
        textAddHex(out, s, len);
        textAddStr(out, "\"}");
    }
    }
}

typedef struct dumpFrame {
    jsonIter it;
    jsonRef container;
    uint32_t index;
    uint32_t last_pos;
    int is_obj;
} dumpFrame;

static void dumpOpen(const jsonRef *r, dumpFrame *f, text *out) {
    char line[128];
    f->container = *r;
    f->index = 0;
    f->last_pos = 0;
    f->is_obj = jsonRefType(r) == JSON_OBJECT;
    jsonIterInit(&f->it, r);
    snprintf(line, sizeof(line), "{\"n\":%u,\"dp\":%u,\"f\":%" PRIu64 ",\"%s\":[", jsonRefCount(r), jsonRefDepth(r),
             jsonRefFields(r), f->is_obj ? "o" : "a");
    textAddStr(out, line);
}

/* Checks one child against every other way of reaching it. */
static void checkChild(jsonDoc d,
                       dumpFrame *f,
                       const jsonRef *val,
                       const jsonStep *step,
                       const char *name,
                       uint32_t name_len,
                       const jsonStep *loc,
                       int nloc,
                       int deep) {
    jsonRef got;
    jsonStep st;
    const jsonRef *c = &f->container;
    /* Reaching a child of a packed container by index or key is a scan, so
     * in a very large container only some of the children are checked. */
    const uint32_t count = jsonRefCount(c);
    if (count > 2048 && f->index >= 64 && f->index + 64 < count && f->index % 61 != 0) {
        f->last_pos = step->pos;
        return;
    }
    if (f->is_obj) {
        uint32_t key_len;
        CHECK(step->keyid != 0);
        CHECK(f->index == 0 || step->pos > f->last_pos); /* Positions order the members. */
        const char *key = jsonKeyName(step->keyid, &key_len);
        CHECK(key == name && key_len == name_len);
        const uint32_t id = jsonKeyFind(name, name_len);
        CHECK(id == step->keyid);
        jsonKeyRelease(id);
        CHECK(jsonObjectGet(c, name, name_len, &got, &st));
        CHECK(st.keyid == step->keyid && st.pos == step->pos);
        checkSame(val, &got, deep);
        CHECK(jsonObjectGetById(c, step->keyid, &got, &st));
        CHECK(st.keyid == step->keyid && st.pos == step->pos);
        checkSame(val, &got, 0);
        CHECK(!jsonArrayGet(c, f->index, &got));
        /* The key identifies the member: a stale position does not matter. */
        st.pos = step->pos + 12345;
        CHECK(jsonChildByStep(c, &st, &got));
        checkSame(val, &got, 0);
    } else {
        CHECK(step->keyid == 0 && step->pos == f->index);
        CHECK(jsonArrayGet(c, f->index, &got));
        checkSame(val, &got, deep);
        CHECK(!jsonObjectGetById(c, 1, &got, NULL));
    }
    CHECK(jsonChildByStep(c, step, &got));
    checkSame(val, &got, 0);
    CHECK(jsonDocResolve(d, loc, nloc, &got));
    checkSame(val, &got, deep);
    if (deep) checkCopyOut(val);
    f->last_pos = step->pos;
}

static void dumpDoc(jsonDoc d, int deep, text *out) {
    static dumpFrame *stack;
    static jsonStep *loc;
    static uint32_t cap;
    uint32_t sp = 0;
    jsonRef root, val;
    jsonStep step;
    out->len = 0;
    jsonDocRoot(d, &root);
    CHECK(jsonDocResolve(d, NULL, 0, &val));
    checkSame(&root, &val, deep);
    if (jsonRefType(&root) < JSON_ARRAY) {
        dumpScalar(&root, out);
        return;
    }
    if (!cap) {
        cap = 64;
        stack = malloc(cap * sizeof(*stack));
        loc = malloc(cap * sizeof(*loc));
    }
    dumpOpen(&root, &stack[sp++], out);
    while (sp) {
        dumpFrame *f = &stack[sp - 1];
        const char *name = NULL;
        uint32_t name_len = 0;
        if (!jsonIterNext(&f->it, &val, &step, &name, &name_len)) {
            jsonRef got;
            CHECK(f->index == jsonRefCount(&f->container));
            CHECK(!jsonArrayGet(&f->container, f->index, &got));
            CHECK(!jsonObjectGet(&f->container, "\x01no such member", 15, &got, NULL));
            textAddStr(out, "]}");
            sp--;
            continue;
        }
        if (f->index) textAddStr(out, ",");
        loc[sp - 1] = step;
        checkChild(d, f, &val, &step, name, name_len, loc, (int)sp, deep);
        f->index++;
        if (f->is_obj) {
            /* An object is a flat list: name, value, name, value, ... */
            textAddStr(out, "\"");
            textAddHex(out, name, name_len);
            textAddStr(out, "\",");
        }
        if (jsonRefType(&val) >= JSON_ARRAY) {
            if (sp == cap) {
                cap *= 2;
                stack = realloc(stack, cap * sizeof(*stack));
                loc = realloc(loc, cap * sizeof(*loc));
                f = &stack[sp - 1];
            }
            dumpOpen(&val, &stack[sp++], out);
        } else {
            dumpScalar(&val, out);
        }
    }
}

/* ----------------------------------------------------------------------------
 * unit: tests that need no corpus
 * ------------------------------------------------------------------------- */

static void expectText(jsonDoc d, const char *want) {
    static text t;
    jsonRef root;
    jsonDocRoot(d, &root);
    serialize(&root, NULL, &t);
    if (t.len != strlen(want) || memcmp(t.p, want, t.len) != 0) {
        fprintf(stderr, "expected %s, got %.*s\n", want, (int)t.len, t.p);
        exit(1);
    }
}

static jsonDoc mustParse(const char *s) {
    jsonErr err;
    size_t off;
    const jsonDoc d = parseDoc(s, strlen(s), &err, &off);
    if (!d) {
        fprintf(stderr, "parse failed (%s at %zu): %s\n", errName(err), off, s);
        exit(1);
    }
    return d;
}

static void expectValue(jsonVal *v, const char *want, jsonType type, uint32_t depth) {
    jsonRef root;
    CHECK(v != NULL);
    CHECK(jsonValDepth(v) == depth);
    CHECK(jsonValSizeEstimate(v) > 0 || type < JSON_STRING);
    const jsonDoc d = jsonValToDoc(v);
    jsonDocRoot(d, &root);
    CHECK(jsonRefType(&root) == type);
    expectText(d, want);
    checkDoc(d);
    jsonDocFree(d);
}

static void unitConstructors(void) {
    expectValue(jsonValNull(), "null", JSON_NULL, 0);
    expectValue(jsonValBool(1), "true", JSON_TRUE, 0);
    expectValue(jsonValBool(0), "false", JSON_FALSE, 0);
    expectValue(jsonValInt(0), "0", JSON_INT, 0);
    expectValue(jsonValInt(-1), "-1", JSON_INT, 0);
    expectValue(jsonValInt(INT64_MAX), "9223372036854775807", JSON_INT, 0);
    expectValue(jsonValInt(INT64_MIN), "-9223372036854775808", JSON_INT, 0);
    expectValue(jsonValInt((int64_t)1 << 60), "1152921504606846976", JSON_INT, 0);
    /* A double whose canonical text is short plain notation is a decimal,
     * which reads as a number with a text of its own. */
    expectValue(jsonValDouble(1.5), "1.5", JSON_RAWNUM, 0);
    expectValue(jsonValDouble(3.0), "3.0", JSON_RAWNUM, 0);
    expectValue(jsonValDouble(0.0), "0.0", JSON_RAWNUM, 0);
    expectValue(jsonValDouble(-19.9), "-19.9", JSON_RAWNUM, 0);
    expectValue(jsonValDouble(0.1 + 0.2), "0.30000000000000004", JSON_RAWNUM, 0); /* 17 digits: a decimal. */
    expectValue(jsonValDouble(1e21), "1e+21", JSON_DOUBLE, 0);
    expectValue(jsonValDouble(-0.0), "-0.0", JSON_DOUBLE, 0);
    expectValue(jsonValDouble(1e300), "1e+300", JSON_DOUBLE, 0);
    expectValue(jsonValDouble(5e-324), "5e-324", JSON_DOUBLE, 0);
    expectValue(jsonValNumText("12", 2), "12", JSON_INT, 0);
    expectValue(jsonValNumText("-0", 2), "0", JSON_INT, 0);
    expectValue(jsonValNumText("1.5", 3), "1.5", JSON_RAWNUM, 0);
    expectValue(jsonValNumText("1.50", 4), "1.50", JSON_RAWNUM, 0);
    expectValue(jsonValNumText("1e5", 3), "1e5", JSON_RAWNUM, 0);
    expectValue(jsonValNumText("1e-7", 4), "1e-7", JSON_DOUBLE, 0);
    expectValue(jsonValNumText("1e400", 5), "1e400", JSON_RAWNUM, 0);
    expectValue(jsonValNumText("18446744073709551616", 20), "18446744073709551616", JSON_RAWNUM, 0);
    CHECK(jsonValNumText("1x", 2) == NULL && jsonValNumText("", 0) == NULL && jsonValNumText("01", 2) == NULL);
    expectValue(jsonValString("", 0), "\"\"", JSON_STRING, 0);
    expectValue(jsonValString("abcdefg", 7), "\"abcdefg\"", JSON_STRING, 0);
    expectValue(jsonValString("abcdefgh", 8), "\"abcdefgh\"", JSON_STRING, 0);
    expectValue(jsonValString("a\"b\\c\n\x01\x7f/", 9), "\"a\\\"b\\\\c\\n\\u0001\x7f/\"", JSON_STRING, 0);
    expectValue(jsonValString("nul\0in", 6), "\"nul\\u0000in\"", JSON_STRING, 0);
    expectValue(jsonValEmptyArray(), "[]", JSON_ARRAY, 1);
    expectValue(jsonValEmptyObject(), "{}", JSON_OBJECT, 1);
    jsonValFree(NULL);
    jsonValFree(jsonValInt(5));

    /* A value that is parsed and discarded gives its keys back. */
    jsonEngineStats before, after;
    jsonEngineGetStats(&before);
    jsonErr err;
    const char *once = "{\"only-here-1\":{\"only-here-2\":[1,{\"only-here-3\":2}]}}";
    jsonVal *v = jsonValParse(once, strlen(once), 0, 0, &err, NULL);
    CHECK(v != NULL && jsonValDepth(v) == 4);
    jsonValFree(v);
    jsonEngineGetStats(&after);
    CHECK(after.keytable_keys == before.keytable_keys && after.keytable_live_bytes == before.keytable_live_bytes);
    CHECK(jsonKeyFind("only-here-2", 11) == 0);
}

static void unitBuf(void) {
    char stack[8];
    jsonBuf b;
    jsonBufInit(&b, stack, sizeof(stack));
    jsonBufAppend(&b, "abc", 3);
    CHECK(!b.heap && b.p == stack && b.len == 3);
    jsonBufAppend(&b, "defghijkl", 9);
    CHECK(b.heap && b.len == 12 && memcmp(b.p, "abcdefghijkl", 12) == 0);
    jsonBufAppendQuoted(&b, "q\"\\\b\f\n\r\t\x1f/\xc3\xa9\xff", 13);
    const char *want = "abcdefghijkl\"q\\\"\\\\\\b\\f\\n\\r\\t\\u001F/\xc3\xa9\xff\"";
    CHECK(b.len == strlen(want) && memcmp(b.p, want, b.len) == 0);
    jsonBufFree(&b);
    jsonBufInit(&b, NULL, 0);
    for (int i = 0; i < 1000; i++) jsonBufAppendQuoted(&b, "\x01\x02\x03", 3);
    CHECK(b.len == 1000 * 20);
    jsonBufFree(&b);
    jsonBufFree(&b);

    /* Reserving: room is made once, in caller memory if it is there already. */
    jsonBufInit(&b, stack, sizeof(stack));
    jsonBufAppend(&b, "ab", 2);
    jsonBufReserve(&b, 6);
    CHECK(!b.heap && b.p == stack && b.len == 2);
    jsonBufReserve(&b, 5000);
    CHECK(b.heap && b.cap >= 5002 && b.len == 2 && memcmp(b.p, "ab", 2) == 0);
    const char *reserved = b.p;
    for (int i = 0; i < 5000; i++) jsonBufAppend(&b, "x", 1);
    CHECK(b.p == reserved && b.len == 5002);
    jsonBufReserve(&b, 0);
    jsonBufAppend(&b, "y", 1);
    CHECK(b.len == 5003 && b.p[5002] == 'y');
    jsonBufFree(&b);
}

static void unitErrors(void) {
    static const struct {
        const char *text;
        int flags;
        uint32_t max_depth;
        jsonErr err;
        size_t off;
    } cases[] = {
        {"", 0, 0, JSON_ERR_SYNTAX, 0},
        {"   ", 0, 0, JSON_ERR_SYNTAX, 3},
        {"[1,]", 0, 0, JSON_ERR_SYNTAX, 3},
        {"{\"a\":1,}", 0, 0, JSON_ERR_SYNTAX, 7},
        {"[1] x", 0, 0, JSON_ERR_SYNTAX, 4},
        {"\"\xff\"", 0, 0, JSON_ERR_UTF8, 1},
        {"{\"\xc0\x80\":1}", 0, 0, JSON_ERR_UTF8, 2},
        {"\"a\\u00e9\xed\xa0\x80\"", 0, 0, JSON_ERR_UTF8, 8},
        {"\"\\ud800\"", 0, 0, JSON_ERR_SYNTAX, 1},
        {"\"\\udc00\"", JSON_PARSE_TRUSTED, 0, JSON_ERR_SYNTAX, 1},
        {"\"a\x01\"", JSON_PARSE_TRUSTED, 0, JSON_ERR_SYNTAX, 2},
        {"1e400", 0, 0, JSON_ERR_NUM_RANGE, 5},
        {"[1,-1e999]", 0, 0, JSON_ERR_NUM_RANGE, 9},
        {"[[1]]", 0, 1, JSON_ERR_DEPTH, 1},
        {"{\"a\":{\"b\":[]}}", 0, 2, JSON_ERR_DEPTH, 10},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        jsonErr err = JSON_OK;
        size_t off = 999;
        jsonVal *v = jsonValParse(cases[i].text, strlen(cases[i].text), cases[i].flags, cases[i].max_depth, &err, &off);
        if (v || err != cases[i].err || off != cases[i].off) {
            fprintf(stderr, "error case %zu (%s): got %s at %zu\n", i, cases[i].text, v ? "a value" : errName(err), off);
            exit(1);
        }
    }
    /* The same texts are accepted when the limit or the flag allows. */
    jsonErr err;
    jsonVal *v = jsonValParse("[[1]]", 5, 0, 2, &err, NULL);
    CHECK(v && err == JSON_OK && jsonValDepth(v) == 2);
    jsonValFree(v);
    const char *loose = "[\"\xff\xc0\",1e400,-1e999]";
    v = jsonValParse(loose, strlen(loose), JSON_PARSE_TRUSTED, 0, &err, NULL);
    CHECK(v != NULL);
    const jsonDoc d = jsonValToDoc(v);
    expectText(d, loose);
    jsonDocFree(d);
    CHECK(jsonValParse("[1", 2, 0, 0, NULL, NULL) == NULL);

    /* One byte order mark before the text is skipped, trusted or not. */
    static const char *with_bom[] = {"\xEF\xBB\xBF"
                                     "1",
                                     "\xEF\xBB\xBF {\"a\":[\"\xEF\xBB\xBF\"]}"};
    static const char *without[] = {"1", "{\"a\":[\"\xEF\xBB\xBF\"]}"};
    for (int i = 0; i < 2; i++) {
        for (int flags = 0; flags <= JSON_PARSE_TRUSTED; flags++) {
            v = jsonValParse(with_bom[i], strlen(with_bom[i]), flags, 0, &err, NULL);
            CHECK(v != NULL && err == JSON_OK);
            const jsonDoc doc = jsonValToDoc(v);
            expectText(doc, without[i]);
            jsonDocFree(doc);
        }
    }
    static const char *bad_bom[] = {"\xEF\xBB\xBF", "\xEF\xBB\xBF\xEF\xBB\xBF"
                                                    "1",
                                    " \xEF\xBB\xBF"
                                    "1",
                                    "1\xEF\xBB\xBF", "[\xEF\xBB\xBF"
                                                     "1]",
                                    "\xEF\xBB"
                                    "1"};
    for (size_t i = 0; i < sizeof(bad_bom) / sizeof(bad_bom[0]); i++) {
        for (int flags = 0; flags <= JSON_PARSE_TRUSTED; flags++) {
            CHECK(jsonValParse(bad_bom[i], strlen(bad_bom[i]), flags, 0, &err, NULL) == NULL && err == JSON_ERR_SYNTAX);
        }
    }
    size_t off = 0;
    CHECK(jsonValParse("\xEF\xBB\xBF[1,]", 7, 0, 0, &err, &off) == NULL && off == 6); /* Offsets count the mark. */
}

/* Documents right at the caps: one byte or one entry more leaves the packed
 * form. The default caps are 4096 bytes and 128 entries. */
static void unitCaps(void) {
    text t = {0, 0, 0};
    jsonDoc d;
    for (int n = 127; n <= 130; n++) {
        t.len = 0;
        textAddStr(&t, "[");
        for (int i = 0; i < n; i++) textAddStr(&t, i ? ",null" : "null");
        textAddStr(&t, "]");
        d = mustParse(t.p);
        CHECK((strcmp(jsonDocEncoding(d), "jpack") == 0) == (n <= 128));
        checkDoc(d);
        jsonDocFree(d);
    }
    /* A plain array of one string: 5 bytes of array header, 3 of string
     * header. 4088 string bytes make exactly 4096. */
    for (int len = 4086; len <= 4090; len++) {
        t.len = 0;
        textAddStr(&t, "[\"");
        for (int i = 0; i < len; i++) textAddStr(&t, "s");
        textAddStr(&t, "\"]");
        d = mustParse(t.p);
        CHECK((strcmp(jsonDocEncoding(d), "jpack") == 0) == (len <= 4088));
        CHECK(jsonDocMemUsage(d) == (len <= 4088 ? (size_t)len + 11 : 32 + 8 + 5 + (size_t)len));
        expectText(d, t.p);
        checkDoc(d);
        jsonDocFree(d);
    }
    /* Changed caps apply to what is emitted afterwards, including a copy. */
    d = mustParse("{\"a\":[1,2,3],\"b\":{\"c\":\"0123456789abcdef\"}}");
    CHECK(strcmp(jsonDocEncoding(d), "jpack") == 0 && jsonDocFreeEffort(d) == 1);
    jsonEngineSetCaps(16, 2);
    const jsonDoc small = jsonDocCopy(d);
    CHECK(strcmp(jsonDocEncoding(small), "jtree") == 0 && jsonDocFreeEffort(small) > 1);
    expectText(small, "{\"a\":[1,2,3],\"b\":{\"c\":\"0123456789abcdef\"}}");
    jsonEngineSetCaps(4096, 128);
    const jsonDoc back = jsonDocCopy(small);
    CHECK(strcmp(jsonDocEncoding(back), "jpack") == 0 && jsonDocMemUsage(back) == jsonDocMemUsage(d));
    jsonDocFree(back);
    jsonDocFree(small);
    jsonDocFree(d);
    /* The estimate of a container is what the emitter then requests. */
    static const char *shapes[] = {"[]", "{}", "[1,2,3]", "{\"a\":[1.5,\"x\"],\"b\":{}}", "[0.50,0.25]"};
    for (int caps = 0; caps < 2; caps++) {
        jsonEngineSetCaps(caps ? 16 : 4096, caps ? 2 : 128);
        for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
            jsonErr err;
            jsonVal *v = jsonValParse(shapes[i], strlen(shapes[i]), 0, 0, &err, NULL);
            const size_t estimate = jsonValSizeEstimate(v);
            d = jsonValToDoc(v);
            CHECK(estimate == jsonDocMemUsage(d));
            jsonDocFree(d);
        }
    }
    jsonEngineSetCaps(4096, 128);
    free(t.p);
}

static void *freeOnThread(void *arg) {
    jsonDocFree(*(jsonDoc *)arg);
    return NULL;
}

static void freeElsewhere(jsonDoc d) {
    pthread_t th;
    CHECK(pthread_create(&th, NULL, freeOnThread, &d) == 0);
    CHECK(pthread_join(th, NULL) == 0);
}

static void unitKeyTable(void) {
    jsonEngineStats s0, s1;
    uint32_t len;
    jsonEngineGetStats(&s0);
    CHECK(jsonKeyFind("kt-alpha", 8) == 0);
    jsonDoc d = mustParse("{\"kt-alpha\":1,\"kt-beta\":{\"kt-alpha\":2}}");
    jsonEngineGetStats(&s1);
    CHECK(s1.keytable_keys >= 2 && s1.keytable_bytes > 0);
    const uint32_t id = jsonKeyFind("kt-alpha", 8);
    CHECK(id != 0 && jsonKeyFind("kt-alph", 7) == 0);
    const char *name = jsonKeyName(id, &len);
    CHECK(len == 8 && memcmp(name, "kt-alpha", 8) == 0);
    /* The reference from jsonKeyFind keeps the key after the document and
     * the key cache let go of it. */
    jsonDocFree(d);
    jsonDocFree(mustParse("{\"x\":1,\"y\":{\"x\":2}}"));
    CHECK(jsonKeyFind("kt-beta", 7) == 0);
    CHECK(jsonKeyName(id, &len) == name);
    const uint32_t again = jsonKeyFind("kt-alpha", 8);
    CHECK(again == id);
    jsonKeyRelease(again);
    jsonKeyRelease(id);
    CHECK(jsonKeyFind("kt-alpha", 8) == 0);

    /* Many keys: several segments, hash index growth, then all released. A
     * small ID freed early is handed out again first. */
    text t = {0, 0, 0};
    char member[64];
    textAddStr(&t, "{");
    for (int i = 0; i < 5000; i++) {
        snprintf(member, sizeof(member), "%s\"many-%d\":%d", i ? "," : "", i, i);
        textAddStr(&t, member);
    }
    textAddStr(&t, "}");
    jsonEngineGetStats(&s0);
    d = mustParse(t.p);
    jsonEngineGetStats(&s1);
    CHECK(s1.keytable_keys == s0.keytable_keys + 5000 && s1.keytable_live_bytes > s0.keytable_live_bytes + 5000 * 6);
    const uint32_t first = jsonKeyFind("many-0", 6), last = jsonKeyFind("many-4999", 9);
    CHECK(first && last > first && last >= 5000);
    jsonKeyRelease(first);
    jsonKeyRelease(last);
    checkDoc(d);
    jsonDocFree(d);
    jsonDocFree(mustParse("{\"x\":1}"));
    jsonEngineGetStats(&s1);
    /* Segments were allocated and released on the way: all of it is back. */
    CHECK(s1.keytable_keys == s0.keytable_keys && s1.keytable_live_bytes == s0.keytable_live_bytes);
    d = mustParse("{\"fresh-key\":1}");
    const uint32_t fresh = jsonKeyFind("fresh-key", 9);
    CHECK(fresh != 0 && fresh < 300);
    jsonKeyRelease(fresh);
    jsonDocFree(d);
    free(t.p);

    /* A document freed on another thread leaves its keys at zero until the
     * sweep. The key cache must not hold them, so evict it first. */
    d = mustParse("{\"thr-1\":[{\"thr-2\":1}],\"thr-3\":{\"thr-4\":null}}");
    const jsonDoc big = mustParse("[{\"thr-5\":[1,2,3]},{\"thr-5\":[4,5,6]}]");
    jsonDocFree(mustParse("{\"x\":[{\"x\":1}],\"y\":{\"x\":null}}"));
    jsonDocFree(mustParse("[{\"x\":[1,2,3]}]"));
    jsonEngineGetStats(&s0);
    CHECK(jsonKeyTableSweep() == 0);
    freeElsewhere(d);
    freeElsewhere(big);
    jsonEngineGetStats(&s1);
    CHECK(s1.keytable_keys == s0.keytable_keys); /* Still in the table... */
    CHECK(jsonKeyFind("thr-2", 5) == 0);         /* ...but used by nothing. */
    /* The live bytes fell at once, by five names of five bytes, though the
     * memory waits for the sweep. A key revived before the sweep counts
     * again, and the sweep itself changes nothing. */
    CHECK(s1.keytable_live_bytes == s0.keytable_live_bytes - 25 && s1.keytable_bytes == s0.keytable_bytes);
    d = mustParse("{\"thr-4\":7}");
    jsonEngineGetStats(&s1);
    CHECK(s1.keytable_live_bytes == s0.keytable_live_bytes - 20);
    while (jsonKeyTableSweep()) {
    }
    jsonEngineGetStats(&s1);
    CHECK(s1.keytable_live_bytes == s0.keytable_live_bytes - 20 && s1.keytable_keys == s0.keytable_keys - 4);
    CHECK(!jsonKeyFind("thr-1", 5) && !jsonKeyFind("thr-2", 5) && !jsonKeyFind("thr-3", 5) && !jsonKeyFind("thr-5", 5));
    const uint32_t revived = jsonKeyFind("thr-4", 5);
    CHECK(revived != 0);
    jsonKeyRelease(revived);
    jsonDocFree(d);
    jsonEngineGetStats(&s1);
    CHECK(s1.keytable_live_bytes == s0.keytable_live_bytes - 25 && s1.keytable_keys == s0.keytable_keys - 5);
    CHECK(jsonKeyTableSweep() == 0);
}

/* A spine document freed on another thread, with everything else quiet. */
static void unitThreadFreeSpine(void) {
    text t = {0, 0, 0};
    char member[64];
    textAddStr(&t, "{");
    for (int i = 0; i < 300; i++) {
        snprintf(member, sizeof(member), "%s\"sp-%d\":{\"sp-in\":[%d,\"0123456789\"]}", i ? "," : "", i, i);
        textAddStr(&t, member);
    }
    textAddStr(&t, "}");
    const jsonDoc d = mustParse(t.p);
    CHECK(strcmp(jsonDocEncoding(d), "jtree") == 0);
    jsonDocFree(mustParse("{\"x\":{\"x\":[1]}}"));
    freeElsewhere(d);
    while (jsonKeyTableSweep()) {
    }
    CHECK(jsonKeyFind("sp-7", 4) == 0);

    /* The sweep is bounded: 20000 keys dropped elsewhere take several calls. */
    jsonEngineStats before, after;
    t.len = 0;
    textAddStr(&t, "[");
    for (int i = 0; i < 20000; i++) {
        snprintf(member, sizeof(member), "%s{\"sweep-%d\":0}", i ? "," : "", i);
        textAddStr(&t, member);
    }
    textAddStr(&t, "]");
    const jsonDoc many = mustParse(t.p);
    jsonEngineGetStats(&before);
    freeElsewhere(many);
    int calls = 1;
    while (jsonKeyTableSweep()) calls++;
    jsonEngineGetStats(&after);
    CHECK(calls >= 4 && before.keytable_keys - after.keytable_keys >= 19999);
    /* The key cache still holds the last of them, whose high ID pins every
     * segment. Once that is gone the table gives its memory back. */
    jsonDocFree(mustParse("[{\"x\":0}]"));
    jsonEngineGetStats(&after);
    CHECK(after.keytable_bytes < before.keytable_bytes / 100);
    free(t.p);
}

static void unitFormat(void) {
    static text t;
    jsonRef root;
    const jsonDoc d = mustParse("{\"a\":[1,[],{}],\"b\":{\"c\":null,\"d\":[2.5]},\"e\":\"s\"}");
    jsonDocRoot(d, &root);
    const jsonFormat pretty = {"  ", "\n", " ", 2, 1, 1, 0};
    serialize(&root, &pretty, &t);
    const char *want = "{\n  \"a\": [\n    1,\n    [],\n    {}\n  ],\n  \"b\": {\n    \"c\": null,\n    \"d\": [\n      2.5\n"
                       "    ]\n  },\n  \"e\": \"s\"\n}";
    CHECK(t.len == strlen(want) && memcmp(t.p, want, t.len) == 0);
    const jsonFormat none = {NULL, NULL, NULL, 0, 0, 0, 0};
    serialize(&root, &none, &t);
    want = "{\"a\":[1,[],{}],\"b\":{\"c\":null,\"d\":[2.5]},\"e\":\"s\"}";
    CHECK(t.len == strlen(want) && memcmp(t.p, want, t.len) == 0);
    const jsonFormat space_only = {NULL, NULL, "_", 0, 0, 1, 0};
    serialize(&root, &space_only, &t);
    want = "{\"a\":_[1,[],{}],\"b\":_{\"c\":_null,\"d\":_[2.5]},\"e\":_\"s\"}";
    CHECK(t.len == strlen(want) && memcmp(t.p, want, t.len) == 0);
    jsonDocFree(d);
    CHECK(jsonDocMemUsage(0) == 0 && jsonDocCopy(0) == 0);
    jsonDocFree(0);
}

/* Runs first, on an empty key table, where the counts are exact. The test
 * allocator's usable size is the requested size, so the live bytes of a key
 * are the length of its name. */
static void unitStats(void) {
    jsonEngineStats s;
    jsonEngineGetStats(&s);
    CHECK(s.keytable_keys == 0 && s.keytable_bytes == 0 && s.keytable_live_bytes == 0);
    jsonDoc d = mustParse("{\"a\":1,\"bcd\":{\"a\":2,\"ef\":3}}");
    jsonEngineGetStats(&s);
    CHECK(s.keytable_keys == 3 && s.keytable_bytes >= 6 && s.keytable_live_bytes == 6);
    const size_t bytes = s.keytable_bytes;
    const jsonDoc same = mustParse("{\"a\":1,\"bcd\":{\"a\":2,\"ef\":3}}");
    jsonEngineGetStats(&s);
    CHECK(s.keytable_keys == 3 && s.keytable_bytes == bytes && s.keytable_live_bytes == 6);
    const uint32_t held = jsonKeyFind("ef", 2);
    jsonDocFree(d);
    jsonEngineGetStats(&s);
    CHECK(s.keytable_keys == 3 && s.keytable_live_bytes == 6);
    jsonDocFree(same);
    jsonEngineGetStats(&s);
    CHECK(s.keytable_keys == 1 && s.keytable_live_bytes == 2); /* Only the key this test holds. */
    jsonKeyRelease(held);
    jsonEngineGetStats(&s);
    CHECK(s.keytable_keys == 0 && s.keytable_live_bytes == 0);
    CHECK(jsonEngineShutdown() == 0 && live_bytes == 0 && live_blocks == 0);
    /* A document that is never freed is reported, and the engine can be
     * started again. */
    jsonEngineInit(&test_allocator);
    d = mustParse("{\"leak-1\":{\"leak-2\":[]}}");
    CHECK(jsonEngineShutdown() == 2);
    testFree((void *)(uintptr_t)d); /* The packed root node, which the engine no longer knows. */
    CHECK(live_bytes == 0 && live_blocks == 0);
    jsonEngineInit(&test_allocator);
}

/* jsonRefMemUsage, on documents whose layout is easy to state. The test
 * allocator's usable size is the requested size. */
static void unitMemUsage(void) {
    static const char *doc = "{\"mu-a\":[1,2,3],\"mu-b\":\"0123456789\"}";
    jsonRef root, a, b, elem;
    /* Packed: a 3-byte node header, a 3-byte object header, and per member a
     * one- or two-byte key, then 2 + 3 bytes of typed array and 1 + 10 of string. */
    jsonDoc d = mustParse(doc);
    jsonDocRoot(d, &root);
    CHECK(jsonObjectGet(&root, "mu-a", 4, &a, NULL) && jsonObjectGet(&root, "mu-b", 4, &b, NULL));
    CHECK(jsonArrayGet(&a, 1, &elem));
    CHECK(jsonRefMemUsage(&a) == 5 && jsonRefMemUsage(&b) == 11 && jsonRefMemUsage(&elem) == 1);
    const size_t keys = jsonDocMemUsage(d) - (3 + 3 + 5 + 11);
    CHECK(keys >= 2 && keys <= 4 && jsonRefMemUsage(&root) == jsonDocMemUsage(d));
    jsonDocFree(d);
    /* Spine: an OBJECT node of 32 + 2 * 16 + 4 bytes whose slots point at a
     * typed array node of 16 + 3 bytes and a string node of 2 + 10. A value
     * in a slot counts the slot. */
    jsonEngineSetCaps(16, 1);
    d = mustParse(doc);
    jsonDocRoot(d, &root);
    CHECK(jsonObjectGet(&root, "mu-a", 4, &a, NULL) && jsonObjectGet(&root, "mu-b", 4, &b, NULL));
    CHECK(jsonArrayGet(&a, 1, &elem));
    CHECK(jsonRefMemUsage(&a) == 8 + 19 && jsonRefMemUsage(&b) == 8 + 12 && jsonRefMemUsage(&elem) == 1);
    CHECK(jsonDocMemUsage(d) == 68 + 19 + 12 && jsonRefMemUsage(&root) == 68 + 19 + 12 && jsonDocFreeEffort(d) == 3);
    jsonDocFree(d);
    jsonEngineSetCaps(4096, 128);
}

/* Strings, member names and number text too long for the 24-bit length of
 * a tape entry. */
static void unitHuge(void) {
    const size_t big = ((size_t)1 << 24) + 1000;
    text t = {0, 0, 0}, out = {0, 0, 0};
    char *run = malloc(big);
    jsonRef root, val;
    jsonErr err;
    memset(run, '7', big);
    textAddStr(&t, "{\"");
    textAdd(&t, run, big);
    textAddStr(&t, "\":[\"");
    textAdd(&t, run, big);
    textAddStr(&t, "\\n\",0.");
    textAdd(&t, run, big);
    textAddStr(&t, ",1]}");
    jsonVal *v = jsonValParse(t.p, t.len, 0, 0, &err, NULL);
    CHECK(v != NULL);
    const size_t estimate = jsonValSizeEstimate(v);
    const jsonDoc d = jsonValToDoc(v);
    CHECK(estimate == jsonDocMemUsage(d));
    jsonDocRoot(d, &root);
    serialize(&root, NULL, &out);
    CHECK(out.len == t.len && memcmp(out.p, t.p, t.len) == 0);
    CHECK(jsonObjectGet(&root, run, big, &val, NULL) && jsonRefCount(&val) == 3);
    checkDoc(d);
    jsonDocFree(d);
    v = jsonValString(run, big);
    CHECK(v != NULL && jsonValSizeEstimate(v) == big + 5);
    jsonValFree(v);
    CHECK(jsonValParse(t.p, ((size_t)1 << 29) + 1, 0, 0, &err, NULL) == NULL && err == JSON_ERR_TOO_BIG);
    free(run);
    free(t.p);
    free(out.p);
}

/* ----------------------------------------------------------------------------
 * Corpus operations
 * ------------------------------------------------------------------------- */

/* The live bytes of the key table, which every operation on a document that
 * is added and then removed has to leave where they were. */
static size_t keyLiveBytes(void) {
    jsonEngineStats stats;
    jsonEngineGetStats(&stats);
    return stats.keytable_live_bytes;
}

static void opRoundtrip(const char *s, size_t n, text *out) {
    jsonErr err;
    size_t off = 0;
    jsonRef root;
    char line[64];
    const size_t key_bytes = keyLiveBytes();
    const jsonDoc d = parseDoc(s, n, &err, &off);
    out->len = 0;
    if (!d) {
        snprintf(line, sizeof(line), "!E %s %zu", errName(err), off);
        textAddStr(out, line);
        return;
    }
    checkDoc(d);
    static text raw;
    jsonDocRoot(d, &root);
    serialize(&root, opt.has_fmt ? &opt.fmt : NULL, &raw);
    textAddHex(out, raw.p, raw.len);
    jsonDocFree(d);
    CHECK(keyLiveBytes() == key_bytes);
}

static void opDump(const char *s, size_t n, text *out) {
    jsonErr err;
    const jsonDoc d = parseDoc(s, n, &err, NULL);
    out->len = 0;
    if (!d) {
        textAddStr(out, "!E");
        return;
    }
    checkDoc(d);
    dumpDoc(d, n < 20000, out);
    jsonDocFree(d);
}

static void opEquals(const char *s, size_t n, text *out) {
    jsonErr err;
    jsonRef ra, rb;
    const char *tab = memchr(s, '\t', n);
    CHECK(tab != NULL);
    jsonEngineSetCaps(opt.cap_bytes, opt.cap_entries);
    const jsonDoc a = parseDoc(s, (size_t)(tab - s), &err, NULL);
    jsonEngineSetCaps(opt.cap2_bytes, opt.cap2_entries);
    const jsonDoc b = parseDoc(tab + 1, n - (size_t)(tab + 1 - s), &err, NULL);
    CHECK(a && b);
    jsonDocRoot(a, &ra);
    jsonDocRoot(b, &rb);
    const int eq = jsonRefEquals(&ra, &rb);
    CHECK(eq == jsonRefEquals(&rb, &ra));
    CHECK(jsonRefEquals(&ra, &ra) && jsonRefEquals(&rb, &rb));
    out->len = 0;
    textAddStr(out, eq ? "1" : "0");
    jsonDocFree(a);
    jsonDocFree(b);
}

/* ----------------------------------------------------------------------------
 * mutate: scripts of changes
 *
 * A line is a JSON text followed by changes, separated by 0x1E. The fields of
 * a change are separated by 0x1F: a one-letter code, a path, arguments.
 *
 *   S path json          jsonSet            I path int      jsonSetInt
 *   D path double        jsonSetDouble      N path text     jsonSetNumText
 *   B path 0|1           jsonSetBool        X path          jsonDelete
 *   A path name json     jsonObjectAdd      T path start count  jsonArrayTrim
 *   R path index json... jsonArrayInsert    P path bytes    jsonStringAppend
 *   F mode k             defrag: a = move everything, r = move a random half,
 *                        s = move everything, stopping after every k steps and
 *                        going on after each later change
 *   C                    replace the document by a copy of itself
 *
 * A path is steps joined by '/': an array index, or 'k' and a member name in
 * hex. Names and appended bytes are in hex.
 *
 * For the first document and after every change one line is printed: "=",
 * the serialised document in hex, a tab, and the dump (or "-"). A line "."
 * ends each script.
 * ------------------------------------------------------------------------- */

#define FIELD_SEP 0x1F
#define CHANGE_SEP 0x1E
#define MAX_FIELDS 64
#define MAX_PATH 256
#define MAX_CHANGES 4096

static uint64_t write_counts[128][JSON_WRITE_KINDS]; /* By change code. */

static struct {
    int active;
    char mode;
    long every, steps;
    uint64_t rng;
    size_t moves;
    unsigned long cursor;
} defrag;

static void *defragMove(void *p, void *ctx) {
    (void)ctx;
    if (defrag.mode == 'r') {
        defrag.rng = defrag.rng * 6364136223846793005ull + 1442695040888963407ull;
        if (defrag.rng >> 63) return NULL;
    }
    const size_t size = blockOf(p)->size;
    void *np = testAlloc(size);
    memcpy(np, p, size);
    testFree(p);
    defrag.moves++;
    return np;
}

static int defragStop(void *ctx) {
    (void)ctx;
    if (++defrag.steps < defrag.every) return 0;
    defrag.steps = 0;
    return 1;
}

/* One more call of an incremental defrag. */
static void defragStep(jsonDoc *d) {
    if (!defrag.active) return;
    defrag.active = jsonDocDefrag(d, defragMove, defrag.mode == 's' ? defragStop : NULL, NULL, &defrag.cursor);
    if (defrag.active) CHECK(defrag.cursor != 0);
}

static void defragStart(jsonDoc *d, char mode, long k) {
    while (defrag.active) defragStep(d);
    const size_t effort = jsonDocFreeEffort(*d), usage = jsonDocMemUsage(*d);
    defrag.mode = mode;
    defrag.every = k > 0 ? k : 1;
    defrag.steps = 0;
    defrag.rng = (uint64_t)k * 0x9E3779B97F4A7C15ull + 1;
    defrag.moves = 0;
    defrag.cursor = 0;
    defrag.active = 1;
    defragStep(d);
    if (mode != 's') {
        CHECK(!defrag.active);
        /* With no change in between, every allocation is offered once. */
        if (mode == 'a') CHECK(defrag.moves == (usage ? effort : 0));
    }
}

static size_t splitFields(char *s, size_t n, char sep, char **field, size_t *len, size_t max) {
    size_t count = 0, start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != sep) continue;
        CHECK(count < max);
        field[count] = s + start;
        len[count++] = i - start;
        start = i + 1;
    }
    return count;
}

static size_t unhex(char *s, size_t n) {
    for (size_t i = 0; i < n / 2; i++) s[i] = (char)(hexValue(s[2 * i]) << 4 | hexValue(s[2 * i + 1]));
    return n / 2;
}

/* Turns a path into a locator, through the read functions. */
static int resolvePath(jsonDoc d, char *path, size_t len, jsonStep *loc) {
    char *step[MAX_PATH];
    size_t step_len[MAX_PATH];
    jsonRef cur, next;
    if (!len) return 0;
    const size_t n = splitFields(path, len, '/', step, step_len, MAX_PATH);
    jsonDocRoot(d, &cur);
    for (size_t i = 0; i < n; i++) {
        if (step[i][0] == 'k') {
            const size_t name_len = unhex(step[i] + 1, step_len[i] - 1);
            CHECK(jsonObjectGet(&cur, step[i] + 1, name_len, &next, &loc[i]));
        } else {
            loc[i].pos = (uint32_t)strtoul(step[i], NULL, 10);
            loc[i].keyid = 0;
            CHECK(jsonArrayGet(&cur, loc[i].pos, &next));
        }
        cur = next;
    }
    /* What the engine is given must be what its own lookup finds. */
    CHECK(jsonDocResolve(d, loc, (int)n, &next));
    return (int)n;
}

static jsonVal *parseVal(const char *s, size_t n) {
    jsonErr err;
    jsonVal *v = jsonValParse(s, n, opt.trusted ? JSON_PARSE_TRUSTED : 0, 0, &err, NULL);
    CHECK(v != NULL);
    return v;
}

/* A value that was written into a document must still be whole: turned into
 * a document of its own it reads as its source text does. */
static void checkValStillGood(jsonVal *v, const char *s, size_t n) {
    static text a, b;
    jsonRef ra, rb;
    jsonErr err;
    const jsonDoc from_val = jsonValToDoc(v), from_text = parseDoc(s, n, &err, NULL);
    jsonDocRoot(from_val, &ra);
    jsonDocRoot(from_text, &rb);
    serialize(&ra, NULL, &a);
    serialize(&rb, NULL, &b);
    CHECK(a.len == b.len && memcmp(a.p, b.p, a.len) == 0);
    jsonDocFree(from_val);
    jsonDocFree(from_text);
}

/* Replaces the document by a copy, checking on the way that the original
 * held exactly the bytes and nodes it said it did. */
static void swapForCopy(jsonDoc *d) {
    const jsonDoc copy = jsonDocCopy(*d);
    const size_t usage = jsonDocMemUsage(*d), effort = jsonDocFreeEffort(*d);
    const int tree = strcmp(jsonDocEncoding(*d), "jtree") == 0;
    const size_t bytes_before = live_bytes, blocks_before = live_blocks;
    jsonDocFree(*d);
    CHECK(bytes_before - live_bytes == usage);
    if (tree && effort > 1) {
        CHECK(blocks_before - live_blocks == effort);
    } else {
        CHECK(effort == 1 && blocks_before - live_blocks <= 1);
    }
    *d = copy;
}

/* Checks made on the document after every change, and its output line. */
static void checkAfterChange(jsonDoc d, text *line, int with_dump) {
    static text raw, again, dump;
    jsonRef root, reparsed_root;
    jsonErr err;
    jsonDocRoot(d, &root);
    CHECK(jsonRefMemUsage(&root) == jsonDocMemUsage(d)); /* Tracked total against a walk. */
    if (jsonRefType(&root) >= JSON_ARRAY && strcmp(jsonDocEncoding(d), "jpack") == 0) {
        /* A packed root stays within the caps whatever was done to it: 3 bytes are the node's header. */
        CHECK(jsonRefCount(&root) <= opt.cap_entries && jsonDocMemUsage(d) <= (size_t)opt.cap_bytes + 3);
    }
    serialize(&root, NULL, &raw);
    /* The text parses back to an equal document. */
    const int trusted = opt.trusted;
    opt.trusted = 1;
    const jsonDoc reparsed = parseDoc(raw.p, raw.len, &err, NULL);
    opt.trusted = trusted;
    CHECK(reparsed != 0);
    jsonDocRoot(reparsed, &reparsed_root);
    CHECK(jsonRefEquals(&root, &reparsed_root) && jsonRefEquals(&reparsed_root, &root));
    serialize(&reparsed_root, NULL, &again);
    CHECK(raw.len == again.len && memcmp(raw.p, again.p, raw.len) == 0);
    jsonDocFree(reparsed);
    line->len = 0;
    textAddStr(line, "=");
    textAddHex(line, raw.p, raw.len);
    textAddStr(line, "\t");
    if (with_dump) {
        dumpDoc(d, raw.len < 4000, &dump);
        textAdd(line, dump.p, dump.len);
    } else {
        textAddStr(line, "-");
    }
    fwrite(line->p, 1, line->len, stdout);
    fputc('\n', stdout);
}

static void applyChange(jsonDoc *d, char **f, size_t *flen, size_t nf) {
    jsonStep loc[MAX_PATH];
    jsonVal *vals[MAX_FIELDS];
    const char code = f[0][0];
    uint64_t before[JSON_WRITE_KINDS], after[JSON_WRITE_KINDS];
    for (size_t i = 1; i < nf; i++) f[i][flen[i]] = '\0'; /* Over the separator, or the script's end. */
    if (code == 'C') {
        swapForCopy(d);
        return;
    }
    if (code == 'F') {
        defragStart(d, f[1][0], strtol(f[2], NULL, 10));
        return;
    }
    const int n = resolvePath(*d, f[1], flen[1], loc);
    jsonEngineGetWriteStats(before);
    switch (code) {
    case 'S':
        vals[0] = parseVal(f[2], flen[2]);
        CHECK(jsonValType(vals[0]) <= JSON_OBJECT);
        jsonSet(d, loc, n, vals[0]);
        checkValStillGood(vals[0], f[2], flen[2]);
        break;
    case 'I': jsonSetInt(d, loc, n, strtoll(f[2], NULL, 10)); break;
    case 'D': jsonSetDouble(d, loc, n, strtod(f[2], NULL)); break;
    case 'N': jsonSetNumText(d, loc, n, f[2], flen[2]); break;
    case 'M': { /* Arithmetic as a command does it. */
        jsonRef ref;
        CHECK(jsonDocResolve(*d, loc, n, &ref));
        jsonSetDouble(d, loc, n, jsonRefDouble(&ref) + strtod(f[2], NULL));
        break;
    }
    case 'B': jsonSetBool(d, loc, n, f[2][0] == '1'); break;
    case 'X': jsonDelete(d, loc, n); break;
    case 'A': {
        const size_t name_len = unhex(f[2], flen[2]);
        vals[0] = parseVal(f[3], flen[3]);
        jsonObjectAdd(d, loc, n, f[2], name_len, vals[0]);
        checkValStillGood(vals[0], f[3], flen[3]);
        break;
    }
    case 'R': {
        const uint32_t count = (uint32_t)(nf - 3);
        for (uint32_t i = 0; i < count; i++) vals[i] = parseVal(f[3 + i], flen[3 + i]);
        jsonArrayInsert(d, loc, n, (uint32_t)strtoul(f[2], NULL, 10), (const jsonVal *const *)vals, count);
        for (uint32_t i = 0; i < count; i++) checkValStillGood(vals[i], f[3 + i], flen[3 + i]);
        break;
    }
    case 'T': jsonArrayTrim(d, loc, n, (uint32_t)strtoul(f[2], NULL, 10), (uint32_t)strtoul(f[3], NULL, 10)); break;
    case 'P': jsonStringAppend(d, loc, n, f[2], unhex(f[2], flen[2])); break;
    default: CHECK(!"unknown change");
    }
    jsonEngineGetWriteStats(after);
    for (int k = 0; k < JSON_WRITE_KINDS; k++) write_counts[(unsigned char)code][k] += after[k] - before[k];
}

static void opMutate(const char *s, size_t n, text *out) {
    static char *change[MAX_CHANGES];
    static size_t change_len[MAX_CHANGES];
    char *script = malloc(n + 1), *field[MAX_FIELDS];
    size_t field_len[MAX_FIELDS];
    jsonErr err;
    memcpy(script, s, n);
    script[n] = '\0';
    const size_t changes = splitFields(script, n, CHANGE_SEP, change, change_len, MAX_CHANGES);
    const size_t key_bytes = keyLiveBytes();
    jsonDoc d = parseDoc(change[0], change_len[0], &err, NULL);
    CHECK(d != 0);
    defrag.active = 0;
    checkAfterChange(d, out, 1);
    for (size_t i = 1; i < changes; i++) {
        const size_t nf = splitFields(change[i], change_len[i], FIELD_SEP, field, field_len, MAX_FIELDS);
        applyChange(&d, field, field_len, nf);
        defragStep(&d);
        checkAfterChange(d, out, change_len[0] < 3000 || i % 4 == 0 || i + 1 == changes);
    }
    while (defrag.active) defragStep(&d);
    swapForCopy(&d);
    jsonDocFree(d);
    CHECK(keyLiveBytes() == key_bytes); /* Whatever the script added and removed. */
    free(script);
    out->len = 0;
    textAddStr(out, ".");
}

static void printWriteCounts(void) {
    for (int code = 0; code < 128; code++) {
        const uint64_t *c = write_counts[code];
        if (!(c[0] | c[1] | c[2] | c[3])) continue;
        printf("#%c in_place=%" PRIu64 " splice=%" PRIu64 " reemit=%" PRIu64 " slot=%" PRIu64 "\n", code,
               c[JSON_WRITE_IN_PLACE], c[JSON_WRITE_SPLICE], c[JSON_WRITE_REEMIT], c[JSON_WRITE_SLOT]);
    }
}

/* The additions of the second phase that need no corpus. */
static void unitPhase2(void) {
    static text t;
    jsonRef root, ref;
    jsonStep loc[4];
    const char *want;

    CHECK(jsonEngineAllocator()->alloc == testAlloc && jsonEngineAllocator()->usable == testUsable);

    /* jsonValType, for every kind of root. */
    static const struct {
        const char *text;
        jsonType type;
    } types[] = {
        {"null", JSON_NULL},
        {"false", JSON_FALSE},
        {"true", JSON_TRUE},
        {"12", JSON_INT},
        {"-0", JSON_INT},
        {"1.5", JSON_RAWNUM},
        {"1.50", JSON_RAWNUM},
        {"1e-7", JSON_DOUBLE},
        {"-0.0", JSON_DOUBLE},
        {"0.30000000000000004", JSON_RAWNUM},
        {"1.5e-7", JSON_DOUBLE},
        {"123456789012345680000.0", JSON_DOUBLE},
        {"99999999999999999999", JSON_RAWNUM},
        {"\"s\"", JSON_STRING},
        {"[1]", JSON_ARRAY},
        {"[0.50]", JSON_ARRAY},
        {"{}", JSON_OBJECT},
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        jsonErr err;
        jsonVal *v = jsonValParse(types[i].text, strlen(types[i].text), 0, 0, &err, NULL);
        CHECK(v && jsonValType(v) == types[i].type);
        jsonValFree(v);
    }

    /* A value starting at an indent level, as inside text the caller builds. */
    jsonDoc d = mustParse("{\"a\":[1,{\"b\":[]}],\"c\":2.5}");
    jsonDocRoot(d, &root);
    const jsonFormat at_level_2 = {"--", "\n", " ", 2, 1, 1, 2};
    serialize(&root, &at_level_2, &t);
    want = "{\n------\"a\": [\n--------1,\n--------{\n----------\"b\": []\n--------}\n------],\n------\"c\": 2.5\n----}";
    CHECK(t.len == strlen(want) && memcmp(t.p, want, t.len) == 0);
    jsonDocFree(d);

    /* A change to one value leaves a locator to another good, and a value
     * can be written any number of times. */
    jsonVal *v = jsonValParse("{\"k\":[1,2]}", 11, 0, 0, NULL, NULL);
    d = mustParse("{\"x\":1,\"y\":[null,null,null],\"z\":\"s\"}");
    jsonDocRoot(d, &root);
    CHECK(jsonObjectGet(&root, "y", 1, &ref, &loc[0]));
    for (uint32_t i = 3; i-- > 0;) { /* Higher indexes first. */
        loc[1].pos = i;
        loc[1].keyid = 0;
        jsonSet(&d, loc, 2, v);
    }
    jsonObjectAdd(&d, NULL, 0, "w", 1, v);
    jsonSet(&d, loc, 1, v);
    jsonValFree(v);
    expectText(d, "{\"x\":1,\"y\":{\"k\":[1,2]},\"z\":\"s\",\"w\":{\"k\":[1,2]}}");
    jsonDocFree(d);

    /* A computed double gets its canonical text wherever it is written. The
     * typed decimal array here takes one of its scale in place, widens for a
     * larger one, and becomes a plain array for another scale. */
    d = mustParse("[0.50,0.25,1.25]");
    CHECK(jsonDocMemUsage(d) == 3 + 3 + 3);
    loc[0].keyid = 0;
    loc[0].pos = 0;
    jsonSetDouble(&d, loc, 1, 0.75);
    expectText(d, "[0.75,0.25,1.25]");
    CHECK(jsonDocMemUsage(d) == 3 + 3 + 3);
    jsonSetDouble(&d, loc, 1, 300.25);
    expectText(d, "[300.25,0.25,1.25]");
    CHECK(jsonDocMemUsage(d) == 3 + 3 + 3 * 2);
    loc[0].pos = 1;
    jsonSetDouble(&d, loc, 1, 3.0);
    loc[0].pos = 2;
    jsonSetDouble(&d, loc, 1, 0.1 + 0.2);
    expectText(d, "[300.25,3.0,0.30000000000000004]");
    jsonSetInt(&d, loc, 1, 4);
    v = jsonValNumText("0.500", 5);
    jsonArrayInsert(&d, NULL, 0, 0, (const jsonVal *const *)&v, 1); /* A given value keeps its text. */
    jsonValFree(v);
    expectText(d, "[0.500,300.25,3.0,4]");
    jsonDocFree(d);

    /* With caps of 0 nothing is packed, not even an empty container. */
    jsonEngineSetCaps(0, 0);
    d = mustParse("{\"a\":[],\"b\":{},\"c\":[1,2],\"d\":\"0123456789\"}");
    CHECK(strcmp(jsonDocEncoding(d), "jtree") == 0 && jsonDocFreeEffort(d) == 5);
    checkDoc(d);
    jsonDocFree(d);
    d = mustParse("[]");
    CHECK(strcmp(jsonDocEncoding(d), "jtree") == 0 && jsonDocMemUsage(d) == 32);
    jsonDocFree(d);
    jsonEngineSetCaps(4096, 128);

    /* Defrag of a document that is replaced between steps. */
    text big = {0, 0, 0};
    char member[64];
    textAddStr(&big, "{");
    for (int i = 0; i < 300; i++) {
        snprintf(member, sizeof(member), "%s\"df-%d\":{\"in\":[%d,\"0123456789\"],\"more\":[[1],[2]]}", i ? "," : "", i, i);
        textAddStr(&big, member);
    }
    textAddStr(&big, "}");
    d = mustParse(big.p);
    defrag.active = 0;
    defragStart(&d, 'a', 0);
    defragStart(&d, 'r', 3);
    defragStart(&d, 's', 2);
    CHECK(defrag.active);
    int steps = 0;
    while (defrag.active && steps < 20) {
        defragStep(&d);
        steps++;
    }
    CHECK(defrag.active && defrag.cursor != 0);
    jsonSetInt(&d, NULL, 0, 5); /* Not a spine document any more: the next step ends the run. */
    defragStep(&d);
    CHECK(!defrag.active && defrag.cursor == 0);
    expectText(d, "5");
    jsonDocFree(d);
    free(big.p);

    /* A seed changes the IDs keys hash to, not what documents say. */
    CHECK(jsonEngineShutdown() == 0);
    static const uint8_t seed[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    jsonEngineSetHashSeed(seed);
    d = mustParse("{\"seed-a\":1,\"seed-b\":{\"seed-a\":[1,2,3]}}");
    expectText(d, "{\"seed-a\":1,\"seed-b\":{\"seed-a\":[1,2,3]}}");
    checkDoc(d);
    jsonDocFree(d);
}

/* Decimals and string arrays: what they cost, which only memory shows. */
/* Number equality as JSON.ARRINDEX uses it: the first element of an array
 * against a scalar that was parsed on its own. */
static void unitNumberEquals(void) {
    static const struct {
        const char *array, *scalar;
        int equal;
    } cases[] = {
        /* An integer beyond int64 is compared as a double; within, exactly. */
        {"[9223372036854775808,\"x\"]", "9223372036854775807", 1},
        {"[9223372036854775807,\"x\"]", "9223372036854775808", 1},
        {"[9223372036854775807,\"x\"]", "9223372036854775806", 0},
        {"[9223372036854775806,9223372036854775807]", "9223372036854775807", 0},
        {"[1234567890123456789012345,\"x\"]", "1234567890123456789012346", 1},
        {"[18446744073709551615,\"x\"]", "18446744073709551616", 1},
        {"[18446744073709551615,\"x\"]", "-1", 0},
        {"[-1,\"x\"]", "18446744073709551615", 0},
        {"[-9223372036854775808,\"x\"]", "9223372036854775808", 0},
        {"[-9223372036854775808,\"x\"]", "-9223372036854775809", 1},
        /* 2^53: two integers differ, an integer and a double do not. */
        {"[9007199254740993,\"x\"]", "9007199254740992", 0},
        {"[9007199254740993,\"x\"]", "9007199254740992.0", 1},
        {"[9007199254740993,9007199254740992]", "9.007199254740993e15", 1},
        /* Decimals, doubles and number text against integers and each other. */
        {"[1.50,2.25]", "1.5", 1},
        {"[1.50,2.25]", "150e-2", 1},
        {"[100.00,2.25]", "100", 1},
        {"[100,200]", "1e2", 1},
        {"[0.1,0.2]", "0.10000000000000001", 1},
        {"[0.1,0.25]", "0.1000000000000001", 0},
        {"[0,1]", "-0.0", 1},
        {"[1e0,\"x\"]", "1", 1},
        {"[1,\"x\"]", "\"1\"", 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        jsonRef array, element, scalar;
        const jsonDoc a = mustParse(cases[i].array), b = mustParse(cases[i].scalar);
        jsonDocRoot(a, &array);
        jsonDocRoot(b, &scalar);
        CHECK(jsonArrayGet(&array, 0, &element));
        if (jsonRefEquals(&element, &scalar) != cases[i].equal || jsonRefEquals(&scalar, &element) != cases[i].equal) {
            fprintf(stderr, "%s [0] against %s: expected %d\n", cases[i].array, cases[i].scalar, cases[i].equal);
            exit(1);
        }
        jsonDocFree(a);
        jsonDocFree(b);
    }
}

static void unitDecimals(void) {
    static text t, big;
    jsonRef root, ref;
    jsonDoc d;
    char num[JSON_NUM_BUF];
    uint32_t len;

    /* A typed array of decimals with one scale: tag, count, scale, then the
     * digits in the narrowest width, after the node's 3-byte header. */
    static const struct {
        const char *text;
        size_t bytes;
    } packed[] = {
        {"[19.90,0.05,-1.28]", 3 + 3 + 3 * 2}, /* 1990 needs 16 bits. */
        {"[0.05,1.27,-1.28]", 3 + 3 + 3},
        {"[0.05,1.27,1.28]", 3 + 3 + 3 * 2},
        {"[327.67,-327.68]", 3 + 3 + 2 * 2},
        {"[327.68,0.01]", 3 + 3 + 2 * 4},
        {"[21474836.47,0.01]", 3 + 3 + 2 * 4},
        {"[21474836.48,0.01]", 3 + 3 + 2 * 8},
        {"[9999999999999.99,0.01]", 3 + 3 + 2 * 8},
        {"[0.000000000000001]", 3 + 3 + 1},
        {"[1.5,2.25]", 3 + 3 + 2 + 3},                 /* Two scales: a plain array of two packed decimals. */
        {"[1.5,2]", 3 + 3 + 2 + 1},                    /* A decimal and an integer. */
        {"[99999999999999.99,0.01]", 3 + 3 + 2 * 8},   /* 16 digits. */
        {"[12345678901234567.5,-0.5]", 3 + 3 + 2 * 8}, /* 18 digits. */
        {"[0.1234567890123456,0.5000000000000000]", 3 + 3 + 2 * 8},
        {"[0.0005396780371665955]", 3 + 3 + 8},                   /* Scale 19, the scale in the array's header. */
        {"[0.30000000000000004,0.5]", 3 + 3 + 10 + 2},            /* Two scales: the long form for a scale above 15, then a tag and a byte. */
        {"[123456789012345678.5]", 3 + 3 + 2 + 20},               /* 19 digits and not canonical: text. */
        {"[0.00000000000000000000000000000012]", 3 + 3 + 2 + 34}, /* Scale 32, beyond the limit, and canonical only as 1.2e-31: text. */
        {"[0.0000000000000000000000000000012]", 3 + 3 + 1},       /* Scale 31. */
        {"[-0.0,0.5]", 3 + 3 + 9 + 2},                            /* A negative zero is a double. */
        {"[0.0,0.5]", 3 + 3 + 2},
    };
    for (size_t i = 0; i < sizeof(packed) / sizeof(packed[0]); i++) {
        d = mustParse(packed[i].text);
        if (jsonDocMemUsage(d) != packed[i].bytes) {
            fprintf(stderr, "%s takes %zu bytes, expected %zu\n", packed[i].text, jsonDocMemUsage(d), packed[i].bytes);
            exit(1);
        }
        expectText(d, packed[i].text);
        checkDoc(d);
        jsonDocFree(d);
    }

    /* The value of a decimal is its digits divided by the power of ten. */
    d = mustParse("[19.90,0.1,123456789012.345,-0.007]");
    jsonDocRoot(d, &root);
    static const double values[] = {19.90, 0.1, 123456789012.345, -0.007};
    for (uint32_t i = 0; i < 4; i++) {
        int64_t unused;
        CHECK(jsonArrayGet(&root, i, &ref) && jsonRefType(&ref) == JSON_RAWNUM);
        CHECK(jsonRefDouble(&ref) == values[i] && !jsonRefInt(&ref, &unused));
    }
    CHECK(jsonArrayGet(&root, 0, &ref));
    /* The call first: an argument evaluated before it would read len unset. The
     * text is not NUL-terminated, so it is compared by its length. */
    const char *num_text = jsonRefNumText(&ref, num, &len);
    CHECK(len == 5 && memcmp(num_text, "19.90", 5) == 0);
    jsonDocFree(d);

    /* A decimal beyond 2^53 or 10^22: dividing its digits by the power of ten
     * would round twice. These three are one unit off that way. */
    static const struct {
        const char *text;
        double value;
    } wide[] = {
        {"43591.010316006538", 43591.01031600654},
        {"1.4262204137704003", 1.4262204137704002},
        {"0.9007199254740993", 0.9007199254740993},
        {"-0.000553789539555825165", -0.0005537895395558252},
        {"0.0000000000000000000000000000012", 1.2e-30},
        {"0.0000000001234567890123456789012", 1.234567890123456789012e-10},
        {"999999999999999999.9", 1e18},
        {"-9007199254740993.0", -9007199254740992.0},
    };
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++) {
        snprintf(num, sizeof(num), "[%s,\"x\"]", wide[i].text); /* Packed scalar. */
        d = mustParse(num);
        jsonDocRoot(d, &root);
        CHECK(jsonArrayGet(&root, 0, &ref) && jsonRefType(&ref) == JSON_RAWNUM && jsonRefDouble(&ref) == wide[i].value);
        jsonDocFree(d);
        d = mustParse(wide[i].text); /* A root: in the word, or boxed. */
        jsonDocRoot(d, &root);
        CHECK(jsonRefType(&root) == JSON_RAWNUM && jsonRefDouble(&root) == wide[i].value);
        /* The call first: as an argument of the comparison, len could be read
         * before it is set (gcc on x86-64 does), and the previous length used. */
        const char *got = jsonRefNumText(&root, num, &len);
        CHECK(len == strlen(wide[i].text) && memcmp(got, wide[i].text, len) == 0);
        expectText(d, wide[i].text);
        jsonDocFree(d);
    }
    /* What a word holds: a scale up to 22 and digits up to 2^61 / 22, which
     * covers 17 digits. Anything more is a boxed number of 16 bytes. */
    static const struct {
        const char *text;
        size_t bytes;
    } words[] = {
        {"0.99999999999999999", 0},
        {"-9999999999999999.9", 0},
        {"0.104811045873349724", 0},
        {"-0.104811045873349724", 0},
        {"0.104811045873349725", 16},
        {"-0.104811045873349725", 16},
        {"0.999999999999999999", 16},
        {"0.0000099999999999999999", 0},   /* Scale 22. */
        {"0.00000099999999999999999", 16}, /* Scale 23. */
        {"0.0000000000000000000001", 0},
        {"0.00000000000000000000001", 16},
        {"-0.0000000000000000000000000000001", 16},
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        d = mustParse(words[i].text);
        if (jsonDocMemUsage(d) != words[i].bytes) {
            fprintf(stderr, "%s takes %zu bytes, expected %zu\n", words[i].text, jsonDocMemUsage(d), words[i].bytes);
            exit(1);
        }
        jsonDocRoot(d, &root);
        CHECK(jsonRefType(&root) == JSON_RAWNUM && jsonRefDouble(&root) == strtod(words[i].text, NULL));
        expectText(d, words[i].text);
        jsonDocFree(d);
    }

    /* 1536 doubles written at full precision, with scales that differ: eight
     * bytes each, in the words of a plain array. One of 18 digits is boxed. */
    t.len = 0;
    textAddStr(&t, "[");
    for (int i = 0; i < 1536; i++) {
        /* Sixteen digits behind 0 to 3 zeros, some negative. */
        snprintf(num, sizeof(num), "%s%s0.%.*s%lld", i ? "," : "", i % 7 == 3 ? "-" : "", i % 4, "000", 1234567890123456ll + i * 7919ll);
        textAddStr(&t, num);
    }
    textAddStr(&t, "]");
    d = mustParse(t.p);
    CHECK(jsonDocMemUsage(d) == 32 + 1536 * 8 && jsonDocFreeEffort(d) == 1);
    expectText(d, t.p);
    jsonDocRoot(d, &root);
    CHECK(jsonSerializeSizeHint(&root) >= t.len);
    CHECK(jsonArrayGet(&root, 1535, &ref) && jsonRefType(&ref) == JSON_RAWNUM);
    CHECK(jsonRefDouble(&ref) == strtod(strrchr(t.p, ',') + 1, NULL));
    checkDoc(d);
    jsonDocFree(d);
    t.p[t.len - 1] = ',';
    textAddStr(&t, "0.300000000000000004]");
    d = mustParse(t.p);
    CHECK(jsonDocMemUsage(d) == 32 + 1537 * 8 + 16 && jsonDocFreeEffort(d) == 2);
    expectText(d, t.p);
    checkDoc(d);
    jsonDocFree(d);

    /* 1536 values written with six decimals: four bytes each in one node. */
    t.len = 0;
    textAddStr(&t, "[");
    for (int i = 0; i < 1536; i++) {
        snprintf(num, sizeof(num), "%s%.6f", i ? "," : "", (i - 700) * 0.000731);
        textAddStr(&t, num);
    }
    textAddStr(&t, "]");
    d = mustParse(t.p);
    CHECK(jsonDocMemUsage(d) == 16 + 1536 * 4 && jsonDocFreeEffort(d) == 1);
    expectText(d, t.p);
    jsonDocRoot(d, &root);
    CHECK(jsonSerializeSizeHint(&root) >= t.len);
    checkDoc(d);
    jsonDocFree(d);

    /* Strings of at most 15 bytes in an array too big to pack: 16 bytes
     * each in one node, if at least one would not fit a word. */
    static const struct {
        const char *element;
        size_t per_element; /* 16 in a string array, 8 in a plain array of words. */
    } strings[] = {{"\"0123456789abcde\"", 16}, {"\"01234567\"", 16}, {"\"0123456\"", 8}, {"\"\"", 8}, {"\"q\\\"\\n\\u00e9\\\\345\"", 16}};
    for (size_t k = 0; k < sizeof(strings) / sizeof(strings[0]); k++) {
        big.len = 0;
        textAddStr(&big, "[");
        for (int i = 0; i < 200; i++) {
            if (i) textAddStr(&big, ",");
            textAddStr(&big, strings[k].element);
        }
        textAddStr(&big, "]");
        d = mustParse(big.p);
        CHECK(jsonDocMemUsage(d) == (strings[k].per_element == 16 ? 16 : 32) + 200 * strings[k].per_element);
        jsonDocRoot(d, &root);
        CHECK(jsonArrayGet(&root, 199, &ref) && jsonRefType(&ref) == JSON_STRING);
        const char *bytes = jsonRefString(&ref, &len);
        CHECK(bytes != NULL && len <= 15 && jsonRefMemUsage(&ref) == (strings[k].per_element == 16 ? 16 : 8));
        CHECK(jsonSerializeSizeHint(&root) >= big.len);
        checkDoc(d);
        jsonDocFree(d);
    }
    /* 2000 of null, false and true in an array too big to pack: a byte each
     * in one node. Anything else among them makes it a plain array. */
    big.len = 0;
    textAddStr(&big, "[");
    for (int i = 0; i < 2000; i++) textAddStr(&big, i % 3 == 0 ? "true," : i % 3 == 1 ? "false,"
                                                                                      : "null,");
    big.p[big.len - 1] = ']';
    d = mustParse(big.p);
    CHECK(jsonDocMemUsage(d) == 16 + 2000 && jsonDocFreeEffort(d) == 1);
    expectText(d, big.p);
    jsonDocRoot(d, &root);
    CHECK(jsonRefCount(&root) == 2000 && jsonSerializeSizeHint(&root) >= big.len);
    for (uint32_t i = 0; i < 6; i++) {
        CHECK(jsonArrayGet(&root, 1994 + i, &ref) && jsonRefMemUsage(&ref) == 1);
        CHECK(jsonRefType(&ref) == ((1994 + i) % 3 == 0 ? JSON_TRUE : (1994 + i) % 3 == 1 ? JSON_FALSE
                                                                                          : JSON_NULL));
    }
    checkDoc(d);
    jsonDocFree(d);
    big.p[big.len - 1] = ',';
    textAddStr(&big, "0]");
    d = mustParse(big.p);
    CHECK(jsonDocMemUsage(d) == 32 + 2001 * 8);
    expectText(d, big.p);
    jsonDocFree(d);

    /* One string of 16 bytes among them, and it is a plain array again. */
    big.len = 0;
    textAddStr(&big, "[\"0123456789abcdef\"");
    for (int i = 0; i < 200; i++) textAddStr(&big, ",\"0123456789\"");
    textAddStr(&big, "]");
    d = mustParse(big.p);
    CHECK(jsonDocFreeEffort(d) == 202);
    jsonDocFree(d);
}

/* Key IDs of every varint width a key can take in a packed object: 1 to 4
 * bytes. A document holding 2^21 + 64 distinct names keeps the IDs taken,
 * and small packed objects then use names that fall on each boundary. (A
 * 5-byte varint needs 2^28 live names.) */
static void unitKeyIdWidths(void) {
    const uint32_t n = ((uint32_t)1 << 21) + 64;
    enum {
        NEDGES = 8
    };
    static const uint32_t edges[NEDGES] = {1, 127, 128, 16383, 16384, 2097151, 2097152, 2097152 + 63};
    const size_t nedges = NEDGES;
    char name[NEDGES][24], added[24], member[256];
    text t = {0, 0, 0}, want = {0, 0, 0};
    jsonRef root, val;
    jsonStep step;
    jsonEngineSetCaps(4096, 128);
    textAddStr(&t, "{");
    for (uint32_t i = 0; i < n; i++) {
        snprintf(member, sizeof(member), "%s\"w%" PRIu32 "\":%" PRIu32, i ? "," : "", i, i & 31);
        textAddStr(&t, member);
    }
    textAddStr(&t, "}");
    const jsonDoc big = mustParse(t.p);
    /* The table hands out the smallest free ID, so the names were numbered in
     * order from that of w0. */
    const uint32_t base = jsonKeyFind("w0", 2);
    jsonKeyRelease(base);
    CHECK(base >= 1 && base + n - 1 >= edges[nedges - 1]);
    for (size_t e = 0; e < nedges; e++) {
        snprintf(name[e], sizeof(name[e]), "w%" PRIu32, edges[e] - base);
        const uint32_t id = jsonKeyFind(name[e], strlen(name[e]));
        CHECK(id == edges[e]);
        jsonKeyRelease(id);
    }
    snprintf(added, sizeof(added), "w%" PRIu32, edges[NEDGES - 1] - 1 - base); /* A 4-byte one the object lacks. */
    /* Members of every width side by side, nested, and in an array of objects. */
    want.len = 0;
    textAddStr(&want, "{");
    for (size_t e = 0; e < nedges; e++) {
        snprintf(member, sizeof(member), "%s\"%s\":[%zu,{\"%s\":\"v%zu\"}]", e ? "," : "", name[e], e, name[nedges - 1 - e], e);
        textAddStr(&want, member);
    }
    textAddStr(&want, ",\"fresh\":true}");
    for (int spine = 0; spine < 2; spine++) {
        jsonEngineSetCaps(spine ? 0 : 4096, spine ? 0 : 128);
        jsonDoc d = mustParse(want.p);
        CHECK(strcmp(jsonDocEncoding(d), spine ? "jtree" : "jpack") == 0);
        expectText(d, want.p);
        checkDoc(d);
        jsonDocRoot(d, &root);
        for (size_t e = 0; e < nedges; e++) {
            jsonRef inner, str;
            CHECK(jsonObjectGet(&root, name[e], strlen(name[e]), &val, NULL) && jsonRefCount(&val) == 2);
            CHECK(jsonArrayGet(&val, 1, &inner) && jsonObjectGet(&inner, name[nedges - 1 - e], strlen(name[nedges - 1 - e]), &str, NULL));
            uint32_t len;
            const char *s = jsonRefString(&str, &len);
            snprintf(member, sizeof(member), "v%zu", e);
            CHECK(s && len == strlen(member) && memcmp(s, member, len) == 0);
        }
        /* Changes that splice varints of each width in and out. */
        jsonVal *one = jsonValInt(1);
        jsonObjectAdd(&d, NULL, 0, added, strlen(added), one);
        jsonDocRoot(d, &root);
        CHECK(jsonObjectGet(&root, name[nedges - 1], strlen(name[nedges - 1]), &val, &step));
        jsonDelete(&d, &step, 1);
        jsonDocRoot(d, &root);
        CHECK(jsonObjectGet(&root, name[3], strlen(name[3]), &val, &step));
        jsonDelete(&d, &step, 1);
        jsonDocRoot(d, &root);
        CHECK(jsonObjectGet(&root, added, strlen(added), &val, NULL) && jsonRefType(&val) == JSON_INT);
        CHECK(!jsonObjectGet(&root, name[3], strlen(name[3]), &val, NULL));
        CHECK(jsonRefCount(&root) == nedges + 1 + 1 - 2);
        CHECK(jsonRefMemUsage(&root) == jsonDocMemUsage(d));
        jsonValFree(one);
        jsonDocFree(d);
    }
    jsonDocFree(big);
    jsonEngineSetCaps(4096, 128);
    free(t.p);
    free(want.p);
}

static size_t wide_moves;

static void *wideMove(void *p, void *ctx) {
    (void)ctx;
    const size_t size = (size_t)blockOf(p)->size;
    void *np = testAlloc(size);
    memcpy(np, p, size);
    testFree(p);
    wide_moves++;
    return np;
}

static int stopAlways(void *ctx) {
    (void)ctx;
    return 1;
}

/* Defrag of a root with more children than a 32-bit cursor holds in its
 * two-index form: spine children below and above root index 32767, and one
 * with more than 65535 entries. Stopped after every step, it still offers
 * each allocation exactly once and ends. */
static void unitDefragWideRoot(void) {
    const uint32_t n = 40000;
    char member[64];
    text t = {0, 0, 0}, out = {0, 0, 0};
    jsonRef root;
    jsonEngineSetCaps(16, 1);
    textAddStr(&t, "[");
    for (uint32_t i = 0; i < n; i++) {
        if (i) textAddStr(&t, ",");
        if (i == 100 || i == 35000) {
            textAddStr(&t, "[");
            for (uint32_t j = 0; j < 70000; j++) {
                snprintf(member, sizeof(member), "%s\"e%06" PRIu32 "-0123456789\"", j ? "," : "", j);
                textAddStr(&t, member);
            }
            textAddStr(&t, "]");
        } else if (i % 97 == 0 || (i >= 32760 && i <= 32775)) {
            snprintf(member, sizeof(member), "[%" PRIu32 ",\"c%" PRIu32 "-0123456789\",[1,\"x\"]]", i, i);
            textAddStr(&t, member);
        } else if (i % 5 == 0) {
            snprintf(member, sizeof(member), "\"s%" PRIu32 "-0123456789\"", i);
            textAddStr(&t, member);
        } else {
            snprintf(member, sizeof(member), "%" PRIu32, i);
            textAddStr(&t, member);
        }
    }
    textAddStr(&t, "]");
    jsonDoc d = mustParse(t.p);
    const size_t effort = jsonDocFreeEffort(d);
    unsigned long cursor = 0;
    size_t calls = 0;
    wide_moves = 0;
    while (jsonDocDefrag(&d, wideMove, stopAlways, NULL, &cursor)) {
        CHECK(cursor != 0);
        CHECK(++calls <= effort);
    }
    CHECK(cursor == 0 && wide_moves == effort);
    jsonDocRoot(d, &root);
    serialize(&root, NULL, &out);
    CHECK(out.len == t.len && memcmp(out.p, t.p, t.len) == 0);
    checkDoc(d);
    jsonDocFree(d);
    jsonEngineSetCaps(4096, 128);
    free(t.p);
    free(out.p);
}

/* A pooled tape whose arena was given back after a large value, then used
 * to copy a document that holds empty strings in spine words: the strings
 * must not point at a null arena (glibc's UBSan stops at memcpy(.., NULL, 0)
 * there, and JSON_ENGINE_DEBUG asserts it). */
static void unitEmptyStringAfterTrim(void) {
    text t = {0, 0, 0};
    jsonEngineSetCaps(0, 0);
    const jsonDoc small = mustParse("[\"\",\"\",{\"e\":\"\"},\"x\"]");
    textAddStr(&t, "[\"");
    for (int i = 0; i < 100000; i++) textAdd(&t, "s", 1);
    textAddStr(&t, "\"]");
    jsonDocFree(mustParse(t.p)); /* Its tape returns to the pool with its arena trimmed. */
    const jsonDoc copy = jsonDocCopy(small);
    expectText(copy, "[\"\",\"\",{\"e\":\"\"},\"x\"]");
    jsonDocFree(copy);
    jsonDocFree(small);
    jsonEngineSetCaps(4096, 128);
    free(t.p);
}

static void runUnit(void) {
    unitStats();
    unitConstructors();
    unitBuf();
    unitErrors();
    unitCaps();
    unitKeyTable();
    unitThreadFreeSpine();
    unitFormat();
    unitMemUsage();
    unitHuge();
    unitPhase2();
    unitDecimals();
    unitNumberEquals();
    unitKeyIdWidths();
    unitDefragWideRoot();
    unitEmptyStringAfterTrim();
    puts("unit: ok");
}

static void runFile(const char *op, const char *path) {
    void (*fn)(const char *, size_t, text *) = NULL;
    if (!strcmp(op, "roundtrip")) fn = opRoundtrip;
    if (!strcmp(op, "dump")) fn = opDump;
    if (!strcmp(op, "equals")) fn = opEquals;
    if (!strcmp(op, "mutate")) fn = opMutate;
    if (!fn) {
        fprintf(stderr, "unknown op %s\n", op);
        exit(2);
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    text out = {0, 0, 0};
    while ((n = getline(&line, &cap, f)) > 0) {
        g_line++;
        if (line[n - 1] == '\n') n--;
        if (opt.hex_input) {
            for (ssize_t i = 0; i < n / 2; i++) line[i] = (char)(hexValue(line[2 * i]) << 4 | hexValue(line[2 * i + 1]));
            n /= 2;
        }
        fn(line, (size_t)n, &out);
        fwrite(out.p, 1, out.len, stdout);
        fputc('\n', stdout);
    }
    if (fn == opMutate) printWriteCounts();
    free(line);
    free(out.p);
    fclose(f);
}

int main(int argc, char **argv) {
    int i = 1;
    opt.cap_bytes = opt.cap2_bytes = 4096;
    opt.cap_entries = opt.cap2_entries = 128;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char flag = argv[i][1];
        if (flag == 't') {
            opt.trusted = 1;
        } else if (flag == 'x') {
            opt.hex_input = 1;
        } else if (i + 1 < argc) {
            const char *arg = argv[++i];
            switch (flag) {
            case 'b': opt.cap_bytes = opt.cap2_bytes = (uint32_t)atoi(arg); break;
            case 'e': opt.cap_entries = opt.cap2_entries = (uint32_t)atoi(arg); break;
            case 'B': opt.cap2_bytes = (uint32_t)atoi(arg); break;
            case 'E': opt.cap2_entries = (uint32_t)atoi(arg); break;
            case 'd': opt.max_depth = (uint32_t)atoi(arg); break;
            case 'i':
                opt.fmt.indent = arg;
                opt.fmt.indent_len = strlen(arg);
                opt.has_fmt = 1;
                break;
            case 'n':
                opt.fmt.newline = arg;
                opt.fmt.newline_len = strlen(arg);
                opt.has_fmt = 1;
                break;
            case 's':
                opt.fmt.space = arg;
                opt.fmt.space_len = strlen(arg);
                opt.has_fmt = 1;
                break;
            default: fprintf(stderr, "unknown option -%c\n", flag); return 2;
            }
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: engine_test [options] <roundtrip|dump|equals|unit> [file]\n");
        return 2;
    }
    jsonEngineInit(&test_allocator);
    jsonEngineSetCaps(opt.cap_bytes, opt.cap_entries);
    if (!strcmp(argv[i], "unit")) {
        runUnit();
    } else if (i + 1 < argc) {
        runFile(argv[i], argv[i + 1]);
    } else {
        fprintf(stderr, "%s needs a file\n", argv[i]);
        return 2;
    }
    const size_t leaked_keys = jsonEngineShutdown();
    if (leaked_keys || live_bytes || live_blocks) {
        fprintf(stderr, "LEAK: %zu keys still referenced, %zu bytes in %zu blocks still allocated\n", leaked_keys,
                (size_t)live_bytes, (size_t)live_blocks);
        return 1;
    }
    return 0;
}
