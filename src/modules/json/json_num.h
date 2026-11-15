/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_num.h -- number text for the built-in JSON module.
 *
 * No server header is included and no memory is allocated. Every function is
 * safe to call from any thread. */

#ifndef JSON_NUM_H
#define JSON_NUM_H

#include <stddef.h>
#include <stdint.h>

/* Bytes a caller must provide to jsonDoublePrint and jsonInt64Print. */
#define JSON_DOUBLE_BUF 32

/* Writes the canonical text of a finite double and returns its length. No
 * terminating NUL is written.
 *
 * Canonical text is the shortest decimal digit string that reads back as
 * exactly `d`, laid out the way ECMAScript's Number::toString does (plain
 * notation for 1e-6 <= |d| < 1e21, exponent notation outside it), with one
 * difference: a value with no fractional digits in plain notation carries
 * ".0", so the text never looks like an integer. Examples: 1.5, 3.0, 100.0,
 * 0.000001, 1e-7, 1.5e-7, 123456789012345680000.0, 1e+21, 1.7976931348623157e+308.
 * Negative zero prints "-0.0". */
int jsonDoublePrint(double d, char *buf);

/* The shortest digits of `d` in plain notation, zero-padded to exactly `frac`
 * fractional digits, 1 <= frac <= 15. Example: 0.5 with frac 3 is "0.500".
 * Returns the length, or 0 and writes nothing when that text does not exist
 * or would not read back as `d`: the shortest digits have more than `frac`
 * fractional digits, more than 15 significant digits result, or |d| >= 1e15. */
int jsonDoublePrintFixed(double d, int frac, char *buf);

/* Writes a signed 64-bit integer in decimal and returns its length. */
int jsonInt64Print(int64_t v, char *buf);

/* Reads valid JSON number text of length n and returns the correctly rounded
 * (round-half-even) double. Sets *overflow to 1 when the value overflows
 * to infinity, else 0. `s` need not be NUL-terminated. */
double jsonDoubleParse(const char *s, size_t n, int *overflow);

/* The correctly rounded (round-half-even) double of mantissa * 10^exp10,
 * negated when `negative` is not 0: what jsonDoubleParse returns for that
 * number written out, without the text. Any mantissa and any exponent are
 * accepted. Infinity is returned when the value overflows. */
double jsonDoubleFromDecimal(uint64_t mantissa, int exp10, int negative);

#endif /* JSON_NUM_H */
