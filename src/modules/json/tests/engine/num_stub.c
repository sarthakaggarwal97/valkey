/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* num_stub.c -- a slow, libc-based stand-in for json_num.c.
 *
 * The engine tests link json_num.c by default. `make NUM=stub` links this
 * file instead, which keeps the engine testable on its own and gives a
 * second, independent implementation of the number text to compare with. */

#include "../../json_num.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIGITS 17

#define EXACT_DIGITS 800 /* More than the longest exact decimal expansion of a double. */

/* 1 if the decimal D.DDD * 10^exp10 made of n digits reads back as `a`. */
static int readsBack(const char *digits, int n, int exp10, double a) {
    char buf[48];
    int len = 0;
    buf[len++] = digits[0];
    buf[len++] = '.';
    memcpy(buf + len, digits + 1, (size_t)(n - 1));
    len += n - 1;
    snprintf(buf + len, sizeof(buf) - (size_t)len, "e%d", exp10);
    return strtod(buf, NULL) == a;
}

/* Shortest digits of a positive finite double: the fewest that read back as
 * the same value, and of those the closest to it. Returns the digit count,
 * and in *point the position of the decimal point: the value is
 * 0.DIGITS * 10^point.
 *
 * For each length the two candidates are the value cut to that many digits
 * and that plus one in the last place. Rounding with printf alone is not
 * enough: the correctly rounded 16 digits of a double can fail to read back
 * when its neighbour does. When the value is exactly half way between the
 * two, the one with the even last digit wins. */
static int shortestDigits(double a, char *digits, int *point) {
    char buf[EXACT_DIGITS + 32], exact[EXACT_DIGITS + 2];
    int n_exact = 0;
    snprintf(buf, sizeof(buf), "%.*e", EXACT_DIGITS, a);
    const char *p = buf;
    for (; *p != 'e'; p++) {
        if (*p != '.') exact[n_exact++] = *p;
    }
    exact[n_exact] = '\0';
    const int exp10 = atoi(p + 1);
    for (int n = 1; n <= MAX_DIGITS; n++) {
        char down[MAX_DIGITS + 1], up[MAX_DIGITS + 1];
        int up_exp = exp10, i = n - 1;
        memcpy(down, exact, (size_t)n);
        memcpy(up, exact, (size_t)n);
        while (i >= 0 && up[i] == '9') up[i--] = '0';
        if (i < 0) {
            up[0] = '1'; /* 99..9 + 1: one digit more, which the exponent absorbs. */
            up_exp++;
        } else {
            up[i]++;
        }
        int up_is_nearer = exact[n] >= '5';
        if (exact[n] == '5' && strspn(exact + n + 1, "0") == (size_t)(n_exact - n - 1)) {
            up_is_nearer = (exact[n - 1] - '0') % 2 != 0; /* A tie: prefer the even digit. */
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            const int use_up = (attempt == 0) == up_is_nearer;
            const char *cand = use_up ? up : down;
            const int cand_exp = use_up ? up_exp : exp10;
            if (!readsBack(cand, n, cand_exp, a)) continue;
            int len = n;
            while (len > 1 && cand[len - 1] == '0') len--;
            memcpy(digits, cand, (size_t)len);
            *point = cand_exp + 1;
            return len;
        }
    }
    return 0; /* Not reached: 17 digits always read back. */
}

int jsonDoublePrint(double d, char *buf) {
    char digits[MAX_DIGITS + 1];
    char *p = buf;
    int point;
    if (signbit(d)) {
        *p++ = '-';
        d = -d;
    }
    if (d == 0) {
        memcpy(p, "0.0", 3);
        return (int)(p + 3 - buf);
    }
    const int nd = shortestDigits(d, digits, &point);
    if (point > 0 && point <= 21) {
        /* Plain notation, at least one digit before the point. */
        if (nd <= point) {
            memcpy(p, digits, (size_t)nd);
            p += nd;
            memset(p, '0', (size_t)(point - nd));
            p += point - nd;
            *p++ = '.';
            *p++ = '0';
        } else {
            memcpy(p, digits, (size_t)point);
            p += point;
            *p++ = '.';
            memcpy(p, digits + point, (size_t)(nd - point));
            p += nd - point;
        }
        return (int)(p - buf);
    }
    if (point > -6 && point <= 0) {
        /* Plain notation below 1: down to 0.000001. */
        *p++ = '0';
        *p++ = '.';
        memset(p, '0', (size_t)-point);
        p += -point;
        memcpy(p, digits, (size_t)nd);
        return (int)(p + nd - buf);
    }
    /* Exponent notation as ECMAScript writes it. */
    *p++ = digits[0];
    if (nd > 1) {
        *p++ = '.';
        memcpy(p, digits + 1, (size_t)(nd - 1));
        p += nd - 1;
    }
    char exponent[8];
    const int n = snprintf(exponent, sizeof(exponent), "e%c%d", point - 1 < 0 ? '-' : '+', abs(point - 1));
    memcpy(p, exponent, (size_t)n);
    return (int)(p + n - buf);
}

int jsonDoublePrintFixed(double d, int frac, char *buf) {
    char digits[MAX_DIGITS + 1];
    char *p = buf;
    int point;
    if (frac < 1 || frac > 15 || d - d != 0) return 0;
    const int negative = signbit(d) != 0;
    if (negative) d = -d;
    if (d == 0) {
        if (negative) *p++ = '-';
        *p++ = '0';
        *p++ = '.';
        memset(p, '0', (size_t)frac);
        return (int)(p + frac - buf);
    }
    const int nd = shortestDigits(d, digits, &point);
    /* More fraction digits than asked for, or more than 15 significant
     * digits once padded. */
    if (nd - point > frac || point + frac > 15) return 0;
    if (negative) *p++ = '-';
    if (point > 0) {
        for (int i = 0; i < point; i++) *p++ = i < nd ? digits[i] : '0';
    } else {
        *p++ = '0';
    }
    *p++ = '.';
    for (int j = 0; j < frac; j++) {
        const int i = point + j;
        *p++ = (i >= 0 && i < nd) ? digits[i] : '0';
    }
    return (int)(p - buf);
}

int jsonInt64Print(int64_t v, char *buf) {
    char tmp[24];
    const int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    memcpy(buf, tmp, (size_t)n);
    return n;
}

double jsonDoubleParse(const char *s, size_t n, int *overflow) {
    char small[64];
    char *copy = n < sizeof(small) ? small : malloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    const double d = strtod(copy, NULL);
    if (copy != small) free(copy);
    *overflow = d - d != 0; /* Infinity. */
    return d;
}

double jsonDoubleFromDecimal(uint64_t mantissa, int exp10, int negative) {
    char text[64];
    snprintf(text, sizeof(text), "%s%llue%d", negative ? "-" : "", (unsigned long long)mantissa, exp10);
    return strtod(text, NULL);
}
