/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_num_bench.c -- micro-benchmark for json_num.c.
 *
 * Prints and parses three corpora and reports nanoseconds per number, next to
 * the C library and deps/fpconv (Grisu2) for reference. */

#include "../../json_num.h"
#include "fpconv_dtoa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COUNT 1000000
#define ROUNDS 5
#define TEXT_MAX 40

static uint64_t rngState = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return rngState * 0x2545F4914F6CDD1DULL;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double *values;
static char (*texts)[TEXT_MAX];
static int *lengths;
static volatile uint64_t sink;

/* Best of ROUNDS, in ns per number. */
#define BENCH(result, body)                      \
    do {                                         \
        double best = 1e300;                     \
        for (int rep = 0; rep < ROUNDS; rep++) { \
            uint64_t acc = 0;                    \
            double t0 = now();                   \
            for (int i = 0; i < COUNT; i++) {    \
                body;                            \
            }                                    \
            double t = (now() - t0) / COUNT;     \
            if (t < best) best = t;              \
            sink += acc;                         \
        }                                        \
        result = best;                           \
    } while (0)

static void runCorpus(const char *name) {
    char buf[64];
    double printOurs, printLibc, printFpconv, parseOurs, parseLibc;
    size_t bytes = 0;
    int overflow;

    for (int i = 0; i < COUNT; i++) {
        lengths[i] = jsonDoublePrint(values[i], texts[i]);
        texts[i][lengths[i]] = '\0';
        bytes += (size_t)lengths[i];
    }

    BENCH(printOurs, acc += (uint64_t)jsonDoublePrint(values[i], buf));
    BENCH(printLibc, acc += (uint64_t)snprintf(buf, sizeof(buf), "%.17g", values[i]));
    BENCH(printFpconv, acc += (uint64_t)fpconv_dtoa(values[i], buf));
    BENCH(parseOurs, acc += (uint64_t)jsonDoubleParse(texts[i], (size_t)lengths[i], &overflow));
    BENCH(parseLibc, acc += (uint64_t)strtod(texts[i], NULL));

    printf("%-34s %5.1f | %7.1f %9.1f %8.1f | %7.1f %8.1f\n", name, (double)bytes / COUNT,
           printOurs, printLibc, printFpconv, parseOurs, parseLibc);
}

int main(void) {
    values = malloc(sizeof(*values) * COUNT);
    texts = malloc(sizeof(*texts) * COUNT);
    lengths = malloc(sizeof(*lengths) * COUNT);

    printf("ns per number, best of %d rounds over %d numbers\n\n", ROUNDS, COUNT);
    printf("%-34s %5s | %7s %9s %8s | %7s %8s\n", "", "avg", "print", "print", "print", "parse", "parse");
    printf("%-34s %5s | %7s %9s %8s | %7s %8s\n", "corpus", "chars", "json", "%.17g", "fpconv", "json", "strtod");

    /* Random bit patterns: every exponent, 16 to 17 digits. */
    for (int i = 0; i < COUNT;) {
        uint64_t bits = rng();
        memcpy(&values[i], &bits, 8);
        if (isfinite(values[i])) i++;
    }
    runCorpus("random doubles (all exponents)");

    /* Prices, coordinates: up to four integer digits, six fraction digits. */
    for (int i = 0; i < COUNT; i++) {
        values[i] = (double)((int64_t)(rng() % 20000000000ULL) - 10000000000LL) / 1e6;
    }
    runCorpus("decimals with 6 fraction digits");

    /* Embedding components: uniform in (-1, 1) with a full 53-bit mantissa. */
    for (int i = 0; i < COUNT; i++) {
        values[i] = (double)(rng() >> 11) * 0x1p-53 * (rng() & 1 ? 1.0 : -1.0);
    }
    runCorpus("16-17 significant digits in (-1,1)");

    free(values);
    free(texts);
    free(lengths);
    return 0;
}
