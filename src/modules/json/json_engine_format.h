/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_engine_format.h -- byte formats of the JSON storage engine.
 *
 * Private to json_engine.c. It defines the two encodings a document is made
 * of and the helpers that read them. Only json_engine.c writes them.
 *
 * A document is a tree in which any subtree that fits the caps is one PACKED
 * node, and what does not fit is built from "spine" nodes that point at
 * their children. Both encodings are little-endian and 64-bit only.
 *
 *
 * 1. The packed format
 * --------------------
 *
 * A packed value is a tag byte followed by a payload. It holds no pointers,
 * so a packed subtree can be moved or copied as bytes.
 *
 *   Tag          Payload                                           Bytes
 *   -----------  ------------------------------------------------  ---------
 *   NULL         -                                                 1
 *   FALSE, TRUE  -                                                 1
 *   INT0 + v     - (integer v, 0..31, in the tag)                  1
 *   I8..I64      little-endian two's complement integer            2,3,5,9
 *   F64          IEEE-754 double                                   9
 *   DEC(k, w)    decimal with k fraction digits, k <= 15: its      2,3,5,9
 *                digits as a two's complement integer of 1, 2, 4 or
 *                8 bytes
 *   DECX         scale:1, digits:8: a decimal of any scale         10
 *   RAW8         len:1, number text                                2 + n
 *   RAW16        len:2, number text                                3 + n
 *   SSTR + n     n string bytes, n <= 63, escaping needed on output  1 + n
 *   SSTR_NE + n  n string bytes, n <= 63, no escaping needed        1 + n
 *   STR16        len:2, bytes, escaping needed                     3 + n
 *   STR16_NE     len:2, bytes, no escaping needed                  3 + n
 *   ARR8         size:1, count:1, values                           3 + size
 *   ARR16        size:2, count:2, values                           5 + size
 *   OBJ8         size:1, count:1, (key, value) pairs               3 + size
 *   OBJ16        size:2, count:2, (key, value) pairs               5 + size
 *   TARR(e)      count:1, elements                                 2 + n*w
 *   TARR(e)+1    count:2, elements                                 3 + n*w
 *   TARR(decimal kinds)  count:1 or 2, scale:1, elements           3|4 + n*w
 *
 * Strings are stored unescaped. A key is the varint ID of the member name in
 * the key table: 7 bits per byte, low bits first, the high bit set on every
 * byte but the last. ID 0 is never used, so a key never starts with 0x00.
 *
 * `size` is the number of bytes of values (and keys) that follow the header,
 * so any value can be stepped over without looking inside it. A size or an
 * integer field may be wider than its content needs.
 *
 * A decimal (design 5.4) is a number written in plain notation with k
 * fraction digits, 1 <= k <= 31, and at most 18 digits from its first
 * non-zero one, other than a negative zero. It is held as the integer M that
 * its digits spell without the point, |M| < 10^18, and the scale k: 19.90 is
 * (1990, 2). Its text is M with the point put back, so it is reproduced
 * exactly without printing or parsing a double. Its value is M / 10^k,
 * correctly rounded: one division when |M| <= 2^53 and k <= 22, else
 * jsonDoubleFromDecimal(). The scale and the width of M are in the tag of a
 * packed decimal when the scale is at most 15. Above that the tag is PK_DECX
 * and a scale byte and eight bytes of M follow.
 *
 * A typed array (TARR) holds only numbers of one kind, as raw little-endian
 * elements of width w: integers of 1, 2, 4 or 8 bytes, doubles, or decimals
 * that all have one scale, as their M in 1, 2, 4 or 8 bytes. The emitter
 * makes one only from a non-empty array, but removing elements can leave it
 * empty.
 *
 *
 * 2. The spine
 * ------------
 *
 * A value slot is one 64-bit word. Its low three bits are a tag:
 *
 *   000              pointer to a heap node (the node's first byte is its kind)
 *   001 101          a double: the word is the IEEE bits rotated left by 3,
 *                    so the tag is its sign and the top two exponent bits.
 *                    These are the doubles of magnitude in [2^-511, 2).
 *   010 110          a decimal, positive or negative: the upper 61 bits are
 *                    |M| * 22 + (scale - 1), for a scale of 1..22 and |M|
 *                    up to 61 bits / 22, which is every |M| of 17 digits
 *   011              a signed 61-bit integer in the upper bits
 *   100              a string of up to 7 bytes: byte 0 is tag, length and
 *                    flags, bytes 1..7 are the string
 *   111              null, false, true, +0.0, -0.0 or, in an object entry, a
 *                    key ID in the upper 32 bits
 *
 * Heap nodes. Offsets are in bytes; every multi-byte field is little-endian.
 *
 *   PACKED   [0] kind  [1..2] blob length  [3..] one packed value
 *
 *   STR      [0] kind and flags  [1] length              [2..] bytes
 *            [0] kind and flags, LONG set  [1..4] length [5..] bytes
 *            Flags: NOESC (no escaping needed), RAWNUM (the bytes are the
 *            text of a number, not a string).
 *
 *   BOXNUM   [0] kind  [1] 0 = int64, 1 = double, 2 = decimal
 *            [2] scale of a decimal  [8..15] value, or the M of a decimal
 *            An integer beyond 61 bits, a double whose rotated form does
 *            not carry one of the two double tags, or a decimal that a word
 *            does not hold: 18 digits, or a scale above 22.
 *
 *   TARRAY   [0] kind  [1] element kind  [2] scale of a decimal kind
 *            [4..7] length  [8..11] capacity  [16..] raw elements
 *            Besides the kinds of a packed typed array there are two more.
 *            Strings of at most 15 bytes, 16 bytes an element: byte 0 is the
 *            length and the no-escape flag, the string follows. And null,
 *            false and true, a byte an element: its packed tag.
 *
 *   ARRAY    [0] kind  [4..7] length  [8..11] capacity
 *            [16..23] node count  [24..31] byte count  [32..] words
 *
 *   OBJECT   [0] kind  [1] index entry width (1, 2 or 4)
 *            [2] log2 of the index size  [4..7] entries used
 *            [8..11] entry capacity  [12..15] live members
 *            [16..23] node count  [24..31] byte count
 *            [32..] entries: capacity pairs of (key word, value word)
 *            then the index: open addressing, each cell the number of an
 *            entry or all ones when empty.
 *            Entries keep insertion order. A removed member leaves an entry
 *            whose key word is 0.
 *
 * The node count and byte count of an ARRAY or OBJECT are maintained only in
 * the root node of a document. They give jsonDocMemUsage and
 * jsonDocFreeEffort in O(1). */

#ifndef JSON_ENGINE_FORMAT_H
#define JSON_ENGINE_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ || UINTPTR_MAX != UINT64_MAX
#error "the JSON engine supports 64-bit little-endian targets only"
#endif

#define JSON_LIKELY(x) __builtin_expect(!!(x), 1)
#define JSON_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define JSON_FALLTHROUGH __attribute__((fallthrough))

/* ----------------------------------------------------------------------------
 * Unaligned access
 * ------------------------------------------------------------------------- */

static inline uint16_t loadU16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t loadU32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t loadU64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline double loadF64(const uint8_t *p) {
    double v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void storeU16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, sizeof(v));
}

static inline void storeU32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static inline void storeU64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, sizeof(v));
}

static inline uint64_t doubleBits(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static inline double bitsDouble(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

/* ----------------------------------------------------------------------------
 * Packed tags
 * ------------------------------------------------------------------------- */

enum {
    PK_NULL = 0x00,
    PK_FALSE = 0x01,
    PK_TRUE = 0x02,
    PK_I8 = 0x03,
    PK_I16 = 0x04,
    PK_I32 = 0x05,
    PK_I64 = 0x06,
    PK_F64 = 0x07,
    PK_STR16 = 0x08,
    PK_STR16_NE = 0x09,
    PK_ARR8 = 0x0A,
    PK_ARR16 = 0x0B,
    PK_OBJ8 = 0x0C,
    PK_OBJ16 = 0x0D,
    PK_TARR = 0x0E,     /* PK_TARR + 2 * (element kind - 1) + wide: 0x0E..0x1F */
    PK_TARR_END = 0x20, /* One past the last typed array tag. */
    PK_INT0 = 0x20,     /* 0x20..0x3F: the integers 0..31 */
    PK_SSTR = 0x40,     /* 0x40..0x7F: short string, escaping needed */
    PK_SSTR_NE = 0x80,  /* 0x80..0xBF: short string, no escaping needed */
    PK_DEC = 0xC0,      /* PK_DEC + 4 * (scale - 1) + log2(width of M), scale 1..15: 0xC0..0xFB */
    PK_RAW8 = 0xFC,
    PK_RAW16 = 0xFD,
    PK_DECX = 0xFE /* A decimal of scale 16 or more: scale:1, M:8 */
};

#define PK_INT0_MAX 31        /* Largest integer held in a tag. */
#define PK_SSTR_MAX 63        /* Longest string held with its length in the tag. */
#define PK_SSTR_LEN_MASK 0x3F /* Length bits of a short string tag. */
#define PK_LEN16_MAX 65535    /* Longest string, raw number or container payload. */
#define PK_RAW8_MAX 255       /* Longest raw number with a one-byte length. */
#define PK_NARROW_MAX 255     /* Largest size or count in a one-byte field. */
#define PK_HDR_NARROW 3       /* tag, size:1, count:1 */
#define PK_HDR_WIDE 5         /* tag, size:2, count:2 */

/* Decimals. */
#define DEC_SCALE_MAX 31    /* Most fraction digits: the text then still fits JSON_NUM_BUF with room to spare. */
#define DEC_DIGITS_MAX 18   /* Most digits from the first non-zero one: |M| < 10^18 fits an int64. */
#define PK_DEC_SCALE_MAX 15 /* Largest scale a PK_DEC tag holds. */
#define PK_DECX_SIZE 10     /* tag, scale:1, M:8 */

/* Element kinds of a typed array. The four integer kinds and the four decimal
 * kinds are in order of width, and a decimal kind is its integer kind plus
 * EK_DEC_BASE. */
enum {
    EK_NONE = 0,
    EK_I8,
    EK_I16,
    EK_I32,
    EK_I64,
    EK_F64,
    EK_D8,
    EK_D16,
    EK_D32,
    EK_D64,
    EK_PACKED_END,          /* Kinds up to here can be packed. */
    EK_STR = EK_PACKED_END, /* Strings of at most 15 bytes: TARRAY nodes only. */
    EK_LIT                  /* null, false and true as PK_NULL, PK_FALSE and PK_TRUE: TARRAY nodes only. */
};

#define EK_DEC_BASE (EK_D8 - EK_I8)
#define EK_STR_SIZE 16 /* Bytes per element of a string array. */
#define EK_STR_MAX 15  /* Longest string it holds. */
#define EK_STR_LEN_MASK 0x0F
#define EK_STR_NOESC 0x80 /* In an element's first byte: no escaping needed. */

static const uint8_t EK_WIDTH[12] = {0, 1, 2, 4, 8, 8, 1, 2, 4, 8, EK_STR_SIZE, 1};

static inline int ekIsInt(uint32_t ekind) {
    return ekind >= EK_I8 && ekind <= EK_I64;
}

static inline int ekIsDec(uint32_t ekind) {
    return ekind >= EK_D8 && ekind <= EK_D64;
}

/* The integer kind that holds the elements of an integer or decimal array. */
static inline uint32_t ekIntKind(uint32_t ekind) {
    return ekind >= EK_D8 ? ekind - EK_DEC_BASE : ekind;
}

static inline int pkIsSmallInt(uint8_t tag) {
    return tag >= PK_INT0 && tag < PK_SSTR;
}

static inline int pkIsString(uint8_t tag) {
    return (tag >= PK_SSTR && tag < PK_DEC) || tag == PK_STR16 || tag == PK_STR16_NE;
}

static inline int pkIsDec(uint8_t tag) {
    return (tag >= PK_DEC && tag < PK_RAW8) || tag == PK_DECX;
}

/* Bytes of a packed decimal, tag included. */
static inline uint32_t pkDecSize(uint8_t tag) {
    return tag == PK_DECX ? PK_DECX_SIZE : 1 + ((uint32_t)1 << (tag & 3));
}

static inline int pkIsArray(uint8_t tag) {
    return tag == PK_ARR8 || tag == PK_ARR16;
}

static inline int pkIsObject(uint8_t tag) {
    return tag == PK_OBJ8 || tag == PK_OBJ16;
}

static inline int pkIsTyped(uint8_t tag) {
    return tag >= PK_TARR && tag < PK_TARR_END;
}

/* A container whose children are packed values: array or object, not typed. */
static inline int pkIsNested(uint8_t tag) {
    return tag >= PK_ARR8 && tag <= PK_OBJ16;
}

/* ----------------------------------------------------------------------------
 * Key IDs as varints
 * ------------------------------------------------------------------------- */

static inline uint8_t *varintPut(uint8_t *p, uint32_t v) {
    while (v >= 0x80) {
        *p++ = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static inline const uint8_t *varintGet(const uint8_t *p, uint32_t *v) {
    uint32_t b = *p++;
    if (JSON_LIKELY(b < 0x80)) {
        *v = b;
        return p;
    }
    uint32_t r = b & 0x7F;
    int shift = 7;
    do {
        b = *p++;
        r |= (b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    *v = r;
    return p;
}

static inline uint32_t varintSize(uint32_t v) {
    if (v < 0x80) return 1;
    if (v < 0x4000) return 2;
    if (v < 0x200000) return 3;
    return v < 0x10000000 ? 4 : 5;
}

/* ----------------------------------------------------------------------------
 * Reading packed values
 * ------------------------------------------------------------------------- */

/* A plain array or object at p: its first child, its end and its count. */
static inline const uint8_t *pkOpen(const uint8_t *p, const uint8_t **end, uint32_t *count) {
    const uint8_t *first;
    uint32_t size;
    if (*p & 1) { /* The 16-bit form of each pair of tags is the odd one. */
        size = loadU16(p + 1);
        *count = loadU16(p + 3);
        first = p + PK_HDR_WIDE;
    } else {
        size = p[1];
        *count = p[2];
        first = p + PK_HDR_NARROW;
    }
    *end = first + size;
    return first;
}

/* A typed array at p: its first element, element kind, count and, for a
 * decimal kind, scale. */
static inline const uint8_t *pkTypedOpen(const uint8_t *p, uint32_t *ekind, uint32_t *count, uint8_t *scale) {
    const uint32_t x = (uint32_t)(*p - PK_TARR);
    const uint8_t *q;
    *ekind = x / 2 + 1;
    if (x & 1) {
        *count = loadU16(p + 1);
        q = p + 3;
    } else {
        *count = p[1];
        q = p + 2;
    }
    *scale = ekIsDec(*ekind) ? *q++ : 0;
    return q;
}

/* The byte after the packed value at p. */
static inline const uint8_t *pkSkip(const uint8_t *p) {
    const uint8_t tag = *p;
    if (tag >= PK_INT0) {
        /* Small integers and short strings first, and without a branch
         * between the two: they alternate in most documents. */
        if (JSON_LIKELY(tag < PK_DEC)) return p + 1 + (tag >= PK_SSTR ? (tag & PK_SSTR_LEN_MASK) : 0);
        if (tag < PK_RAW8) return p + 1 + ((size_t)1 << (tag & 3));
        if (tag == PK_DECX) return p + PK_DECX_SIZE;
        return tag == PK_RAW8 ? p + 2 + p[1] : p + 3 + loadU16(p + 1);
    }
    switch (tag) {
    case PK_NULL:
    case PK_FALSE:
    case PK_TRUE: return p + 1;
    case PK_I8: return p + 2;
    case PK_I16: return p + 3;
    case PK_I32: return p + 5;
    case PK_I64:
    case PK_F64: return p + 9;
    case PK_STR16:
    case PK_STR16_NE: return p + 3 + loadU16(p + 1);
    case PK_ARR8:
    case PK_OBJ8: return p + PK_HDR_NARROW + p[1];
    case PK_ARR16:
    case PK_OBJ16: return p + PK_HDR_WIDE + loadU16(p + 1);
    default: {
        uint32_t ekind, count;
        uint8_t scale;
        const uint8_t *q = pkTypedOpen(p, &ekind, &count, &scale);
        return q + (size_t)count * EK_WIDTH[ekind];
    }
    }
}

/* One integer element of a typed array, or the M of a decimal one. */
static inline int64_t ekInt(const uint8_t *p, uint32_t ekind) {
    switch (ekIntKind(ekind)) {
    case EK_I8: return (int8_t)p[0];
    case EK_I16: return (int16_t)loadU16(p);
    case EK_I32: return (int32_t)loadU32(p);
    default: return (int64_t)loadU64(p);
    }
}

/* The integer held by an integer tag. The caller checked the tag. */
static inline int64_t pkInt(const uint8_t *p) {
    switch (*p) {
    case PK_I8: return (int8_t)p[1];
    case PK_I16: return (int16_t)loadU16(p + 1);
    case PK_I32: return (int32_t)loadU32(p + 1);
    case PK_I64: return (int64_t)loadU64(p + 1);
    default: return *p - PK_INT0;
    }
}

static inline int pkIsInt(uint8_t tag) {
    return pkIsSmallInt(tag) || (tag >= PK_I8 && tag <= PK_I64);
}

/* The M of the packed decimal at p, and its scale. */
static inline int64_t pkDecimal(const uint8_t *p, uint32_t *scale) {
    if (*p == PK_DECX) {
        *scale = p[1];
        return (int64_t)loadU64(p + 2);
    }
    *scale = ((uint32_t)(*p - PK_DEC) >> 2) + 1;
    return ekInt(p + 1, EK_I8 + (*p & 3));
}

/* The bytes of a string or raw number at p. */
static inline const uint8_t *pkText(const uint8_t *p, uint32_t *len) {
    const uint8_t tag = *p;
    if (tag >= PK_SSTR && tag < PK_DEC) {
        *len = tag & PK_SSTR_LEN_MASK;
        return p + 1;
    }
    if (tag == PK_RAW8) {
        *len = p[1];
        return p + 2;
    }
    *len = loadU16(p + 1);
    return p + 3;
}

/* ----------------------------------------------------------------------------
 * Spine words
 * ------------------------------------------------------------------------- */

#define W_TAG_MASK 7
#define W_PTR 0
#define W_INT 3
#define W_STR 4
#define W_SPECIAL 7

/* Special words: the kind is in bits 3..7. */
#define W_SPECIAL_MASK 0xFF
#define W_NULL (W_SPECIAL | (0 << 3))
#define W_FALSE (W_SPECIAL | (1 << 3))
#define W_TRUE (W_SPECIAL | (2 << 3))
#define W_PZERO (W_SPECIAL | (3 << 3)) /* +0.0 */
#define W_NZERO (W_SPECIAL | (4 << 3)) /* -0.0 */
#define W_KEYID (W_SPECIAL | (5 << 3)) /* Key ID in the upper 32 bits. */

/* Inline decimals: the tag carries the sign, the 61 bits above it hold
 * |M| * W_DEC_SCALES + (scale - 1). 22 scales are what plain notation needs
 * for the shortest text of any double down to 1e-6, and leave |M| room for
 * 17 digits. */
#define W_DEC_POS 2
#define W_DEC_NEG 6
#define W_DEC_SHIFT 3
#define W_DEC_SCALES 22
#define W_DEC_MAGNITUDE_MAX ((((uint64_t)1 << 61) - W_DEC_SCALES) / W_DEC_SCALES)

/* Inline strings: byte 0 is 100 in bits 0..2, the length in bits 3..5 and
 * two flags. */
#define W_STR_MAX 7
#define W_STR_LEN_SHIFT 3
#define W_STR_NOESC 0x40
#define W_STR_RAWNUM 0x80

/* Inline integers. */
#define W_INT_SHIFT 3
#define W_INT_MIN (-((int64_t)1 << 60))
#define W_INT_MAX (((int64_t)1 << 60) - 1)

static inline unsigned wordTag(uint64_t w) {
    return (unsigned)(w & W_TAG_MASK);
}

static inline int wordIsDouble(uint64_t w) {
    const unsigned tag = wordTag(w);
    return tag == 1 || tag == 5;
}

static inline double wordDouble(uint64_t w) {
    return bitsDouble((w >> 3) | (w << 61));
}

static inline int wordIsDec(uint64_t w) {
    const unsigned tag = wordTag(w);
    return tag == W_DEC_POS || tag == W_DEC_NEG;
}

static inline uint64_t decMagnitude(int64_t mantissa) {
    return mantissa < 0 ? 0 - (uint64_t)mantissa : (uint64_t)mantissa;
}

/* 1 if a word holds the decimal. Otherwise it is a BOXNUM. */
static inline int wordDecFits(int64_t mantissa, uint32_t scale) {
    return scale <= W_DEC_SCALES && decMagnitude(mantissa) <= W_DEC_MAGNITUDE_MAX;
}

static inline uint64_t wordDec(int64_t mantissa, uint32_t scale) {
    return ((decMagnitude(mantissa) * W_DEC_SCALES + (scale - 1)) << W_DEC_SHIFT) | (mantissa < 0 ? W_DEC_NEG : W_DEC_POS);
}

/* The M of an inline decimal, and its scale. */
static inline int64_t wordDecimal(uint64_t w, uint32_t *scale) {
    const uint64_t x = w >> W_DEC_SHIFT, magnitude = x / W_DEC_SCALES;
    *scale = (uint32_t)(x - magnitude * W_DEC_SCALES) + 1;
    return wordTag(w) == W_DEC_NEG ? -(int64_t)magnitude : (int64_t)magnitude;
}

static inline uint64_t wordKey(uint32_t keyid) {
    return ((uint64_t)keyid << 32) | W_KEYID;
}

static inline uint32_t wordKeyId(uint64_t w) {
    return (uint32_t)(w >> 32);
}

/* ----------------------------------------------------------------------------
 * Heap nodes
 * ------------------------------------------------------------------------- */

enum {
    NK_STR = 1,
    NK_BOXNUM,
    NK_PACKED,
    NK_ARRAY,
    NK_TARRAY,
    NK_OBJECT
};

#define NK_MASK 0x0F
#define STR_NOESC 0x10
#define STR_RAWNUM 0x20
#define STR_LONG 0x40
#define STR_SHORT_MAX 255
#define STR_SHORT_HDR 2
#define STR_LONG_HDR 5

#define PACKED_HDR 3

#define BOXNUM_SIZE 16
#define BOXNUM_VALUE 8
#define BOXNUM_SCALE 2
#define BOXNUM_INT 0
#define BOXNUM_DOUBLE 1
#define BOXNUM_DEC 2

#define TARRAY_HDR 16
#define TARRAY_EKIND 1
#define TARRAY_SCALE 2

/* ARRAY and OBJECT share these offsets. Bytes 1..3 and 12..15 are the OBJECT's own. */
#define SPINE_HDR 32
#define SPINE_LEN 4 /* ARRAY: elements. OBJECT: entries used. */
#define SPINE_CAP 8
#define SPINE_NODES 16
#define SPINE_BYTES 24
#define OBJECT_IDXW 1
#define OBJECT_LG 2
#define OBJECT_LIVE 12
#define OBJECT_ENTRY 16 /* Bytes per entry: key word, value word. */
#define OBJECT_MIN_LG 2
#define OBJECT_HASH_MULT 0x9E3779B1u
#define OBJECT_NO_ENTRY 0xFFFFFFFFu

static inline uint8_t *wordNode(uint64_t w) {
    return (uint8_t *)(uintptr_t)w;
}

static inline uint64_t nodeWord(const uint8_t *n) {
    return (uint64_t)(uintptr_t)n;
}

static inline unsigned nodeKind(const uint8_t *n) {
    return n[0] & NK_MASK;
}

static inline uint32_t nodeLen(const uint8_t *n) {
    return loadU32(n + SPINE_LEN);
}

/* The words of an ARRAY, or the entries of an OBJECT. A node comes from the
 * allocator, so offset 32 is aligned for a 64-bit word. */
static inline uint64_t *nodeWords(const uint8_t *n) {
    return (uint64_t *)(void *)(uintptr_t)(n + SPINE_HDR);
}

static inline const uint8_t *tarrayData(const uint8_t *n) {
    return n + TARRAY_HDR;
}

static inline const uint8_t *strNodeBytes(const uint8_t *n, uint32_t *len) {
    if (n[0] & STR_LONG) {
        *len = loadU32(n + 1);
        return n + STR_LONG_HDR;
    }
    *len = n[1];
    return n + STR_SHORT_HDR;
}

static inline const uint8_t *objectIndex(const uint8_t *n) {
    return n + SPINE_HDR + (size_t)loadU32(n + SPINE_CAP) * OBJECT_ENTRY;
}

static inline uint32_t objectIndexGet(const uint8_t *index, uint32_t width, uint32_t i) {
    if (width == 1) return index[i] == 0xFF ? OBJECT_NO_ENTRY : index[i];
    if (width == 2) {
        const uint16_t v = loadU16(index + (size_t)i * 2);
        return v == 0xFFFF ? OBJECT_NO_ENTRY : v;
    }
    return loadU32(index + (size_t)i * 4);
}

static inline uint32_t objectHome(uint32_t keyid, uint32_t lg) {
    return (keyid * OBJECT_HASH_MULT) >> (32 - lg);
}

#endif /* JSON_ENGINE_FORMAT_H */
