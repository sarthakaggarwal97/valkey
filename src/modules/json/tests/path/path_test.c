/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* path_test.c -- tests of the path compiler and evaluator.
 *
 *   path_test compile CASES VERDICTS
 *                                Compile-only tests. CASES holds lines of
 *                                "path<TAB>program text". VERDICTS holds lines
 *                                of "path<TAB>error", the error the reference
 *                                server gave for the path on every document,
 *                                or "-" if that depended on the document.
 *                                Needs no engine.
 *   path_test dump               Reads paths from stdin, one per line, and
 *                                prints "path<TAB>program text" for each.
 *   path_test golden < GOLDEN    Evaluates every (document, path) pair recorded
 *                                from the reference server by gen_corpus.py
 *                                and compares reads, sets, deletes and array
 *                                appends with what the module did.
 *   path_test unit               Checks of the API that the golden data cannot
 *                                express.
 *   path_test bench              Timing loops: evaluation of a compiled path.
 *   path_test use                Timing loops: compile, evaluate and free, as
 *                                a command does, for a repeated path and for
 *                                many different ones.
 *
 * Built with -DJSON_PATH_NO_ENGINE the binary carries stubs instead of the
 * engine and only "compile" and "dump" work. */

#include "../../json_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

size_t jsonPathDebugDump(const jsonPath *p, char *buf, size_t cap);
#ifndef JSON_PATH_OLD
jsonPath *jsonPathDebugCompileFull(const char *s, size_t n);
#endif

#ifdef JSON_PATH_OLD
/* For timing a json_path.c from before the cache existed. */
void jsonPathCacheClear(void) {
}
#endif

/* ----------------------------------------------------------------------------
 * Allocator: counts, so tests can assert "no allocation" and "no leak"
 * ------------------------------------------------------------------------- */

static long long live_allocs, total_allocs, total_bytes, live_bytes, peak_bytes;

/* Each block carries its size, so that the bytes in use and their peak are known. */
#define T_HEADER 16

static void tCount(long long delta) {
    live_bytes += delta;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
}
static void *tAlloc(size_t n) {
    char *raw = malloc(n + T_HEADER);
    memcpy(raw, &n, sizeof(n));
    live_allocs++;
    total_allocs++;
    total_bytes += (long long)n;
    tCount((long long)n);
    return raw + T_HEADER;
}
static void *tRealloc(void *p, size_t n) {
    if (!p) return tAlloc(n);
    size_t old;
    char *raw = (char *)p - T_HEADER;
    memcpy(&old, raw, sizeof(old));
    raw = realloc(raw, n + T_HEADER);
    memcpy(raw, &n, sizeof(n));
    total_bytes += (long long)n;
    tCount((long long)n - (long long)old);
    return raw + T_HEADER;
}
static void tFree(void *p) {
    if (!p) return;
    size_t old;
    memcpy(&old, (char *)p - T_HEADER, sizeof(old));
    live_allocs--;
    tCount(-(long long)old);
    free((char *)p - T_HEADER);
}
static size_t tUsable(void *p) {
    size_t n;
    memcpy(&n, (char *)p - T_HEADER, sizeof(n));
    return n;
}
static const jsonAllocator test_allocator = {tAlloc, tRealloc, tFree, tUsable};

/* The engine does not export its allocator yet. When it does, its definition
 * replaces this one. */
__attribute__((weak)) const jsonAllocator *jsonEngineAllocator(void) {
    return &test_allocator;
}

#ifdef JSON_PATH_NO_ENGINE
/* Just enough of the engine for json_path.c to link. Nothing is evaluated. */
static void noEngine(void) {
    fprintf(stderr, "this build has no engine\n");
    abort();
}
uint32_t jsonKeyFind(const char *name, size_t len) {
    (void)name;
    (void)len;
    return 0;
}
void jsonKeyRelease(uint32_t keyid) {
    (void)keyid;
}
void jsonDocRoot(jsonDoc d, jsonRef *out) {
    (void)d;
    (void)out;
    noEngine();
}
jsonType jsonRefType(const jsonRef *r) {
    (void)r;
    noEngine();
    return JSON_NULL;
}
uint32_t jsonRefCount(const jsonRef *r) {
    (void)r;
    noEngine();
    return 0;
}
uint64_t jsonRefFields(const jsonRef *r) {
    (void)r;
    noEngine();
    return 0;
}
int jsonRefInt(const jsonRef *r, int64_t *out) {
    (void)r;
    (void)out;
    noEngine();
    return 0;
}
double jsonRefDouble(const jsonRef *r) {
    (void)r;
    noEngine();
    return 0;
}
const char *jsonRefString(const jsonRef *r, uint32_t *len) {
    (void)r;
    (void)len;
    noEngine();
    return NULL;
}
const char *jsonRefNumText(const jsonRef *r, char *buf, uint32_t *len) {
    (void)r;
    (void)buf;
    (void)len;
    noEngine();
    return NULL;
}
int jsonArrayGet(const jsonRef *arr, uint32_t index, jsonRef *out) {
    (void)arr;
    (void)index;
    (void)out;
    noEngine();
    return 0;
}
int jsonObjectGetById(const jsonRef *obj, uint32_t keyid, jsonRef *out, jsonStep *step) {
    (void)obj;
    (void)keyid;
    (void)out;
    (void)step;
    noEngine();
    return 0;
}
void jsonIterInit(jsonIter *it, const jsonRef *container) {
    (void)it;
    (void)container;
    noEngine();
}
int jsonIterNext(jsonIter *it, jsonRef *val, jsonStep *step, const char **name, uint32_t *name_len) {
    (void)it;
    (void)val;
    (void)step;
    (void)name;
    (void)name_len;
    noEngine();
    return 0;
}
#endif

static int failures;
static long long direct_checked;

/* Plain paths are compiled by a direct compiler. Its program must be the one
 * the full compiler makes. */
static void checkDirect(const char *path, size_t n) {
#ifndef JSON_PATH_OLD
    static char a[1 << 14], b[1 << 14];
    jsonPathErr err;
    jsonPath *p = jsonPathCompile(path, n, &err), *q = jsonPathDebugCompileFull(path, n);
    if (!p || !q) {
        if (p || q) failures++;
    } else {
        size_t la = jsonPathDebugDump(p, a, sizeof(a)), lb = jsonPathDebugDump(q, b, sizeof(b));
        if (la != lb || memcmp(a, b, la) != 0 || jsonPathIsSingular(p) != jsonPathIsSingular(q) ||
            jsonPathIsV2(p) != jsonPathIsV2(q) || jsonPathIsRoot(p) != jsonPathIsRoot(q)) {
            failures++;
            if (failures < 20) fprintf(stderr, "FAIL direct compile of %.*s\n  direct %s\n  full   %s\n", (int)n, path, a, b);
        }
    }
    jsonPathFree(p);
    jsonPathFree(q);
    direct_checked++;
#else
    (void)path;
    (void)n;
#endif
}

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

/* ----------------------------------------------------------------------------
 * Compile-only tests
 * ------------------------------------------------------------------------- */

static char *readAll(FILE *f, size_t *len) {
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    for (;;) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    *len = n;
    return buf;
}

static int cmdDump(void) {
    size_t len;
    char *in = readAll(stdin, &len), *line = in, *end = in + len;
    static char dump[1 << 16];
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t n = nl ? (size_t)(nl - line) : (size_t)(end - line);
        jsonPathErr err;
        jsonPath *p = jsonPathCompile(line, n, &err);
        if (p) {
            jsonPathDebugDump(p, dump, sizeof(dump));
            printf("%.*s\t%s\n", (int)n, line, dump);
            jsonPathFree(p);
        } else {
            printf("%.*s\tNULL(%d)\n", (int)n, line, err);
        }
        line += n + 1;
    }
    free(in);
    return 0;
}

static void checkProps(const char *path, int v2, int root, int singular) {
    jsonPathErr err;
    jsonPath *p = jsonPathCompile(path, strlen(path), &err);
    CHECK(p != NULL, "%s does not compile", path);
    if (!p) return;
    CHECK(jsonPathIsV2(p) == v2, "%s: v2 is %d", path, jsonPathIsV2(p));
    CHECK(jsonPathIsRoot(p) == root, "%s: root is %d", path, jsonPathIsRoot(p));
    CHECK(jsonPathIsSingular(p) == singular, "%s: singular is %d", path, jsonPathIsSingular(p));
    jsonPathFree(p);
}

/* The error a compiled path reports whatever the document is, read off the
 * program text: an error as the first step, or as the first thing a leading
 * filter can reach. 0 if the outcome depends on the document. */
static int staticError(const char *path, const char *dump) {
    const char *d = dump;
    if (*d == '+') d = strchr(d, ' ') + 1;
    if (d[0] == '$' && d[1] == ' ') d += 2;
    if (!strncmp(d, "ERR(", 4) && !strstr(d, ",arr")) return atoi(d + 4);
    if (!strncmp(d, "..!", 3) && path[0] != '$') return JSON_PATH_ERR_DOT_SEQUENCE;
    if (!strncmp(d, "f{", 2)) {
        for (d += 2; *d && *d != '}';) {
            if (!strncmp(d, "err(", 4)) return atoi(d + 4);
            if (strncmp(d, "cmp(", 4) && strncmp(d, "and", 3) && strncmp(d, "or", 2)) return 0;
            /* A comparison can hold any text in a quoted literal, so give up on one. */
            if (!strncmp(d, "cmp(", 4) && strchr(d, '\'') && strchr(d, '\'') < strchr(d, ')')) return 0;
            d = strchr(d, ' ');
            if (!d) return 0;
            d++;
        }
    }
    return 0;
}

static int cmdCompile(const char *cases, const char *verdicts) {
    FILE *f = fopen(cases, "rb");
    if (!f) {
        perror(cases);
        return 2;
    }
    size_t len, count = 0, uniform = 0, predicted = 0;
    char *in = readAll(f, &len), *line = in, *end = in + len;
    static char dump[1 << 16];
    fclose(f);
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t n = nl ? (size_t)(nl - line) : (size_t)(end - line);
        char *tab = memchr(line, '\t', n);
        if (tab) {
            jsonPathErr err = JSON_PATH_OK;
            size_t plen = (size_t)(tab - line), elen = n - plen - 1;
            jsonPath *p = jsonPathCompile(line, plen, &err);
            if (p) {
                jsonPathDebugDump(p, dump, sizeof(dump));
                jsonPathFree(p);
            } else {
                snprintf(dump, sizeof(dump), "NULL(%d)", err);
            }
            CHECK(strlen(dump) == elen && memcmp(dump, tab + 1, elen) == 0, "compile %.*s\n  expected %.*s\n  got      %s",
                  (int)plen, line, (int)elen, tab + 1, dump);
            checkDirect(line, plen);
            count++;
        }
        line += n + 1;
    }
    free(in);

    /* Against the reference server: a path the compiler rejects whatever the
     * document is must be one the module rejected, with the same error, on
     * every document it was tried on. */
    f = fopen(verdicts, "rb");
    if (!f) {
        perror(verdicts);
        return 2;
    }
    in = readAll(f, &len);
    fclose(f);
    for (line = in, end = in + len; line < end;) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t n = nl ? (size_t)(nl - line) : (size_t)(end - line);
        char *tab = memchr(line, '\t', n);
        if (tab) {
            jsonPathErr err = JSON_PATH_OK;
            size_t plen = (size_t)(tab - line), vlen = n - plen - 1;
            int is_uniform = !(vlen == 1 && tab[1] == '-');
            jsonPath *p = jsonPathCompile(line, plen, &err);
            CHECK(p != NULL, "%.*s does not compile", (int)plen, line);
            if (p) {
                char first = line[plen];
                jsonPathDebugDump(p, dump, sizeof(dump));
                line[plen] = '\0';
                int e = staticError(line, dump);
                /* A `$` path is not rejected for an error outside the syntax class. */
                if (line[0] == '$' && !jsonPathErrIsSyntax((jsonPathErr)e)) e = 0;
                line[plen] = first;
                const char *msg = e ? jsonPathErrMsg((jsonPathErr)e) : "";
                uniform += (size_t)is_uniform;
                if (e) {
                    predicted++;
                    CHECK(strlen(msg) == vlen && memcmp(msg, tab + 1, vlen) == 0,
                          "%.*s is rejected at compile time with \"%s\" but the module said \"%.*s\"", (int)plen, line, msg,
                          (int)vlen, tab + 1);
                }
                jsonPathFree(p);
            }
        }
        line += n + 1;
    }
    free(in);
    printf("compile: %zu paths the module rejects on every document, %zu of them rejected from the program alone\n", uniform,
           predicted);

    /* Properties of a compiled path. */
    checkProps("$", 1, 1, 1);
    checkProps(".", 0, 1, 1);
    checkProps("", 0, 0, 1);
    checkProps("$.", 1, 0, 0);
    checkProps("$.a", 1, 0, 1);
    checkProps(".a", 0, 0, 1);
    checkProps("a", 0, 0, 1);
    checkProps("a.b[0]['c'][\"d\"][-1]", 0, 0, 1);
    checkProps("$.a.b.c.d", 1, 0, 1);
    checkProps("$.a[*]", 1, 0, 0);
    checkProps("$.a[0:1]", 1, 0, 0);
    checkProps("$.a[0,1]", 1, 0, 0);
    checkProps("$['a','b']", 1, 0, 0);
    checkProps("$..a", 1, 0, 0);
    checkProps("..a", 0, 0, 0);
    checkProps("$.a[?(@.b)]", 1, 0, 0);
    checkProps("$.a[", 1, 0, 0);
    checkProps("$.a b", 1, 0, 0);

    /* The query size limit is the only compile failure. A path ends at a NUL. */
    size_t big = 128 * 1024;
    char *long_path = malloc(big + 16);
    jsonPathErr err = JSON_PATH_OK;
    memset(long_path, 'a', big + 16);
    long_path[0] = '$';
    long_path[1] = '.';
    jsonPath *p = jsonPathCompile(long_path, big, &err);
    CHECK(p != NULL && err == JSON_PATH_OK, "a path of exactly the limit must compile");
    jsonPathFree(p);
    p = jsonPathCompile(long_path, big + 1, &err);
    CHECK(p == NULL && err == JSON_PATH_ERR_TOO_LONG, "a path above the limit must fail, err %d", err);
    long_path[3] = '\0';
    p = jsonPathCompile(long_path, big + 1, &err);
    CHECK(p != NULL && jsonPathIsSingular(p), "a path ends at its first NUL");
    jsonPathFree(p);
    free(long_path);

    /* Every code has the module's text, and the syntax class is the module's. */
    static const struct {
        jsonPathErr e;
        const char *msg;
        int syntax;
    } msgs[] = {
        {JSON_PATH_ERR_SYNTAX, "SYNTAXERR Invalid JSON path", 1},
        {JSON_PATH_ERR_TOO_LONG, "LIMIT Query string size limit is exceeded", 1},
        {JSON_PATH_ERR_TOO_DEEP, "LIMIT Parser recursion depth is exceeded", 1},
        {JSON_PATH_ERR_TOO_MANY_DESCENTS,
         "LIMIT Total number of recursive descent tokens in the query string exceeds the limit", 1},
        {JSON_PATH_ERR_NOT_EXIST, "NONEXISTENT JSON path does not exist", 0},
        {JSON_PATH_ERR_INDEX_RANGE, "OUTOFBOUNDARIES Array index is out of bounds", 0},
        {JSON_PATH_ERR_NOT_ARRAY, "WRONGTYPE JSON element is not an array", 0},
        {JSON_PATH_ERR_NOT_OBJECT, "WRONGTYPE JSON element is not an object", 0},
        {JSON_PATH_ERR_INSERT_ARRAY, "ERROR Cannot insert a member into a non-object value", 0},
        {JSON_PATH_ERR_INSERT_PARENT, "NONEXISTENT Parent element does not exist", 1},
        {JSON_PATH_ERR_MEMBER_NAME, "SYNTAXERR Invalid object member name", 1},
        {JSON_PATH_ERR_NUMBER, "SYNTAXERR Invalid number", 1},
        {JSON_PATH_ERR_IDENTIFIER, "SYNTAXERR Invalid identifier", 1},
        {JSON_PATH_ERR_DOT_SEQUENCE, "SYNTAXERR Invalid dot sequence", 0},
        {JSON_PATH_ERR_EMPTY_EXPR, "SYNTAXERR Expression token cannot be empty", 1},
        {JSON_PATH_ERR_INDEX_NOT_NUMBER, "SYNTAXERR Array index is not a number", 1},
        {JSON_PATH_ERR_STEP_ZERO, "SYNTAXERR Step in the slice cannot be zero", 1},
        {JSON_PATH_ERR_WILDCARD, "ERR Invalid use of wildcard", 0},
        {JSON_PATH_ERR_DOLLAR, "SYNTAXERR Dollar sign cannot apply to non-root element", 0},
        {JSON_PATH_ERR_JSON_PARSE, "SYNTAXERR Failed to parse JSON string due to syntax error", 0},
    };
    for (size_t i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        CHECK(strcmp(jsonPathErrMsg(msgs[i].e), msgs[i].msg) == 0, "message of %d", msgs[i].e);
        CHECK(jsonPathErrIsSyntax(msgs[i].e) == msgs[i].syntax, "syntax class of %d", msgs[i].e);
    }
    /* Random strings that are plain paths, or nearly. */
    static const char *pieces[] = {"$", ".", ".", ".", "[", "]", "[0]", "[12]", ".a", ".b1", "_", "-", "x", "y", "7", "0",
                                   "..", "*", " ", "'", "\xc3\xa9", "[-1]", "[1:2]", ".a.b", "$.", "[123456789012345678]",
                                   "[1234567890123456789]", "?", "\""};
    uint64_t seed = 0x2545F4914F6CDD1DULL;
    for (int i = 0; i < 400000; i++) {
        char path[64];
        size_t n = 0;
        seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
        int parts = 1 + (int)(seed % 6);
        for (uint64_t r = seed >> 8; parts--; r = r * 6364136223846793005ULL + 1442695040888963407ULL) {
            const char *piece = pieces[(r >> 33) % ((r >> 20) % 8 ? 16 : sizeof(pieces) / sizeof(pieces[0]))];
            size_t pl = strlen(piece);
            if (n + pl >= sizeof(path)) break;
            memcpy(path + n, piece, pl);
            n += pl;
        }
        checkDirect(path, n);
    }
    printf("compile: %lld paths compiled both directly and in full\n", direct_checked);
    jsonPathCacheClear();
    CHECK(live_allocs == 0, "%lld allocations leaked", live_allocs);
    printf("compile: %zu program cases, %d failures\n", count, failures);
    return failures != 0;
}

#ifndef JSON_PATH_NO_ENGINE

/* ----------------------------------------------------------------------------
 * Helpers over the engine
 * ------------------------------------------------------------------------- */

static jsonDoc parseDoc(const char *s, size_t n) {
    jsonErr err = JSON_OK;
    size_t off = 0;
    jsonVal *v = jsonValParse(s, n, 0, 0, &err, &off);
    if (!v) {
        fprintf(stderr, "engine rejected a test document (error %d at %zu): %.60s\n", err, off, s);
        exit(2);
    }
    return jsonValToDoc(v);
}

static jsonPathErr evalPath(const char *path, size_t n, jsonDoc d, jsonPathMode mode, jsonMatches *m, jsonPath **keep) {
    jsonPathErr err = JSON_PATH_OK;
    jsonPath *p = jsonPathCompile(path, n, &err);
    if (!p) return err;
    err = jsonPathEval(p, d, mode, m);
    if (keep) {
        *keep = p;
    } else {
        jsonPathFree(p);
    }
    return err;
}

static int sameLoc(const jsonStep *a, uint32_t na, const jsonStep *b, uint32_t nb) {
    if (na != nb) return 0;
    for (uint32_t i = 0; i < na; i++) {
        if (a[i].keyid != b[i].keyid || (!a[i].keyid && a[i].pos != b[i].pos)) return 0;
    }
    return 1;
}

static int isMatched(const jsonMatches *m, const jsonStep *loc, uint32_t depth) {
    for (uint32_t i = 0; i < m->count; i++) {
        if (sameLoc(m->items[i].loc, m->items[i].depth, loc, depth)) return 1;
    }
    return 0;
}

/* What a write does to the document, computed with the read interface only:
 * the document is serialised with the change folded in. */
typedef enum {
    W_SET,
    W_DEL,
    W_APPEND
} writeKind;

static void emitChanged(jsonBuf *out, const jsonRef *r, jsonStep *loc, uint32_t depth, const jsonMatches *m, writeKind kind) {
    jsonType t = jsonRefType(r);
    int matched = isMatched(m, loc, depth);
    if (kind == W_SET && matched) {
        jsonBufAppend(out, "\"NEW\"", 5);
        return;
    }
    if (t != JSON_ARRAY && t != JSON_OBJECT) {
        jsonSerialize(r, NULL, out);
        return;
    }
    jsonIter it;
    jsonRef child;
    const char *name;
    uint32_t name_len, emitted = 0;
    jsonIterInit(&it, r);
    jsonBufAppend(out, t == JSON_ARRAY ? "[" : "{", 1);
    while (jsonIterNext(&it, &child, &loc[depth], &name, &name_len)) {
        if (kind == W_DEL && isMatched(m, loc, depth + 1)) continue;
        if (emitted++) jsonBufAppend(out, ",", 1);
        if (t == JSON_OBJECT) {
            jsonBufAppendQuoted(out, name, name_len);
            jsonBufAppend(out, ":", 1);
        }
        emitChanged(out, &child, loc, depth + 1, m, kind);
    }
    if (kind == W_APPEND && matched && t == JSON_ARRAY) {
        if (emitted++) jsonBufAppend(out, ",", 1);
        jsonBufAppend(out, "999", 3);
    }
    if (kind == W_SET && t == JSON_OBJECT) {
        for (uint32_t i = 0; i < m->insert_count; i++) {
            if (!sameLoc(m->inserts[i].parent, m->inserts[i].parent_depth, loc, depth)) continue;
            if (emitted++) jsonBufAppend(out, ",", 1);
            jsonBufAppendQuoted(out, m->inserts[i].name, m->inserts[i].name_len);
            jsonBufAppend(out, ":\"NEW\"", 6);
        }
    }
    jsonBufAppend(out, t == JSON_ARRAY ? "]" : "}", 1);
}

static void changedDoc(jsonBuf *out, jsonDoc d, const jsonMatches *m, writeKind kind) {
    static jsonStep loc[4096];
    jsonRef root;
    jsonDocRoot(d, &root);
    emitChanged(out, &root, loc, 0, m, kind);
}

/* 1 if a write through these matches is one the module gets wrong because it
 * addresses values by JSON Pointer: a name with "~" on the way to a match, or
 * a new name with "~" or "/" (behaviour change B5). */
static int pointerHazard(const jsonMatches *m) {
    for (uint32_t i = 0; i < m->count; i++) {
        for (uint32_t k = 0; k < m->items[i].depth; k++) {
            uint32_t len, keyid = m->items[i].loc[k].keyid;
            if (!keyid) continue;
            const char *name = jsonKeyName(keyid, &len);
            if (memchr(name, '~', len)) return 1;
        }
    }
    for (uint32_t i = 0; i < m->insert_count; i++) {
        const jsonInsert *in = &m->inserts[i];
        if (memchr(in->name, '~', in->name_len) || memchr(in->name, '/', in->name_len)) return 1;
        for (uint32_t k = 0; k < in->parent_depth; k++) {
            uint32_t len, keyid = in->parent[k].keyid;
            if (!keyid) continue;
            const char *name = jsonKeyName(keyid, &len);
            if (memchr(name, '~', len)) return 1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------------
 * Golden tests
 * ------------------------------------------------------------------------- */

typedef struct field {
    const char *s;
    size_t n;
} field;

static const char *readField(const char *p, const char *end, field *f) {
    size_t n = 0;
    while (p < end && *p >= '0' && *p <= '9') n = n * 10 + (size_t)(*p++ - '0');
    p++; /* newline */
    f->s = p;
    f->n = n;
    return p + n + 1;
}

static int fieldIs(const field *f, const char *s, size_t n) {
    return f->n == n && memcmp(f->s, s, n) == 0;
}

typedef struct tally {
    long long checked, failed, skipped;
} tally;

static void report(tally *t, const char *what, const field *doc, const field *path, const field *want, const char *got, size_t got_len) {
    t->checked++;
    if (fieldIs(want, got, got_len)) return;
    t->failed++;
    if (t->failed <= 15) {
        fprintf(stderr, "MISMATCH %s\n  doc  %.*s\n  path %.*s\n  want %.*s\n  got  %.*s\n", what,
                (int)(doc->n > 200 ? 200 : doc->n), doc->s, (int)(path->n > 200 ? 200 : path->n), path->s,
                (int)(want->n > 300 ? 300 : want->n), want->s, (int)(got_len > 300 ? 300 : got_len), got);
    }
}

static void appendErr(jsonBuf *b, jsonPathErr e) {
    const char *msg = jsonPathErrMsg(e);
    jsonBufAppend(b, "E", 1);
    jsonBufAppend(b, msg, strlen(msg));
}

static void appendInt(jsonBuf *b, long long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", v);
    jsonBufAppend(b, tmp, (size_t)n);
}

static int cmdGolden(void) {
    size_t len;
    char *in = readAll(stdin, &len);
    const char *p = in, *end = in + len;
    enum {
        MAXDOCS = 256
    };
    static jsonDoc docs[MAXDOCS];
    static field doc_text[MAXDOCS];
    static field doc_canon[MAXDOCS];
    static char *canon_store[MAXDOCS];
    int ndocs = 0;
    tally get = {0, 0, 0}, set = {0, 0, 0}, setdoc = {0, 0, 0}, del = {0, 0, 0}, deldoc = {0, 0, 0}, arr = {0, 0, 0},
          arrdoc = {0, 0, 0}, modes = {0, 0, 0}, room = {0, 0, 0};
    long long pairs = 0;
    jsonBuf b, b2;
    static char init1[1024], init2[1024];
    jsonBufInit(&b, init1, sizeof(init1));
    jsonBufInit(&b2, init2, sizeof(init2));

    while (p < end) {
        if (*p == 'D') {
            field text;
            p = readField(p + 2, end, &text);
            docs[ndocs] = parseDoc(text.s, text.n);
            doc_text[ndocs] = text;
            jsonRef root;
            jsonDocRoot(docs[ndocs], &root);
            b.len = 0;
            jsonSerialize(&root, NULL, &b);
            canon_store[ndocs] = malloc(b.len + 1);
            memcpy(canon_store[ndocs], b.p, b.len);
            doc_canon[ndocs].s = canon_store[ndocs];
            doc_canon[ndocs].n = b.len;
            ndocs++;
            continue;
        }
        if (*p != 'C') break;
        int di = atoi(p + 2);
        field path, g_get, g_set, g_setdoc, g_del, g_deldoc, g_arr, g_arrdoc, g_room;
        p = memchr(p, '\n', (size_t)(end - p)) + 1;
        p = readField(p, end, &path);
        p = readField(p, end, &g_get);
        p = readField(p, end, &g_set);
        p = readField(p, end, &g_setdoc);
        p = readField(p, end, &g_del);
        p = readField(p, end, &g_deldoc);
        p = readField(p, end, &g_arr);
        p = readField(p, end, &g_arrdoc);
        p = readField(p, end, &g_room);
        pairs++;

        jsonDoc d = docs[di];
        const field *doc = &doc_text[di];
        jsonMatches m;
        jsonMatchesInit(&m);
        jsonPath *cp = NULL;

        checkDirect(path.s, path.n);

        /* JSON.GET: a legacy path replies the first match, a `$` path an array of all. */
        jsonPathErr rc = evalPath(path.s, path.n, d, JSON_PATH_READ, &m, &cp);
        b.len = 0;
        if (rc != JSON_PATH_OK) {
            appendErr(&b, rc);
        } else if (!jsonMatchesIsV2(&m)) {
            jsonBufAppend(&b, "S", 1);
            jsonSerialize(&m.items[0].ref, NULL, &b);
        } else {
            jsonBufAppend(&b, "S[", 2);
            for (uint32_t i = 0; i < m.count; i++) {
                if (i) jsonBufAppend(&b, ",", 1);
                jsonSerialize(&m.items[i].ref, NULL, &b);
            }
            jsonBufAppend(&b, "]", 1);
        }
        report(&get, "GET", doc, &path, &g_get, b.p, b.len);

        /* Every match has to resolve to the value it carries. */
        if (rc == JSON_PATH_OK) {
            modes.checked++;
            for (uint32_t i = 0; i < m.count; i++) {
                jsonRef r;
                if (!jsonDocResolve(d, m.items[i].loc, (int)m.items[i].depth, &r) || !jsonRefEquals(&r, &m.items[i].ref)) {
                    modes.failed++;
                    if (modes.failed <= 5) fprintf(stderr, "LOCATOR does not resolve: path %.*s\n", (int)path.n, path.s);
                    break;
                }
            }
        }

        if (!cp) {
            /* The only compile failure, the query size limit, fails every command. */
            report(&set, "SET reply", doc, &path, &g_set, b.p, b.len);
            report(&del, "DEL reply", doc, &path, &g_del, b.p, b.len);
            if (!fieldIs(&g_arr, "-", 1)) report(&arr, "ARRAPPEND reply", doc, &path, &g_arr, b.p, b.len);
        }

        /* JSON.SET with the value "NEW". */
        if (cp) {
            rc = jsonPathEval(cp, d, JSON_PATH_INSERT, &m);
            b.len = b2.len = 0;
            if (rc != JSON_PATH_OK) {
                appendErr(&b, rc);
                jsonBufAppend(&b2, "=", 1);
            } else {
                jsonBufAppend(&b, "SOK", 3);
                changedDoc(&b2, d, &m, W_SET);
                if (fieldIs(&doc_canon[di], b2.p, b2.len)) {
                    b2.len = 0;
                    jsonBufAppend(&b2, "=", 1);
                }
            }
            report(&set, "SET reply", doc, &path, &g_set, b.p, b.len);
            /* The deepest value the module still accepted at this path says
             * what its selector reported as the path depth. */
            if (rc == JSON_PATH_OK && !fieldIs(&g_room, "-", 1)) {
                char tmp[16];
                int tn = snprintf(tmp, sizeof(tmp), "%d", 128 - (int)jsonMatchesMaxDepth(&m));
                report(&room, "path depth (128 - depth)", doc, &path, &g_room, tmp, (size_t)tn);
            }
            if (rc == JSON_PATH_OK && ((m.count == 1 && m.insert_count >= 1) || pointerHazard(&m))) {
                setdoc.skipped++; /* Module defects: design 2.5 and B5. */
            } else {
                report(&setdoc, "SET document", doc, &path, &g_setdoc, b2.p, b2.len);
            }

            /* JSON.DEL. The command deletes the key for a root path, replies 0
             * for the two "nothing there" errors, and cannot delete the root
             * through any other path. */
            rc = jsonPathEval(cp, d, JSON_PATH_DELETE, &m);
            b.len = b2.len = 0;
            int hazard = 0;
            if (jsonPathIsRoot(cp)) {
                jsonBufAppend(&b, "I1", 2);
                jsonBufAppend(&b2, "!gone", 5);
            } else if (rc == JSON_PATH_ERR_SYNTAX || rc == JSON_PATH_ERR_NOT_EXIST) {
                jsonBufAppend(&b, "I0", 2);
                jsonBufAppend(&b2, "=", 1);
            } else if (rc != JSON_PATH_OK) {
                appendErr(&b, rc);
                jsonBufAppend(&b2, "=", 1);
            } else {
                long long n = 0;
                for (uint32_t i = 0; i < m.count; i++) n += m.items[i].depth > 0;
                jsonBufAppend(&b, "I", 1);
                appendInt(&b, n);
                changedDoc(&b2, d, &m, W_DEL);
                if (fieldIs(&doc_canon[di], b2.p, b2.len)) {
                    b2.len = 0;
                    jsonBufAppend(&b2, "=", 1);
                }
                hazard = pointerHazard(&m);
            }
            if (hazard) {
                del.skipped++;
                deldoc.skipped++;
            } else {
                report(&del, "DEL reply", doc, &path, &g_del, b.p, b.len);
                report(&deldoc, "DEL document", doc, &path, &g_deldoc, b2.p, b2.len);
            }

            /* JSON.ARRAPPEND 999: fails on any selector error, appends to every
             * distinct array matched, replies the new lengths in match order. */
            if (!fieldIs(&g_arr, "-", 1)) {
                rc = jsonPathEval(cp, d, JSON_PATH_UPDATE, &m);
                jsonPathErr raw = jsonMatchesRawErr(&m);
                int v2 = jsonMatchesIsV2(&m), arrays = 0;
                for (uint32_t i = 0; i < m.count; i++) arrays += jsonRefType(&m.items[i].ref) == JSON_ARRAY;
                b.len = b2.len = 0;
                if (raw != JSON_PATH_OK) {
                    appendErr(&b, raw);
                } else if (!v2 && m.count == 0) {
                    appendErr(&b, JSON_PATH_ERR_NOT_EXIST);
                } else if (!v2 && !arrays) {
                    appendErr(&b, JSON_PATH_ERR_NOT_ARRAY);
                } else {
                    if (v2) jsonBufAppend(&b, "A[", 2);
                    for (uint32_t i = 0; i < m.count; i++) {
                        int is_array = jsonRefType(&m.items[i].ref) == JSON_ARRAY;
                        if (!v2) {
                            if (!is_array) continue;
                            jsonBufAppend(&b, "I", 1);
                            appendInt(&b, (long long)jsonRefCount(&m.items[i].ref) + 1);
                            break;
                        }
                        if (i) jsonBufAppend(&b, ",", 1);
                        if (is_array) {
                            appendInt(&b, (long long)jsonRefCount(&m.items[i].ref) + 1);
                        } else {
                            jsonBufAppend(&b, "null", 4);
                        }
                    }
                    if (v2) jsonBufAppend(&b, "]", 1);
                    changedDoc(&b2, d, &m, W_APPEND);
                }
                if (!b2.len || fieldIs(&doc_canon[di], b2.p, b2.len)) {
                    b2.len = 0;
                    jsonBufAppend(&b2, "=", 1);
                }
                report(&arr, "ARRAPPEND reply", doc, &path, &g_arr, b.p, b.len);
                report(&arrdoc, "ARRAPPEND document", doc, &path, &g_arrdoc, b2.p, b2.len);
            }
            jsonPathFree(cp);
        }
        jsonMatchesFree(&m);
    }
    jsonBufFree(&b);
    jsonBufFree(&b2);
    for (int i = 0; i < ndocs; i++) {
        jsonDocFree(docs[i]);
        free(canon_store[i]);
    }
    free(in);
    printf("golden: %d documents, %lld (document, path) pairs\n", ndocs, pairs);
    const struct {
        const char *name;
        tally *t;
    } rows[] = {{"GET reply", &get}, {"match locators", &modes}, {"SET reply", &set}, {"SET document", &setdoc}, {"DEL reply", &del}, {"DEL document", &deldoc}, {"ARRAPPEND reply", &arr}, {"ARRAPPEND document", &arrdoc}, {"path depth", &room}};
    long long failed = 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        printf("  %-20s checked %6lld  failed %5lld  skipped %4lld\n", rows[i].name, rows[i].t->checked, rows[i].t->failed,
               rows[i].t->skipped);
        failed += rows[i].t->failed;
    }
    jsonPathCacheClear();
    size_t keys_left = jsonEngineShutdown();
    if (keys_left) printf("  note: %zu key references were not released\n", keys_left);
    if (live_allocs) printf("  note: %lld allocations were not freed\n", live_allocs);
    failed += (long long)keys_left + live_allocs + failures;
    printf("  %lld paths compiled both directly and in full, %d differences\n", direct_checked, failures);
    return failed != 0;
}

/* ----------------------------------------------------------------------------
 * Unit checks
 * ------------------------------------------------------------------------- */


/* ----------------------------------------------------------------------------
 * The cache of compiled paths
 * ------------------------------------------------------------------------- */

static jsonDoc parseText(const char *s) {
    return parseDoc(s, strlen(s));
}

static long long firstInt(const jsonMatches *m) {
    int64_t v = -1;
    if (m->count < 1 || !jsonRefInt(&m->items[0].ref, &v)) return -1;
    return (long long)v;
}

/* Compiles, evaluates and frees, as a command does. Returns the first match as an integer, or -1. */
static long long useInt(const char *path, jsonDoc d, jsonMatches *m, uint32_t *count) {
    jsonPathErr err;
    jsonPath *p = jsonPathCompile(path, strlen(path), &err);
    jsonPathEval(p, d, JSON_PATH_READ, m);
    jsonPathFree(p);
    *count = m->count;
    return firstInt(m);
}

static void cacheTests(void) {
    jsonMatches m;
    jsonPathErr rc;
    jsonPath *p, *q;
    uint32_t n;
    long long before;
    jsonMatchesInit(&m);

    /* A repeated path: no allocation at all, in any mode. */
    jsonDoc d = parseText("{\"address\":{\"city\":7},\"s\":5}");
    CHECK(useInt("$.address.city", d, &m, &n) == 7, "first use");
    before = total_allocs;
    for (int i = 0; i < 1000; i++) {
        p = jsonPathCompile("$.address.city", 14, &rc);
        rc = jsonPathEval(p, d, (jsonPathMode)(i % 4), &m);
        if (rc != JSON_PATH_OK || firstInt(&m) != 7) failures++;
        jsonPathFree(p);
    }
    CHECK(total_allocs == before, "a repeated path allocated %lld times", total_allocs - before);

    /* Documents with a name come and go while paths with that name sit in the
     * cache. The cache keeps the name's key, so its ID cannot go to another
     * name meanwhile. */
    static const char *zpaths[] = {"$.zeta", "$['zeta','zz']", "$.in[?(@.zeta>0)].zeta", "$..zeta", "$[?(@.zeta>0)].zeta"};
    static const char *shape1 = "{\"f1\":0,\"in\":[{\"f2\":0}]}", *shape2 = "{\"f4\":0,\"f5\":0,\"in\":[{\"f6\":0,\"f7\":0}]}";
    jsonDoc d1 = parseText("{\"zeta\":1,\"in\":[{\"zeta\":1}]}");
    for (int i = 0; i < 5; i++) CHECK(useInt(zpaths[i], d1, &m, &n) == 1, "%s before: %u matches", zpaths[i], n);
    uint32_t zeta_id = jsonKeyFind("zeta", 4);
    jsonKeyRelease(zeta_id);
    jsonDocFree(d1);
    jsonDoc d2 = parseText("{\"omega\":2,\"in\":[{\"omega\":2}]}");
    for (int i = 0; i < 5; i++) {
        long long v = useInt(zpaths[i], d2, &m, &n);
        CHECK(n == 0, "%s matched %u values (first %lld) in a document without zeta", zpaths[i], n, v);
    }
    CHECK(useInt("$.omega", d2, &m, &n) == 2, "omega");
    jsonDoc d3 = parseText("{\"omega\":5,\"zeta\":3,\"in\":[{\"omega\":5,\"zeta\":3}]}");
    for (int i = 0; i < 5; i++) CHECK(useInt(zpaths[i], d3, &m, &n) == 3, "%s after: %u matches", zpaths[i], n);
    CHECK(useInt("$.omega", d3, &m, &n) == 5, "omega beside zeta");
    jsonDocFree(d2);
    for (int i = 0; i < 5; i++) CHECK(useInt(zpaths[i], d3, &m, &n) == 3, "%s after omega's first document went", zpaths[i]);
    jsonDocFree(d3);
    for (int i = 0; i < 5; i++) {
        useInt(zpaths[i], d, &m, &n);
        CHECK(n == 0, "%s with both names gone: %u matches", zpaths[i], n);
    }

    /* Now the cache lets go of zeta: every path that names it is pushed out
     * by other paths, and documents of the same shapes push it out of the
     * engine parser's memory of recent keys. The name leaves the key table. */
    char path[160];
    for (int i = 0; i < 1200; i++) {
        sprintf(path, "$.push%d", i);
        useInt(path, d, &m, &n);
    }
    jsonDocFree(parseText(shape1));
    jsonDocFree(parseText(shape2));
    uint32_t still = jsonKeyFind("zeta", 4);
    if (still) jsonKeyRelease(still);
    CHECK(still == 0, "zeta is still in the key table after its cache entries were replaced");
    /* New names take the lowest free ID, so one of these soon has the ID zeta had. */
    enum {
        THIEVES = 64
    };
    jsonDoc thieves[THIEVES];
    int nthieves = 0, thief = -1;
    char tdoc[96];
    while (nthieves < THIEVES && thief < 0) {
        sprintf(tdoc, "{\"thief%d\":2,\"in\":[{\"thief%d\":2}]}", nthieves, nthieves);
        thieves[nthieves] = parseText(tdoc);
        sprintf(tdoc, "thief%d", nthieves);
        uint32_t thief_id = jsonKeyFind(tdoc, strlen(tdoc));
        jsonKeyRelease(thief_id);
        if (thief_id == zeta_id) thief = nthieves;
        nthieves++;
    }
    CHECK(thief >= 0, "the test relies on a new name taking the ID zeta had (%u)", zeta_id);
    if (thief >= 0) {
        for (int i = 0; i < 5; i++) {
            long long v = useInt(zpaths[i], thieves[thief], &m, &n);
            CHECK(n == 0, "%s matched %u values (first %lld) through the ID that zeta used to have", zpaths[i], n, v);
        }
        sprintf(tdoc, "{\"thief%d\":5,\"zeta\":3,\"in\":[{\"thief%d\":5,\"zeta\":3}]}", thief, thief);
        d3 = parseText(tdoc);
        for (int i = 0; i < 5; i++) CHECK(useInt(zpaths[i], d3, &m, &n) == 3, "%s with zeta back under a new ID: %u", zpaths[i], n);
        jsonDocFree(d3);
    }
    for (int i = 0; i < nthieves; i++) jsonDocFree(thieves[i]);

    /* One compiled path kept across such changes, as one command could keep it. */
    p = jsonPathCompile("$.kept", 6, &rc);
    d1 = parseText("{\"kept\":1}");
    rc = jsonPathEval(p, d1, JSON_PATH_READ, &m);
    CHECK(firstInt(&m) == 1, "kept path, first document");
    jsonDocFree(d1);
    for (int i = 0; i < 8; i++) {
        sprintf(tdoc, "{\"other%d\":2}", i);
        d2 = parseText(tdoc);
        rc = jsonPathEval(p, d2, JSON_PATH_READ, &m);
        CHECK(rc == JSON_PATH_OK && m.count == 0, "kept path matched other%d", i);
        rc = jsonPathEval(p, d2, JSON_PATH_INSERT, &m);
        CHECK(rc == JSON_PATH_OK && m.count == 0 && m.insert_count == 1, "kept path must insert beside other%d", i);
        jsonDocFree(d2);
    }
    d3 = parseText("{\"other3\":5,\"kept\":3}");
    rc = jsonPathEval(p, d3, JSON_PATH_READ, &m);
    CHECK(firstInt(&m) == 3, "kept path, name back: %lld", firstInt(&m));
    jsonPathFree(p);
    jsonDocFree(d3);

    /* Cached while it matched nothing, then the name appears. */
    CHECK(useInt("$.appears", d, &m, &n) == -1 && n == 0, "name not there yet");
    CHECK(useInt("$.appears", d, &m, &n) == -1 && n == 0, "name still not there");
    d1 = parseText("{\"appears\":9}");
    CHECK(useInt("$.appears", d1, &m, &n) == 9, "name there now: %u matches", n);
    CHECK(useInt("$.appears", d, &m, &n) == -1 && n == 0, "other document still without it");
    jsonDocFree(d1);
    CHECK(useInt("$.appears", d, &m, &n) == -1 && n == 0, "name gone again");

    /* Two live compilations of one string are two paths: forcing one to `$` leaves the other alone. */
    for (int round = 0; round < 3; round++) {
        if (round == 2) before = total_allocs;
        p = jsonPathCompile(".s.*", 4, &rc);
        q = jsonPathCompile(".s.*", 4, &rc);
        CHECK(p != q, "one path handed out twice");
        CHECK(!jsonPathIsV2(p) && !jsonPathIsV2(q), "a forced `$` survived in the cache");
        jsonPathForceV2(p);
        CHECK(jsonPathIsV2(p) && !jsonPathIsV2(q), "forcing one copy changed the other");
        rc = jsonPathEval(p, d, JSON_PATH_READ, &m);
        CHECK(rc == JSON_PATH_OK && m.count == 0, "forced copy: %d", rc);
        rc = jsonPathEval(q, d, JSON_PATH_READ, &m);
        CHECK(rc == JSON_PATH_ERR_SYNTAX, "other copy: %d", rc);
        jsonPathFree(p);
        jsonPathFree(q);
    }
    CHECK(total_allocs == before, "two live copies allocated in steady state");

    /* More distinct paths than the cache has entries, three times round. One
     * path stays out the whole time and must not be reused under its owner. */
    enum {
        MANY = 1000
    };
    char *big = malloc(MANY * 16);
    size_t len = 0;
    for (int i = 0; i < MANY; i++) len += (size_t)sprintf(big + len, "%s\"k%d\":%d", i ? "," : "{", i, i);
    big[len++] = '}';
    jsonDoc dk = parseDoc(big, len);
    free(big);
    jsonPath *held = jsonPathCompile("$.k500", 6, &rc);
    for (int round = 0; round < 3; round++) {
        if (round == 2) before = total_allocs;
        for (int i = 0; i < MANY; i++) {
            sprintf(path, "$.k%d", i);
            if (useInt(path, dk, &m, &n) != i) failures++;
            sprintf(path, ".k%d", MANY - 1 - i);
            if (useInt(path, dk, &m, &n) != MANY - 1 - i) failures++;
        }
    }
    CHECK(total_allocs == before, "cycling %d short paths allocated %lld times once warm", 2 * MANY, total_allocs - before);
    rc = jsonPathEval(held, dk, JSON_PATH_READ, &m);
    CHECK(rc == JSON_PATH_OK && firstInt(&m) == 500, "the path held across the evictions");
    jsonPathFree(held);

    /* More live paths than entries: the rest are compiled on the heap, and all of them work. */
    enum {
        LIVE = 300
    };
    static jsonPath *live[2 * LIVE];
    for (int i = 0; i < LIVE; i++) {
        sprintf(path, "$.k%d", i);
        live[i] = jsonPathCompile(path, strlen(path), &rc);
        live[LIVE + i] = jsonPathCompile("$.k7", 4, &rc);
    }
    for (int i = 0; i < LIVE; i++) {
        rc = jsonPathEval(live[i], dk, JSON_PATH_READ, &m);
        if (firstInt(&m) != i) failures++;
        rc = jsonPathEval(live[LIVE + i], dk, JSON_PATH_READ, &m);
        if (firstInt(&m) != 7) failures++;
        for (int j = 0; j < i; j += 37) {
            if (live[j] == live[i] || live[LIVE + j] == live[LIVE + i]) failures++;
        }
    }
    for (int i = 0; i < 2 * LIVE; i++) jsonPathFree(live[i]);
    CHECK(useInt("$.k7", dk, &m, &n) == 7, "after many live paths");
    jsonDocFree(dk);

    /* At the longest cacheable length, and one byte over. */
    char doc128[400], p128[140], p129[140];
    memset(p128, 'n', sizeof(p128));
    memset(p129, 'm', sizeof(p129));
    memcpy(p128, "$.", 2);
    memcpy(p129, "$.", 2);
    p128[128] = p129[129] = '\0';
    sprintf(doc128, "{\"%s\":128,\"%s\":129}", p128 + 2, p129 + 2);
    jsonDoc dl = parseDoc(doc128, strlen(doc128));
    CHECK(useInt(p128, dl, &m, &n) == 128 && useInt(p129, dl, &m, &n) == 129, "long names");
    before = total_allocs;
    CHECK(useInt(p128, dl, &m, &n) == 128, "128 bytes again");
    CHECK(total_allocs == before, "a path of 128 bytes was not served from the cache");
    long long live_before = live_allocs;
    CHECK(useInt(p129, dl, &m, &n) == 129, "129 bytes again");
    CHECK(total_allocs > before && live_allocs == live_before, "a path of 129 bytes: %lld allocations, %lld left",
          total_allocs - before, live_allocs - live_before);
    jsonDocFree(dl);

    /* A path ends at a NUL, so these are the same entry. And the empty path is the root. */
    p = jsonPathCompile("$.s\0junk", 8, &rc);
    rc = jsonPathEval(p, d, JSON_PATH_READ, &m);
    CHECK(rc == JSON_PATH_OK && firstInt(&m) == 5, "NUL in a path");
    jsonPathFree(p);
    for (int i = 0; i < 2; i++) {
        p = jsonPathCompile("", 0, &rc);
        rc = jsonPathEval(p, d, JSON_PATH_READ, &m);
        CHECK(rc == JSON_PATH_OK && m.count == 1 && m.items[0].depth == 0 && jsonPathIsSingular(p), "empty path");
        jsonPathFree(p);
    }

    /* A filter and a long path keep working when their entry is recompiled for another path and back. */
    static const struct {
        const char *path;
        uint32_t n;
        long long first;
    } shapes[] = {{"$.a.b.c.d.e.f.g.h.i.j.k.l", 0, -1}, {"$[?(@.address)].s", 1, 5}, {"$[?(@.s==5 || @.s=='x' && @.s>1)].s", 1, 5}, {"$.s", 1, 5}, {"$['address','s','address']", 3, -1}, {"$..city", 1, 7}, {"$[?(@.s==$.s)].address.city", 1, 7}};
    for (int round = 0; round < 400; round++) {
        int k = (round * 5) % 7;
        long long v = useInt(shapes[k].path, d, &m, &n);
        CHECK(n == shapes[k].n && v == shapes[k].first, "%s in round %d: %u matches, first %lld", shapes[k].path, round, n, v);
        if (n != shapes[k].n || v != shapes[k].first) break;
        sprintf(path, "$.evict%d", round); /* Pushes the shapes out of the cache now and then. */
        useInt(path, d, &m, &n);
    }

    /* Clearing the cache with a path still out: the path stays usable and is freed by its owner. */
    p = jsonPathCompile("$.s", 3, &rc);
    jsonPathCacheClear();
    rc = jsonPathEval(p, d, JSON_PATH_READ, &m);
    CHECK(rc == JSON_PATH_OK && firstInt(&m) == 5, "a path that outlived the cache");
    q = jsonPathCompile("$.s", 3, &rc);
    CHECK(q != p, "the cache gave out a path it no longer owns");
    jsonPathFree(q);
    jsonPathFree(p);

    /* Clearing the cache gives back its memory and its keys. */
    CHECK(useInt("$.pinned", d, &m, &n) == -1, "a name in no document");
    jsonDoc dp = parseText("{\"pinned\":4}");
    CHECK(useInt("$.pinned", dp, &m, &n) == 4, "the name in a document");
    jsonDocFree(dp);
    jsonDocFree(parseText("{\"f9\":0}"));
    uint32_t pinned = jsonKeyFind("pinned", 6);
    if (pinned) jsonKeyRelease(pinned);
    CHECK(pinned != 0, "the cached path should be holding its name's key");
    jsonMatchesFree(&m);
    jsonDocFree(d);
    long long held_by_cache = live_allocs;
    jsonPathCacheClear();
    pinned = jsonKeyFind("pinned", 6);
    if (pinned) jsonKeyRelease(pinned);
    CHECK(pinned == 0, "clearing the cache left a key behind");
    held_by_cache -= live_allocs;
    CHECK(held_by_cache >= 1 && held_by_cache <= 256 * 6, "clearing the cache freed %lld allocations", held_by_cache);
    before = live_allocs;
    jsonPathCacheClear();
    CHECK(live_allocs == before, "clearing an empty cache");
}


/* ----------------------------------------------------------------------------
 * Indexes, slice bounds and steps at and past the edges of their types
 * ------------------------------------------------------------------------- */

static double now(void);

static int hasDuplicates(const jsonMatches *m) {
    for (uint32_t i = 0; i < m->count; i++) {
        for (uint32_t j = i + 1; j < m->count; j++) {
            if (sameLoc(m->items[i].loc, m->items[i].depth, m->items[j].loc, m->items[j].depth)) return 1;
        }
    }
    for (uint32_t i = 0; i < m->insert_count; i++) {
        for (uint32_t j = i + 1; j < m->insert_count; j++) {
            if (sameLoc(m->inserts[i].parent, m->inserts[i].parent_depth, m->inserts[j].parent, m->inserts[j].parent_depth)) {
                return 1;
            }
        }
    }
    return 0;
}

/* Evaluates in one mode and renders the integer matches as "1,3,5", or the error as "E<code>". */
static const char *listOf(const char *path, jsonDoc d, jsonPathMode mode, jsonMatches *m) {
    static char out[4096];
    jsonPathErr rc = evalPath(path, strlen(path), d, mode, m, NULL);
    size_t n = 0;
    out[0] = '\0';
    if (rc != JSON_PATH_OK) {
        snprintf(out, sizeof(out), "E%d", rc);
        return out;
    }
    for (uint32_t i = 0; i < m->count && n + 32 < sizeof(out); i++) {
        int64_t v = -1;
        jsonRefInt(&m->items[i].ref, &v);
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s%lld", i ? "," : "", (long long)v);
    }
    return out;
}

/* The same result is expected in every mode: none of these paths repeats a value. */
static void expectAll(const char *path, jsonDoc d, const char *want, jsonMatches *m) {
    for (int mode = JSON_PATH_READ; mode <= JSON_PATH_DELETE; mode++) {
        const char *got = listOf(path, d, (jsonPathMode)mode, m);
        CHECK(strcmp(got, want) == 0, "%s in mode %d: want [%s], got [%s]", path, mode, want, got);
        CHECK(!hasDuplicates(m), "%s in mode %d returned a value twice", path, mode);
    }
}

/* A path that repeats values: a read keeps them, a write gets each once, first occurrence. */
static void expectDup(const char *path, jsonDoc d, const char *read, const char *write, jsonMatches *m) {
    const char *got = listOf(path, d, JSON_PATH_READ, m);
    CHECK(strcmp(got, read) == 0, "%s read: want [%s], got [%s]", path, read, got);
    for (int mode = JSON_PATH_UPDATE; mode <= JSON_PATH_DELETE; mode++) {
        got = listOf(path, d, (jsonPathMode)mode, m);
        CHECK(strcmp(got, write) == 0, "%s in mode %d: want [%s], got [%s]", path, mode, write, got);
        CHECK(!hasDuplicates(m), "%s in mode %d returned a value twice", path, mode);
    }
}

static void stepTests(void) {
    jsonMatches m;
    jsonMatchesInit(&m);
    jsonDoc d = parseText("[0,1,2,3,4,5]");
    char err_zero[8], err_range[8], err_union[8];
    snprintf(err_zero, sizeof(err_zero), "E%d", JSON_PATH_ERR_STEP_ZERO);
    snprintf(err_range, sizeof(err_range), "E%d", JSON_PATH_ERR_INDEX_RANGE);
    snprintf(err_union, sizeof(err_union), "E%d", JSON_PATH_ERR_SYNTAX);

    /* The reported bug: a step of 2^32 repeated element 1 about a thousand times. */
    expectAll("$[1:6:4294967296]", d, "1", &m);

    /* A step whose low 32 bits have its own sign is a slice with that step, as in the module. */
    expectAll("$[1:6:4294967297]", d, "1,2,3,4,5", &m);
    expectAll("$[1:6:4294967298]", d, "1,3,5", &m);
    expectAll("$[1:6:8589934593]", d, "1,2,3,4,5", &m);
    expectAll("$[5:0:-4294967297]", d, "5,4,3,2,1", &m);
    expectAll("$[5:0:-4294967298]", d, "5,3,1", &m);
    expectAll("$[5:0:-2147483648]", d, "5", &m);
    expectAll("$[5:0:-2147483647]", d, "5", &m);
    expectAll("$[0:6:2147483647]", d, "0", &m);
    expectAll("$[1:6:2147483647]", d, "1", &m);
    expectAll("$[1:6:2147483646]", d, "1", &m);

    /* Low 32 bits zero or of the other sign: the module hangs or walks the
     * wrong way. Here the step is larger than the array: the first element alone. */
    expectAll("$[1:6:2147483648]", d, "1", &m);
    expectAll("$[1:6:2147483649]", d, "1", &m);
    expectAll("$[1:6:4294967295]", d, "1", &m);
    expectAll("$[1:6:8589934592]", d, "1", &m);
    expectAll("$[1:6:9223372036854775807]", d, "1", &m);
    expectAll("$[5:0:-4294967296]", d, "5", &m);
    expectAll("$[5:0:-4294967295]", d, "5", &m);
    expectAll("$[5:0:-2147483649]", d, "5", &m);
    expectAll("$[5:0:-9223372036854775808]", d, "5", &m);
    expectAll("$[::4294967296]", d, "0", &m);
    expectAll("$[::-4294967296]", d, "", &m);
    expectAll("$[6:6:4294967296]", d, "", &m);
    expectAll("$[3:1:4294967296]", d, "", &m);
    expectAll("$[6:0:-4294967296]", d, "", &m);
    expectAll("$[1:3:-4294967296]", d, "", &m);
    /* The module's index wraps although the step does not. Here it does not wrap. */
    expectAll("$[5:6:2147483647]", d, "5", &m);
    expectAll("$[4:6:2147483646]", d, "4", &m);

    /* 2^63 and beyond saturate: still a step, a bound or an index beyond any array. */
    expectAll("$[1:6:9223372036854775808]", d, "1", &m);
    expectAll("$[1:6:18446744073709551616]", d, "1", &m);
    expectAll("$[1:6:18446744073709551617]", d, "1", &m);
    expectAll("$[1:6:99999999999999999999999999999]", d, "1", &m);
    expectAll("$[5:0:-9223372036854775809]", d, "5", &m);
    expectAll("$[5:0:-99999999999999999999999999999]", d, "5", &m);
    expectAll("$[-9223372036854775808:9223372036854775807]", d, "0,1,2,3,4,5", &m);
    expectAll("$[-99999999999999999999:99999999999999999999]", d, "0,1,2,3,4,5", &m);
    expectAll("$[9223372036854775807:-9223372036854775808:-1]", d, "5,4,3,2,1", &m);
    expectAll("$[18446744073709551617:]", d, "", &m);
    expectAll("$[:-18446744073709551617]", d, "", &m);
    expectAll("$[0,18446744073709551616,5,-18446744073709551616]", d, "0,5", &m);
    const char *huge[] = {"[18446744073709551617]", "[-18446744073709551615]", "[9223372036854775807]", "[-9223372036854775808]",
                          "[99999999999999999999]", "[4294967296]", "[-4294967297]", "[6]", "[-7]"};
    for (size_t i = 0; i < sizeof(huge) / sizeof(huge[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "$%s", huge[i]);
        for (int mode = JSON_PATH_READ; mode <= JSON_PATH_DELETE; mode++) {
            const char *got = listOf(path, d, (jsonPathMode)mode, &m);
            const char *want = mode == JSON_PATH_READ || mode == JSON_PATH_UPDATE ? "" : err_range;
            CHECK(strcmp(got, want) == 0 && jsonMatchesRawErr(&m) == JSON_PATH_ERR_INDEX_RANGE, "%s in mode %d: got [%s]", path, mode, got);
        }
        CHECK(strcmp(listOf(path + 1, d, JSON_PATH_READ, &m), err_range) == 0, "legacy %s", path + 1);
    }
    expectAll("$[1:6:0]", d, err_zero, &m);
    expectAll("$[1:6:-0]", d, err_zero, &m);
    expectAll("$[1:6:00000000000000000000000]", d, err_zero, &m);
    expectAll("$[0:3,1:2]", d, err_union, &m);

    /* Values selected more than once. */
    expectDup("$[1,1,1]", d, "1,1,1", "1", &m);
    expectDup("$[1,-5,1,7,-1,5,-7]", d, "1,1,1,5,5", "1,5", &m);
    expectDup("$[5,0,5,0]", d, "5,0,5,0", "5,0", &m);
    expectDup("$[0,0][?(@>=0)]", d, "0,0", "0", &m);
    jsonDoc dn = parseText("{\"a\":1,\"b\":2,\"c\":[3,[3,4]],\"d\":{\"a\":5,\"c\":[6]}}");
    expectDup("$['a','a','b','a']", dn, "1,1,2,1", "1,2", &m);
    expectDup("$[\"b\",'b']", dn, "2,2", "2", &m);
    expectDup("$['d','d'].a", dn, "5,5", "5", &m);
    expectDup("$['d','d']['a','a']", dn, "5,5,5,5", "5", &m);
    expectDup("$..a", dn, "1,5", "1,5", &m);
    expectDup("$..[0]", dn, "3,3,6", "3,3,6", &m); /* Three different values. */
    expectDup("$..c..[0]", dn, "3,3,6", "3,3,6", &m);
    expectDup("$..*..a", dn, "5", "5", &m);
    expectDup("$..[?(@.a)].a", dn, "1,5", "1,5", &m);
    expectDup("$..d..a", dn, "5", "5", &m);
    expectDup("$..['a','a']", dn, "1,5", "1,5", &m); /* After a descent the module drops duplicates in reads too. */
    for (int mode = JSON_PATH_UPDATE; mode <= JSON_PATH_DELETE; mode++) {
        static const char *nested[] = {"$..*", "$..*..*", "$..[?(@.a)]", "$..c[0,0,-2]", "$..['c','c'][1,1]..[0,0]", "$[*,*]"};
        for (size_t i = 0; i < sizeof(nested) / sizeof(nested[0]); i++) {
            evalPath(nested[i], strlen(nested[i]), dn, (jsonPathMode)mode, &m, NULL);
            CHECK(!hasDuplicates(&m), "%s in mode %d returned a value twice", nested[i], mode);
        }
    }
    /* A new member under one object named twice is one insert. */
    jsonPathErr rc = evalPath("$['d','d'].zz", 13, dn, JSON_PATH_INSERT, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 0 && m.insert_count == 1, "one insert: %u", m.insert_count);
    jsonDoc df = parseText("[{\"c\":[1]},{\"c\":[2,1]}]");
    expectAll("$[?(@.c[18446744073709551616]==1)].c[0]", df, "", &m);
    expectAll("$[?(@.c[-18446744073709551615]==1)].c[0]", df, "", &m);
    expectAll("$[?(@.c[-1]==1)].c[0]", df, "1,2", &m);

    /* Every start, end and step from a list that covers the edges: never a
     * value twice, never more matches than elements, the order of a slice,
     * and for an ordinary step exactly the elements a slice with it has. */
    static const char *steps[] = {"1", "2", "3", "5", "6", "7", "2147483646", "2147483647", "2147483648", "2147483649", "4294967294",
                                  "4294967295", "4294967296", "4294967297", "4294967298", "4294967303", "8589934592", "8589934594",
                                  "9223372036854775806", "9223372036854775807", "9223372036854775808", "9223372036854775809",
                                  "18446744073709551615", "18446744073709551616", "18446744073709551617", "340282366920938463463374607431768211457"};
    static const long long ordinary[] = {1, 2, 3, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 1, 2, 7, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    long long combos = 0;
    for (size_t si = 0; si < sizeof(steps) / sizeof(steps[0]); si++) {
        for (int neg = 0; neg < 2; neg++) {
            for (int start = -8; start <= 8; start++) {
                for (int end = -8; end <= 8; end++) {
                    char path[128], want[64] = "";
                    snprintf(path, sizeof(path), "$[%d:%d:%s%s]", start, end, neg ? "-" : "", steps[si]);
                    const char *got = listOf(path, d, (jsonPathMode)(combos % 4), &m);
                    combos++;
                    /* What a slice is, with the step at its ordinary size or else past the array. */
                    long long s = start < 0 ? start + 6 : start, e = end < 0 ? end + 6 : end, st = ordinary[si] ? ordinary[si] : 100;
                    size_t wn = 0;
                    s = s < 0 ? 0 : s > 6 ? 6
                                          : s;
                    e = e < 0 ? 0 : e > 6 ? 6
                                          : e;
                    for (long long i = s; neg ? i > e : i < e; i += neg ? -st : st) {
                        if (i < 6) wn += (size_t)snprintf(want + wn, sizeof(want) - wn, "%s%lld", wn ? "," : "", i);
                    }
                    if (strcmp(got, want) != 0 || m.count > 6 || hasDuplicates(&m)) {
                        failures++;
                        if (failures < 20) fprintf(stderr, "FAIL %s: want [%s], got [%s]\n", path, want, got);
                    }
                }
            }
        }
    }
    printf("unit: %lld slices over every start, end and edge step\n", combos);
    jsonDocFree(df);
    jsonDocFree(dn);

    /* Work is bounded by the array, not by the step: a million slices of a
     * large array with steps the module would loop on. */
    enum {
        BIG = 100000
    };
    char *big = malloc(BIG * 8);
    size_t len = 0;
    for (int i = 0; i < BIG; i++) len += (size_t)sprintf(big + len, "%s%d", i ? "," : "[", i);
    big[len++] = ']';
    jsonDoc db = parseDoc(big, len);
    free(big);
    static const char *slow[] = {"$[7::4294967296]", "$[7::2147483648]", "$[7::4294967295]", "$[7::9223372036854775807]",
                                 "$[7:0:-4294967296]", "$[7:0:-4294967295]", "$[99999::2147483647]", "$[7::2147483649]"};
    double t0 = now();
    long long total = 0;
    for (int i = 0; i < 1000000; i++) {
        jsonPathErr e2;
        const char *sp = slow[i % 8];
        jsonPath *p = jsonPathCompile(sp, strlen(sp), &e2);
        jsonPathEval(p, db, (jsonPathMode)(i % 4), &m);
        total += m.count;
        jsonPathFree(p);
    }
    double per = (now() - t0) / 1e6;
    CHECK(total == 1000000, "each of these slices is one element: %lld", total);
    CHECK(per < 5000, "a slice with a huge step took %.0f ns", per);
    CHECK(strcmp(listOf("$[99990::4294967298]", db, JSON_PATH_DELETE, &m), "99990,99992,99994,99996,99998") == 0, "step 2^32+2 on a large array");
    jsonDocFree(db);
    jsonDocFree(d);
    jsonMatchesFree(&m);
}

/* ----------------------------------------------------------------------------
 * Bounds on the time and memory of one evaluation
 * ------------------------------------------------------------------------- */

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SLOW_BUILD 1
#endif
#endif
#ifdef __SANITIZE_ADDRESS__
#define SLOW_BUILD 1
#endif
#ifdef SLOW_BUILD
#define TIME_SCALE 12.0
#else
#define TIME_SCALE 1.0
#endif

static char *repeatText(const char *head, const char *unit, int times, const char *tail) {
    size_t hl = strlen(head), ul = strlen(unit), tl = strlen(tail);
    char *s = malloc(hl + ul * (size_t)times + tl + 1), *w = s;
    memcpy(w, head, hl);
    w += hl;
    for (int i = 0; i < times; i++, w += ul) memcpy(w, unit, ul);
    memcpy(w, tail, tl + 1);
    return s;
}

static jsonDoc nested(const char *open, const char *leaf, const char *close, int depth) {
    char *left = repeatText("", open, depth, leaf), *right = repeatText("", close, depth, "");
    char *text = repeatText(left, right, 1, "");
    jsonDoc d = parseDoc(text, strlen(text));
    free(left);
    free(right);
    free(text);
    return d;
}

typedef struct cost {
    double ms;
    long long bytes;
    jsonPathErr rc;
    uint32_t count;
} cost;

/* Compiles and evaluates, and says what it cost. */
static cost costOf(const char *path, jsonDoc d, jsonPathMode mode, jsonMatches *m) {
    cost c;
    jsonPathErr err;
    long long b0 = live_bytes;
    peak_bytes = live_bytes;
    double t0 = now();
    jsonPath *p = jsonPathCompile(path, strlen(path), &err);
    c.rc = p ? jsonPathEval(p, d, mode, m) : err;
    c.ms = (now() - t0) / 1e6;
    c.bytes = peak_bytes - b0; /* The most the evaluation held at one time, its result included. */
    c.count = m->count;
    jsonPathFree(p);
    return c;
}

/* One of the reproductions from the review: it has to finish, one way or the other, well inside 100 ms. */
static cost repro(const char *what, const char *head, const char *unit, int times, jsonDoc d, jsonPathMode mode, jsonMatches *m) {
    char *path = repeatText(head, unit, times, "");
    cost c = costOf(path, d, mode, m);
    CHECK(c.ms < 100 * TIME_SCALE, "%s: %s%s x %d in mode %d took %.1f ms", what, head, unit, times, mode, c.ms);
    printf("unit: %-34s %s%s x %-4d mode %d: %8.3f ms, %7lld KB, %s, %u matches\n", what, head, unit, times, mode, c.ms,
           c.bytes / 1024, c.rc == JSON_PATH_OK ? "ok" : jsonPathErrMsg(c.rc), c.count);
    free(path);
    return c;
}

static void limitTests(void) {
    jsonMatches m;
    cost c;
    jsonMatchesInit(&m);

    /* 1. Unions that name one member twice, chained. A read is refused once
     * the repeats outgrow the document. A write sees each value once and is
     * as cheap as the path is long. Small cases still return every repeat. */
    jsonDoc o30 = nested("{\"a\":", "1", "}", 30), a30 = nested("[", "1", "]", 30), o60 = nested("{\"a\":", "1", "}", 60);
    c = repro("names twice, read", "$", "['a','a']", 22, o30, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS, "22 repeated unions read: %d", c.rc);
    c = repro("names twice, read, small", "$", "['a','a']", 10, o30, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_OK && c.count == 1024, "10 repeated unions read: %d, %u", c.rc, c.count);
    c = repro("names twice, read, small", "$", "['a','a']", 15, o30, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_OK && c.count == 32768, "15 repeated unions read: %d, %u", c.rc, c.count);
    for (int mode = JSON_PATH_UPDATE; mode <= JSON_PATH_DELETE; mode++) {
        c = repro("names twice, write", "$", "['a','a']", 26, o30, (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 1 && m.items[0].depth == 26, "26 repeated unions in mode %d: %d, %u", mode, c.rc, c.count);
        c = repro("names twice, 60 deep, write", "$", "[\"a\",'a','a']", 60, o60, (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 1 && m.items[0].depth == 60, "60 repeated unions in mode %d: %d, %u", mode, c.rc, c.count);
        c = repro("indexes twice, write", "$", "[0,0]", 24, a30, (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 1 && m.items[0].depth == 24, "24 repeated indexes in mode %d: %d, %u", mode, c.rc, c.count);
        c = repro("40 indexes, write", "$", "[0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]", 28, a30,
                  (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 1 && m.items[0].depth == 28, "40-entry unions in mode %d: %d, %u", mode, c.rc, c.count);
    }
    c = repro("names twice, 60 deep, read", "$", "['a','a']", 60, o60, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS, "60 repeated unions read: %d", c.rc);
    c = repro("indexes twice, read", "$", "[0,0]", 24, a30, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS, "24 repeated indexes read: %d", c.rc);
    c = repro("40 indexes, read", "$", "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]", 12, a30,
              JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS, "40-entry unions read: %d", c.rc);
    c = repro("repeats then a descent, read", "$", "[0,0]", 20, a30, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS, "repeats: %d", c.rc);
    {
        char *path = repeatText("$", "[0,0]", 18, "..[0]");
        c = costOf(path, a30, JSON_PATH_READ, &m);
        /* The module's own limit on descent tokens stops this one first, as it does in the module. */
        CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_DESCENTS && c.ms < 100 * TIME_SCALE, "repeats before a descent: %d in %.1f ms", c.rc, c.ms);
        free(path);
        path = repeatText("$", "[0,0]", 18, "[?(@==1 || @>0)]");
        c = costOf(path, a30, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS && c.ms < 100 * TIME_SCALE, "repeats before a filter: %d in %.1f ms", c.rc, c.ms);
        free(path);
    }
    /* One long union of one index is as many matches as it has entries, as in the module. */
    {
        char *path = repeatText("$[0", ",0", 59999, "]");
        jsonDoc one = parseText("[7]");
        c = costOf(path, one, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 60000, "one union of 60,000 entries: %d, %u", c.rc, c.count);
        c = costOf(path, one, JSON_PATH_DELETE, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 1, "the same in a write: %d, %u", c.rc, c.count);
        jsonDocFree(one);
        free(path);
    }
    /* The allowance follows the document: twice a large array is a result of
     * the document's order of size and is returned, twelve times is not. */
    {
        enum {
            WIDE = 100000
        };
        char *text = malloc(WIDE * 8 + 16);
        size_t len = 0;
        len += (size_t)sprintf(text, "[[");
        for (int i = 0; i < WIDE; i++) len += (size_t)sprintf(text + len, "%s%d", i ? "," : "", i);
        len += (size_t)sprintf(text + len, "]]");
        jsonDoc wide = parseDoc(text, len);
        free(text);
        c = costOf("$[0,0][*]", wide, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 2 * WIDE, "an array twice: %d, %u", c.rc, c.count);
        c = costOf("$[0,0,0,0,0,0,0,0][*]", wide, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 8 * WIDE, "an array eight times: %d, %u", c.rc, c.count);
        c = costOf("$[0,0,0,0,0,0,0,0,0,0,0,0][*]", wide, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS, "an array twelve times: %d, %u", c.rc, c.count);
        c = costOf("$[0,0,0,0,0,0,0,0,0,0,0,0][*]", wide, JSON_PATH_UPDATE, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == WIDE, "an array twelve times in a write: %d, %u", c.rc, c.count);
        c = costOf("$[0,0,0][?(@>=0)]", wide, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 3 * WIDE, "a filter three times: %d, %u", c.rc, c.count);
        c = costOf("$[0,0,0,0,0,0,0,0,0,0,0,0][?(@>=0)]", wide, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS && c.ms < 100 * TIME_SCALE, "a filter twelve times: %d in %.1f ms", c.rc, c.ms);

        /* The filter code. An operand that is itself a path over the whole
         * document is evaluated once, not once per value filtered... */
        char *big = malloc(40 * 20000 + 64);
        len = (size_t)sprintf(big, "{\"b\":7,\"v\":[");
        for (int i = 0; i < 20000; i++) len += (size_t)sprintf(big + len, "%s{\"a\":%d,\"c\":[%d]}", i ? "," : "", i % 10, i);
        len += (size_t)sprintf(big + len, "]}");
        jsonDoc objs = parseDoc(big, len);
        free(big);
        c = costOf("$..[?(@.a==$..b)].a", objs, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 2000 && c.ms < 100 * TIME_SCALE, "path operand under a descent: %d, %u, %.1f ms", c.rc,
              c.count, c.ms);
        printf("unit: a path operand under a descent over 60,000 values: %.3f ms\n", c.ms);
        c = costOf("$.v[?(@.a==$..b && @.c[0]>$.v[19999].a || @.a<$.b)].a", objs, JSON_PATH_DELETE, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 15999 && c.ms < 100 * TIME_SCALE, "three path operands: %d, %u, %.1f ms", c.rc, c.count, c.ms);
        jsonDocFree(objs);
        /* ... and a long chain of || and && holds scratch memory for its nesting, not for its length. */
        char *chain = repeatText("$[0][?(@>=0", " || @<0 && @>5", 150, ")]");
        c = costOf(chain, wide, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == WIDE, "a long filter: %d, %u", c.rc, c.count);
        /* The result is 100,000 matches, about 17 MB with the doubling of its array. One list per comparison would be 120 MB more. */
        CHECK(c.bytes < 40 * 1024 * 1024, "a filter of 301 comparisons over 100,000 values allocated %lld KB", c.bytes / 1024);
        printf("unit: a filter of 301 comparisons over 100,000 values: %.1f ms, %lld KB\n", c.ms, c.bytes / 1024);
        free(chain);
        jsonDocFree(wide);
    }

    /* 2. Depth. A recursive descent stops 4,096 levels below where it starts,
     * with the recursion limit error. Up to there, nested matches share their
     * locators, so the result is linear in the depth. A plain chain of steps
     * has no depth limit, as in the module. */
    jsonDoc a20k = nested("[", "1", "]", 20000), a4k = nested("[", "1", "]", 4000), o20k = nested("{\"a\":", "1", "}", 20000);
    for (int mode = JSON_PATH_READ; mode <= JSON_PATH_DELETE; mode++) {
        c = repro("20,000 deep", "$..*", "", 0, a20k, (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_DEEP && c.bytes < 4 * 1024 * 1024, "descent into 20,000 arrays: %d, %lld KB", c.rc, c.bytes / 1024);
        c = repro("20,000 deep", "$..a", "", 0, o20k, (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_DEEP && c.bytes < 4 * 1024 * 1024, "descent into 20,000 objects: %d, %lld KB", c.rc, c.bytes / 1024);
        c = repro("4,000 deep", "$..*", "", 0, a4k, (jsonPathMode)mode, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 4000 && m.items[3999].depth == 4000 && c.bytes < 4 * 1024 * 1024,
              "descent into 4,000 arrays: %d, %u matches, %lld KB", c.rc, c.count, c.bytes / 1024);
        for (uint32_t i = 0; i < m.count; i += 97) {
            jsonRef r;
            CHECK(m.items[i].depth == i + 1 && jsonDocResolve(a4k, m.items[i].loc, (int)m.items[i].depth, &r) &&
                      jsonRefEquals(&r, &m.items[i].ref),
                  "shared locator of match %u", i);
        }
    }
    c = repro("20,000 deep, legacy", "..[0]", "", 0, a20k, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_ERR_TOO_DEEP, "legacy descent: %d", c.rc);
    c = repro("20,000 deep, plain chain", "$", "[0]", 20000, a20k, JSON_PATH_READ, &m);
    CHECK(c.rc == JSON_PATH_OK && c.count == 1 && m.items[0].depth == 20000, "20,000 plain steps: %d, %u", c.rc, c.count);
    c = repro("descent from 16,000 down", "$", "[0]", 16000, a20k, JSON_PATH_READ, &m);
    {
        char *path = repeatText("$", "[0]", 16000, "..*");
        c = costOf(path, a20k, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 4000 && m.items[3999].depth == 20000, "descent over the last 4,000 levels: %d, %u", c.rc, c.count);
        free(path);
        path = repeatText("$", "[0]", 15000, "..*");
        c = costOf(path, a20k, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_DEEP, "descent over the last 5,000 levels: %d", c.rc);
        free(path);
        /* Exactly at the limit: 4,096 levels below the start is the last that is reached. */
        jsonDoc edge = nested("[", "1", "]", 4097), over = nested("[", "1", "]", 4098);
        c = costOf("$..*", edge, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_OK && c.count == 4097, "descent of 4,096 levels: %d, %u", c.rc, c.count);
        c = costOf("$..*", over, JSON_PATH_READ, &m);
        CHECK(c.rc == JSON_PATH_ERR_TOO_DEEP, "descent of 4,097 levels: %d", c.rc);
        jsonDocFree(edge);
        jsonDocFree(over);
    }
    jsonDocFree(a20k);
    jsonDocFree(a4k);
    jsonDocFree(o20k);
    jsonDocFree(o30);
    jsonDocFree(o60);
    jsonDocFree(a30);

    /* 3. Adversarial paths against documents 1 to 2,000 levels deep: chains
     * of unions with repeats, wildcards, descents, slices and filters, alone
     * and mixed. Each evaluation gets a budget of time and of allocation that
     * depends only on the size of the document and of the result. */
    static const int depths[] = {1, 2, 5, 30, 200, 1000, 2000};
    static const char *units[] = {"[0,0]", "['a','a']", "[0,0,0]", "[0,-1]", "[*]", ".*", "..a", "..[0]", "..*", "[0:1]", "[::1]",
                                  "[0:2][-1,0]", "[?(@.a)]", "[?(@==1||@.a)]", "[*][?(@.a||@==1)]", ".a", "[0]", "[2]",
                                  "[?((@.a||(@==1))&&(@.a||@>=1||@.b))]", "['a','b','a']", "[?(@.a==$..b)]", "[2,2,0,0]",
                                  "..[?(@.a)]", "..['a','a']", "..[0,0]", "[::-1]", "[1:0:-1][0,0]"};
    enum {
        NUNITS = sizeof(units) / sizeof(units[0])
    };
    static const int repeats[] = {1, 2, 3, 10, 30, 100, 300, 1000};
    long long evals = 0, refused = 0, worst_bytes = 0;
    double worst_ms = 0;
    const char *worst_path = "";
    static char worst[96];
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (size_t di = 0; di < sizeof(depths) / sizeof(depths[0]); di++) {
        const int depth = depths[di];
        jsonDoc docs[5];
        docs[0] = nested("[", "1", "]", depth);
        docs[1] = nested("{\"a\":", "1", "}", depth);
        docs[2] = nested("{\"a\":[", "1", "],\"b\":2}", (depth + 1) / 2);
        docs[3] = nested("[1,{\"a\":1},", "1", "]", depth);
        docs[4] = nested("[[],[", "{\"a\":[1,1],\"b\":1}", "],1]", depth);
        for (int k = 0; k < 5; k++) {
            jsonRef root;
            jsonDocRoot(docs[k], &root);
            const double values = (double)jsonRefFields(&root);
            for (int trial = 0; trial < NUNITS * 8 + 120; trial++) {
                char *path;
                if (trial < NUNITS * 8) {
                    path = repeatText("$", units[trial / 8], repeats[trial % 8], "");
                } else {
                    /* A random mix of the units. */
                    size_t len = 1, cap = 4096;
                    path = malloc(cap);
                    path[0] = '$';
                    seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
                    int parts = 2 + (int)(seed % 40);
                    for (uint64_t r = seed; parts-- && len + 64 < cap; r = r * 6364136223846793005ULL + 1442695040888963407ULL) {
                        const char *u = units[(r >> 33) % NUNITS];
                        int times = 1 + (int)((r >> 20) % 6);
                        while (times-- && len + 64 < cap) {
                            memcpy(path + len, u, strlen(u));
                            len += strlen(u);
                        }
                    }
                    path[len] = '\0';
                }
                for (int mode = JSON_PATH_READ; mode <= JSON_PATH_DELETE; mode++) {
                    c = costOf(path, docs[k], (jsonPathMode)mode, &m);
                    evals++;
                    refused += c.rc == JSON_PATH_ERR_TOO_MANY_REPEATS || c.rc == JSON_PATH_ERR_TOO_DEEP;
                    /* Time: a fixed part, and a part for results of the document's
                     * size, each with a locator as long as the document is deep.
                     * Nested recursive descents rebuild such a result up to 20
                     * times before the module's token limit ends them. */
                    const double ms_budget = (40 + values * (1 + depth) * 60e-6) * TIME_SCALE;
                    /* Memory held at one time: a fixed part (which covers the
                     * repeats a read may return), the frames and tables of a
                     * walk, and one result of the document's size, whose
                     * matches each have a locator as long as their depth. */
                    const double byte_budget = 12e6 + values * (2048 + 16.0 * depth) + (double)c.count * 64;
                    if (c.ms > ms_budget || (double)c.bytes > byte_budget) {
                        failures++;
                        if (failures < 20) {
                            fprintf(stderr, "FAIL over budget: depth %d, document %d, mode %d: %.1f ms (budget %.0f), %lld KB (budget %.0f), %.60s...\n",
                                    depth, k, mode, c.ms, ms_budget, c.bytes / 1024, byte_budget / 1024, path);
                        }
                    }
                    if (mode != JSON_PATH_READ && c.rc == JSON_PATH_OK && m.count < 200 && hasDuplicates(&m)) {
                        failures++;
                        fprintf(stderr, "FAIL a value twice in mode %d: %.80s\n", mode, path);
                    }
                    if (c.ms > worst_ms) {
                        worst_ms = c.ms;
                        snprintf(worst, sizeof(worst), "depth %d, document %d, %.50s", depth, k, path);
                        worst_path = worst;
                    }
                    if (c.bytes > worst_bytes) worst_bytes = c.bytes;
                }
                free(path);
            }
            jsonDocFree(docs[k]);
        }
    }
    printf("unit: %lld adversarial evaluations, %lld refused by a limit, slowest %.1f ms (%s), highest peak %lld KB\n", evals, refused,
           worst_ms, worst_path, worst_bytes / 1024);
    jsonMatchesFree(&m);
}

static int cmdUnit(void) {
    const char *text = "{\"a\":{\"b\":[10,20,{\"c\":\"x\"}]},\"k\":[[1,2],[3,4]],\"s\":5,\"e\":[]}";
    jsonDoc d = parseDoc(text, strlen(text));
    jsonMatches m;
    jsonPath *p;
    jsonPathErr rc;
    jsonMatchesInit(&m);

    /* The fast path allocates nothing and fills the inline storage. */
    rc = evalPath("$.a.b[2].c", 10, d, JSON_PATH_READ, &m, &p);
    long long before = total_allocs;
    for (int mode = JSON_PATH_READ; mode <= JSON_PATH_DELETE; mode++) {
        rc = jsonPathEval(p, d, (jsonPathMode)mode, &m);
        CHECK(rc == JSON_PATH_OK && m.count == 1 && m.items == m.inline_items && m.items[0].loc == m.inline_steps,
              "singular path in mode %d", mode);
        CHECK(m.items[0].depth == 4 && jsonMatchesMaxDepth(&m) == 4, "depth %u", m.items[0].depth);
    }
    CHECK(total_allocs == before, "the singular path allocated %lld times", total_allocs - before);
    jsonPathFree(p);

    /* A missing last member is an insert with a literal name (B5), and it does not allocate either. */
    rc = evalPath("$.a.b~1c", 8, d, JSON_PATH_INSERT, &m, &p);
    before = total_allocs;
    rc = jsonPathEval(p, d, JSON_PATH_INSERT, &m);
    CHECK(rc == JSON_PATH_OK && m.count == 0 && m.insert_count == 1, "insert: rc %d count %u inserts %u", rc, m.count,
          m.insert_count);
    CHECK(m.insert_count == 1 && m.inserts[0].name_len == 4 && memcmp(m.inserts[0].name, "b~1c", 4) == 0 &&
              m.inserts[0].parent_depth == 1,
          "insert name is literal");
    CHECK(jsonMatchesMaxDepth(&m) == 2, "insert depth %u", jsonMatchesMaxDepth(&m));
    CHECK(total_allocs == before, "the singular insert allocated");
    jsonPathFree(p);

    /* The same path in the other modes finds nothing. */
    rc = evalPath("$.a.zz", 6, d, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 0, "missing member, read");
    rc = evalPath(".a.zz", 5, d, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_ERR_NOT_EXIST, "missing member, legacy read: %d", rc);
    rc = evalPath(".a.zz", 5, d, JSON_PATH_INSERT, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.insert_count == 1, "missing member, legacy insert: %d", rc);
    rc = evalPath("$.zz.yy", 7, d, JSON_PATH_INSERT, &m, NULL);
    CHECK(rc == JSON_PATH_ERR_NOT_EXIST && m.count == 0 && m.insert_count == 0, "missing parent: %d", rc);

    /* No document: nothing matches. */
    rc = evalPath("$.a", 3, 0, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 0, "no document");

    /* A `$` read folds an error that is not a syntax error and keeps the code. */
    rc = evalPath("$.a.b[9]", 8, d, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 0 && jsonMatchesRawErr(&m) == JSON_PATH_ERR_INDEX_RANGE, "folded error: %d", rc);
    rc = evalPath(".a.b[9]", 7, d, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_ERR_INDEX_RANGE, "legacy index error: %d", rc);
    rc = evalPath("$.a.b[9]", 8, d, JSON_PATH_DELETE, &m, NULL);
    CHECK(rc == JSON_PATH_ERR_INDEX_RANGE, "delete returns every error: %d", rc);
    rc = evalPath("$.s.x", 5, d, JSON_PATH_DELETE, &m, NULL);
    CHECK(rc == JSON_PATH_ERR_INSERT_ARRAY, "member of a scalar in a write: %d", rc);
    rc = evalPath("$.s.x", 5, d, JSON_PATH_UPDATE, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 0, "member of a scalar in an update: %d", rc);

    /* Reads keep duplicates, the write modes do not. */
    rc = evalPath("$.a.b[0,0,1,0]", 14, d, JSON_PATH_READ, &m, &p);
    CHECK(rc == JSON_PATH_OK && m.count == 4, "read keeps duplicates: %u", m.count);
    for (int mode = JSON_PATH_UPDATE; mode <= JSON_PATH_DELETE; mode++) {
        rc = jsonPathEval(p, d, (jsonPathMode)mode, &m);
        CHECK(rc == JSON_PATH_OK && m.count == 2 && m.items[0].loc[2].pos == 0 && m.items[1].loc[2].pos == 1,
              "mode %d drops duplicates: %u", mode, m.count);
    }
    jsonPathFree(p);

    /* A wildcard on a scalar is a syntax error for a legacy path, and not once the path is forced to `$`. */
    rc = evalPath(".s.*", 4, d, JSON_PATH_READ, &m, &p);
    CHECK(rc == JSON_PATH_ERR_SYNTAX, "legacy wildcard on a scalar: %d", rc);
    jsonPathForceV2(p);
    rc = jsonPathEval(p, d, JSON_PATH_READ, &m);
    CHECK(rc == JSON_PATH_OK && m.count == 0 && jsonMatchesIsV2(&m) && jsonMatchesRawErr(&m) == JSON_PATH_ERR_WILDCARD,
          "forced `$` wildcard on a scalar: %d", rc);
    jsonPathFree(p);

    /* A legacy path that meets "$" at the root becomes a `$` path. */
    rc = evalPath("..$.s", 5, d, JSON_PATH_READ, &m, &p);
    CHECK(rc == JSON_PATH_OK && m.count == 1 && !jsonPathIsV2(p) && jsonMatchesIsV2(&m), "legacy path turned `$`: %d", rc);
    jsonPathFree(p);

    /* Depth: a recursive descent reports the deepest value it walked over. */
    rc = evalPath("$..zz", 5, d, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 0 && jsonMatchesMaxDepth(&m) == 4, "descent depth %u", jsonMatchesMaxDepth(&m));

    /* Apply order: deeper first, and within an array the higher index first. */
    rc = evalPath("$..*", 4, d, JSON_PATH_DELETE, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 15, "all values: %u", m.count);
    uint32_t order[64];
    jsonMatchesApplyOrder(&m, order);
    int seen[64] = {0}, ok = 1;
    for (uint32_t i = 0; i < m.count; i++) {
        const jsonMatch *x = &m.items[order[i]];
        seen[order[i]]++;
        for (uint32_t j = i + 1; j < m.count; j++) {
            const jsonMatch *y = &m.items[order[j]];
            if (jsonMatchIsAncestor(x, y)) ok = 0; /* An ancestor applied before something inside it. */
            if (x->depth == y->depth && x->depth > 0 && !x->loc[x->depth - 1].keyid &&
                sameLoc(x->loc, x->depth - 1, y->loc, y->depth - 1) && x->loc[x->depth - 1].pos < y->loc[y->depth - 1].pos) {
                ok = 0; /* A lower index of the same array applied first. */
            }
        }
    }
    for (uint32_t i = 0; i < m.count; i++) ok &= seen[i] == 1;
    CHECK(ok, "apply order");
    /* In selector order the root's children come first: a, k, s, e, then a.b. */
    CHECK(m.items[4].depth == 2 && jsonMatchIsAncestor(&m.items[0], &m.items[4]) &&
              !jsonMatchIsAncestor(&m.items[4], &m.items[0]) && !jsonMatchIsAncestor(&m.items[1], &m.items[4]) &&
              !jsonMatchIsAncestor(&m.items[0], &m.items[0]),
          "ancestor");

    /* A name that no document uses matches nothing, and is found once a document has it. */
    rc = evalPath("$.later", 7, d, JSON_PATH_READ, &m, &p);
    CHECK(rc == JSON_PATH_OK && m.count == 0, "unknown name");
    jsonDoc d2 = parseDoc("{\"later\":1}", 11);
    rc = jsonPathEval(p, d2, JSON_PATH_READ, &m);
    CHECK(rc == JSON_PATH_OK && m.count == 1, "name interned after the path was compiled");
    jsonPathFree(p);
    jsonDocFree(d2);

    /* A result can be reused, with or without heap storage in it. */
    rc = evalPath("$..*", 4, d, JSON_PATH_READ, &m, NULL);
    rc = evalPath("$.s", 3, d, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 1 && m.items == m.inline_items, "result reuse");

    /* Depth of the document does not become depth of the C stack: a recursive
     * descent through 4,000 nested arrays. */
    enum {
        DEEP = 4000
    };
    char *deep = malloc(2 * DEEP + 2);
    memset(deep, '[', DEEP);
    deep[DEEP] = '7';
    memset(deep + DEEP + 1, ']', DEEP);
    d2 = parseDoc(deep, 2 * DEEP + 1);
    free(deep);
    rc = evalPath("$..[0]", 6, d2, JSON_PATH_READ, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == DEEP && m.items[DEEP - 1].depth == DEEP &&
              jsonRefType(&m.items[DEEP - 1].ref) == JSON_INT && jsonMatchesMaxDepth(&m) == DEEP,
          "deep descent: rc %d, %u matches", rc, m.count);
    rc = evalPath("$..[?(@==7)]", 12, d2, JSON_PATH_DELETE, &m, NULL);
    CHECK(rc == JSON_PATH_OK && m.count == 1 && m.items[0].depth == DEEP, "deep filter: rc %d, %u matches", rc, m.count);
    jsonDocFree(d2);

    /* Random paths: nothing may crash, loop or leak, whatever the text. */
    static const char alphabet[] = "$..[[]]()''\"\"**??@@::,,  --++0123456789abcx==<<>>!!&&||\\\\";
    static const char *fuzz_docs[] = {"{\"a\":[1,2,{\"b\":[3,\"x\",null,{\"a\":{\"c\":true}}],\"c\":\"x\"}],\"b\":{\"a\":1.5,\"x\":[]},\"c\":\"s\",\"x\":0}",
                                      "[[1,[2,3]],{\"a\":[{\"a\":1},{\"b\":2}]},\"a\",0,[],{}]", "3"};
    uint64_t seed = 88172645463325252ULL;
    long long evals = 0;
    for (size_t di = 0; di < 3; di++) {
        d2 = parseDoc(fuzz_docs[di], strlen(fuzz_docs[di]));
        for (int i = 0; i < 60000; i++) {
            char path[48];
            size_t n = 0, want;
            seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
            want = 1 + (size_t)(seed % 40);
            for (uint64_t r = seed; n < want; n++) {
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                path[n] = alphabet[(r >> 33) % (sizeof(alphabet) - 1)];
            }
            if (i % 3) path[0] = '$';
            jsonPath *fp = jsonPathCompile(path, n, &rc);
            for (int mode = JSON_PATH_READ; mode <= JSON_PATH_DELETE; mode++) {
                rc = jsonPathEval(fp, d2, (jsonPathMode)mode, &m);
                for (uint32_t k = 0; rc == JSON_PATH_OK && k < m.count; k++) {
                    jsonRef r;
                    if (!jsonDocResolve(d2, m.items[k].loc, (int)m.items[k].depth, &r)) failures++;
                }
                if (mode != JSON_PATH_READ && m.count < 64 && hasDuplicates(&m)) {
                    failures++;
                    fprintf(stderr, "FAIL %.*s returned a value twice in mode %d\n", (int)n, path, mode);
                }
                evals++;
            }
            jsonPathFree(fp);
        }
        jsonDocFree(d2);
    }
    printf("unit: %lld evaluations of random paths\n", evals);
    jsonMatchesFree(&m);
    jsonDocFree(d);
    cacheTests();
    stepTests();
    limitTests();
    d = 0;

    jsonMatchesFree(&m);
    jsonDocFree(d);
    jsonPathCacheClear();
    size_t keys_left = jsonEngineShutdown();
    CHECK(keys_left == 0, "%zu key references not released", keys_left);
    CHECK(live_allocs == 0, "%lld allocations leaked", live_allocs);
    printf("unit: %d failures\n", failures);
    return failures != 0;
}

/* ----------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void benchOne(const char *label, jsonDoc d, const char *path) {
    jsonMatches m;
    jsonPathErr err;
    size_t n = strlen(path);
    jsonMatchesInit(&m);
    jsonPath *p = jsonPathCompile(path, n, &err);
    jsonPathEval(p, d, JSON_PATH_READ, &m);
    uint32_t count = m.count;
    /* Size the loop for about a fifth of a second, then keep the best of five runs. */
    long iters = 1;
    for (;;) {
        double t0 = now();
        for (long i = 0; i < iters; i++) jsonPathEval(p, d, JSON_PATH_READ, &m);
        if (now() - t0 > 2e7 || iters > (1L << 28)) break;
        iters *= 4;
    }
    double best_eval = 1e30, best_compile = 1e30;
    for (int run = 0; run < 5; run++) {
        double t0 = now();
        for (long i = 0; i < iters; i++) jsonPathEval(p, d, JSON_PATH_READ, &m);
        double t = (now() - t0) / (double)iters;
        if (t < best_eval) best_eval = t;
    }
    long citers = 200000;
    for (int run = 0; run < 5; run++) {
        double t0 = now();
        for (long i = 0; i < citers; i++) jsonPathFree(jsonPathCompile(path, n, &err));
        double t = (now() - t0) / (double)citers;
        if (t < best_compile) best_compile = t;
    }
    printf("  %-8s %-28s matches %6u  eval %10.1f ns  compile+free %7.1f ns\n", label, path, count, best_eval, best_compile);
    jsonPathFree(p);
    jsonMatchesFree(&m);
}

static int cmdBench(void) {
    const char *small = "{\"name\":\"Ada\",\"age\":36,\"address\":{\"city\":\"London\",\"geo\":{\"lat\":51.5,\"lon\":-0.1,"
                        "\"tags\":[\"a\",\"b\",\"c\"]}},\"items\":[{\"name\":\"x\",\"price\":5},{\"name\":\"y\",\"price\":15},"
                        "{\"name\":\"z\",\"price\":25}]}";
    jsonDoc ds = parseDoc(small, strlen(small));
    /* Large: 10,000 records of six members, one of them a nested object. */
    size_t cap = 2 << 20, len = 0;
    char *large = malloc(cap);
    len += (size_t)snprintf(large + len, cap - len, "{\"meta\":{\"owner\":{\"id\":{\"n\":7}}},\"items\":[");
    for (int i = 0; i < 10000; i++) {
        len += (size_t)snprintf(large + len, cap - len,
                                "%s{\"id\":%d,\"name\":\"item%d\",\"price\":%d.5,\"tags\":[\"t%d\",\"u\"],\"dim\":{\"w\":%d,\"name\":\"d\"},\"ok\":%s}",
                                i ? "," : "", i, i, i % 100, i % 7, i % 13, i % 2 ? "true" : "false");
    }
    len += (size_t)snprintf(large + len, cap - len, "]}");
    jsonDoc dl = parseDoc(large, len);
    printf("small document: %zu bytes of JSON. large document: %zu bytes of JSON, 10,000 records.\n", strlen(small), len);
    benchOne("small", ds, "$.name");
    benchOne("small", ds, "$.address.city");
    benchOne("small", ds, "$.address.geo.lat");
    benchOne("small", ds, "$.address.geo.tags[1]");
    benchOne("small", ds, ".address.geo.tags[1]");
    benchOne("small", ds, "$..name");
    benchOne("small", ds, "$.items[*].name");
    benchOne("small", ds, "$.items[?(@.price>10)]");
    benchOne("small", ds, "$.items[?(@.price>10)].name");
    benchOne("large", dl, "$.meta");
    benchOne("large", dl, "$.meta.owner");
    benchOne("large", dl, "$.meta.owner.id");
    benchOne("large", dl, "$.meta.owner.id.n");
    benchOne("large", dl, "$.items[5000].name");
    benchOne("large", dl, "$.items[9999].dim.w");
    benchOne("large", dl, "$..name");
    benchOne("large", dl, "$.items[*].name");
    benchOne("large", dl, "$.items[?(@.price>50)]");
    benchOne("large", dl, "$.items[?(@.price>50)].name");
    free(large);
    jsonDocFree(ds);
    jsonDocFree(dl);
    return 0;
}


/* Compile, evaluate and free, which is what a command does with its path.
 * "hot" repeats one string. "cold" cycles through k different strings, each
 * against a small document that has the member it names. */
static void useOne(const char *fmt, int k, jsonDoc base, jsonDoc *docs) {
    enum {
        MAXK = 1000
    };
    static char text[MAXK][48];
    static size_t lens[MAXK];
    jsonMatches m;
    jsonPathErr err;
    char label[64];
    jsonMatchesInit(&m);
    for (int i = 0; i < k; i++) {
        char suffix[16] = "";
        if (k > 1) snprintf(suffix, sizeof(suffix), "%d", i);
        lens[i] = (size_t)snprintf(text[i], sizeof(text[i]), fmt, suffix);
    }
    const long iters = k > 1 ? 1000000 : 3000000;
    double best = 1e30;
    long long allocs = 0, matched = 0;
    for (int run = 0; run < 6; run++) { /* The first run warms up. */
        long long a0 = total_allocs;
        double t0 = now();
        int j = 0;
        for (long i = 0; i < iters; i++) {
            jsonPath *p = jsonPathCompile(text[j], lens[j], &err);
            jsonPathEval(p, k > 1 ? docs[j] : base, JSON_PATH_READ, &m);
            matched += m.count;
            jsonPathFree(p);
            if (++j == k) j = 0;
        }
        double t = (now() - t0) / (double)iters;
        if (run && t < best) best = t;
        allocs = total_allocs - a0;
    }
    snprintf(label, sizeof(label), fmt, k > 1 ? "<i>" : "");
    printf("  %-26s %-10s %7.1f ns  %5.2f allocations per call%s\n", label, k == 1 ? "hot" : k == 64 ? "cold 64"
                                                                                                     : "cold 1000",
           best,
           (double)allocs / (double)iters, matched == 6 * iters ? "" : "  (WRONG MATCH COUNT)");
    jsonMatchesFree(&m);
}

static int cmdUse(void) {
    enum {
        MAXK = 1000
    };
    static jsonDoc docs[MAXK];
    static const char *fmts[] = {"$.a%s", "$.address.city%s", ".address.city%s", "$.prefs.notify.push%s", "$.items[1].price%s"};
    static const char *doc_fmt = "{\"a%s\":1,\"name\":\"Ada\",\"address\":{\"city%s\":\"London\",\"zip\":\"N1\"},"
                                 "\"prefs\":{\"notify\":{\"push%s\":true,\"mail\":false}},"
                                 "\"items\":[{\"price%s\":5,\"qty\":1},{\"price%s\":15,\"qty\":2}]}";
    char text[512];
    int n = snprintf(text, sizeof(text), doc_fmt, "", "", "", "", "");
    jsonDoc base = parseDoc(text, (size_t)n);
    printf("compile + evaluate + free, small documents of %d bytes of JSON\n", n);
    for (int i = 0; i < MAXK; i++) {
        char sfx[16];
        snprintf(sfx, sizeof(sfx), "%d", i);
        n = snprintf(text, sizeof(text), doc_fmt, sfx, sfx, sfx, sfx, sfx);
        docs[i] = parseDoc(text, (size_t)n);
    }
    for (size_t f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
        useOne(fmts[f], 1, base, docs);
        useOne(fmts[f], 64, base, docs);
        useOne(fmts[f], 1000, base, docs);
    }
    return 0;
}

#endif /* JSON_PATH_NO_ENGINE */

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "";
#ifndef JSON_PATH_NO_ENGINE
    jsonEngineInit(&test_allocator);
#endif
    if (!strcmp(cmd, "dump")) return cmdDump();
    if (!strcmp(cmd, "compile") && argc > 3) return cmdCompile(argv[2], argv[3]);
#ifndef JSON_PATH_NO_ENGINE
    if (!strcmp(cmd, "golden")) return cmdGolden();
    if (!strcmp(cmd, "unit")) return cmdUnit();
    if (!strcmp(cmd, "bench")) return cmdBench();
    if (!strcmp(cmd, "use")) return cmdUse();
#endif
    fprintf(stderr, "usage: path_test compile CASES VERDICTS | dump | golden | unit | bench | use\n");
    return 2;
}
