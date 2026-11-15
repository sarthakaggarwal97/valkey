/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_num_test.c -- standalone tests for json_num.c.
 *
 *   json_num_test selftest   Built-in checks, no input.
 *   json_num_test print      stdin: raw doubles. stdout: one text per line.
 *   json_num_test parse      stdin: one number per line. stdout: raw doubles.
 *   json_num_test fixed      stdin: {double, int32 frac} records (12 bytes).
 *                            stdout: one text per line, empty for a 0 return.
 *   json_num_test decimal    stdin: {uint64 mantissa, int32 exp10, int32
 *                            negative} records (16 bytes). stdout: raw doubles
 *                            from jsonDoubleFromDecimal.
 *
 * The four filter modes are driven by check_num.py, which compares their
 * output with Python's repr() and float(). Each mode also checks what it can
 * on its own and exits with status 1 after reporting the first failures. */

#include "../../json_num.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void fail(const char *what, const char *text, size_t len, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    if (failures++ < 20) {
        fprintf(stderr, "FAIL %s: text '%.*s' double %.17g (0x%016" PRIx64 ")\n", what, (int)len, text, d, bits);
    }
}

static void put(const void *data, size_t len) {
    if (fwrite(data, 1, len, stdout) != len) {
        perror("write");
        exit(2);
    }
}

/* The binary records check_num.py exchanges with the filters below are
 * little-endian whatever the byte order here: the script need not run on the
 * machine this program runs on (tests/engine/docker_check.sh runs it on the
 * host and the program in an emulated container). */
static int readLE(void *dst, size_t n) {
    uint8_t b[8];
    if (n > sizeof(b) || fread(b, 1, n, stdin) != n) return 0;
    for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ ? n - 1 - i : i] = b[i];
    return 1;
}

static void putLE(const void *src, size_t n) {
    uint8_t b[8];
    for (size_t i = 0; i < n && i < sizeof(b); i++) b[i] = ((const uint8_t *)src)[__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ ? n - 1 - i : i];
    put(b, n);
}

static int sameBits(double a, double b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}

/* Parses from a heap copy of exactly n bytes, so that AddressSanitizer catches
 * any read past the end of the text. */
static double parseExact(const char *s, size_t n, int *overflow) {
    char *copy = malloc(n ? n : 1);
    memcpy(copy, s, n);
    double d = jsonDoubleParse(copy, n, overflow);
    free(copy);
    return d;
}

/* xorshift64* */
static uint64_t rngState = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return rngState * 0x2545F4914F6CDD1DULL;
}

/* ----------------------------------------------------------------------------
 * selftest
 * ------------------------------------------------------------------------- */

static void expectPrint(double d, const char *want) {
    char buf[JSON_DOUBLE_BUF + 1];
    int n = jsonDoublePrint(d, buf);
    if ((size_t)n != strlen(want) || memcmp(buf, want, (size_t)n) != 0) {
        buf[n] = '\0';
        fprintf(stderr, "FAIL print: got '%s' want '%s'\n", buf, want);
        failures++;
    }
}

static void expectFixed(double d, int frac, const char *want) {
    char buf[JSON_DOUBLE_BUF + 1];
    memset(buf, '#', sizeof(buf));
    int n = jsonDoublePrintFixed(d, frac, buf);
    if ((size_t)n != strlen(want) || memcmp(buf, want, (size_t)n) != 0 || (n == 0 && buf[0] != '#')) {
        buf[n] = '\0';
        fprintf(stderr, "FAIL fixed(%.17g, %d): got '%s' want '%s'\n", d, frac, buf, want);
        failures++;
    }
}

static void expectParse(const char *text, double want, int wantOverflow) {
    int overflow = -1;
    double got = parseExact(text, strlen(text), &overflow);
    if (!sameBits(got, want) || overflow != wantOverflow) {
        fprintf(stderr, "FAIL parse '%s': got %.17g overflow %d, want %.17g overflow %d\n",
                text, got, overflow, want, wantOverflow);
        failures++;
    }
}

static void expectInt(int64_t v) {
    char buf[JSON_DOUBLE_BUF + 1], want[32];
    int n = jsonInt64Print(v, buf);
    int m = snprintf(want, sizeof(want), "%" PRId64, v);
    if (n != m || memcmp(buf, want, (size_t)n) != 0) {
        buf[n] = '\0';
        fprintf(stderr, "FAIL int64: got '%s' want '%s'\n", buf, want);
        failures++;
    }
}

static int selftest(void) {
    /* The examples of json_num.h. */
    expectPrint(1.5, "1.5");
    expectPrint(3.0, "3.0");
    expectPrint(100.0, "100.0");
    expectPrint(0.000001, "0.000001");
    expectPrint(1e-7, "1e-7");
    expectPrint(1.5e-7, "1.5e-7");
    expectPrint(123456789012345680000.0, "123456789012345680000.0");
    expectPrint(1e21, "1e+21");
    expectPrint(1.7976931348623157e308, "1.7976931348623157e+308");
    expectPrint(-0.0, "-0.0");
    /* More layout. */
    expectPrint(0.0, "0.0");
    expectPrint(-1.5, "-1.5");
    expectPrint(5e-324, "5e-324");
    expectPrint(-5e-324, "-5e-324");
    expectPrint(2.2250738585072014e-308, "2.2250738585072014e-308");
    expectPrint(9007199254740992.0, "9007199254740992.0");
    expectPrint(9007199254740993.0, "9007199254740992.0");
    expectPrint(999999999999999900000.0, "999999999999999900000.0");
    expectPrint(1e20, "100000000000000000000.0");
    expectPrint(1.2e21, "1.2e+21");
    expectPrint(1e22, "1e+22");
    expectPrint(1e23, "1e+23");
    expectPrint(0.1, "0.1");
    expectPrint(0.3, "0.3");
    expectPrint(0.1 + 0.2, "0.30000000000000004");
    expectPrint(0.00001, "0.00001");
    expectPrint(0.0000012345, "0.0000012345");
    expectPrint(9.999999e-7, "9.999999e-7");
    expectPrint(1e100, "1e+100");
    expectPrint(1.2345e-100, "1.2345e-100");
    expectPrint(123456.789, "123456.789");
    expectPrint(-1234567890123456.0, "-1234567890123456.0");
    expectPrint(4.35, "4.35");
    expectPrint(5e-7, "5e-7");
    /* Longest outputs must fit JSON_DOUBLE_BUF. */
    expectPrint(-0.000001234567890123456, "-0.000001234567890123456");
    expectPrint(-2.2250738585072014e-308, "-2.2250738585072014e-308");
    expectPrint(-123456789012345680000.0, "-123456789012345680000.0");

    expectFixed(0.5, 3, "0.500");
    expectFixed(-0.5, 1, "-0.5");
    expectFixed(0.0, 2, "0.00");
    expectFixed(-0.0, 15, "-0.000000000000000");
    expectFixed(3.0, 2, "3.00");
    expectFixed(100.0, 1, "100.0");
    expectFixed(1234.5, 4, "1234.5000");
    expectFixed(0.125, 3, "0.125");
    expectFixed(0.125, 2, "");
    expectFixed(0.001, 5, "0.00100");
    expectFixed(1e-7, 8, "0.00000010");
    expectFixed(1e-15, 15, "0.000000000000001");
    expectFixed(1e-16, 15, "");
    expectFixed(0.1 + 0.2, 15, "");
    expectFixed(123456789012345.0, 1, ""); /* 16 significant digits */
    expectFixed(12345678901234.0, 1, "12345678901234.0");
    expectFixed(999999999999999.0, 1, "");
    expectFixed(1e15, 1, "");
    expectFixed(1e14, 1, ""); /* 16 significant digits */
    expectFixed(1e13, 1, "10000000000000.0");
    expectFixed(1e13, 2, "");
    expectFixed(0.123456789012345, 15, "0.123456789012345");
    expectFixed(1.23456789012345, 15, ""); /* 16 significant digits */
    expectFixed(1.23456789012345, 14, "1.23456789012345");
    expectFixed(0.5, 0, "");
    expectFixed(0.5, 16, "");
    expectFixed(INFINITY, 3, "");
    expectFixed(NAN, 3, "");

    expectParse("0", 0.0, 0);
    expectParse("-0", -0.0, 0);
    expectParse("-0.0e5", -0.0, 0);
    expectParse("1", 1.0, 0);
    expectParse("-1.5", -1.5, 0);
    expectParse("1E2", 100.0, 0);
    expectParse("1e+2", 100.0, 0);
    expectParse("1e-2", 0.01, 0);
    expectParse("0.000001", 0.000001, 0);
    expectParse("9007199254740993", 9007199254740992.0, 0);
    expectParse("9007199254740993.0000000000000000000000001", 9007199254740994.0, 0);
    expectParse("9007199254740995", 9007199254740996.0, 0);
    expectParse("123456789012345680000", 123456789012345680000.0, 0);
    expectParse("1.7976931348623157e308", 1.7976931348623157e308, 0);
    expectParse("1.7976931348623158e308", 1.7976931348623157e308, 0);
    expectParse("1.7976931348623159e308", INFINITY, 1);
    expectParse("-1.7976931348623159e308", -INFINITY, 1);
    expectParse("1e309", INFINITY, 1);
    expectParse("-1e99999999999999999999999", -INFINITY, 1);
    expectParse("1e-99999999999999999999999", 0.0, 0);
    expectParse("0e99999999999999999999999", 0.0, 0);
    expectParse("5e-324", 5e-324, 0);
    expectParse("4.9e-324", 5e-324, 0);
    expectParse("2.4703282292062327e-324", 0.0, 0);
    expectParse("2.4703282292062328e-324", 5e-324, 0);
    expectParse("2.2250738585072011e-308", 2.2250738585072011e-308, 0);
    expectParse("2.2250738585072012e-308", 2.2250738585072014e-308, 0);
    expectParse("0.1e-99999999999999999999999", 0.0, 0);

    expectInt(0);
    expectInt(-1);
    expectInt(INT64_MAX);
    expectInt(INT64_MIN);
    for (int i = 0; i < 2000000; i++) {
        int64_t v = (int64_t)rng() >> (rng() & 63);
        expectInt(v);
        expectInt(v % 1000);
    }

    /* Round trips, against the C library: on every supported platform strtod
     * is correctly rounded and the default "C" locale is in effect. */
    for (int i = 0; i < 3000000; i++) {
        char buf[JSON_DOUBLE_BUF + 1], alt[64];
        uint64_t bits = rng();
        double d;
        int overflow;
        if (i % 4 == 1) bits &= 0x800FFFFFFFFFFFFFULL; /* subnormal */
        if (i % 4 == 2) bits = (bits & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(1023 + (int)(rng() % 80) - 20) << 52);
        memcpy(&d, &bits, 8);
        if (!isfinite(d)) continue;
        if (i % 4 == 3) d = (double)((int64_t)rng() >> (rng() % 64)) / (double)(1 << (rng() % 12));

        int n = jsonDoublePrint(d, buf);
        if (n <= 0 || n > JSON_DOUBLE_BUF) fail("print length", buf, 0, d);
        buf[n] = '\0';
        if (!sameBits(strtod(buf, NULL), d)) fail("strtod(print)", buf, (size_t)n, d);
        if (!sameBits(parseExact(buf, (size_t)n, &overflow), d) || overflow) fail("parse(print)", buf, (size_t)n, d);

        /* The shortest text is never longer than 17 significant digits. */
        int m = snprintf(alt, sizeof(alt), "%.17g", d);
        if (!sameBits(parseExact(alt, (size_t)m, &overflow), d) || overflow) fail("parse(%.17g)", alt, (size_t)m, d);
        m = snprintf(alt, sizeof(alt), "%.25e", d);
        if (!sameBits(parseExact(alt, (size_t)m, &overflow), d) || overflow) fail("parse(%.25e)", alt, (size_t)m, d);
    }

    /* Random text against strtod, including long digit strings. */
    for (int i = 0; i < 2000000; i++) {
        char text[1200];
        size_t len = 0;
        int digits = 1 + (int)(rng() % (i % 16 == 0 ? 900 : 40));
        int point = (int)(rng() % (unsigned)(digits + 1));
        int overflow;
        if (rng() & 1) text[len++] = '-';
        for (int j = 0; j < digits; j++) {
            if (j == point && j != 0) text[len++] = '.';
            text[len++] = (char)('0' + (j == 0 && point > 1 ? 1 + rng() % 9 : rng() % 10));
        }
        if (rng() % 3) {
            int e = (int)(rng() % 700) - 350 - (point > 300 ? point : 0);
            len += (size_t)snprintf(text + len, 16, "e%d", e);
        }
        text[len] = '\0';
        double want = strtod(text, NULL);
        double got = parseExact(text, len, &overflow);
        if (!sameBits(got, want) || overflow != (isinf(want) != 0)) fail("parse vs strtod", text, len, want);
    }

    if (failures) {
        fprintf(stderr, "selftest: %d FAILURES\n", failures);
        return 1;
    }
    printf("selftest: ok\n");
    return 0;
}

/* ----------------------------------------------------------------------------
 * Filters for check_num.py
 * ------------------------------------------------------------------------- */

static int printMode(void) {
    double d;
    while (readLE(&d, sizeof(d))) {
        char buf[JSON_DOUBLE_BUF + 1];
        int overflow;
        int n = jsonDoublePrint(d, buf);
        if (n <= 0 || n > JSON_DOUBLE_BUF) {
            fail("print length", "", 0, d);
            n = 0;
        }
        if (!sameBits(parseExact(buf, (size_t)n, &overflow), d) || overflow) fail("parse(print)", buf, (size_t)n, d);
        buf[n] = '\n';
        put(buf, (size_t)n + 1);
    }
    return failures != 0;
}

static int parseMode(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) > 0) {
        int overflow = -1;
        if (line[len - 1] == '\n') len--;
        double d = parseExact(line, (size_t)len, &overflow);
        if (overflow != (isinf(d) != 0)) fail("overflow flag", line, (size_t)len, d);
        putLE(&d, sizeof(d));
    }
    free(line);
    return failures != 0;
}

/* Besides Python's check of the result, the same number written out must
 * parse to the same double. */
static int decimalMode(void) {
    struct {
        uint64_t mantissa;
        int32_t exp10, negative;
    } rec;
    while (readLE(&rec.mantissa, 8) && readLE(&rec.exp10, 4) && readLE(&rec.negative, 4)) {
        char text[64];
        int overflow;
        double d = jsonDoubleFromDecimal(rec.mantissa, rec.exp10, rec.negative);
        int n = snprintf(text, sizeof(text), "%s%" PRIu64 "e%d", rec.negative ? "-" : "", rec.mantissa, rec.exp10);
        if (!sameBits(parseExact(text, (size_t)n, &overflow), d)) fail("decimal vs parse", text, (size_t)n, d);
        putLE(&d, sizeof(d));
    }
    return failures != 0;
}

static int fixedMode(void) {
    struct {
        double d;
        int32_t frac;
    } rec;
    while (readLE(&rec.d, 8) && readLE(&rec.frac, 4)) {
        char buf[JSON_DOUBLE_BUF + 1];
        int overflow;
        memset(buf, '#', sizeof(buf));
        int n = jsonDoublePrintFixed(rec.d, rec.frac, buf);
        if (n < 0 || n > JSON_DOUBLE_BUF) {
            fail("fixed length", "", 0, rec.d);
            n = 0;
        }
        if (n == 0 && buf[0] != '#') fail("fixed wrote on 0", "", 0, rec.d);
        if (n > 0 && (!sameBits(parseExact(buf, (size_t)n, &overflow), rec.d) || overflow)) {
            fail("parse(fixed)", buf, (size_t)n, rec.d);
        }
        buf[n] = '\n';
        put(buf, (size_t)n + 1);
    }
    return failures != 0;
}

int main(int argc, char **argv) {
    static char inbuf[1 << 16], outbuf[1 << 16];
    setvbuf(stdin, inbuf, _IOFBF, sizeof(inbuf));
    setvbuf(stdout, outbuf, _IOFBF, sizeof(outbuf));
    if (argc == 2 && !strcmp(argv[1], "selftest")) return selftest();
    if (argc == 2 && !strcmp(argv[1], "print")) return printMode();
    if (argc == 2 && !strcmp(argv[1], "parse")) return parseMode();
    if (argc == 2 && !strcmp(argv[1], "fixed")) return fixedMode();
    if (argc == 2 && !strcmp(argv[1], "decimal")) return decimalMode();
    fprintf(stderr, "usage: %s selftest|print|parse|fixed|decimal\n", argv[0]);
    return 2;
}
