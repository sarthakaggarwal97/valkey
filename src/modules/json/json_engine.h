/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_engine.h -- storage engine of the built-in JSON module.
 *
 * The engine owns the in-memory form of a document, the parser, the
 * serialiser and the global key table. It includes no server header and takes
 * its allocator from the caller, so it also builds as a standalone test
 * program.
 *
 * Threads. Every function is called on the main thread, with two exceptions
 * that the engine supports: jsonDocFree may run on a lazy-free thread, and
 * jsonSerialize (with everything it calls) may run on a save thread while the
 * main thread works on other documents. */

#ifndef JSON_ENGINE_H
#define JSON_ENGINE_H

#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------- */

typedef struct jsonAllocator {
    void *(*alloc)(size_t n);
    void *(*realloc)(void *p, size_t n);
    void (*free)(void *p);
    size_t (*usable)(void *p); /* Usable size of a live allocation. */
} jsonAllocator;

/* Must be called once before anything else. */
void jsonEngineInit(const jsonAllocator *a);
/* The allocator given to jsonEngineInit, for code that works beside the
 * engine and must allocate the same way. */
const jsonAllocator *jsonEngineAllocator(void);
/* Seeds the hash of the key table. Call once, after jsonEngineInit and before
 * any key is interned. Without it a fixed seed is used. */
void jsonEngineSetHashSeed(const uint8_t seed[16]);
/* Caps for the packed form (json.max-packed-bytes, json.max-packed-entries).
 * They apply to nodes built after the call. */
void jsonEngineSetCaps(uint32_t max_packed_bytes, uint32_t max_packed_entries);
/* Frees the key table. Returns the number of keys still referenced, which is
 * zero when every document and value was freed. For tests. */
size_t jsonEngineShutdown(void);

typedef struct jsonEngineStats {
    size_t keytable_bytes; /* Memory held by the key table. */
    size_t keytable_keys;  /* Live keys. */
    /* Name storage of the keys that are referenced now. Unlike keytable_bytes
     * it falls as soon as the last reference to a key is dropped, on whatever
     * thread, and so returns to its earlier value when a document is added
     * and removed. */
    size_t keytable_live_bytes;
} jsonEngineStats;
void jsonEngineGetStats(jsonEngineStats *s);

/* Releases key table slots whose last reference was dropped. Does a bounded
 * amount of work and returns 1 if more remains. Called from the server cron. */
int jsonKeyTableSweep(void);

/* ----------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

/* A document: one 8-byte word. 0 means "no document". */
typedef uint64_t jsonDoc;

typedef enum {
    JSON_NULL = 0,
    JSON_FALSE,
    JSON_TRUE,
    JSON_INT,    /* Integer syntax within int64. */
    JSON_DOUBLE, /* A number held as a double: its text is jsonDoublePrint(). */
    JSON_RAWNUM, /* A number held as its source text. */
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} jsonType;

/* A reference to one value inside a document. It stays valid until the
 * document is changed or freed. The fields are private to the engine. */
typedef struct jsonRef {
    const uint8_t *p;
    uint64_t w;
    uint32_t a, b;
    uint8_t kind, sub;
} jsonRef;

/* One step of a locator. A locator names a value by the path of children
 * from the root, and stays meaningful across changes to other parts of the
 * document, which a jsonRef does not.
 *   - In an array: keyid is 0 and pos is the element index.
 *   - In an object: keyid is the member's key ID and identifies it. pos is the
 *     member's position when the step was recorded and is used for ordering
 *     only. */
typedef struct jsonStep {
    uint32_t pos;
    uint32_t keyid;
} jsonStep;

/* ----------------------------------------------------------------------------
 * Values: parsed or built, not yet part of a document
 * ------------------------------------------------------------------------- */

typedef struct jsonVal jsonVal;

typedef enum {
    JSON_OK = 0,
    JSON_ERR_SYNTAX,    /* Not valid JSON. */
    JSON_ERR_UTF8,      /* Invalid UTF-8 in a string (untrusted input only). */
    JSON_ERR_NUM_RANGE, /* A number too large for a double (untrusted only). */
    JSON_ERR_DEPTH,     /* Nesting deeper than max_depth. */
    JSON_ERR_TOO_BIG    /* Longer than the engine can represent. */
} jsonErr;

/* Trusted input (RDB, RESTORE, replication stream): skips the UTF-8 check and
 * the number range check. Pass max_depth 0 for no limit. */
#define JSON_PARSE_TRUSTED (1 << 0)

/* Parses one JSON text. Returns NULL and sets *err on failure. *err_off, if
 * not NULL, receives the byte offset of the error. A duplicate member name
 * keeps the last value at the first position, as the module does. */
jsonVal *jsonValParse(const char *s, size_t n, int flags, uint32_t max_depth, jsonErr *err, size_t *err_off);

jsonVal *jsonValNull(void);
jsonVal *jsonValBool(int v);
jsonVal *jsonValInt(int64_t v);
jsonVal *jsonValDouble(double d);                 /* Finite. */
jsonVal *jsonValNumText(const char *s, size_t n); /* Valid JSON number text. Classified as INT, DOUBLE or RAWNUM. */
jsonVal *jsonValString(const char *s, size_t n);  /* Decoded bytes, not JSON-escaped. */
jsonVal *jsonValEmptyArray(void);
jsonVal *jsonValEmptyObject(void);
jsonVal *jsonValFromRef(const jsonRef *r); /* Deep copy of a value inside a document. */

jsonType jsonValType(const jsonVal *v);       /* Type of the value itself (its root). */
uint32_t jsonValDepth(const jsonVal *v);      /* 0 for a scalar, 1 for [] or [1], ... */
size_t jsonValSizeEstimate(const jsonVal *v); /* Bytes it would add to a document. */
jsonDoc jsonValToDoc(jsonVal *v);             /* Becomes a whole document. Consumes v. */
void jsonValFree(jsonVal *v);                 /* Accepts NULL. */

/* ----------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

void jsonDocRoot(jsonDoc d, jsonRef *out);

jsonType jsonRefType(const jsonRef *r);
/* Elements of an array or members of an object. 0 for anything else. */
uint32_t jsonRefCount(const jsonRef *r);
/* 1 and *out for JSON_INT, else 0. */
int jsonRefInt(const jsonRef *r, int64_t *out);
/* The value of any number as a double. 0.0 for a non-number. */
double jsonRefDouble(const jsonRef *r);
/* Decoded bytes of a string. The pointer is valid while *r and the document
 * are unchanged: a short string can live inside *r itself. */
const char *jsonRefString(const jsonRef *r, uint32_t *len);
/* The text of any number. buf must hold JSON_NUM_BUF bytes and is used when
 * the text has to be produced. Returns the text and sets *len. */
#define JSON_NUM_BUF 40
const char *jsonRefNumText(const jsonRef *r, char *buf, uint32_t *len);

/* Element `index` of an array. Returns 0 if out of range or not an array. */
int jsonArrayGet(const jsonRef *arr, uint32_t index, jsonRef *out);
/* Member of an object by name. Returns 0 if absent or not an object. On
 * success fills *out and, if step is not NULL, the member's locator step. */
int jsonObjectGet(const jsonRef *obj, const char *name, size_t len, jsonRef *out, jsonStep *step);
/* The same by key ID (see jsonKeyFind). */
int jsonObjectGetById(const jsonRef *obj, uint32_t keyid, jsonRef *out, jsonStep *step);
/* The child a locator step names. Returns 0 if it is not there. */
int jsonChildByStep(const jsonRef *container, const jsonStep *step, jsonRef *out);
/* Resolves a whole locator from the root. Returns 0 if any step is missing. */
int jsonDocResolve(jsonDoc d, const jsonStep *loc, int n, jsonRef *out);

/* Iterates the children of an array or object in document order. The fields
 * are private to the engine. */
typedef struct jsonIter {
    const uint8_t *p, *end;
    uint64_t base;
    uint32_t i, n, a, b;
    uint8_t kind, sub;
} jsonIter;
void jsonIterInit(jsonIter *it, const jsonRef *container);
/* Returns 0 at the end. Otherwise fills *val and *step. For an object member
 * it also sets *name and *name_len (decoded bytes, owned by the key table and
 * valid while the document holds the member). name may be NULL. */
int jsonIterNext(jsonIter *it, jsonRef *val, jsonStep *step, const char **name, uint32_t *name_len);

/* Deep equality as JSON.ARRINDEX defines it in the module. */
int jsonRefEquals(const jsonRef *a, const jsonRef *b);
/* Nesting depth below this value: 0 for a scalar. Not recursive. */
uint32_t jsonRefDepth(const jsonRef *r);
/* Number of values below and including this one, as JSON.DEBUG FIELDS counts
 * them in the module. */
uint64_t jsonRefFields(const jsonRef *r);
/* Bytes this value occupies inside its document (JSON.DEBUG MEMORY with a
 * path, and the size-limit estimate for a value about to be removed). */
size_t jsonRefMemUsage(const jsonRef *r);

/* ----------------------------------------------------------------------------
 * Key table
 * ------------------------------------------------------------------------- */

/* Looks a member name up without creating it. Returns its ID and takes one
 * reference, or returns 0 if no document uses the name. */
uint32_t jsonKeyFind(const char *name, size_t len);
void jsonKeyRelease(uint32_t keyid);
/* The decoded bytes of a key. */
const char *jsonKeyName(uint32_t keyid, uint32_t *len);

/* ----------------------------------------------------------------------------
 * Serialising
 * ------------------------------------------------------------------------- */

/* A growable output buffer. It starts in caller memory (often a stack array)
 * and moves to the heap only if it has to. */
typedef struct jsonBuf {
    char *p;
    size_t len, cap;
    int heap;
} jsonBuf;
void jsonBufInit(jsonBuf *b, char *initial, size_t cap);
void jsonBufAppend(jsonBuf *b, const char *s, size_t n);
/* Makes room for n more bytes, so that writing them does not grow the buffer
 * again. Pairs with jsonSerializeSizeHint. */
void jsonBufReserve(jsonBuf *b, size_t n);
void jsonBufFree(jsonBuf *b);

/* NULL members mean "none". A NULL jsonFormat means compact output.
 * initial_level is the indent level the value itself starts at: the number
 * of containers the caller has open around it in the text it is building. */
typedef struct jsonFormat {
    const char *indent, *newline, *space;
    size_t indent_len, newline_len, space_len;
    uint32_t initial_level;
} jsonFormat;

/* Appends the JSON text of a value. Not recursive. */
void jsonSerialize(const jsonRef *r, const jsonFormat *fmt, jsonBuf *out);
/* A cheap estimate of the size of the compact text of a value, for reserving
 * the output buffer once. It is an upper bound for a typed array, and a
 * guess that is usually a little high for anything else: the buffer still
 * grows if the text turns out longer. */
size_t jsonSerializeSizeHint(const jsonRef *r);
/* Appends s as a JSON string literal, quoted and escaped. */
void jsonBufAppendQuoted(jsonBuf *b, const char *s, size_t n);

/* ----------------------------------------------------------------------------
 * Whole-document operations
 * ------------------------------------------------------------------------- */

jsonDoc jsonDocCopy(jsonDoc d);
void jsonDocFree(jsonDoc d); /* May run on a lazy-free thread. */
/* Allocator bytes held by the document, O(1). */
size_t jsonDocMemUsage(jsonDoc d);
/* 1 for a packed root, else the number of heap nodes. */
size_t jsonDocFreeEffort(jsonDoc d);
/* "jpack" or "jtree", for diagnostics. */
const char *jsonDocEncoding(jsonDoc d);

/* Active defrag. `move` is given a pointer the engine allocated and returns
 * its new address or NULL if it was not moved. `stop`, if not NULL, is polled
 * between steps and returns nonzero when the time slice is over. *cursor is 0
 * on the first call. Returns 1 if more work remains (and *cursor says where
 * to resume), 0 when the document is done. The document may have been changed
 * between calls. */
int jsonDocDefrag(jsonDoc *d, void *(*move)(void *ptr, void *ctx), int (*stop)(void *ctx), void *ctx, unsigned long *cursor);

/* ----------------------------------------------------------------------------
 * Changing a document
 *
 * What one string and one array can hold. Lengths and counts are 32 bits, so
 * a caller that makes a string longer or an array larger checks the result
 * against these first. */
#define JSON_STRING_MAX 0xFFFFFFF0u /* Bytes of a string. */
#define JSON_ARRAY_MAX 0xFFFFFFF0u  /* Elements of an array. */

/* These cannot fail: the caller has resolved the locator and checked every
 * limit first. `loc` with n == 0 names the root. Each call may replace *d.
 * A caller applying several changes in one command applies them so that no
 * change invalidates a locator still to be used: within one array, higher
 * indexes first.
 * ------------------------------------------------------------------------- */

/* Replaces the value at loc with a copy of v. v is unchanged and reusable. */
void jsonSet(jsonDoc *d, const jsonStep *loc, int n, const jsonVal *v);
/* Scalar replacements that need no jsonVal. */
void jsonSetInt(jsonDoc *d, const jsonStep *loc, int n, int64_t v);
void jsonSetDouble(jsonDoc *d, const jsonStep *loc, int n, double v);
void jsonSetNumText(jsonDoc *d, const jsonStep *loc, int n, const char *s, size_t len);
void jsonSetBool(jsonDoc *d, const jsonStep *loc, int n, int v);
/* Removes an array element or an object member. n >= 1. */
void jsonDelete(jsonDoc *d, const jsonStep *loc, int n);
/* Adds a member at the end of the object at loc. The name must be absent. */
void jsonObjectAdd(jsonDoc *d, const jsonStep *loc, int n, const char *name, size_t len, const jsonVal *v);
/* Inserts copies of vals before element `index` of the array at loc.
 * index == length appends. */
void jsonArrayInsert(jsonDoc *d, const jsonStep *loc, int n, uint32_t index, const jsonVal *const *vals, uint32_t count);
/* Keeps `count` elements of the array at loc starting at `start` and removes
 * the rest. */
void jsonArrayTrim(jsonDoc *d, const jsonStep *loc, int n, uint32_t start, uint32_t count);
/* Appends decoded bytes to the string at loc. */
void jsonStringAppend(jsonDoc *d, const jsonStep *loc, int n, const char *s, size_t len);

#endif /* JSON_ENGINE_H */
