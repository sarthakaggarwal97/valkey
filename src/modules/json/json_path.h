/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_path.h -- path compiler and evaluator of the built-in JSON module.
 *
 * A path string in either syntax (legacy ".a.b[0]" or JSONPath "$.a[?(@.b>1)]")
 * is compiled once into a flat step array, then evaluated against a document.
 * Evaluation yields matches in selector order. Each match carries a reference
 * to the value (valid until the document changes) and a locator (valid across
 * changes elsewhere in the document, see jsonStep in json_engine.h).
 *
 * No server header is included. Memory comes from the engine's allocator.
 * Main thread only. */

#ifndef JSON_PATH_H
#define JSON_PATH_H

#include "json_engine.h"

/* Every way compiling or evaluating a path can fail. jsonPathErrMsg() gives
 * the reply text, which is the external module's text for the same case. */
typedef enum {
    JSON_PATH_OK = 0,
    JSON_PATH_ERR_SYNTAX,            /* Malformed path ("Invalid JSON path"). */
    JSON_PATH_ERR_TOO_LONG,          /* Query string above the size limit. */
    JSON_PATH_ERR_TOO_DEEP,          /* Evaluator nesting limit reached. */
    JSON_PATH_ERR_TOO_MANY_DESCENTS, /* Too many recursive descent tokens. */
    JSON_PATH_ERR_NOT_EXIST,         /* Nothing at the path. */
    JSON_PATH_ERR_INDEX_RANGE,       /* Array index out of range. */
    JSON_PATH_ERR_NOT_ARRAY,         /* Index applied to a non-array. */
    JSON_PATH_ERR_NOT_OBJECT,        /* "JSON element is not an object". The selector never produces it. */
    JSON_PATH_ERR_INSERT_ARRAY,      /* Write: a member step applied to a value that is not an object. */
    JSON_PATH_ERR_INSERT_PARENT,     /* "Parent element does not exist". The selector never produces it: a new
                                      * member that is not the last step is JSON_PATH_ERR_NOT_EXIST. */
    JSON_PATH_ERR_OTHER,             /* Not produced. */
    /* The module's remaining selector errors, each with its own reply text. */
    JSON_PATH_ERR_MEMBER_NAME,      /* "Invalid object member name". */
    JSON_PATH_ERR_NUMBER,           /* "Invalid number" in a filter. */
    JSON_PATH_ERR_IDENTIFIER,       /* "Invalid identifier" in a filter. */
    JSON_PATH_ERR_DOT_SEQUENCE,     /* Three or more dots in a row. */
    JSON_PATH_ERR_EMPTY_EXPR,       /* "[" at the end of the path. */
    JSON_PATH_ERR_INDEX_NOT_NUMBER, /* "Array index is not a number". */
    JSON_PATH_ERR_STEP_ZERO,        /* Slice step of zero. */
    JSON_PATH_ERR_WILDCARD,         /* `$` path: wildcard applied to a scalar. */
    JSON_PATH_ERR_DOLLAR,           /* "$" in the middle of a path, applied to a value that is not the root. */
    JSON_PATH_ERR_JSON_PARSE,       /* A quoted name or a filter literal that is not valid JSON. */
    JSON_PATH_ERR_TOO_MANY_REPEATS  /* A read through unions that name one element many times over: the result
                                     * would be out of all proportion to the document. Not a module error. */
} jsonPathErr;

const char *jsonPathErrMsg(jsonPathErr e);

/* 1 for the errors the module calls syntax errors: they fail a command even
 * with a `$` path, and they stop an evaluation at once. Every other error
 * only ends the branch of the evaluation that met it. This is the module's
 * Selector::isSyntaxError, so it is not the same as "the text starts with
 * SYNTAXERR": DOT_SEQUENCE, DOLLAR and JSON_PARSE are not syntax errors. */
int jsonPathErrIsSyntax(jsonPathErr e);

/* A compiled path. */
typedef struct jsonPath jsonPath;

/* Compiles a path. Returns NULL and sets *err on failure. The syntax is
 * detected as the module detects it.
 *
 * The only compile failure is JSON_PATH_ERR_TOO_LONG. The module parses a path
 * while it evaluates it, and stops parsing a branch as soon as the branch has
 * no value left, so whether a malformed path is an error depends on the
 * document: `$.a[` is an error when the document has a member `a` and matches
 * nothing when it has not. A malformed path therefore compiles, and
 * jsonPathEval reports the error exactly when the module would. As the module
 * does, the path ends at the first NUL byte. */
jsonPath *jsonPathCompile(const char *s, size_t n, jsonPathErr *err);
void jsonPathFree(jsonPath *p); /* Accepts NULL. */

/* Compiled paths of up to 128 bytes are kept in a small cache keyed by the
 * path text (at most 256 of them), so compiling a path that was used recently
 * neither parses nor allocates, and freeing it does not free. A cached path
 * keeps its references on the keys of its member names, so such a name stays
 * in the key table until its entry is replaced or the cache is cleared.
 *
 * This empties the cache: its memory and its key references are released. A
 * path that is compiled and not yet freed stays valid. It must be called
 * before jsonEngineShutdown, which otherwise counts those keys as leaked. */
void jsonPathCacheClear(void);

/* 1 for JSONPath ("$...") syntax, 0 for legacy syntax. They differ in how
 * commands shape their replies. */
int jsonPathIsV2(const jsonPath *p);
/* Makes a legacy path behave as a `$` path. The module does this to every
 * path of a JSON.GET with several paths when at least one of them starts
 * with `$`. */
void jsonPathForceV2(jsonPath *p);
/* 1 if the path names the document root: exactly "$" or ".". */
int jsonPathIsRoot(const jsonPath *p);
/* 1 if the path is only member and index steps, so it matches at most one
 * value. */
int jsonPathIsSingular(const jsonPath *p);

typedef enum {
    JSON_PATH_READ = 0, /* Keeps duplicate matches as the module does for reads. */
    JSON_PATH_UPDATE,   /* Matches to change in place. A value matched twice is kept once, first occurrence. */
    JSON_PATH_INSERT,   /* As UPDATE, plus places where the last step would add a new object member. */
    JSON_PATH_DELETE    /* Matches to remove. De-duplicated. */
} jsonPathMode;
/* How the modes map to the module. READ is Selector::getValues and its result
 * set. UPDATE is getValues followed by getUniqueResultSet, which is what
 * NUMINCRBY, TOGGLE and the array commands use. INSERT is prepareSetValues
 * (JSON.SET, JSON.MSET, JSON.MERGE). DELETE is the selection part of
 * deleteValues. In INSERT and DELETE a member step applied to a value that is
 * not an object is JSON_PATH_ERR_INSERT_ARRAY, as in the module. */

typedef struct jsonMatch {
    jsonRef ref;         /* The value, as the document was when evaluated. */
    const jsonStep *loc; /* Locator from the root, `depth` steps. Owned by the result. */
    uint32_t depth;
} jsonMatch;

/* A place where JSON_PATH_INSERT would add a member: the object at
 * (parent, parent_depth) gets a member called name. The name is literal. */
typedef struct jsonInsert {
    const jsonStep *parent;
    uint32_t parent_depth;
    const char *name; /* Decoded bytes. Owned by the result or the path. */
    uint32_t name_len;
} jsonInsert;

#define JSON_MATCHES_INLINE 4
#define JSON_MATCHES_INLINE_STEPS 32

/* The result of one evaluation. Small results live inside the struct, so the
 * common single-match command allocates nothing. Initialise with
 * jsonMatchesInit, release with jsonMatchesFree. */
typedef struct jsonMatches {
    jsonMatch *items; /* In selector order. */
    uint32_t count;
    jsonInsert *inserts; /* JSON_PATH_INSERT only, in selector order. */
    uint32_t insert_count;
    /* Private. */
    uint32_t cap, insert_cap;
    void *arena;
    uint32_t max_depth;
    uint8_t raw_err, v2;
    jsonInsert inline_insert;
    jsonMatch inline_items[JSON_MATCHES_INLINE];
    jsonStep inline_steps[JSON_MATCHES_INLINE_STEPS];
} jsonMatches;

void jsonMatchesInit(jsonMatches *m);
void jsonMatchesFree(jsonMatches *m);

/* Evaluates a compiled path against a document. `d` may be 0 (no document):
 * then nothing matches. Returns JSON_PATH_OK when evaluation completed, even
 * with zero matches, except that a legacy path reports the module's errors
 * for a missing member, a bad index and so on. On error the result is empty.
 *
 * In full:
 *   - A syntax error (jsonPathErrIsSyntax) is returned in every mode.
 *   - INSERT and DELETE return every selector error, as JSON.SET and JSON.DEL
 *     do.
 *   - READ and UPDATE with a legacy path return every selector error.
 *   - READ and UPDATE with a `$` path return JSON_PATH_OK for an error that is
 *     not a syntax error, with the matches found before it, which is what
 *     most commands do. A command that fails on any selector error even with
 *     a `$` path (NUMINCRBY, NUMMULTBY, STRAPPEND, the array writes, CLEAR)
 *     checks jsonMatchesRawErr.
 *   - A legacy path that matches nothing is JSON_PATH_ERR_NOT_EXIST in READ,
 *     UPDATE and DELETE. In INSERT it is JSON_PATH_OK with nothing to do.
 *
 * `out` may be reused: its previous contents are released first.
 *
 * Two bounds are the evaluator's own, where the module has none:
 *   - A recursive descent goes at most 4,096 levels below the value it starts
 *     from, then fails with JSON_PATH_ERR_TOO_DEEP. (The module recurses once
 *     per level there and runs out of stack on a deep enough document.)
 *   - A union that names one element more than once ("[0,0]", "['a','a']")
 *     makes a read return that element's matches once per mention, as in the
 *     module. The work a read may spend on such repeats is 100,000 values
 *     plus 8 per value in the document. Past that it fails with
 *     JSON_PATH_ERR_TOO_MANY_REPEATS, which is a syntax-class error. The
 *     other modes skip a repeated mention where it occurs, so they are never
 *     refused and never see a value twice.
 *
 * Not recursive on document depth or path length: the work stack is bounded
 * by the evaluator nesting limit, plus the depth of the document under a
 * recursive descent. */
jsonPathErr jsonPathEval(const jsonPath *p, jsonDoc d, jsonPathMode mode, jsonMatches *out);

/* The selector's own return code for the last evaluation into `m`, before the
 * rules above were applied. */
jsonPathErr jsonMatchesRawErr(const jsonMatches *m);
/* 1 if the evaluation ran as a `$` path. It is jsonPathIsV2 except for a
 * legacy path that reaches a "$" at the root after a recursive descent or a
 * filter ("..$.a"), which the module then treats as a `$` path. */
int jsonMatchesIsV2(const jsonMatches *m);
/* The module's Selector::getMaxPathDepth for the evaluation: the deepest
 * nesting level it visited, counting a member about to be inserted. A write
 * adds the depth of the new value to it for the json.max-path-limit check.
 * After a recursive descent it is the depth of the deepest value below the
 * descent, matched or not. */
uint32_t jsonMatchesMaxDepth(const jsonMatches *m);

/* Orders matches for applying changes: after sorting, applying each change in
 * array order never invalidates a locator later in the array (deeper before
 * shallower, and within one array higher indexes first). `order` receives a
 * permutation of 0..count-1 and must hold `count` entries. */
void jsonMatchesApplyOrder(const jsonMatches *m, uint32_t *order);
/* 1 if match `a` is a strict ancestor of match `b` (b lies inside a). */
int jsonMatchIsAncestor(const jsonMatch *a, const jsonMatch *b);

#endif /* JSON_PATH_H */
