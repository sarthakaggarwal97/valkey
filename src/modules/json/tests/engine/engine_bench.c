/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* engine_bench.c -- timings for json_engine.c: parse, serialise, free and
 * lookup on the session, profile and order record shapes, and every kind of
 * change on those shapes, on a 3.8 KB document and on two spine documents.
 *
 * usage: engine_bench <dir> [docs|read|write]
 *        with the files that `gen_docs.py corpus <dir> 100000` writes
 *
 * The method is the measurement prototype's, so that the numbers can be put
 * next to json-m0/proto/RESULTS.md: documents are handled in batches of 1000
 * (parse the batch, serialise it, look paths up in it, free it), a run
 * covers the whole file, and each number is the median of 9 timed runs taken
 * in 3 rounds of one warm-up run and 3 timed runs. Lookups use member names
 * already resolved to key IDs, as a compiled path holds them. The allocator
 * is the system one. The "prototype" column is copied from RESULTS.md
 * (K1-L0, key cache on, -O3 -mcpu=native, Apple M3 Pro): it is a reference
 * from another day, not a measurement of this run. */

#include "../../json_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <malloc/malloc.h>
static size_t usableSize(void *p) {
    return malloc_size(p);
}
#else
#include <malloc.h>
static size_t usableSize(void *p) {
    return malloc_usable_size(p);
}
#endif

#define ROUNDS 3
#define RUNS_PER_ROUND 3
#define RUNS (ROUNDS * RUNS_PER_ROUND)
#define BATCH 1000
#define LOOKUP_REPEAT 4
#define MAX_PATHS 6
#define MAX_STEPS 4

typedef struct corpus {
    char *buf;
    size_t *off;
    size_t n, bytes;
} corpus;

typedef struct path {
    const char *text;
    double prototype_ns;
} path;

typedef struct shape {
    const char *name;
    double proto_parse, proto_serialize, proto_free;
    int npaths;
    path paths[MAX_PATHS];
} shape;

static const shape SHAPES[] = {
    {"session", 376, 117, 63, 4, {{"sid", 4.9}, {"last_path", 14.4}, {"flags.mfa", 14.2}, {"flags.beta", 16.8}}},
    {"profile",
     709,
     306,
     115,
     5,
     {{"id", 5.2}, {"bio", 19.7}, {"address.street", 17.2}, {"address.geo.lat", 23.3}, {"prefs.notify.push", 27.1}}},
    {"order",
     1138,
     549,
     165,
     6,
     {{"order_id", 6.7},
      {"notes", 17.9},
      {"totals.subtotal", 17.0},
      {"ship_to.zip", 22.1},
      {"items.0.sku", 16.3},
      {"items.2.price", 22.6}}},
};

static volatile uint64_t g_sink;

static uint64_t nowNs(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); /* CLOCK_MONOTONIC ticks in microseconds there. */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static corpus load(const char *dir, const char *name) {
    char file[1024];
    corpus c = {NULL, NULL, 0, 0};
    snprintf(file, sizeof(file), "%s/%s.jsonl", dir, name);
    FILE *f = fopen(file, "rb");
    if (!f) {
        perror(file);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    const size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    c.buf = malloc(size + 1);
    if (fread(c.buf, 1, size, f) != size) {
        perror("read");
        exit(2);
    }
    fclose(f);
    size_t cap = 1024, start = 0;
    c.off = malloc(sizeof(size_t) * cap);
    for (size_t i = 0; i < size; i++) {
        if (c.buf[i] != '\n') continue;
        if (c.n + 2 > cap) {
            cap *= 2;
            c.off = realloc(c.off, sizeof(size_t) * cap);
        }
        c.off[c.n++] = start;
        c.bytes += i - start;
        start = i + 1;
    }
    c.off[c.n] = start;
    return c;
}

static int cmpDouble(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n) {
    qsort(v, (size_t)n, sizeof(double), cmpDouble);
    return v[n / 2];
}

/* "a.2.b" to steps. Names are resolved to key IDs, which takes a reference. */
static int resolvePath(const char *text, jsonStep *steps) {
    int n = 0;
    while (*text) {
        const char *end = strchr(text, '.');
        const size_t len = end ? (size_t)(end - text) : strlen(text);
        if (*text >= '0' && *text <= '9') {
            steps[n].pos = (uint32_t)atoi(text);
            steps[n].keyid = 0;
        } else {
            steps[n].pos = 0;
            steps[n].keyid = jsonKeyFind(text, len);
            if (!steps[n].keyid) {
                fprintf(stderr, "path %s: no such key\n", text);
                exit(2);
            }
        }
        n++;
        text += len + (end != NULL);
    }
    return n;
}

typedef struct sample {
    double parse, serialize, free_, lookup[MAX_PATHS];
} sample;

static void runOnce(const shape *sh, const corpus *c, sample *out) {
    static jsonDoc docs[BATCH];
    uint64_t t_parse = 0, t_ser = 0, t_free = 0, t_lookup[MAX_PATHS] = {0}, sink = 0;
    jsonBuf buf;
    jsonBufInit(&buf, NULL, 0);
    for (size_t b0 = 0; b0 < c->n; b0 += BATCH) {
        const size_t nb = c->n - b0 < BATCH ? c->n - b0 : BATCH;
        jsonErr err;
        jsonRef ref;
        uint64_t t0 = nowNs();
        for (size_t i = 0; i < nb; i++) {
            const char *text = c->buf + c->off[b0 + i];
            const size_t len = c->off[b0 + i + 1] - c->off[b0 + i] - 1;
            jsonVal *v = jsonValParse(text, len, 0, 0, &err, NULL);
            if (!v) {
                fprintf(stderr, "%s: document %zu does not parse\n", sh->name, b0 + i);
                exit(2);
            }
            docs[i] = jsonValToDoc(v);
        }
        uint64_t t1 = nowNs();
        for (size_t i = 0; i < nb; i++) {
            buf.len = 0;
            jsonDocRoot(docs[i], &ref);
            jsonSerialize(&ref, NULL, &buf);
            sink += buf.len;
        }
        uint64_t t2 = nowNs();
        t_parse += t1 - t0;
        t_ser += t2 - t1;
        for (int p = 0; p < sh->npaths; p++) {
            jsonStep steps[MAX_STEPS];
            const int nsteps = resolvePath(sh->paths[p].text, steps);
            size_t found = 0;
            t0 = nowNs();
            for (int k = 0; k < LOOKUP_REPEAT; k++) {
                for (size_t i = 0; i < nb; i++) {
                    found += (size_t)jsonDocResolve(docs[i], steps, nsteps, &ref);
                    sink += (uint64_t)jsonRefType(&ref);
                }
            }
            t_lookup[p] += nowNs() - t0;
            if (found != LOOKUP_REPEAT * nb) {
                fprintf(stderr, "%s: lookup of %s missed\n", sh->name, sh->paths[p].text);
                exit(2);
            }
            for (int s = 0; s < nsteps; s++) jsonKeyRelease(steps[s].keyid);
        }
        t0 = nowNs();
        for (size_t i = 0; i < nb; i++) jsonDocFree(docs[i]);
        t_free += nowNs() - t0;
    }
    jsonBufFree(&buf);
    g_sink += sink;
    out->parse = (double)t_parse / (double)c->n;
    out->serialize = (double)t_ser / (double)c->n;
    out->free_ = (double)t_free / (double)c->n;
    for (int p = 0; p < sh->npaths; p++) out->lookup[p] = (double)t_lookup[p] / (double)(c->n * LOOKUP_REPEAT);
}

static void report(const char *shape_name, const char *metric, double *v, double prototype) {
    const double m = median(v, RUNS); /* Sorts: v[0] is then the fastest run. */
    printf("%-8s %-28s %9.1f %9.1f %10.1f %+8.1f%%\n", shape_name, metric, m, v[0], prototype, (m / prototype - 1) * 100);
}

/* ----------------------------------------------------------------------------
 * Changes
 *
 * A pass applies one change to each of up to 1,000 targets and is timed as a
 * whole. For the record shapes a target is one document of a batch. For the
 * two spine documents the targets are places inside the one document. The
 * locators are resolved before the clock starts, as a compiled path would
 * have them. Passes run in order on the same documents, so the ones that add
 * are followed by the ones that remove.
 * ------------------------------------------------------------------------- */

#define WRITE_TARGETS 1000
#define WRITE_DOCS 20000 /* Documents of a record shape used per run. */
#define WRITE_MAX_PASSES 16

typedef struct writePass {
    const char *label;
    char op;          /* i jsonSetInt, s jsonSet, a jsonObjectAdd, x jsonDelete, p append, f insert at 0. */
    const char *path; /* Steps joined by '.'. "#": a different element per target. "^": the last element.
                       * A '%' in a name is replaced by the target's number. */
    const char *arg;  /* s, p, f: the value as JSON text. a: the member name. */
    double prototype; /* ns in RESULTS.md where the prototype measured the same thing, else 0. */
    int targets;      /* 0 = WRITE_TARGETS. */
} writePass;

typedef struct writeCase {
    const char *file;
    int single; /* One document, targets inside it. */
    int npasses;
    writePass passes[WRITE_MAX_PASSES];
} writeCase;

#define LONG_STRING "\"a-string-of-forty-eight-bytes-0123456789-abcdefg\""
#define ORDER_ITEM "{\"sku\":\"SKU-0BADF00D\",\"name\":\"alpha bravo charlie\",\"qty\":1,\"price\":9.99}"
#define RECORD "{\"id\":100000,\"name\":\"item-100000\",\"price\":12.5,\"qty\":3,\"tags\":[\"alpha\",\"bravo\"]}"

static const writeCase WRITE_CASES[] = {
    {"session",
     0,
     10,
     {{"set int in place (uid)", 'i', "uid", NULL, 5.7, 0},
      {"set string, same length (sid)", 's', "sid", "\"0123456789abcdef0123456789abcdef\"", 0, 0},
      {"set string, longer (last_path)", 's', "last_path", LONG_STRING, 93.3, 0},
      {"set string, shorter (last_path)", 's', "last_path", "\"/a\"", 0, 0},
      {"add member", 'a', "", "bench_added", 0, 0},
      {"delete member", 'x', "bench_added", NULL, 0, 0},
      {"array append (roles)", 'p', "roles", "\"appended\"", 0, 0},
      {"array pop (roles)", 'x', "roles.^", NULL, 0, 0},
      {"array insert at 0 (roles)", 'f', "roles", "\"appended\"", 0, 0},
      {"array delete at 0 (roles)", 'x', "roles.0", NULL, 0, 0}}},
    {"profile",
     0,
     10,
     {{"set int in place (age)", 'i', "age", NULL, 10.8, 0},
      {"set string, same length (zip)", 's', "address.zip", "\"12345\"", 0, 0},
      {"set string, longer (email)", 's', "email", LONG_STRING, 85.9, 0},
      {"set string, shorter (email)", 's', "email", "\"a@b\"", 0, 0},
      {"add member", 'a', "", "bench_added", 0, 0},
      {"delete member", 'x', "bench_added", NULL, 0, 0},
      {"array append (tags)", 'p', "tags", "\"appended\"", 70.9, 0},
      {"array pop (tags)", 'x', "tags.^", NULL, 0, 0},
      {"array insert at 0 (tags)", 'f', "tags", "\"appended\"", 0, 0},
      {"array delete at 0 (tags)", 'x', "tags.0", NULL, 0, 0}}},
    {"order",
     0,
     10,
     {{"set int in place (customer_id)", 'i', "customer_id", NULL, 6.0, 0},
      {"set string, same length (order_id)", 's', "order_id", "\"ORD-123456789\"", 0, 0},
      {"set string, longer (status)", 's', "status", LONG_STRING, 98.0, 0},
      {"set string, shorter (status)", 's', "status", "\"ok\"", 0, 0},
      {"add member", 'a', "", "bench_added", 0, 0},
      {"delete member", 'x', "bench_added", NULL, 0, 0},
      {"array append (items)", 'p', "items", ORDER_ITEM, 0, 0},
      {"array pop (items)", 'x', "items.^", NULL, 0, 0},
      {"array insert at 0 (items)", 'f', "items", ORDER_ITEM, 0, 0},
      {"array delete at 0 (items)", 'x', "items.0", NULL, 0, 0}}},
    {"big",
     0,
     15,
     {{"set int in place (age, 4th)", 'i', "age", NULL, 18.5, 0},
      {"set int in place (tail_count, last)", 'i', "tail_count", NULL, 208.0, 0},
      {"set string, same length (zip)", 's', "address.zip", "\"12345\"", 0, 0},
      {"set string, longer (email, 3rd)", 's', "email", LONG_STRING, 138.4, 0},
      {"set string, shorter (email, 3rd)", 's', "email", "\"a@b\"", 0, 0},
      {"set string, longer (tail_note)", 's', "tail_note", LONG_STRING, 254.4, 0},
      {"set string, shorter (tail_note)", 's', "tail_note", "\"ok\"", 0, 0},
      {"add member", 'a', "", "bench_added", 0, 0},
      {"delete member", 'x', "bench_added", NULL, 0, 0},
      {"array append (tags, 8th)", 'p', "tags", "\"appended\"", 123.2, 0},
      {"array pop (tags, 8th)", 'x', "tags.^", NULL, 0, 0},
      {"array append (tail_tags)", 'p', "tail_tags", "\"appended\"", 232.4, 0},
      {"array pop (tail_tags)", 'x', "tail_tags.^", NULL, 0, 0},
      {"array insert at 0 (tags)", 'f', "tags", "\"appended\"", 0, 0},
      {"array delete at 0 (tags)", 'x', "tags.0", NULL, 0, 0}}},
    {"obj1000",
     1,
     5,
     {{"set int in a slot", 'i', "field_%04d", NULL, 0, 0},
      {"set 10-byte string in a slot", 's', "field_%04d", "\"abcdefghij\"", 0, 0},
      {"set int over that string", 'i', "field_%04d", NULL, 0, 0},
      {"add member", 'a', "", "bench_%04d", 0, 0},
      {"delete member", 'x', "bench_%04d", NULL, 0, 0}}},
    {"records100k",
     1,
     9,
     {{"set int in place (record.qty)", 'i', "#.qty", NULL, 0, 0},
      {"set string, longer (record.name)", 's', "#.name", LONG_STRING, 0, 0},
      {"set string, shorter (record.name)", 's', "#.name", "\"i\"", 0, 0},
      {"add member to a record", 'a', "#", "bench_added", 0, 0},
      {"delete member of a record", 'x', "#.bench_added", NULL, 0, 0},
      {"array append (a record)", 'p', "", RECORD, 0, 0},
      {"array pop", 'x', "^", NULL, 0, 0},
      {"array insert at 0 (a record)", 'f', "", RECORD, 0, 200},
      {"array delete at 0", 'x', "0", NULL, 0, 200}}},
};

typedef struct writeTarget {
    jsonStep loc[MAX_STEPS];
    int n;
    uint32_t index; /* p: where to insert. */
    char name[32];  /* a: the member name. */
} writeTarget;

/* Resolves the path of a pass for one target, through the read functions. */
static void resolveTarget(jsonDoc d, const writePass *pass, int target, int earlier_on_doc, writeTarget *t) {
    char step[64];
    jsonRef cur, next;
    const char *text = pass->path;
    jsonDocRoot(d, &cur);
    t->n = 0;
    while (*text) {
        const char *end = strchr(text, '.');
        const size_t len = end ? (size_t)(end - text) : strlen(text);
        jsonStep *s = &t->loc[t->n++];
        int found;
        if (*text == '#' || *text == '^' || (*text >= '0' && *text <= '9')) {
            const uint32_t count = jsonRefCount(&cur);
            s->keyid = 0;
            s->pos = *text == '#' ? ((uint32_t)target * 97u) % count : *text == '^' ? count - 1 - (uint32_t)earlier_on_doc
                                                                                    : (uint32_t)atoi(text);
            found = jsonArrayGet(&cur, s->pos, &next);
        } else {
            char name[64];
            memcpy(step, text, len);
            step[len] = '\0';
            snprintf(name, sizeof(name), step, target);
            found = jsonObjectGet(&cur, name, strlen(name), &next, s);
        }
        if (!found) {
            fprintf(stderr, "pass \"%s\": path %s not found\n", pass->label, pass->path);
            exit(2);
        }
        cur = next;
        text += len + (end != NULL);
    }
    t->index = pass->op == 'p' ? jsonRefCount(&cur) + (uint32_t)earlier_on_doc : 0;
    if (pass->op == 'a') snprintf(t->name, sizeof(t->name), pass->arg, target);
}

/* One pass over `count` targets. docs[i] is the document of target i, or
 * docs[0] for every target when `single`. Returns the time in ns. */
static uint64_t runPass(const writePass *pass, jsonDoc *docs, int count, int single) {
    static writeTarget targets[WRITE_TARGETS];
    jsonErr err;
    jsonVal *val = NULL;
    if (pass->op == 'a') {
        val = jsonValInt(7);
    } else if (pass->arg) {
        val = jsonValParse(pass->arg, strlen(pass->arg), 0, 0, &err, NULL);
    }
    for (int i = 0; i < count; i++) resolveTarget(docs[single ? 0 : i], pass, i, single ? i : 0, &targets[i]);
    const jsonVal *const vals[1] = {val};
    const uint64_t t0 = nowNs();
    for (int i = 0; i < count; i++) {
        jsonDoc *d = &docs[single ? 0 : i];
        const writeTarget *t = &targets[i];
        switch (pass->op) {
        case 'i': jsonSetInt(d, t->loc, t->n, 7); break;
        case 's': jsonSet(d, t->loc, t->n, val); break;
        case 'a': jsonObjectAdd(d, t->loc, t->n, t->name, strlen(t->name), val); break;
        case 'x': jsonDelete(d, t->loc, t->n); break;
        case 'p': jsonArrayInsert(d, t->loc, t->n, t->index, vals, 1); break;
        default: jsonArrayInsert(d, t->loc, t->n, 0, vals, 1);
        }
    }
    const uint64_t elapsed = nowNs() - t0;
    jsonValFree(val);
    return elapsed;
}

/* One run of a case: every pass on every batch. out[p] = ns per change. */
static void runWriteCase(const writeCase *wc, const corpus *c, double *out) {
    static jsonDoc docs[BATCH];
    uint64_t total[WRITE_MAX_PASSES] = {0}, ops[WRITE_MAX_PASSES] = {0};
    const size_t ndocs = wc->single ? 1 : (c->n < WRITE_DOCS ? c->n : WRITE_DOCS);
    for (size_t b0 = 0; b0 < ndocs; b0 += BATCH) {
        const size_t nb = ndocs - b0 < BATCH ? ndocs - b0 : BATCH;
        jsonErr err;
        for (size_t i = 0; i < nb; i++) {
            const char *text = c->buf + c->off[b0 + i];
            docs[i] = jsonValToDoc(jsonValParse(text, c->off[b0 + i + 1] - c->off[b0 + i] - 1, 0, 0, &err, NULL));
        }
        for (int p = 0; p < wc->npasses; p++) {
            const writePass *pass = &wc->passes[p];
            const int count = wc->single ? (pass->targets ? pass->targets : WRITE_TARGETS) : (int)nb;
            total[p] += runPass(pass, docs, count, wc->single);
            ops[p] += (uint64_t)count;
        }
        for (size_t i = 0; i < nb; i++) jsonDocFree(docs[i]);
    }
    for (int p = 0; p < wc->npasses; p++) out[p] = (double)total[p] / (double)ops[p];
}

static void benchWrites(const char *dir) {
    const int ncases = (int)(sizeof(WRITE_CASES) / sizeof(WRITE_CASES[0]));
    static double samples[8][WRITE_MAX_PASSES][RUNS];
    corpus corpora[8];
    for (int k = 0; k < ncases; k++) corpora[k] = load(dir, WRITE_CASES[k].file);
    for (int round = 0; round < ROUNDS; round++) {
        for (int k = 0; k < ncases; k++) {
            double once[WRITE_MAX_PASSES];
            runWriteCase(&WRITE_CASES[k], &corpora[k], once);
            for (int r = 0; r < RUNS_PER_ROUND; r++) {
                runWriteCase(&WRITE_CASES[k], &corpora[k], once);
                for (int p = 0; p < WRITE_CASES[k].npasses; p++) samples[k][p][round * RUNS_PER_ROUND + r] = once[p];
            }
        }
    }
    printf("\nns per change, median and fastest of %d runs. Locators are resolved beforehand.\n", RUNS);
    printf("The prototype's figures include resolving the member names (about 7 ns a step).\n");
    printf("%-12s %-38s %9s %9s %10s\n", "document", "change", "median", "fastest", "prototype");
    for (int k = 0; k < ncases; k++) {
        for (int p = 0; p < WRITE_CASES[k].npasses; p++) {
            const writePass *pass = &WRITE_CASES[k].passes[p];
            const double m = median(samples[k][p], RUNS);
            printf("%-12s %-38s %9.1f %9.1f ", WRITE_CASES[k].file, pass->label, m, samples[k][p][0]);
            if (pass->prototype > 0) {
                printf("%10.1f\n", pass->prototype);
            } else {
                printf("%10s\n", "-");
            }
        }
    }
}

/* ----------------------------------------------------------------------------
 * Whole documents: parse, serialise and free on every shape of the gate
 * corpus, and the bytes a document takes.
 * ------------------------------------------------------------------------- */

typedef struct docShape {
    const char *file;
    size_t max_docs; /* Documents used per run. */
    int repeats;     /* Times the documents are gone through per run. */
} docShape;

static const docShape DOC_SHAPES[] = {
    {"session", 20000, 1},
    {"profile", 20000, 1},
    {"order", 20000, 1},
    {"emb_short", 1, 1000},
    {"emb_full", 1, 1000},
    {"ints100k", 1, 40},
    {"str10k", 1, 200},
    {"records100k", 1, 3},
};
#define DOC_SHAPE_COUNT (sizeof(DOC_SHAPES) / sizeof(DOC_SHAPES[0]))

/* The allocator of the memory pass. It records the size asked for and
 * reports the jemalloc size class of it as the usable size, so that the
 * bytes are what the server would count, whatever allocator runs here. */
typedef struct sizedBlock {
    size_t size;
    size_t unused;
} sizedBlock;

static size_t jemallocClass(size_t n) {
    if (n <= 8) return 8;
    if (n <= 128) return (n + 15) & ~(size_t)15;
    const int lg = 63 - __builtin_clzll((unsigned long long)(n - 1));
    const size_t delta = (size_t)1 << (lg - 2);
    return (n + delta - 1) & ~(delta - 1);
}

static void *sizedAlloc(size_t n) {
    sizedBlock *b = malloc(sizeof(*b) + n);
    b->size = n;
    return b + 1;
}

static void *sizedRealloc(void *p, size_t n) {
    sizedBlock *b = realloc((sizedBlock *)p - 1, sizeof(*b) + n);
    b->size = n;
    return b + 1;
}

static void sizedFree(void *p) {
    free((sizedBlock *)p - 1);
}

static size_t sizedUsable(void *p) {
    return jemallocClass(((sizedBlock *)p - 1)->size);
}

static jsonDoc parseAt(const corpus *c, size_t i) {
    jsonErr err;
    jsonVal *v = jsonValParse(c->buf + c->off[i], c->off[i + 1] - c->off[i] - 1, 0, 0, &err, NULL);
    if (!v) {
        fprintf(stderr, "document %zu does not parse\n", i);
        exit(2);
    }
    return jsonValToDoc(v);
}

/* Bytes per document, in jemalloc size classes. Runs on an engine of its own. */
static void benchMemory(const char *dir) {
    const jsonAllocator sized = {sizedAlloc, sizedRealloc, sizedFree, sizedUsable};
    jsonEngineInit(&sized);
    printf("bytes per document (jemalloc size classes), and the text it was parsed from\n");
    printf("%-12s %12s %12s %8s  %s\n", "shape", "text", "document", "ratio", "encoding");
    for (size_t k = 0; k < DOC_SHAPE_COUNT; k++) {
        corpus c = load(dir, DOC_SHAPES[k].file);
        const size_t n = c.n < DOC_SHAPES[k].max_docs ? c.n : DOC_SHAPES[k].max_docs;
        size_t bytes = 0, text = 0;
        const char *encoding = "";
        for (size_t i = 0; i < n; i++) {
            const jsonDoc d = parseAt(&c, i);
            bytes += jsonDocMemUsage(d);
            text += c.off[i + 1] - c.off[i] - 1;
            encoding = jsonDocEncoding(d);
            jsonDocFree(d);
        }
        printf("%-12s %12.1f %12.1f %7.1f%%  %s\n", DOC_SHAPES[k].file, (double)text / (double)n, (double)bytes / (double)n,
               100.0 * (double)bytes / (double)text, encoding);
        free(c.buf);
        free(c.off);
    }
    jsonEngineShutdown();
}

typedef struct docSample {
    double parse, serialize, free_;
} docSample;

static void runDocs(const docShape *sh, const corpus *c, docSample *out) {
    static jsonDoc docs[BATCH];
    const size_t n = c->n < sh->max_docs ? c->n : sh->max_docs;
    uint64_t t_parse = 0, t_ser = 0, t_free = 0, sink = 0;
    jsonRef ref;
    jsonBuf buf;
    jsonBufInit(&buf, NULL, 0);
    for (int rep = 0; rep < sh->repeats; rep++) {
        for (size_t b0 = 0; b0 < n; b0 += BATCH) {
            const size_t nb = n - b0 < BATCH ? n - b0 : BATCH;
            uint64_t t0 = nowNs();
            for (size_t i = 0; i < nb; i++) docs[i] = parseAt(c, b0 + i);
            uint64_t t1 = nowNs();
            for (size_t i = 0; i < nb; i++) {
                buf.len = 0;
                jsonDocRoot(docs[i], &ref);
                jsonSerialize(&ref, NULL, &buf);
                sink += buf.len;
            }
            uint64_t t2 = nowNs();
            for (size_t i = 0; i < nb; i++) jsonDocFree(docs[i]);
            t_free += nowNs() - t2;
            t_parse += t1 - t0;
            t_ser += t2 - t1;
        }
    }
    jsonBufFree(&buf);
    g_sink += sink;
    const double total = (double)n * sh->repeats;
    out->parse = (double)t_parse / total;
    out->serialize = (double)t_ser / total;
    out->free_ = (double)t_free / total;
}

/* Reading every element of an array as a double: ns per element. */
static double benchElementValues(const corpus *c) {
    jsonErr err;
    jsonRef root, el;
    double runs[RUNS], sink = 0;
    const jsonDoc d = jsonValToDoc(jsonValParse(c->buf + c->off[0], c->off[1] - c->off[0] - 1, 0, 0, &err, NULL));
    jsonDocRoot(d, &root);
    const uint32_t count = jsonRefCount(&root);
    const int reps = 1 + (int)(2000000 / (count ? count : 1));
    for (int r = 0; r < RUNS; r++) {
        const uint64_t t0 = nowNs();
        for (int k = 0; k < reps; k++) {
            for (uint32_t i = 0; i < count; i++) {
                jsonArrayGet(&root, i, &el);
                sink += jsonRefDouble(&el);
            }
        }
        runs[r] = (double)(nowNs() - t0) / ((double)reps * count);
    }
    if (sink == 12345.678) printf(" "); /* Keeps the loop. */
    jsonDocFree(d);
    return median(runs, RUNS);
}

static void benchDocs(const char *dir) {
    static docSample samples[DOC_SHAPE_COUNT][RUNS];
    corpus corpora[DOC_SHAPE_COUNT];
    for (size_t k = 0; k < DOC_SHAPE_COUNT; k++) corpora[k] = load(dir, DOC_SHAPES[k].file);
    for (int round = 0; round < ROUNDS; round++) {
        for (size_t k = 0; k < DOC_SHAPE_COUNT; k++) {
            docSample warmup;
            runDocs(&DOC_SHAPES[k], &corpora[k], &warmup);
            for (int r = 0; r < RUNS_PER_ROUND; r++) runDocs(&DOC_SHAPES[k], &corpora[k], &samples[k][round * RUNS_PER_ROUND + r]);
        }
    }
    printf("\nns per document, median of %d runs\n", RUNS);
    printf("%-12s %14s %14s %14s\n", "shape", "parse", "serialise", "free");
    for (size_t k = 0; k < DOC_SHAPE_COUNT; k++) {
        double p[RUNS], s[RUNS], f[RUNS];
        for (int r = 0; r < RUNS; r++) {
            p[r] = samples[k][r].parse;
            s[r] = samples[k][r].serialize;
            f[r] = samples[k][r].free_;
        }
        printf("%-12s %14.1f %14.1f %14.1f\n", DOC_SHAPES[k].file, median(p, RUNS), median(s, RUNS), median(f, RUNS));
    }
    printf("\nns per element: every element of the array read as a double\n");
    for (size_t k = 0; k < DOC_SHAPE_COUNT; k++) {
        if (strncmp(DOC_SHAPES[k].file, "emb_", 4) == 0 || strncmp(DOC_SHAPES[k].file, "ints", 4) == 0) {
            printf("%-12s %14.1f\n", DOC_SHAPES[k].file, benchElementValues(&corpora[k]));
        }
    }
}

static void benchReads(const char *dir) {
    const int nshapes = (int)(sizeof(SHAPES) / sizeof(SHAPES[0]));
    static sample samples[3][RUNS];
    corpus corpora[3];
    for (int s = 0; s < nshapes; s++) corpora[s] = load(dir, SHAPES[s].name);
    /* Shapes are interleaved within a round, so that a disturbance cannot
     * fall on every run of one shape. */
    for (int round = 0; round < ROUNDS; round++) {
        for (int s = 0; s < nshapes; s++) {
            sample warmup;
            runOnce(&SHAPES[s], &corpora[s], &warmup);
            for (int r = 0; r < RUNS_PER_ROUND; r++) runOnce(&SHAPES[s], &corpora[s], &samples[s][round * RUNS_PER_ROUND + r]);
        }
    }
    printf("ns per document (lookups: ns per lookup), median and fastest of %d runs\n", RUNS);
    printf("%-8s %-28s %9s %9s %10s %9s\n", "shape", "operation", "median", "fastest", "prototype", "vs proto");
    for (int s = 0; s < nshapes; s++) {
        const shape *sh = &SHAPES[s];
        double v[RUNS];
        char metric[64];
        for (int r = 0; r < RUNS; r++) v[r] = samples[s][r].parse;
        report(sh->name, "parse", v, sh->proto_parse);
        for (int r = 0; r < RUNS; r++) v[r] = samples[s][r].serialize;
        report(sh->name, "serialise", v, sh->proto_serialize);
        for (int r = 0; r < RUNS; r++) v[r] = samples[s][r].free_;
        report(sh->name, "free", v, sh->proto_free);
        for (int p = 0; p < sh->npaths; p++) {
            for (int r = 0; r < RUNS; r++) v[r] = samples[s][r].lookup[p];
            snprintf(metric, sizeof(metric), "lookup %s", sh->paths[p].text);
            report(sh->name, metric, v, sh->paths[p].prototype_ns);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: engine_bench <dir> [docs|read|write]\n");
        return 2;
    }
    const jsonAllocator a = {malloc, realloc, free, usableSize};
    if (argc == 2 || !strcmp(argv[2], "docs")) benchMemory(argv[1]);
    jsonEngineInit(&a);
    if (argc == 2 || !strcmp(argv[2], "docs")) benchDocs(argv[1]);
    if (argc == 2 || !strcmp(argv[2], "read")) benchReads(argv[1]);
    if (argc == 2 || !strcmp(argv[2], "write")) benchWrites(argv[1]);
    const size_t leaked = jsonEngineShutdown();
    if (leaked) {
        fprintf(stderr, "LEAK: %zu keys still referenced\n", leaked);
        return 1;
    }
    return 0;
}
