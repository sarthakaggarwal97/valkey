/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_cmd.c -- the commands of the built-in JSON module.
 *
 * The 24 JSON.* commands of the external valkey-json module, on top of the
 * engine (json_engine.h) and the path evaluator (json_path.h). Replies, error
 * strings, keyspace events, replication and statistics calls follow the
 * external module, except where the design lists a change (B1 to B10).
 *
 * A write command runs in five steps:
 *   1. Open the key.
 *   2. Check: resolve the path against the document as it is, parse the new
 *      value, test every limit and compute every reply value. A failure here
 *      leaves the document untouched.
 *   3. Apply, in the order jsonMatchesApplyOrder() gives. This cannot fail.
 *   4. Replicate and notify.
 *   5. Reply, from the values computed in step 2.
 *
 * Nothing here recurses on document depth. */

#include "json_module.h"
#include "json_api.h"
#include "json_num.h"
#include "json_path.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ----------------------------------------------------------------------------
 * Error strings of the external module (src/json/util.cc, src/json/json.cc)
 * ------------------------------------------------------------------------- */

#define JSON_MSG_PARSE "SYNTAXERR Failed to parse JSON string due to syntax error"
#define JSON_MSG_INVALID_PATH "SYNTAXERR Invalid JSON path"
#define JSON_MSG_PATH_NOT_EXIST "NONEXISTENT JSON path does not exist"
#define JSON_MSG_KEY_NOT_FOUND "NONEXISTENT Document key does not exist"
#define JSON_MSG_NOT_DOC_KEY "WRONGTYPE Not a JSON document key"
#define JSON_MSG_NOT_NUMBER "WRONGTYPE JSON element is not a number"
#define JSON_MSG_NOT_BOOL "WRONGTYPE JSON element is not a bool"
#define JSON_MSG_NOT_STRING "WRONGTYPE JSON element is not a string"
#define JSON_MSG_NOT_OBJECT "WRONGTYPE JSON element is not an object"
#define JSON_MSG_NOT_ARRAY "WRONGTYPE JSON element is not an array"
#define JSON_MSG_VALUE_NOT_NUMBER "WRONGTYPE Value is not a number"
#define JSON_MSG_VALUE_NOT_STRING "WRONGTYPE Value is not a string"
#define JSON_MSG_VALUE_NOT_INTEGER "WRONGTYPE Value is not an integer"
#define JSON_MSG_COMMAND_SYNTAX "SYNTAXERR Command syntax error"
#define JSON_MSG_MULT_OVERFLOW "OVERFLOW Multiplication would overflow"
#define JSON_MSG_ADD_OVERFLOW "OVERFLOW Addition would overflow"
#define JSON_MSG_INDEX_RANGE "OUTOFBOUNDARIES Array index is out of bounds"
#define JSON_MSG_UNKNOWN_SUBCOMMAND "SYNTAXERR Unknown subcommand"
#define JSON_MSG_SIZE_LIMIT "LIMIT Document size limit is exceeded"
#define JSON_MSG_DEPTH_LIMIT "LIMIT Document path nesting limit is exceeded"
#define JSON_MSG_NEW_KEY_NOT_ROOT "SYNTAXERR A new Valkey key's path must be root"
#define JSON_MSG_KEY_OPEN "ERROR Unable to open valkeymodule key"
/* Not in the external module, which has no such limit. */
#define JSON_MSG_FORMAT_LIMIT "LIMIT Formatted JSON reply size limit is exceeded"

/* ----------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

#define JSON_TYPE_BIT(t) (1u << (t))
#define JSON_MASK_NUMBER (JSON_TYPE_BIT(JSON_INT) | JSON_TYPE_BIT(JSON_DOUBLE) | JSON_TYPE_BIT(JSON_RAWNUM))
#define JSON_MASK_BOOL (JSON_TYPE_BIT(JSON_FALSE) | JSON_TYPE_BIT(JSON_TRUE))
#define JSON_MASK_STRING JSON_TYPE_BIT(JSON_STRING)
#define JSON_MASK_OBJECT JSON_TYPE_BIT(JSON_OBJECT)
#define JSON_MASK_ARRAY JSON_TYPE_BIT(JSON_ARRAY)
#define JSON_MASK_CONTAINER (JSON_MASK_OBJECT | JSON_MASK_ARRAY)

/* Size of the stack buffer a reply is serialised into before it moves to the
 * heap. */
#define JSON_REPLY_STACK 1024

/* Returns `stack` if it holds `need` bytes, else a heap block. */
static void *jsonScratch(void *stack, size_t stack_bytes, size_t need) {
    return need <= stack_bytes ? stack : ValkeyModule_Alloc(need);
}

static void jsonScratchFree(void *p, void *stack) {
    if (p != stack) ValkeyModule_Free(p);
}

/* A path argument. The external module reads paths as C strings, so a path
 * ends at its first NUL byte. */
static const char *jsonArgPath(ValkeyModuleString *arg, size_t *len) {
    const char *s = ValkeyModule_StringPtrLen(arg, len);
    *len = strlen(s);
    return s;
}

/* The external module's jsonutil_is_root_path(): only the two literal
 * spellings count. */
static int jsonIsRootPath(const char *path, size_t len) {
    return len == 1 && (path[0] == '.' || path[0] == '$');
}

/* Input from a source that already accepted the data: the replication stream
 * and AOF loading. A replica or an AOF load must end up with what the primary
 * stored, whatever implementation or configuration the primary had. So no
 * limit and no check that the primary may not have had is applied to it:
 * nesting, size, UTF-8, number range. It is asked about only when a check is
 * about to fail, which keeps it off the path of an ordinary command. */
static int jsonCmdTrusted(ValkeyModuleCtx *ctx) {
    return (ValkeyModule_GetContextFlags(ctx) & (VALKEYMODULE_CTX_FLAGS_REPLICATED | VALKEYMODULE_CTX_FLAGS_LOADING)) !=
           0;
}

/* ----------------------------------------------------------------------------
 * Keys
 * ------------------------------------------------------------------------- */

typedef enum {
    JSON_KEY_DOC = 0,
    JSON_KEY_MISSING,
    JSON_KEY_WRONGTYPE
} jsonKeyState;

/* The external module's verify_open_doc_key(). Accepts a NULL key, which is
 * what opening a missing key for reading returns. */
static jsonKeyState jsonKeyCheck(ValkeyModuleKey *key) {
    if (ValkeyModule_ModuleTypeGetType(key) == jsonModuleType) return JSON_KEY_DOC; /* The usual case: one call. */
    return ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_EMPTY ? JSON_KEY_MISSING : JSON_KEY_WRONGTYPE;
}

static jsonDoc jsonKeyDoc(ValkeyModuleKey *key) {
    return jsonDocFromValue(ValkeyModule_ModuleTypeGetValue(key));
}

/* Stores the document word back after a change. The engine has already
 * released whatever the old word referred to, so the old value is not freed
 * here. The key keeps its TTL. */
static void jsonKeyStore(ValkeyModuleKey *key, jsonDoc before, jsonDoc after) {
    if (after == before) return;
    jsonKeyStoreDoc(key, after);
}

/* Opens a key for a write command. Returns NULL after replying when the key
 * cannot be opened, which only happens while a forkless save holds it. */
static ValkeyModuleKey *jsonOpenWrite(ValkeyModuleCtx *ctx, ValkeyModuleString *name) {
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, name, VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (key == NULL) ValkeyModule_ReplyWithError(ctx, JSON_MSG_KEY_OPEN);
    return key;
}

/* Replies for a key that is not a document, the way most commands do: an
 * error for another type, `missing_msg` for no key (NULL means a null reply).
 * Returns 1 if it replied. */
static int jsonReplyKeyState(ValkeyModuleCtx *ctx, jsonKeyState ks, const char *missing_msg) {
    if (ks == JSON_KEY_DOC) return 0;
    if (ks == JSON_KEY_WRONGTYPE) {
        ValkeyModule_ReplyWithError(ctx, JSON_MSG_NOT_DOC_KEY);
    } else if (missing_msg != NULL) {
        ValkeyModule_ReplyWithError(ctx, missing_msg);
    } else {
        ValkeyModule_ReplyWithNull(ctx);
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */

/* The size limit that applies to this command, or 0 for none. It is not
 * applied on a trusted path. */
static size_t jsonCmdSizeLimit(ValkeyModuleCtx *ctx) {
    if (jsonConfig.max_document_size <= 0) return 0;
    if (jsonCmdTrusted(ctx)) return 0;
    return (size_t)jsonConfig.max_document_size;
}

/* The estimate of design 8.4: the document's size, plus what the write adds,
 * minus what it removes. */
static int jsonSizeExceeded(size_t limit, size_t current, size_t added, size_t removed) {
    size_t total = current + added;
    if (limit == 0) return 0;
    total = removed < total ? total - removed : 0;
    return total > limit;
}

/* The external module's CHECK_DOCUMENT_PATH_LIMIT: the nesting the document
 * would have is the depth of the path plus the depth of the new value. Not
 * applied on a trusted path, which is held to the server's load limit. */
static int jsonDepthExceeded(ValkeyModuleCtx *ctx, uint32_t path_depth, uint32_t value_depth) {
    size_t depth = (size_t)path_depth + value_depth;
    if (jsonCmdTrusted(ctx)) {
        if (depth > jsonLoadDepthLimit()) return 1;
    } else if (depth > (size_t)jsonConfig.max_path_limit) {
        return 1;
    }
    return 0;
}

/* Parses JSON text from a command argument. Returns NULL and sets *err to the
 * reply text on failure.
 *
 * For an ordinary client the text is held to the nesting limit, invalid UTF-8
 * (B1) and a number too large for a double (B2) are errors, and so is a NUL
 * byte anywhere in it.
 *
 * On a trusted path none of that applies except the server's load depth
 * limit (jsonLoadDepthLimit), and the text ends at its first NUL:
 * the external module reads the argument as a C string, so that is what a
 * module primary stored for the same command. */
static jsonVal *jsonCmdParse(ValkeyModuleCtx *ctx, const char *s, size_t n, const char **err) {
    jsonErr e = JSON_OK;
    uint32_t max_depth = jsonConfig.max_path_limit > 0 ? (uint32_t)jsonConfig.max_path_limit : 0;
    jsonVal *v = jsonValParse(s, n, 0, max_depth, &e, NULL);
    if (v != NULL && max_depth == 0 && jsonValDepth(v) > 0) {
        /* A limit of 0 admits no container at all. The engine reads 0 as "no limit". */
        jsonValFree(v);
        v = NULL;
        e = JSON_ERR_DEPTH;
    }
    if (v == NULL && jsonCmdTrusted(ctx)) {
        const char *nul = memchr(s, '\0', n);
        v = jsonValParse(s, nul != NULL ? (size_t)(nul - s) : n, JSON_PARSE_TRUSTED, jsonLoadDepthLimit(), &e, NULL);
    }
    if (v != NULL) return v;
    if (e == JSON_ERR_DEPTH) {
        *err = JSON_MSG_DEPTH_LIMIT;
    } else if (e == JSON_ERR_TOO_BIG) {
        *err = JSON_MSG_SIZE_LIMIT;
    } else {
        *err = JSON_MSG_PARSE;
    }
    return NULL;
}

/* Strict UTF-8: no overlong form, no surrogate, nothing above U+10FFFF. */
static int jsonUtf8Valid(const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;

    while (i < n) {
        unsigned char c = s[i];
        size_t more;
        uint32_t cp, min;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            more = 1, cp = c & 0x1F, min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            more = 2, cp = c & 0x0F, min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            more = 3, cp = c & 0x07, min = 0x10000;
        } else {
            return 0;
        }
        if (n - i <= more) return 0;
        for (size_t k = 1; k <= more; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += more + 1;
    }
    return 1;
}

/* A member name a path is about to add is JSON text as much as the value is:
 * for an ordinary client it must be valid UTF-8 (B1), or the document could no
 * longer be written out as valid JSON. */
static int jsonInsertNamesValid(ValkeyModuleCtx *ctx, const jsonMatches *m) {
    for (uint32_t i = 0; i < m->insert_count; i++) {
        if (!jsonUtf8Valid(m->inserts[i].name, m->inserts[i].name_len)) return jsonCmdTrusted(ctx);
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 * Queries: one path, compiled and evaluated
 * ------------------------------------------------------------------------- */

typedef struct jsonQuery {
    jsonPath *path;
    jsonMatches m;
    int v2;           /* The evaluation ran as a JSONPath ("$") path: replies are arrays. */
    jsonPathErr cerr; /* Why the path did not compile. */
    jsonPathErr err;  /* The outcome of the last evaluation. */
} jsonQuery;

static void jsonQueryCompile(jsonQuery *q, const char *s, size_t n) {
    q->cerr = JSON_PATH_OK;
    q->err = JSON_PATH_OK;
    jsonMatchesInit(&q->m);
    q->path = jsonPathCompile(s, n, &q->cerr);
    if (q->path != NULL) {
        q->cerr = JSON_PATH_OK;
        q->v2 = jsonPathIsV2(q->path);
    } else {
        if (q->cerr == JSON_PATH_OK) q->cerr = JSON_PATH_ERR_SYNTAX;
        q->v2 = n > 0 && s[0] == '$';
    }
}

/* Evaluates against a document. May be called again for another document. */
static void jsonQueryEval(jsonQuery *q, jsonDoc d, jsonPathMode mode) {
    if (q->path == NULL) {
        q->err = q->cerr;
        return;
    }
    q->err = jsonPathEval(q->path, d, mode, &q->m);
    q->v2 = jsonMatchesIsV2(&q->m);
    /* A legacy path that selects nothing. The module's selector does not call
     * that an error: each command decides what it means. When a path adds a
     * member, the same code is a real error (a missing parent) and stays. */
    if (q->err == JSON_PATH_ERR_NOT_EXIST && mode != JSON_PATH_INSERT) q->err = JSON_PATH_OK;
}

static void jsonQueryRun(jsonQuery *q, const char *s, size_t n, jsonDoc d, jsonPathMode mode) {
    jsonQueryCompile(q, s, n);
    jsonQueryEval(q, d, mode);
}

static void jsonQueryFree(jsonQuery *q) {
    jsonMatchesFree(&q->m);
    jsonPathFree(q->path);
}

/* The error a command replies with, or NULL to go on. With a JSONPath path
 * the evaluator only reports the errors the module calls syntax errors. Any
 * other error ends the search there, and a lenient command (the reads and
 * JSON.TOGGLE) works with what was selected. A strict command fails on every
 * selector error, as the module's other writes do. */
static const char *jsonQueryError(const jsonQuery *q, int lenient) {
    jsonPathErr e = q->err;
    if (e == JSON_PATH_OK && !lenient && q->path != NULL) {
        e = jsonMatchesRawErr(&q->m);
        if (e == JSON_PATH_ERR_NOT_EXIST) e = JSON_PATH_OK;
    }
    return e == JSON_PATH_OK ? NULL : jsonPathErrMsg(e);
}

static int jsonMatchesHaveType(const jsonMatches *m, unsigned mask) {
    for (uint32_t i = 0; i < m->count; i++) {
        if (JSON_TYPE_BIT(jsonRefType(&m->items[i].ref)) & mask) return 1;
    }
    return 0;
}

/* What the module checks for a legacy path before it works on the selection:
 * something is selected, and at least one selected value is of the kind the
 * command applies to. */
static const char *jsonLegacyCheck(const jsonQuery *q, unsigned mask, const char *wrongtype) {
    if (q->v2) return NULL;
    if (q->m.count == 0) return JSON_MSG_PATH_NOT_EXIST;
    if (!jsonMatchesHaveType(&q->m, mask)) return wrongtype;
    return NULL;
}

/* The longest locator among the matches. */
static uint32_t jsonMatchesDeepest(const jsonMatches *m) {
    uint32_t max = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->items[i].depth > max) max = m->items[i].depth;
    }
    return max;
}

/* The order to apply changes in. Returns `stack` or a heap block to release
 * with jsonScratchFree(). */
#define JSON_INLINE_MATCHES 8

static uint32_t *jsonApplyOrder(const jsonMatches *m, uint32_t *stack) {
    uint32_t *order = jsonScratch(stack, JSON_INLINE_MATCHES * sizeof(uint32_t), m->count * sizeof(uint32_t));
    if (m->count == 1) {
        order[0] = 0;
    } else if (m->count > 1) {
        jsonMatchesApplyOrder(m, order);
    }
    return order;
}

/* Sorting matches into document order, to find the ones nested in another. */
static const jsonMatches *jsonSortMatches;

static int jsonMatchDocOrder(const void *pa, const void *pb) {
    const jsonMatch *a = &jsonSortMatches->items[*(const uint32_t *)pa];
    const jsonMatch *b = &jsonSortMatches->items[*(const uint32_t *)pb];
    uint32_t n = a->depth < b->depth ? a->depth : b->depth;
    for (uint32_t i = 0; i < n; i++) {
        if (a->loc[i].pos != b->loc[i].pos) return a->loc[i].pos < b->loc[i].pos ? -1 : 1;
    }
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    return 0;
}

/* Flags the matches that lie inside another match. A command that replaces or
 * removes whole values applies only the outermost ones (B4). Returns NULL when
 * no match is nested, which is every path without a recursive descent, else an
 * array of `count` flags to release with ValkeyModule_Free(). */
static uint8_t *jsonNestedMatches(const jsonMatches *m) {
    uint32_t i, top, nested = 0;
    uint32_t *idx;
    uint8_t *flags;

    if (m->count < 2) return NULL;
    for (i = 1; i < m->count; i++) {
        if (m->items[i].depth != m->items[0].depth) break;
    }
    if (i == m->count) return NULL; /* Equal depths: distinct values cannot nest. */

    idx = ValkeyModule_Alloc(m->count * sizeof(uint32_t));
    for (i = 0; i < m->count; i++) idx[i] = i;
    jsonSortMatches = m;
    qsort(idx, m->count, sizeof(uint32_t), jsonMatchDocOrder);

    /* In document order an ancestor comes right before everything inside it. */
    flags = ValkeyModule_Calloc(m->count, 1);
    top = idx[0];
    for (i = 1; i < m->count; i++) {
        if (jsonMatchIsAncestor(&m->items[top], &m->items[idx[i]])) {
            flags[idx[i]] = 1;
            nested++;
        } else {
            top = idx[i];
        }
    }
    ValkeyModule_Free(idx);
    if (nested == 0) {
        ValkeyModule_Free(flags);
        return NULL;
    }
    return flags;
}

/* ----------------------------------------------------------------------------
 * Walking a value without recursion
 * ------------------------------------------------------------------------- */

typedef struct jsonWalkFrame {
    jsonIter it;
    int is_obj;
    uint32_t aux; /* For the caller. */
} jsonWalkFrame;

#define JSON_WALK_INLINE 16

typedef struct jsonWalk {
    jsonWalkFrame *f;
    uint32_t n, cap;
    jsonWalkFrame inl[JSON_WALK_INLINE];
} jsonWalk;

static void jsonWalkInit(jsonWalk *w) {
    w->f = w->inl;
    w->n = 0;
    w->cap = JSON_WALK_INLINE;
}

static jsonWalkFrame *jsonWalkPush(jsonWalk *w, const jsonRef *container) {
    jsonWalkFrame *fr;
    if (w->n == w->cap) {
        uint32_t cap = w->cap * 2;
        if (w->f == w->inl) {
            w->f = ValkeyModule_Alloc(cap * sizeof(jsonWalkFrame));
            memcpy(w->f, w->inl, sizeof(w->inl));
        } else {
            w->f = ValkeyModule_Realloc(w->f, cap * sizeof(jsonWalkFrame));
        }
        w->cap = cap;
    }
    fr = &w->f[w->n++];
    jsonIterInit(&fr->it, container);
    fr->is_obj = jsonRefType(container) == JSON_OBJECT;
    fr->aux = 0;
    return fr;
}

static void jsonWalkFree(jsonWalk *w) {
    if (w->f != w->inl) ValkeyModule_Free(w->f);
}

static int jsonIsContainer(const jsonRef *r) {
    return (JSON_TYPE_BIT(jsonRefType(r)) & JSON_MASK_CONTAINER) != 0;
}

/* The external module's dom_path_depth(): the longest path to any value below
 * `root`. A scalar and an empty container are 0, [1] and [[]] are 1. */
static uint32_t jsonValuePathDepth(const jsonRef *root) {
    jsonWalk w;
    uint32_t max = 0;

    if (!jsonIsContainer(root)) return 0;
    jsonWalkInit(&w);
    jsonWalkPush(&w, root);
    while (w.n > 0) {
        jsonRef val;
        jsonStep step;
        const char *name;
        uint32_t name_len;
        if (!jsonIterNext(&w.f[w.n - 1].it, &val, &step, &name, &name_len)) {
            w.n--;
            continue;
        }
        if (w.n > max) max = w.n;
        if (jsonIsContainer(&val)) jsonWalkPush(&w, &val);
    }
    jsonWalkFree(&w);
    return max;
}

/* ----------------------------------------------------------------------------
 * Replies shared by several commands
 * ------------------------------------------------------------------------- */

/* One result per match. JSON_NA marks a match of a kind the command does not
 * apply to: null in a JSONPath reply, skipped by a legacy reply. `a` and `b`
 * carry what the apply step needs. */
#define JSON_NA LLONG_MIN

typedef struct jsonOp {
    long long reply;
    uint32_t a, b;
} jsonOp;

static jsonOp *jsonOpsAlloc(jsonOp *stack, uint32_t count) {
    return jsonScratch(stack, JSON_INLINE_MATCHES * sizeof(jsonOp), count * sizeof(jsonOp));
}

/* Integer results. A JSONPath path replies an array with one entry per match.
 * A legacy path replies one integer: the first applicable match, or the last
 * for the commands that say so. The caller has checked that a legacy path has
 * an applicable match. */
static int jsonReplyOps(ValkeyModuleCtx *ctx, const jsonOp *ops, uint32_t count, int v2, int pick_last) {
    if (v2) {
        ValkeyModule_ReplyWithArray(ctx, count);
        for (uint32_t i = 0; i < count; i++) {
            if (ops[i].reply == JSON_NA) {
                ValkeyModule_ReplyWithNull(ctx);
            } else {
                ValkeyModule_ReplyWithLongLong(ctx, ops[i].reply);
            }
        }
        return VALKEYMODULE_OK;
    }
    for (uint32_t i = 0; i < count; i++) {
        const jsonOp *op = &ops[pick_last ? count - 1 - i : i];
        if (op->reply != JSON_NA) return ValkeyModule_ReplyWithLongLong(ctx, op->reply);
    }
    return ValkeyModule_ReplyWithNull(ctx); /* Not reached. */
}

/* ----------------------------------------------------------------------------
 * Serialising for JSON.GET, JSON.MGET and the shared API
 * ------------------------------------------------------------------------- */

static void jsonAppendOpt(jsonBuf *out, const char *s, size_t n) {
    if (s != NULL) jsonBufAppend(out, s, n);
}

/* Room for the compact text of the first `count` matches and what goes
 * between them. */
static void jsonBufReserveMatches(jsonBuf *out, const jsonMatches *m, uint32_t count) {
    size_t hint = 2;
    for (uint32_t i = 0; i < count; i++) hint += jsonSerializeSizeHint(&m->items[i].ref) + 1;
    jsonBufReserve(out, hint);
}

/* The matches as a JSON array that itself sits `level - 1` containers deep,
 * as the module's build_json_array() and appendPathAndValues() write it. */
static void jsonAppendMatchArray(jsonBuf *out, const jsonMatches *m, const jsonFormat *fmt, uint32_t level) {
    jsonFormat inner_fmt;
    const jsonFormat *inner = NULL;

    if (fmt != NULL) {
        inner_fmt = *fmt;
        inner_fmt.initial_level = level;
        inner = &inner_fmt;
    }
    jsonBufAppend(out, "[", 1);
    if (fmt != NULL) jsonAppendOpt(out, fmt->newline, fmt->newline_len);
    for (uint32_t i = 0; i < m->count; i++) {
        if (fmt != NULL) {
            for (uint32_t l = 0; l < level; l++) jsonAppendOpt(out, fmt->indent, fmt->indent_len);
        }
        jsonSerialize(&m->items[i].ref, inner, out);
        if (i + 1 < m->count) jsonBufAppend(out, ",", 1);
        if (fmt != NULL) jsonAppendOpt(out, fmt->newline, fmt->newline_len);
    }
    if (fmt != NULL) {
        for (uint32_t l = 0; l + 1 < level; l++) jsonAppendOpt(out, fmt->indent, fmt->indent_len);
    }
    jsonBufAppend(out, "]", 1);
}

/* INDENT, NEWLINE and SPACE are written once or more per value, so long ones
 * multiply the size of a document. Formatting strings that together are longer
 * than JSON_FORMAT_UNCHECKED bytes are allowed only while the reply stays
 * under JSON_FORMAT_MAX_REPLY, the largest bulk string a client may send by
 * default. The external module builds whatever is asked for. */
#define JSON_FORMAT_UNCHECKED 64
#define JSON_FORMAT_MAX_REPLY (512.0 * 1024 * 1024)

static int jsonFormatNeedsCheck(const jsonFormat *fmt) {
    return fmt != NULL && fmt->indent_len + fmt->newline_len + fmt->space_len > JSON_FORMAT_UNCHECKED;
}

/* An upper bound of the formatted text of the first `count` matches, printed
 * `level` containers deep: every value is preceded by at most one line break
 * and its indent, every container closes with one more, and every member has
 * one space. A double, because it is only compared with a limit. */
static double jsonFormatBound(const jsonMatches *m, uint32_t count, const jsonFormat *fmt, uint32_t level) {
    double total = 0;
    for (uint32_t i = 0; i < count; i++) {
        const jsonRef *r = &m->items[i].ref;
        double values = (double)jsonRefFields(r) + 1;
        double depth = (double)jsonRefDepth(r) + level + 1;
        double per_line = (double)fmt->newline_len + depth * (double)fmt->indent_len;
        total += (double)jsonSerializeSizeHint(r) + values * (2 * per_line + (double)fmt->space_len);
    }
    return total;
}

/* What JSON.GET with one path replies, for a query already evaluated. A legacy
 * path gives its first value, a JSONPath path an array of all of them. Returns
 * NULL after appending, or the error text with `out` unchanged. The read is
 * counted in the statistics unless `count_read` is 0. */
static const char *jsonAppendGetValue(jsonBuf *out, const jsonQuery *q, const jsonFormat *fmt, int count_read) {
    const char *err = jsonQueryError(q, 1);
    size_t from = out->len;

    if (err != NULL) return err;
    if (!q->v2 && q->m.count == 0) return JSON_MSG_PATH_NOT_EXIST;
    if (jsonFormatNeedsCheck(fmt) &&
        jsonFormatBound(&q->m, q->v2 ? q->m.count : 1, fmt, q->v2 ? 1 : 0) > JSON_FORMAT_MAX_REPLY) {
        return JSON_MSG_FORMAT_LIMIT;
    }
    if (!q->v2) {
        jsonBufReserveMatches(out, &q->m, 1);
        jsonSerialize(&q->m.items[0].ref, fmt, out);
    } else if (q->m.count == 0) {
        jsonBufAppend(out, "[]", 2);
    } else {
        jsonBufReserveMatches(out, &q->m, q->m.count);
        jsonAppendMatchArray(out, &q->m, fmt, 1);
    }
    if (count_read) jsonStatsOnRead(out->len - from);
    return NULL;
}

static const char *jsonAppendGet(jsonBuf *out, const jsonQuery *q, const jsonFormat *fmt) {
    return jsonAppendGetValue(out, q, fmt, 1);
}

/* The JSON_GetValue shared API (the module's src/json/shared_api.cc). */
int jsonSharedGetValue(ValkeyModuleKey *key, const char *path, size_t path_len, jsonBuf *out) {
    jsonQuery q;
    const char *err;

    if (jsonKeyCheck(key) != JSON_KEY_DOC) return VALKEYMODULE_ERR;
    jsonQueryRun(&q, path, path_len, jsonKeyDoc(key), JSON_PATH_READ);
    err = jsonAppendGet(out, &q, NULL);
    jsonQueryFree(&q);
    return err == NULL ? VALKEYMODULE_OK : VALKEYMODULE_ERR;
}

/* ----------------------------------------------------------------------------
 * JSON.GET key [INDENT s] [NEWLINE s] [SPACE s] [NOESCAPE] [path ...]
 * ------------------------------------------------------------------------- */

/* How many arguments a JSON.GET option takes, itself included. 0 for a path. */
static int jsonGetOptionArgs(const char *token) {
    char c = token[0] | 0x20;
    if (c != 'n' && c != 's' && c != 'i') return 0; /* A path, as a rule: no comparison needed. */
    if (!strcasecmp(token, "NEWLINE") || !strcasecmp(token, "SPACE") || !strcasecmp(token, "INDENT")) return 2;
    if (!strcasecmp(token, "NOESCAPE")) return 1; /* Accepted and ignored. */
    return 0;
}

/* Several paths: one JSON object with a member per path. If any path is in
 * JSONPath syntax every member is an array, and a path that selects nothing
 * gives an empty one. Returns NULL or the error text. */
static const char *jsonAppendGetMulti(jsonBuf *out,
                                      jsonDoc d,
                                      ValkeyModuleString **argv,
                                      int argc,
                                      int num_paths,
                                      const jsonFormat *fmt) {
    jsonFormat inner_fmt;
    const jsonFormat *inner = NULL;
    const char *err = NULL;
    int any_v2 = 0, seen = 0;
    double formatted = 0;

    if (fmt != NULL) {
        inner_fmt = *fmt;
        inner_fmt.initial_level = 1;
        inner = &inner_fmt;
    }

    for (int i = 2; i < argc;) {
        const char *token = ValkeyModule_StringPtrLen(argv[i], NULL);
        int skip = jsonGetOptionArgs(token);
        if (skip == 0 && token[0] == '$') any_v2 = 1;
        i += skip ? skip : 1;
    }

    jsonBufAppend(out, "{", 1);
    if (fmt != NULL) jsonAppendOpt(out, fmt->newline, fmt->newline_len);
    for (int i = 2; i < argc && err == NULL;) {
        size_t path_len;
        const char *path = jsonArgPath(argv[i], &path_len);
        int skip = jsonGetOptionArgs(path);
        jsonQuery q;

        if (skip) {
            i += skip;
            continue;
        }
        i++;
        seen++;
        jsonQueryCompile(&q, path, path_len);
        if (any_v2 && q.path != NULL) jsonPathForceV2(q.path);
        jsonQueryEval(&q, d, JSON_PATH_READ);
        if (q.err != JSON_PATH_OK) {
            err = jsonPathErrMsg(q.err);
        } else if (!any_v2 && q.m.count == 0) {
            err = JSON_MSG_PATH_NOT_EXIST;
        } else if (jsonFormatNeedsCheck(fmt) &&
                   (formatted += jsonFormatBound(&q.m, any_v2 ? q.m.count : 1, fmt, any_v2 ? 2 : 1)) >
                       JSON_FORMAT_MAX_REPLY) {
            err = JSON_MSG_FORMAT_LIMIT;
        } else {
            if (fmt != NULL) jsonAppendOpt(out, fmt->indent, fmt->indent_len);
            jsonBufAppendQuoted(out, path, path_len);
            jsonBufAppend(out, ":", 1);
            if (fmt != NULL) jsonAppendOpt(out, fmt->space, fmt->space_len);
            jsonBufReserveMatches(out, &q.m, any_v2 ? q.m.count : 1);
            if (any_v2) {
                jsonAppendMatchArray(out, &q.m, fmt, 2);
            } else {
                jsonSerialize(&q.m.items[0].ref, inner, out);
            }
            if (seen < num_paths) jsonBufAppend(out, ",", 1);
            if (fmt != NULL) jsonAppendOpt(out, fmt->newline, fmt->newline_len);
        }
        jsonQueryFree(&q);
    }
    if (err != NULL) return err;
    jsonBufAppend(out, "}", 1);
    jsonStatsOnRead(out->len);
    return NULL;
}

static int jsonGetCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    jsonFormat fmt = {NULL, NULL, NULL, 0, 0, 0, 0};
    const jsonFormat *use_fmt;
    int num_paths = 0;
    const char *first_path = ".";
    char stack[JSON_REPLY_STACK];
    jsonBuf out;
    const char *err = NULL;
    ValkeyModuleKey *key;
    jsonKeyState ks;

    if (argc < 2) return ValkeyModule_WrongArity(ctx);
    for (int i = 2; i < argc;) {
        const char *token = ValkeyModule_StringPtrLen(argv[i], NULL);
        int skip = jsonGetOptionArgs(token);
        if (skip == 2) {
            const char *arg;
            if (i == argc - 1) return ValkeyModule_ReplyWithError(ctx, JSON_MSG_COMMAND_SYNTAX);
            arg = ValkeyModule_StringPtrLen(argv[i + 1], NULL);
            if (!strcasecmp(token, "NEWLINE")) {
                fmt.newline = arg;
                fmt.newline_len = strlen(arg);
            } else if (!strcasecmp(token, "SPACE")) {
                fmt.space = arg;
                fmt.space_len = strlen(arg);
            } else {
                fmt.indent = arg;
                fmt.indent_len = strlen(arg);
            }
        } else if (skip == 0) {
            if (num_paths++ == 0) first_path = token;
        }
        i += skip ? skip : 1;
    }
    use_fmt = (fmt.newline != NULL || fmt.space != NULL || fmt.indent != NULL) ? &fmt : NULL;

    key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    ks = jsonKeyCheck(key);
    if (jsonReplyKeyState(ctx, ks, NULL)) {
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    jsonBufInit(&out, stack, sizeof(stack));
    if (num_paths <= 1) {
        jsonQuery q;
        jsonQueryRun(&q, first_path, strlen(first_path), jsonKeyDoc(key), JSON_PATH_READ);
        err = jsonAppendGet(&out, &q, use_fmt);
        jsonQueryFree(&q);
    } else {
        err = jsonAppendGetMulti(&out, jsonKeyDoc(key), argv, argc, num_paths, use_fmt);
    }
    if (err != NULL) {
        ValkeyModule_ReplyWithError(ctx, err);
    } else {
        ValkeyModule_ReplyWithStringBuffer(ctx, out.p, out.len);
    }
    jsonBufFree(&out);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.MGET key [key ...] path
 * ------------------------------------------------------------------------- */

static int jsonMgetCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    int num_keys = argc - 2;
    size_t path_len;
    const char *path, *err = NULL;
    jsonQuery q;
    char stack[JSON_REPLY_STACK];
    jsonBuf out;
    size_t ends_stack[32], *ends;

    if (argc < 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgPath(argv[argc - 1], &path_len);
    jsonQueryCompile(&q, path, path_len);
    jsonBufInit(&out, stack, sizeof(stack));
    ends = jsonScratch(ends_stack, sizeof(ends_stack), (size_t)num_keys * sizeof(size_t));

    /* Everything is gathered first: an error on any key is the whole reply. A
     * missing key, a path that does not exist and an invalid path give null. */
    for (int i = 0; i < num_keys && err == NULL; i++) {
        ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[i + 1], VALKEYMODULE_READ);
        jsonKeyState ks = jsonKeyCheck(key);
        if (ks == JSON_KEY_WRONGTYPE) {
            err = JSON_MSG_NOT_DOC_KEY;
        } else if (ks == JSON_KEY_DOC) {
            const char *e;
            jsonQueryEval(&q, jsonKeyDoc(key), JSON_PATH_READ);
            e = jsonAppendGet(&out, &q, NULL);
            if (e != NULL && strcmp(e, JSON_MSG_INVALID_PATH) != 0 && strcmp(e, JSON_MSG_PATH_NOT_EXIST) != 0) err = e;
        }
        ValkeyModule_CloseKey(key);
        ends[i] = out.len;
    }

    if (err != NULL) {
        ValkeyModule_ReplyWithError(ctx, err);
    } else {
        size_t from = 0;
        ValkeyModule_ReplyWithArray(ctx, num_keys);
        for (int i = 0; i < num_keys; i++) {
            if (ends[i] == from) {
                ValkeyModule_ReplyWithNull(ctx);
            } else {
                ValkeyModule_ReplyWithStringBuffer(ctx, out.p + from, ends[i] - from);
            }
            from = ends[i];
        }
    }
    jsonScratchFree(ends, ends_stack);
    jsonBufFree(&out);
    jsonQueryFree(&q);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * Read commands that reply one value per match
 * ------------------------------------------------------------------------- */

/* Parses "key [path]". The path defaults to the root. */
static const char *jsonArgOptionalPath(ValkeyModuleString **argv, int argc, size_t *len) {
    if (argc == 3) return jsonArgPath(argv[2], len);
    *len = 1;
    return ".";
}

/* Opens a key for a read command and evaluates the path with the module's
 * rules for reads. Returns 1 to go on. Otherwise it has replied: null for a
 * missing key unless `missing_msg` is set. */
static int jsonReadQuery(ValkeyModuleCtx *ctx,
                         ValkeyModuleString *keyname,
                         const char *path,
                         size_t path_len,
                         const char *missing_msg,
                         ValkeyModuleKey **key,
                         jsonQuery *q) {
    const char *err;

    *key = ValkeyModule_OpenKey(ctx, keyname, VALKEYMODULE_READ);
    if (jsonReplyKeyState(ctx, jsonKeyCheck(*key), missing_msg)) {
        ValkeyModule_CloseKey(*key);
        return 0;
    }
    jsonQueryRun(q, path, path_len, jsonKeyDoc(*key), JSON_PATH_READ);
    err = jsonQueryError(q, 1);
    if (err != NULL) {
        ValkeyModule_ReplyWithError(ctx, err);
        jsonQueryFree(q);
        ValkeyModule_CloseKey(*key);
        return 0;
    }
    return 1;
}

static void jsonReadDone(ValkeyModuleKey *key, jsonQuery *q) {
    jsonQueryFree(q);
    ValkeyModule_CloseKey(key);
}

/* JSON.STRLEN, JSON.OBJLEN and JSON.ARRLEN. */
static int jsonLengthCommand(ValkeyModuleCtx *ctx,
                             ValkeyModuleString **argv,
                             int argc,
                             unsigned mask,
                             const char *wrongtype) {
    size_t path_len;
    const char *path, *err;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    if (!jsonReadQuery(ctx, argv[1], path, path_len, NULL, &key, &q)) return VALKEYMODULE_OK;

    err = jsonLegacyCheck(&q, mask, wrongtype);
    if (err != NULL) {
        ValkeyModule_ReplyWithError(ctx, err);
        jsonReadDone(key, &q);
        return VALKEYMODULE_OK;
    }
    ops = jsonOpsAlloc(ops_stack, q.m.count);
    for (uint32_t i = 0; i < q.m.count; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        if (!(JSON_TYPE_BIT(jsonRefType(r)) & mask)) {
            ops[i].reply = JSON_NA;
        } else if (mask == JSON_MASK_STRING) {
            uint32_t len = 0;
            jsonRefString(r, &len);
            ops[i].reply = len;
        } else {
            ops[i].reply = jsonRefCount(r);
        }
    }
    jsonReplyOps(ctx, ops, q.m.count, q.v2, 0);
    jsonScratchFree(ops, ops_stack);
    jsonReadDone(key, &q);
    return VALKEYMODULE_OK;
}

static int jsonStrlenCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonLengthCommand(ctx, argv, argc, JSON_MASK_STRING, JSON_MSG_NOT_STRING);
}

static int jsonObjlenCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonLengthCommand(ctx, argv, argc, JSON_MASK_OBJECT, JSON_MSG_NOT_OBJECT);
}

static int jsonArrlenCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonLengthCommand(ctx, argv, argc, JSON_MASK_ARRAY, JSON_MSG_NOT_ARRAY);
}

/* ----------------------------------------------------------------------------
 * JSON.OBJKEYS key [path]
 * ------------------------------------------------------------------------- */

static void jsonReplyObjectKeys(ValkeyModuleCtx *ctx, const jsonRef *obj) {
    jsonIter it;
    jsonRef val;
    jsonStep step;
    const char *name;
    uint32_t name_len, count = jsonRefType(obj) == JSON_OBJECT ? jsonRefCount(obj) : 0;

    if (count == 0) {
        ValkeyModule_ReplyWithEmptyArray(ctx);
        return;
    }
    ValkeyModule_ReplyWithArray(ctx, count);
    jsonIterInit(&it, obj);
    while (jsonIterNext(&it, &val, &step, &name, &name_len)) ValkeyModule_ReplyWithStringBuffer(ctx, name, name_len);
}

static int jsonObjkeysCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len;
    const char *path, *err;
    ValkeyModuleKey *key;
    jsonQuery q;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (jsonReplyKeyState(ctx, jsonKeyCheck(key), NULL)) {
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }
    jsonQueryRun(&q, path, path_len, jsonKeyDoc(key), JSON_PATH_READ);
    err = jsonQueryError(&q, 1);
    if (err == NULL) err = jsonLegacyCheck(&q, JSON_MASK_OBJECT, JSON_MSG_NOT_OBJECT);
    if (err != NULL) {
        /* An invalid path and a path that does not exist reply null. */
        if (strcmp(err, JSON_MSG_INVALID_PATH) == 0 || strcmp(err, JSON_MSG_PATH_NOT_EXIST) == 0) {
            ValkeyModule_ReplyWithNull(ctx);
        } else {
            ValkeyModule_ReplyWithError(ctx, err);
        }
    } else if (q.v2) {
        ValkeyModule_ReplyWithArray(ctx, q.m.count);
        for (uint32_t i = 0; i < q.m.count; i++) jsonReplyObjectKeys(ctx, &q.m.items[i].ref);
    } else {
        /* The first object that has keys, else an empty array. */
        uint32_t i;
        for (i = 0; i < q.m.count; i++) {
            const jsonRef *r = &q.m.items[i].ref;
            if (jsonRefType(r) == JSON_OBJECT && jsonRefCount(r) > 0) break;
        }
        if (i < q.m.count) {
            jsonReplyObjectKeys(ctx, &q.m.items[i].ref);
        } else {
            ValkeyModule_ReplyWithEmptyArray(ctx);
        }
    }
    jsonReadDone(key, &q);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.TYPE key [path]
 * ------------------------------------------------------------------------- */

static const char *jsonTypeName(const jsonRef *r) {
    switch (jsonRefType(r)) {
    case JSON_NULL: return "null";
    case JSON_FALSE:
    case JSON_TRUE: return "boolean";
    case JSON_INT: return "integer";
    case JSON_DOUBLE:
    case JSON_RAWNUM: return "number";
    case JSON_STRING: return "string";
    case JSON_ARRAY: return "array";
    case JSON_OBJECT: return "object";
    }
    return "null";
}

static int jsonTypeCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len;
    const char *path, *err;
    ValkeyModuleKey *key;
    jsonQuery q;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (jsonReplyKeyState(ctx, jsonKeyCheck(key), NULL)) {
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }
    jsonQueryRun(&q, path, path_len, jsonKeyDoc(key), JSON_PATH_READ);
    err = jsonQueryError(&q, 1);
    if (err == NULL && !q.v2 && q.m.count == 0) err = JSON_MSG_PATH_NOT_EXIST;
    if (err != NULL) {
        if (strcmp(err, JSON_MSG_INVALID_PATH) == 0 || strcmp(err, JSON_MSG_PATH_NOT_EXIST) == 0) {
            ValkeyModule_ReplyWithNull(ctx);
        } else {
            ValkeyModule_ReplyWithError(ctx, err);
        }
    } else if (q.v2) {
        ValkeyModule_ReplyWithArray(ctx, q.m.count);
        for (uint32_t i = 0; i < q.m.count; i++) {
            ValkeyModule_ReplyWithSimpleString(ctx, jsonTypeName(&q.m.items[i].ref));
        }
    } else {
        ValkeyModule_ReplyWithSimpleString(ctx, jsonTypeName(&q.m.items[0].ref));
    }
    jsonReadDone(key, &q);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.ARRINDEX key path value [start [stop]]
 * ------------------------------------------------------------------------- */

/* The module's internal_array_index_of(). `start` is already at least 0.
 * A stop of 0 or -1 means the end. No other negative index is understood. */
static long long jsonArrayIndexOf(const jsonRef *arr, const jsonRef *needle, long long start, long long stop) {
    long long size = jsonRefCount(arr);
    jsonIter it;
    jsonRef el;
    jsonStep step;
    const char *name;
    uint32_t name_len;

    if (size == 0) return -1;
    if (stop == 0 || stop == -1) stop = size;
    if (stop > size) stop = size;
    if (start > stop) return -1;
    jsonIterInit(&it, arr);
    for (long long i = 0; i < stop; i++) {
        if (!jsonIterNext(&it, &el, &step, &name, &name_len)) break;
        if (i >= start && jsonRefEquals(&el, needle)) return i;
    }
    return -1;
}

static int jsonArrindexCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long start = 0, stop = 0;
    size_t path_len, json_len;
    const char *path, *json, *err;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonVal *v;
    jsonDoc needle_doc;
    jsonRef needle;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;

    if (argc < 4 || argc > 6) return ValkeyModule_WrongArity(ctx);
    path = jsonArgPath(argv[2], &path_len);
    json = ValkeyModule_StringPtrLen(argv[3], &json_len);
    if (argc > 4 && ValkeyModule_StringToLongLong(argv[4], &start) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, JSON_MSG_VALUE_NOT_INTEGER);
    }
    if (argc > 5 && ValkeyModule_StringToLongLong(argv[5], &stop) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, JSON_MSG_VALUE_NOT_INTEGER);
    }
    if (!jsonReadQuery(ctx, argv[1], path, path_len, JSON_MSG_KEY_NOT_FOUND, &key, &q)) return VALKEYMODULE_OK;

    err = jsonLegacyCheck(&q, JSON_MASK_ARRAY, JSON_MSG_NOT_ARRAY);
    v = err == NULL ? jsonCmdParse(ctx, json, json_len, &err) : NULL;
    if (err != NULL) {
        ValkeyModule_ReplyWithError(ctx, err);
        jsonReadDone(key, &q);
        return VALKEYMODULE_OK;
    }
    needle_doc = jsonValToDoc(v);
    jsonDocRoot(needle_doc, &needle);
    if (start < 0) start = 0;

    ops = jsonOpsAlloc(ops_stack, q.m.count);
    for (uint32_t i = 0; i < q.m.count; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        ops[i].reply = jsonRefType(r) == JSON_ARRAY ? jsonArrayIndexOf(r, &needle, start, stop) : JSON_NA;
    }
    jsonReplyOps(ctx, ops, q.m.count, q.v2, 0);
    jsonScratchFree(ops, ops_stack);
    jsonDocFree(needle_doc);
    jsonReadDone(key, &q);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.RESP key [path]
 * ------------------------------------------------------------------------- */

/* A number that is not an int64, as the module prints it here: the shortest
 * text of its value as a double, with no '+' in an exponent. */
static void jsonReplyRespDouble(ValkeyModuleCtx *ctx, const jsonRef *r) {
    char buf[JSON_DOUBLE_BUF > JSON_NUM_BUF ? JSON_DOUBLE_BUF : JSON_NUM_BUF];
    double d = jsonRefDouble(r);
    int len, at;

    if (!isfinite(d)) { /* Only a number that came in on a trusted path. */
        uint32_t raw_len = 0;
        const char *raw = jsonRefNumText(r, buf, &raw_len);
        ValkeyModule_ReplyWithStringBuffer(ctx, raw, raw_len);
        return;
    }
    len = jsonDoublePrint(d, buf);
    for (at = 0; at < len && buf[at] != '+'; at++) {
    }
    if (at < len) {
        memmove(buf + at, buf + at + 1, (size_t)(len - at - 1));
        len--;
    }
    ValkeyModule_ReplyWithStringBuffer(ctx, buf, (size_t)len);
}

/* Replies a scalar, or opens the reply of a container and returns 1. An array
 * is "[" and its elements. An object is "{" and a [name, value] pair per
 * member. */
static int jsonReplyRespOpen(ValkeyModuleCtx *ctx, const jsonRef *r) {
    int64_t i;
    uint32_t len = 0;
    const char *s;

    switch (jsonRefType(r)) {
    case JSON_NULL: ValkeyModule_ReplyWithNull(ctx); return 0;
    case JSON_FALSE: ValkeyModule_ReplyWithSimpleString(ctx, "false"); return 0;
    case JSON_TRUE: ValkeyModule_ReplyWithSimpleString(ctx, "true"); return 0;
    case JSON_INT:
        jsonRefInt(r, &i);
        ValkeyModule_ReplyWithLongLong(ctx, i);
        return 0;
    case JSON_DOUBLE:
    case JSON_RAWNUM: jsonReplyRespDouble(ctx, r); return 0;
    case JSON_STRING:
        s = jsonRefString(r, &len);
        ValkeyModule_ReplyWithStringBuffer(ctx, s, len);
        return 0;
    case JSON_ARRAY:
        ValkeyModule_ReplyWithArray(ctx, (long)jsonRefCount(r) + 1);
        ValkeyModule_ReplyWithSimpleString(ctx, "[");
        return 1;
    case JSON_OBJECT:
        ValkeyModule_ReplyWithArray(ctx, (long)jsonRefCount(r) + 1);
        ValkeyModule_ReplyWithSimpleString(ctx, "{");
        return 1;
    }
    return 0;
}

static void jsonReplyResp(ValkeyModuleCtx *ctx, const jsonRef *root) {
    jsonWalk w;

    if (!jsonReplyRespOpen(ctx, root)) return;
    jsonWalkInit(&w);
    jsonWalkPush(&w, root);
    while (w.n > 0) {
        jsonWalkFrame *fr = &w.f[w.n - 1];
        jsonRef val;
        jsonStep step;
        const char *name = NULL;
        uint32_t name_len = 0;
        if (!jsonIterNext(&fr->it, &val, &step, &name, &name_len)) {
            w.n--;
            continue;
        }
        if (fr->is_obj) {
            ValkeyModule_ReplyWithArray(ctx, 2);
            ValkeyModule_ReplyWithStringBuffer(ctx, name, name_len);
        }
        if (jsonReplyRespOpen(ctx, &val)) jsonWalkPush(&w, &val);
    }
    jsonWalkFree(&w);
}

static int jsonRespCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len;
    const char *path;
    ValkeyModuleKey *key;
    jsonQuery q;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    if (!jsonReadQuery(ctx, argv[1], path, path_len, NULL, &key, &q)) return VALKEYMODULE_OK;

    if (q.m.count == 0) {
        if (q.v2) {
            ValkeyModule_ReplyWithEmptyArray(ctx);
        } else {
            ValkeyModule_ReplyWithError(ctx, JSON_MSG_PATH_NOT_EXIST);
        }
    } else if (q.v2) {
        ValkeyModule_ReplyWithArray(ctx, q.m.count);
        for (uint32_t i = 0; i < q.m.count; i++) jsonReplyResp(ctx, &q.m.items[i].ref);
    } else {
        /* A legacy path replies its first value. The module sends one reply
         * per selected value here, which breaks the protocol when a legacy
         * wildcard selects several. That is not reproduced. */
        jsonReplyResp(ctx, &q.m.items[0].ref);
    }
    jsonReadDone(key, &q);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * Write commands: shared steps
 * ------------------------------------------------------------------------- */

/* Steps 1 and the first half of 2 for a command that changes an existing
 * document: opens the key and resolves the path into the values to change. A
 * strict command fails on any path error, as the module does even for a
 * JSONPath path. Returns 1 to go on, 0 after replying. */
static int jsonWriteQuery(ValkeyModuleCtx *ctx,
                          ValkeyModuleString *keyname,
                          const char *path,
                          size_t path_len,
                          int lenient,
                          ValkeyModuleKey **key,
                          jsonQuery *q) {
    const char *err;

    *key = jsonOpenWrite(ctx, keyname);
    if (*key == NULL) return 0;
    if (jsonReplyKeyState(ctx, jsonKeyCheck(*key), JSON_MSG_KEY_NOT_FOUND)) {
        ValkeyModule_CloseKey(*key);
        return 0;
    }
    jsonQueryRun(q, path, path_len, jsonKeyDoc(*key), JSON_PATH_UPDATE);
    err = jsonQueryError(q, lenient);
    if (err != NULL) {
        ValkeyModule_ReplyWithError(ctx, err);
        jsonQueryFree(q);
        ValkeyModule_CloseKey(*key);
        return 0;
    }
    return 1;
}

static int jsonWriteFail(ValkeyModuleCtx *ctx, ValkeyModuleKey *key, jsonQuery *q, const char *err) {
    ValkeyModule_ReplyWithError(ctx, err);
    jsonQueryFree(q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* Step 4. The module does both for every write that got this far, including
 * one whose JSONPath path selected nothing. */
static void jsonWritePublish(ValkeyModuleCtx *ctx, const char *event, ValkeyModuleString *keyname) {
    ValkeyModule_ReplicateVerbatim(ctx);
    ValkeyModule_NotifyKeyspaceEvent(ctx, VALKEYMODULE_NOTIFY_GENERIC, event, keyname);
}

static size_t jsonSizeDelta(size_t a, size_t b) {
    return a > b ? a - b : b - a;
}

/* ----------------------------------------------------------------------------
 * JSON.NUMINCRBY key path number, JSON.NUMMULTBY key path number
 * ------------------------------------------------------------------------- */

enum {
    JSON_NUM_NONE = 0,
    JSON_NUM_INT,
    JSON_NUM_DOUBLE
};

typedef struct jsonNumber {
    int kind;
    int64_t i;
    double d;
} jsonNumber;

static int jsonIsDigit(char c) {
    return c >= '0' && c <= '9';
}

static int jsonIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Reads the number argument: one JSON number, with JSON whitespace allowed
 * around it. Integer syntax within int64 is an integer, anything else a
 * double. Returns 0 if the text is not a number or does not fit a double. */
static int jsonParseNumberArg(const char *s, size_t n, jsonNumber *out) {
    size_t i = 0, end = n, begin;
    int is_int = 1, overflow = 0;

    while (i < end && jsonIsSpace(s[i])) i++;
    while (end > i && jsonIsSpace(s[end - 1])) end--;
    begin = i;
    if (i < end && s[i] == '-') i++;
    if (i >= end) return 0;
    if (s[i] == '0') {
        i++;
    } else if (jsonIsDigit(s[i])) {
        while (i < end && jsonIsDigit(s[i])) i++;
    } else {
        return 0;
    }
    if (i < end && s[i] == '.') {
        i++;
        if (i >= end || !jsonIsDigit(s[i])) return 0;
        while (i < end && jsonIsDigit(s[i])) i++;
        is_int = 0;
    }
    if (i < end && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < end && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= end || !jsonIsDigit(s[i])) return 0;
        while (i < end && jsonIsDigit(s[i])) i++;
        is_int = 0;
    }
    if (i != end) return 0;

    if (is_int) {
        int neg = s[begin] == '-';
        int64_t v = 0;
        for (i = begin + (size_t)neg; i < end; i++) {
            int64_t digit = s[i] - '0';
            if (__builtin_mul_overflow(v, 10, &v) ||
                (neg ? __builtin_sub_overflow(v, digit, &v) : __builtin_add_overflow(v, digit, &v))) {
                break;
            }
        }
        if (i == end) {
            out->kind = JSON_NUM_INT;
            out->i = v;
            out->d = (double)v;
            return 1;
        }
    }
    out->kind = JSON_NUM_DOUBLE;
    out->i = 0;
    out->d = jsonDoubleParse(s + begin, end - begin, &overflow);
    return !overflow;
}

/* Design 5.4: two integers give an exact integer when it fits int64. Anything
 * else is done in doubles, and a result that is not finite is an error. */
static int jsonArith(const jsonRef *r, const jsonNumber *by, int mult, jsonNumber *out) {
    int64_t a;

    if (by->kind == JSON_NUM_INT && jsonRefInt(r, &a)) {
        int overflow = mult ? __builtin_mul_overflow(a, by->i, &out->i) : __builtin_add_overflow(a, by->i, &out->i);
        if (!overflow) {
            out->kind = JSON_NUM_INT;
            return 1;
        }
    }
    out->kind = JSON_NUM_DOUBLE;
    out->d = mult ? jsonRefDouble(r) * by->d : jsonRefDouble(r) + by->d;
    return isfinite(out->d);
}

static void jsonAppendNumber(jsonBuf *out, const jsonNumber *num) {
    char buf[JSON_DOUBLE_BUF];
    int len = num->kind == JSON_NUM_INT ? jsonInt64Print(num->i, buf) : jsonDoublePrint(num->d, buf);
    jsonBufAppend(out, buf, (size_t)len);
}

static int jsonNumCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int mult) {
    size_t path_len, arg_len, orig_size;
    const char *path, *arg, *err;
    jsonNumber by, res_stack[JSON_INLINE_MATCHES], *res;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d, before;
    char stack[256];
    jsonBuf out;

    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    path = jsonArgPath(argv[2], &path_len);
    arg = ValkeyModule_StringPtrLen(argv[3], &arg_len);
    if (!jsonParseNumberArg(arg, arg_len, &by)) {
        /* On a trusted path the argument ends at its first NUL, as the module reads it. */
        const char *nul = memchr(arg, '\0', arg_len);
        if (nul == NULL || !jsonCmdTrusted(ctx) || !jsonParseNumberArg(arg, (size_t)(nul - arg), &by)) {
            return ValkeyModule_ReplyWithError(ctx, JSON_MSG_VALUE_NOT_NUMBER);
        }
    }
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 0, &key, &q)) return VALKEYMODULE_OK;
    err = jsonLegacyCheck(&q, JSON_MASK_NUMBER, JSON_MSG_NOT_NUMBER);
    if (err != NULL) return jsonWriteFail(ctx, key, &q, err);

    /* Check: every result is computed before anything changes (B6). */
    res = jsonScratch(res_stack, sizeof(res_stack), q.m.count * sizeof(jsonNumber));
    for (uint32_t i = 0; i < q.m.count; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        if (!(JSON_TYPE_BIT(jsonRefType(r)) & JSON_MASK_NUMBER)) {
            res[i].kind = JSON_NUM_NONE;
        } else if (!jsonArith(r, &by, mult, &res[i])) {
            jsonScratchFree(res, res_stack);
            return jsonWriteFail(ctx, key, &q, mult ? JSON_MSG_MULT_OVERFLOW : JSON_MSG_ADD_OVERFLOW);
        }
    }

    /* Apply. */
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        const jsonNumber *num = &res[order[k]];
        if (num->kind == JSON_NUM_INT) {
            jsonSetInt(&d, mt->loc, (int)mt->depth, num->i);
        } else if (num->kind == JSON_NUM_DOUBLE) {
            jsonSetDouble(&d, mt->loc, (int)mt->depth, num->d);
        }
    }
    jsonScratchFree(order, order_stack);
    jsonKeyStore(key, before, d);
    jsonStatsOnUpdate(orig_size, jsonDocMemUsage(d), arg_len);
    jsonWritePublish(ctx, mult ? "json.nummultby" : "json.numincrby", argv[1]);

    /* Reply: a bulk string. A legacy path gives the last number changed, a
     * JSONPath path a JSON array with null for each value that is not a
     * number. */
    jsonBufInit(&out, stack, sizeof(stack));
    if (q.v2) {
        jsonBufAppend(&out, "[", 1);
        for (uint32_t i = 0; i < q.m.count; i++) {
            if (i > 0) jsonBufAppend(&out, ",", 1);
            if (res[i].kind == JSON_NUM_NONE) {
                jsonBufAppend(&out, "null", 4);
            } else {
                jsonAppendNumber(&out, &res[i]);
            }
        }
        jsonBufAppend(&out, "]", 1);
    } else {
        for (uint32_t i = q.m.count; i-- > 0;) {
            if (res[i].kind != JSON_NUM_NONE) {
                jsonAppendNumber(&out, &res[i]);
                break;
            }
        }
    }
    ValkeyModule_ReplyWithStringBuffer(ctx, out.p, out.len);
    jsonBufFree(&out);
    jsonScratchFree(res, res_stack);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

static int jsonNumincrbyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonNumCommand(ctx, argv, argc, 0);
}

static int jsonNummultbyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonNumCommand(ctx, argv, argc, 1);
}

/* ----------------------------------------------------------------------------
 * JSON.STRAPPEND key [path] json-string
 * ------------------------------------------------------------------------- */

static int jsonStrappendCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len = 1, json_len, orig_size;
    const char *path = ".", *json, *err, *app = NULL;
    uint32_t app_len = 0, strings = 0;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonVal *v;
    jsonDoc d, before, app_doc = 0;
    jsonRef app_ref;

    if (argc != 3 && argc != 4) return ValkeyModule_WrongArity(ctx);
    if (argc == 4) path = jsonArgPath(argv[2], &path_len);
    json = ValkeyModule_StringPtrLen(argv[argc - 1], &json_len);
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 0, &key, &q)) return VALKEYMODULE_OK;
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);

    /* Check. */
    err = jsonLegacyCheck(&q, JSON_MASK_STRING, JSON_MSG_NOT_STRING);
    v = err == NULL ? jsonCmdParse(ctx, json, json_len, &err) : NULL;
    if (v != NULL && jsonValType(v) != JSON_STRING) {
        jsonValFree(v);
        err = JSON_MSG_VALUE_NOT_STRING;
    } else if (v != NULL) {
        /* The decoded bytes are read through a reference, which needs a document. */
        app_doc = jsonValToDoc(v);
        jsonDocRoot(app_doc, &app_ref);
        app = jsonRefString(&app_ref, &app_len);
    }
    ops = jsonOpsAlloc(ops_stack, q.m.count);
    for (uint32_t i = 0; i < q.m.count && err == NULL; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        uint32_t len = 0;
        if (jsonRefType(r) != JSON_STRING) {
            ops[i].reply = JSON_NA;
            continue;
        }
        jsonRefString(r, &len);
        ops[i].reply = (long long)len + app_len;
        strings++;
    }
    if (err == NULL && jsonSizeExceeded(jsonCmdSizeLimit(ctx), orig_size, (size_t)strings * app_len, 0)) {
        err = JSON_MSG_SIZE_LIMIT;
    }
    if (err != NULL) {
        jsonScratchFree(ops, ops_stack);
        if (app_doc != 0) jsonDocFree(app_doc);
        return jsonWriteFail(ctx, key, &q, err);
    }

    /* Apply. The whole string is appended: the module cuts both operands at
     * an embedded NUL, which is not reproduced. */
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        if (ops[order[k]].reply != JSON_NA) jsonStringAppend(&d, mt->loc, (int)mt->depth, app, app_len);
    }
    jsonScratchFree(order, order_stack);
    jsonDocFree(app_doc);
    jsonKeyStore(key, before, d);
    jsonStatsOnUpdate(orig_size, jsonDocMemUsage(d), json_len);
    jsonWritePublish(ctx, "json.strappend", argv[1]);

    jsonReplyOps(ctx, ops, q.m.count, q.v2, 1);
    jsonScratchFree(ops, ops_stack);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.TOGGLE key [path]
 * ------------------------------------------------------------------------- */

static int jsonToggleCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len;
    const char *path, *err;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d, before;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    /* The one write that treats path errors the way the reads do. */
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 1, &key, &q)) return VALKEYMODULE_OK;
    err = jsonLegacyCheck(&q, JSON_MASK_BOOL, JSON_MSG_NOT_BOOL);
    if (err != NULL) return jsonWriteFail(ctx, key, &q, err);

    ops = jsonOpsAlloc(ops_stack, q.m.count);
    for (uint32_t i = 0; i < q.m.count; i++) {
        jsonType t = jsonRefType(&q.m.items[i].ref);
        ops[i].reply = t == JSON_FALSE ? 1 : t == JSON_TRUE ? 0
                                                            : JSON_NA;
    }

    d = before = jsonKeyDoc(key);
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        if (ops[order[k]].reply != JSON_NA) jsonSetBool(&d, mt->loc, (int)mt->depth, (int)ops[order[k]].reply);
    }
    jsonScratchFree(order, order_stack);
    jsonKeyStore(key, before, d);
    jsonWritePublish(ctx, "json.toggle", argv[1]); /* The module updates no statistics here. */

    if (q.v2) {
        jsonReplyOps(ctx, ops, q.m.count, 1, 0);
    } else {
        /* A legacy path replies the first new value as "true" or "false". */
        for (uint32_t i = 0; i < q.m.count; i++) {
            if (ops[i].reply == JSON_NA) continue;
            ValkeyModule_ReplyWithStringBuffer(ctx, ops[i].reply ? "true" : "false", ops[i].reply ? 4 : 5);
            break;
        }
    }
    jsonScratchFree(ops, ops_stack);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.ARRAPPEND key path json [json ...]
 * JSON.ARRINSERT key path index json [json ...]
 * ------------------------------------------------------------------------- */

static int jsonArrayAddCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int insert) {
    int first_val = insert ? 4 : 3;
    uint32_t num_vals, parsed = 0, arrays = 0, max_depth;
    long long index = 0;
    size_t path_len, total_json_len = 0, estimate = 0, orig_size, new_size;
    const char *path, *err;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;
    jsonVal *vals_stack[JSON_INLINE_MATCHES], **vals;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d, before;

    if (argc < first_val + 1) return ValkeyModule_WrongArity(ctx);
    num_vals = (uint32_t)(argc - first_val);
    path = jsonArgPath(argv[2], &path_len);
    if (insert && ValkeyModule_StringToLongLong(argv[3], &index) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, JSON_MSG_VALUE_NOT_INTEGER);
    }
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 0, &key, &q)) return VALKEYMODULE_OK;
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);

    /* Check, in the module's order: the kind of the selected values, each new
     * value and its depth, the size limit, then the index against each array. */
    err = jsonLegacyCheck(&q, JSON_MASK_ARRAY, JSON_MSG_NOT_ARRAY);
    vals = jsonScratch(vals_stack, sizeof(vals_stack), num_vals * sizeof(jsonVal *));
    ops = jsonOpsAlloc(ops_stack, q.m.count);
    max_depth = jsonMatchesMaxDepth(&q.m);
    for (; parsed < num_vals && err == NULL; parsed++) {
        size_t len;
        const char *json = ValkeyModule_StringPtrLen(argv[first_val + (int)parsed], &len);
        total_json_len += len;
        vals[parsed] = jsonCmdParse(ctx, json, len, &err);
        if (vals[parsed] == NULL) break;
        estimate += jsonValSizeEstimate(vals[parsed]);
        /* The module adds the depth of the array, not of its new element, so
         * for a client an appended value may nest one level past the limit.
         * A trusted command is counted exactly: the load bound it is held to
         * is the one a later reload enforces. */
        if (jsonDepthExceeded(ctx, max_depth + (jsonCmdTrusted(ctx) ? 1 : 0), jsonValDepth(vals[parsed]))) {
            err = JSON_MSG_DEPTH_LIMIT;
        }
    }
    for (uint32_t i = 0; i < q.m.count; i++) {
        if (jsonRefType(&q.m.items[i].ref) == JSON_ARRAY) arrays++;
    }
    if (err == NULL && jsonSizeExceeded(jsonCmdSizeLimit(ctx), orig_size, (size_t)arrays * estimate, 0)) {
        err = JSON_MSG_SIZE_LIMIT;
    }
    for (uint32_t i = 0; i < q.m.count && err == NULL; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        long long size, at;
        if (jsonRefType(r) != JSON_ARRAY) {
            ops[i].reply = JSON_NA;
            continue;
        }
        size = at = jsonRefCount(r);
        if (insert) {
            /* A negative index counts from the end. Past either end is an error. */
            at = index < 0 ? (size == 0 ? 0 : size + index) : index;
            if (at < 0 || at > size) err = JSON_MSG_INDEX_RANGE;
        }
        ops[i].reply = size + num_vals;
        ops[i].a = (uint32_t)at;
    }

    /* Apply. */
    if (err == NULL) {
        order = jsonApplyOrder(&q.m, order_stack);
        for (uint32_t k = 0; k < q.m.count; k++) {
            const jsonMatch *mt = &q.m.items[order[k]];
            const jsonOp *op = &ops[order[k]];
            if (op->reply == JSON_NA) continue;
            jsonArrayInsert(&d, mt->loc, (int)mt->depth, op->a, (const jsonVal *const *)vals, num_vals);
        }
        jsonScratchFree(order, order_stack);
        jsonKeyStore(key, before, d);
        new_size = jsonDocMemUsage(d);
        if (insert) {
            jsonStatsOnInsert(0, orig_size, new_size, total_json_len);
        } else {
            jsonStatsOnUpdate(orig_size, new_size, total_json_len);
        }
        jsonWritePublish(ctx, insert ? "json.arrinsert" : "json.arrappend", argv[1]);
        jsonReplyOps(ctx, ops, q.m.count, q.v2, 0);
    } else {
        ValkeyModule_ReplyWithError(ctx, err);
    }
    for (uint32_t i = 0; i < parsed; i++) jsonValFree(vals[i]);
    jsonScratchFree(vals, vals_stack);
    jsonScratchFree(ops, ops_stack);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

static int jsonArrappendCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonArrayAddCommand(ctx, argv, argc, 0);
}

static int jsonArrinsertCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return jsonArrayAddCommand(ctx, argv, argc, 1);
}

/* ----------------------------------------------------------------------------
 * JSON.ARRPOP key [path [index]]
 * ------------------------------------------------------------------------- */

#define JSON_INLINE_STEPS 64

static int jsonArrpopCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long index = -1;
    size_t path_len = 1, orig_size, new_size, from;
    const char *path = ".", *err;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;
    jsonStep steps_stack[JSON_INLINE_STEPS], *steps;
    size_t ends_stack[JSON_INLINE_MATCHES], *ends;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d, before;
    char stack[JSON_REPLY_STACK];
    jsonBuf out;

    if (argc < 2 || argc > 4) return ValkeyModule_WrongArity(ctx);
    if (argc > 2) path = jsonArgPath(argv[2], &path_len);
    if (argc > 3 && ValkeyModule_StringToLongLong(argv[3], &index) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, JSON_MSG_VALUE_NOT_INTEGER);
    }
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 0, &key, &q)) return VALKEYMODULE_OK;
    err = jsonLegacyCheck(&q, JSON_MASK_ARRAY, JSON_MSG_NOT_ARRAY);
    if (err != NULL) return jsonWriteFail(ctx, key, &q, err);

    /* Check: pick the element of each array and serialise it. An index past
     * either end is brought to that end. An empty array gives null. */
    jsonBufInit(&out, stack, sizeof(stack));
    ops = jsonOpsAlloc(ops_stack, q.m.count);
    ends = jsonScratch(ends_stack, sizeof(ends_stack), q.m.count * sizeof(size_t));
    for (uint32_t i = 0; i < q.m.count; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        long long size = jsonRefType(r) == JSON_ARRAY ? jsonRefCount(r) : 0;
        jsonRef el;
        ops[i].reply = JSON_NA;
        if (size > 0) {
            long long at = index < 0 ? size + index : index;
            if (at >= size) at = size - 1;
            if (at < 0) at = 0;
            if (jsonArrayGet(r, (uint32_t)at, &el)) {
                jsonBufReserve(&out, jsonSerializeSizeHint(&el));
                jsonSerialize(&el, NULL, &out);
                ops[i].reply = at;
            }
        }
        ends[i] = out.len;
    }

    /* Apply. */
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);
    steps = jsonScratch(steps_stack, sizeof(steps_stack), ((size_t)jsonMatchesDeepest(&q.m) + 1) * sizeof(jsonStep));
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        if (ops[order[k]].reply == JSON_NA) continue;
        if (mt->depth > 0) memcpy(steps, mt->loc, mt->depth * sizeof(jsonStep));
        steps[mt->depth].pos = (uint32_t)ops[order[k]].reply;
        steps[mt->depth].keyid = 0;
        jsonDelete(&d, steps, (int)mt->depth + 1);
    }
    jsonScratchFree(order, order_stack);
    jsonScratchFree(steps, steps_stack);
    jsonKeyStore(key, before, d);
    new_size = jsonDocMemUsage(d);
    jsonStatsOnDelete(0, orig_size, new_size, jsonSizeDelta(orig_size, new_size));
    jsonWritePublish(ctx, "json.arrpop", argv[1]);

    /* Reply: the popped values as JSON text. */
    from = 0;
    if (q.v2) {
        ValkeyModule_ReplyWithArray(ctx, q.m.count);
        for (uint32_t i = 0; i < q.m.count; i++) {
            if (ends[i] == from) {
                ValkeyModule_ReplyWithNull(ctx);
            } else {
                ValkeyModule_ReplyWithStringBuffer(ctx, out.p + from, ends[i] - from);
            }
            from = ends[i];
        }
    } else {
        uint32_t i;
        for (i = 0; i < q.m.count && ends[i] == from; i++) {
        }
        if (i < q.m.count) {
            ValkeyModule_ReplyWithStringBuffer(ctx, out.p + from, ends[i] - from);
        } else {
            ValkeyModule_ReplyWithNull(ctx);
        }
    }
    jsonScratchFree(ends, ends_stack);
    jsonScratchFree(ops, ops_stack);
    jsonBufFree(&out);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.ARRTRIM key path start stop
 * ------------------------------------------------------------------------- */

static int jsonArrtrimCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long start, stop;
    size_t path_len, orig_size, new_size;
    const char *path, *err;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d, before;

    if (argc != 5) return ValkeyModule_WrongArity(ctx);
    path = jsonArgPath(argv[2], &path_len);
    if (ValkeyModule_StringToLongLong(argv[3], &start) == VALKEYMODULE_ERR ||
        ValkeyModule_StringToLongLong(argv[4], &stop) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, JSON_MSG_VALUE_NOT_INTEGER);
    }
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 0, &key, &q)) return VALKEYMODULE_OK;
    err = jsonLegacyCheck(&q, JSON_MASK_ARRAY, JSON_MSG_NOT_ARRAY);
    if (err != NULL) return jsonWriteFail(ctx, key, &q, err);

    /* Check: the range of each array that stays. Both ends are inclusive. A
     * negative index is not counted from the end: a negative start is 0 and
     * a negative stop empties the array, as in the module. */
    ops = jsonOpsAlloc(ops_stack, q.m.count);
    for (uint32_t i = 0; i < q.m.count; i++) {
        const jsonRef *r = &q.m.items[i].ref;
        long long size, from = start < 0 ? 0 : start, to = stop;
        if (jsonRefType(r) != JSON_ARRAY) {
            ops[i].reply = JSON_NA;
            continue;
        }
        size = jsonRefCount(r);
        if (to >= size) to = size - 1;
        if (size == 0 || from >= size || from > to) {
            from = 0;
            to = -1;
        }
        ops[i].reply = to - from + 1;
        ops[i].a = (uint32_t)from;
        ops[i].b = ops[i].reply != size; /* Whether anything is removed. */
    }

    /* Apply. */
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        const jsonOp *op = &ops[order[k]];
        if (op->reply == JSON_NA || !op->b) continue;
        jsonArrayTrim(&d, mt->loc, (int)mt->depth, op->a, (uint32_t)op->reply);
    }
    jsonScratchFree(order, order_stack);
    jsonKeyStore(key, before, d);
    new_size = jsonDocMemUsage(d);
    jsonStatsOnDelete(0, orig_size, new_size, jsonSizeDelta(orig_size, new_size));
    jsonWritePublish(ctx, "json.arrtrim", argv[1]);

    jsonReplyOps(ctx, ops, q.m.count, q.v2, 0);
    jsonScratchFree(ops, ops_stack);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.CLEAR key [path]
 * ------------------------------------------------------------------------- */

enum {
    JSON_CLEAR_NOTHING = 0,
    JSON_CLEAR_ARRAY,
    JSON_CLEAR_OBJECT,
    JSON_CLEAR_BOOL,
    JSON_CLEAR_STRING,
    JSON_CLEAR_INT,
    JSON_CLEAR_DOUBLE
};

/* What clearing does to a value, or JSON_CLEAR_NOTHING if it is already
 * clear. Containers are emptied, a string becomes "", true becomes false, an
 * integer becomes 0 and any other number 0.0. Null is left alone. */
static int jsonClearAction(const jsonRef *r) {
    uint32_t len = 0;
    int64_t i;

    switch (jsonRefType(r)) {
    case JSON_ARRAY: return jsonRefCount(r) > 0 ? JSON_CLEAR_ARRAY : JSON_CLEAR_NOTHING;
    case JSON_OBJECT: return jsonRefCount(r) > 0 ? JSON_CLEAR_OBJECT : JSON_CLEAR_NOTHING;
    case JSON_TRUE: return JSON_CLEAR_BOOL;
    case JSON_STRING:
        jsonRefString(r, &len);
        return len > 0 ? JSON_CLEAR_STRING : JSON_CLEAR_NOTHING;
    case JSON_INT:
        jsonRefInt(r, &i);
        return i != 0 ? JSON_CLEAR_INT : JSON_CLEAR_NOTHING;
    case JSON_DOUBLE:
    case JSON_RAWNUM: return jsonRefDouble(r) != 0.0 ? JSON_CLEAR_DOUBLE : JSON_CLEAR_NOTHING;
    default: return JSON_CLEAR_NOTHING;
    }
}

static int jsonClearCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len, orig_size, new_size;
    const char *path;
    long long cleared = 0;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    jsonOp ops_stack[JSON_INLINE_MATCHES], *ops;
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d, before;
    jsonVal *empty_object = NULL, *empty_string = NULL;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    /* No check on what a legacy path selected: nothing selected clears
     * nothing and still counts as a write, as in the module. */
    if (!jsonWriteQuery(ctx, argv[1], path, path_len, 0, &key, &q)) return VALKEYMODULE_OK;

    /* Check: each value is judged as the document is now, so a value inside
     * another selected value counts too (design 2.6, Q1 and Q2). */
    ops = jsonOpsAlloc(ops_stack, q.m.count);
    for (uint32_t i = 0; i < q.m.count; i++) {
        ops[i].a = (uint32_t)jsonClearAction(&q.m.items[i].ref);
        if (ops[i].a != JSON_CLEAR_NOTHING) cleared++;
    }

    /* Apply. Inner values go first and are then removed with the outer one. */
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        int n = (int)mt->depth;
        switch (ops[order[k]].a) {
        case JSON_CLEAR_ARRAY: jsonArrayTrim(&d, mt->loc, n, 0, 0); break;
        case JSON_CLEAR_OBJECT:
            if (empty_object == NULL) empty_object = jsonValEmptyObject();
            jsonSet(&d, mt->loc, n, empty_object);
            break;
        case JSON_CLEAR_BOOL: jsonSetBool(&d, mt->loc, n, 0); break;
        case JSON_CLEAR_STRING:
            if (empty_string == NULL) empty_string = jsonValString("", 0);
            jsonSet(&d, mt->loc, n, empty_string);
            break;
        case JSON_CLEAR_INT: jsonSetInt(&d, mt->loc, n, 0); break;
        case JSON_CLEAR_DOUBLE: jsonSetDouble(&d, mt->loc, n, 0.0); break;
        default: break;
        }
    }
    jsonScratchFree(order, order_stack);
    jsonValFree(empty_object);
    jsonValFree(empty_string);
    jsonKeyStore(key, before, d);
    new_size = jsonDocMemUsage(d);
    jsonStatsOnDelete(0, orig_size, new_size, jsonSizeDelta(orig_size, new_size));
    jsonWritePublish(ctx, "json.clear", argv[1]);

    ValkeyModule_ReplyWithLongLong(ctx, cleared);
    jsonScratchFree(ops, ops_stack);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.DEL key [path], JSON.FORGET key [path]
 * ------------------------------------------------------------------------- */

static int jsonDelCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len, orig_size, new_size;
    const char *path, *err;
    long long deleted = 0;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    ValkeyModuleKey *key;
    jsonKeyState ks;
    jsonQuery q;
    jsonDoc d, before;

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);
    path = jsonArgOptionalPath(argv, argc, &path_len);
    key = jsonOpenWrite(ctx, argv[1]);
    if (key == NULL) return VALKEYMODULE_OK;
    ks = jsonKeyCheck(key);
    if (ks != JSON_KEY_DOC) {
        if (ks == JSON_KEY_MISSING) {
            ValkeyModule_ReplyWithLongLong(ctx, 0);
        } else {
            ValkeyModule_ReplyWithError(ctx, JSON_MSG_NOT_DOC_KEY);
        }
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    if (jsonIsRootPath(path, path_len)) {
        ValkeyModule_DeleteKey(key);
        jsonWritePublish(ctx, "json.del", argv[1]);
        ValkeyModule_ReplyWithLongLong(ctx, 1);
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);
    jsonQueryRun(&q, path, path_len, d, JSON_PATH_DELETE);
    err = jsonQueryError(&q, 0);
    if (err == NULL && !q.v2 && q.m.count == 0) err = JSON_MSG_PATH_NOT_EXIST;
    if (err != NULL) {
        /* An invalid path and a legacy path that selects nothing reply 0 and
         * are not a write. */
        if (strcmp(err, JSON_MSG_INVALID_PATH) == 0 || strcmp(err, JSON_MSG_PATH_NOT_EXIST) == 0) {
            ValkeyModule_ReplyWithLongLong(ctx, 0);
        } else {
            ValkeyModule_ReplyWithError(ctx, err);
        }
        jsonQueryFree(&q);
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    /* Apply. A value inside another selected value is removed first and
     * counted, as in the module. The root cannot be removed by a path. */
    order = jsonApplyOrder(&q.m, order_stack);
    for (uint32_t k = 0; k < q.m.count; k++) {
        const jsonMatch *mt = &q.m.items[order[k]];
        if (mt->depth == 0) continue;
        jsonDelete(&d, mt->loc, (int)mt->depth);
        deleted++;
    }
    jsonScratchFree(order, order_stack);
    jsonKeyStore(key, before, d);
    new_size = jsonDocMemUsage(d);
    jsonStatsOnDelete(0, orig_size, new_size, jsonSizeDelta(orig_size, new_size));
    jsonWritePublish(ctx, "json.del", argv[1]);

    ValkeyModule_ReplyWithLongLong(ctx, deleted);
    jsonQueryFree(&q);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * Setting a value at a path (JSON.SET, JSON.MSET, and the inserts of
 * JSON.MERGE)
 * ------------------------------------------------------------------------- */

/* Adds the new members a path asked for. Runs before any value is replaced,
 * so every parent locator still resolves. A member added inside a value that
 * is then replaced goes with it, which is the outermost-wins rule (B4). The
 * name is literal (B5). */
static void jsonApplyInserts(jsonDoc *d, const jsonMatches *m, const jsonVal *v) {
    for (uint32_t i = 0; i < m->insert_count; i++) {
        const jsonInsert *ins = &m->inserts[i];
        if (m->insert_count > 1) {
            /* The same place can be named twice, by a union for example. */
            jsonRef parent, existing;
            if (!jsonDocResolve(*d, ins->parent, (int)ins->parent_depth, &parent)) continue;
            if (jsonObjectGet(&parent, ins->name, ins->name_len, &existing, NULL)) continue;
        }
        jsonObjectAdd(d, ins->parent, (int)ins->parent_depth, ins->name, ins->name_len, v);
    }
}

enum {
    JSON_SCALAR_NONE = 0,
    JSON_SCALAR_INT,
    JSON_SCALAR_BOOL
};

typedef struct jsonSetPlan {
    jsonQuery q;
    jsonVal *val;       /* The new value, unless it is a scalar held below. */
    uint8_t *nested;    /* Matches inside another match, or NULL if there are none. */
    int scalar;         /* JSON_SCALAR_*: the value needs no jsonVal. */
    int64_t scalar_int; /* The integer, or 0 and 1 for false and true. */
} jsonSetPlan;

/* Recognises the two most common new values without the parser: an integer
 * of at most 18 digits, and true or false. Anything else, surrounding
 * whitespace included, is left to the parser. */
static int jsonScalarArg(const char *s, size_t n, int64_t *out) {
    size_t i = 0;
    int64_t v = 0;

    if (n == 4 && memcmp(s, "true", 4) == 0) {
        *out = 1;
        return JSON_SCALAR_BOOL;
    }
    if (n == 5 && memcmp(s, "false", 5) == 0) {
        *out = 0;
        return JSON_SCALAR_BOOL;
    }
    if (n == 0 || n > 19) return JSON_SCALAR_NONE;
    if (s[0] == '-') i = 1;
    if (n - i == 0 || n - i > 18 || (s[i] == '0' && n - i > 1)) return JSON_SCALAR_NONE;
    for (; i < n; i++) {
        if (!jsonIsDigit(s[i])) return JSON_SCALAR_NONE;
        v = v * 10 + (s[i] - '0');
    }
    *out = s[0] == '-' ? -v : v; /* "-0" is the integer 0, as the parser has it. */
    return JSON_SCALAR_INT;
}

static void jsonSetPlanFree(jsonSetPlan *p) {
    jsonQueryFree(&p->q);
    jsonValFree(p->val);
    if (p->nested != NULL) ValkeyModule_Free(p->nested);
}

enum {
    JSON_SET_NX = 1,
    JSON_SET_XX = 2
};
enum {
    JSON_SET_GO = 0,
    JSON_SET_SKIP,
    JSON_SET_FAIL
};

/* The check step of a sub-path set, in the module's order (dom_set_value):
 * resolve the path, test NX and XX, parse the value, check the nesting and
 * the size. `parsed`, if not NULL, is the value already parsed and is taken
 * over. Returns JSON_SET_GO with the plan filled in, JSON_SET_SKIP when NX or
 * XX says no, or JSON_SET_FAIL with *err set. The plan is released unless the
 * result is JSON_SET_GO. */
static int jsonSetCheck(ValkeyModuleCtx *ctx,
                        jsonDoc d,
                        const char *path,
                        size_t path_len,
                        const char *json,
                        size_t json_len,
                        jsonVal *parsed,
                        int cond,
                        jsonSetPlan *p,
                        const char **err) {
    size_t limit;
    int rc = JSON_SET_FAIL;

    p->val = parsed;
    p->nested = NULL;
    p->scalar = JSON_SCALAR_NONE;
    jsonQueryRun(&p->q, path, path_len, d, JSON_PATH_INSERT);
    *err = jsonQueryError(&p->q, 0);
    if (*err != NULL) goto out;

    if ((cond == JSON_SET_NX && p->q.m.count > 0) || (cond == JSON_SET_XX && p->q.m.insert_count > 0)) {
        rc = JSON_SET_SKIP;
        goto out;
    }
    if (!jsonInsertNamesValid(ctx, &p->q.m)) {
        *err = JSON_MSG_PARSE;
        goto out;
    }
    if (p->val == NULL && p->q.m.count == 1 && p->q.m.insert_count == 0 && jsonConfig.max_document_size <= 0) {
        /* One value replaced by a plain scalar, and no size to estimate. */
        p->scalar = jsonScalarArg(json, json_len, &p->scalar_int);
        if (p->scalar != JSON_SCALAR_NONE) {
            if (!jsonDepthExceeded(ctx, jsonMatchesMaxDepth(&p->q.m), 0)) return JSON_SET_GO;
            *err = JSON_MSG_DEPTH_LIMIT;
            goto out;
        }
    }
    if (p->val == NULL) {
        p->val = jsonCmdParse(ctx, json, json_len, err);
        if (p->val == NULL) goto out;
    }
    if (jsonDepthExceeded(ctx, jsonMatchesMaxDepth(&p->q.m), jsonValDepth(p->val))) {
        *err = JSON_MSG_DEPTH_LIMIT;
        goto out;
    }
    p->nested = jsonNestedMatches(&p->q.m);
    limit = jsonCmdSizeLimit(ctx);
    if (limit > 0) {
        size_t places = p->q.m.insert_count, removed = 0;
        for (uint32_t i = 0; i < p->q.m.count; i++) {
            if (p->nested != NULL && p->nested[i]) continue;
            removed += jsonRefMemUsage(&p->q.m.items[i].ref);
            places++;
        }
        if (jsonSizeExceeded(limit, jsonDocMemUsage(d), places * jsonValSizeEstimate(p->val), removed)) {
            *err = JSON_MSG_SIZE_LIMIT;
            goto out;
        }
    }
    return JSON_SET_GO;

out:
    jsonSetPlanFree(p);
    return rc;
}

/* The apply step. A path that selected nothing and adds nothing changes
 * nothing, and the command still succeeds, as in the module. */
static void jsonSetApply(jsonDoc *d, const jsonSetPlan *p) {
    const jsonMatches *m = &p->q.m;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;

    if (p->scalar == JSON_SCALAR_INT) {
        jsonSetInt(d, m->items[0].loc, (int)m->items[0].depth, p->scalar_int);
        return;
    }
    if (p->scalar == JSON_SCALAR_BOOL) {
        jsonSetBool(d, m->items[0].loc, (int)m->items[0].depth, (int)p->scalar_int);
        return;
    }
    jsonApplyInserts(d, m, p->val);
    if (m->count == 0) return;
    order = jsonApplyOrder(m, order_stack);
    for (uint32_t k = 0; k < m->count; k++) {
        const jsonMatch *mt = &m->items[order[k]];
        if (p->nested != NULL && p->nested[order[k]]) continue;
        jsonSet(d, mt->loc, (int)mt->depth, p->val);
    }
    jsonScratchFree(order, order_stack);
}

/* A root set: the value becomes the whole document. Checks the size limit,
 * as the module's dom_parse() does. Returns NULL and sets *err on failure. */
static jsonVal *jsonParseDocument(ValkeyModuleCtx *ctx, const char *json, size_t json_len, const char **err) {
    jsonVal *v = jsonCmdParse(ctx, json, json_len, err);
    if (v == NULL) return NULL;
    if (jsonSizeExceeded(jsonCmdSizeLimit(ctx), 0, jsonValSizeEstimate(v), 0)) {
        jsonValFree(v);
        *err = JSON_MSG_SIZE_LIMIT;
        return NULL;
    }
    return v;
}

/* Makes `v` the value of the key. Any earlier value is freed and the TTL is
 * discarded. Consumes v. */
static void jsonKeySetDocument(ValkeyModuleKey *key, jsonVal *v) {
    jsonDoc d = jsonValToDoc(v);
    size_t size = jsonDocMemUsage(d);
    ValkeyModule_ModuleTypeSetValue(key, jsonModuleType, jsonValueNew(d));
    jsonStatsOnInsert(1, 0, size, size);
}

/* ----------------------------------------------------------------------------
 * JSON.SET key path json [NX | XX]
 * ------------------------------------------------------------------------- */

static int jsonSetCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len, json_len;
    const char *path, *json, *err = NULL;
    int cond = 0, is_new, is_root;
    ValkeyModuleKey *key;
    jsonKeyState ks;

    if (argc != 4 && argc != 5) return ValkeyModule_WrongArity(ctx);
    path = jsonArgPath(argv[2], &path_len);
    json = ValkeyModule_StringPtrLen(argv[3], &json_len);
    if (argc == 5) {
        const char *opt = ValkeyModule_StringPtrLen(argv[4], NULL);
        if (!strcasecmp(opt, "NX")) {
            cond = JSON_SET_NX;
        } else if (!strcasecmp(opt, "XX")) {
            cond = JSON_SET_XX;
        } else {
            return ValkeyModule_ReplyWithError(ctx, JSON_MSG_COMMAND_SYNTAX);
        }
    }

    key = jsonOpenWrite(ctx, argv[1]);
    if (key == NULL) return VALKEYMODULE_OK;
    ks = jsonKeyCheck(key);
    is_new = ks == JSON_KEY_MISSING;
    is_root = jsonIsRootPath(path, path_len);
    if (ks == JSON_KEY_WRONGTYPE) {
        err = JSON_MSG_NOT_DOC_KEY;
    } else if (is_new && !is_root) {
        err = JSON_MSG_NEW_KEY_NOT_ROOT;
    }
    if (err != NULL) goto fail;
    if (is_new ? cond == JSON_SET_XX : (is_root && cond == JSON_SET_NX)) goto skip;

    if (is_root) {
        jsonVal *v = jsonParseDocument(ctx, json, json_len, &err);
        if (v == NULL) goto fail;
        jsonKeySetDocument(key, v);
    } else {
        jsonDoc d, before;
        size_t orig_size;
        jsonSetPlan plan;
        int rc;

        d = before = jsonKeyDoc(key);
        orig_size = jsonDocMemUsage(d);
        rc = jsonSetCheck(ctx, d, path, path_len, json, json_len, NULL, cond, &plan, &err);
        if (rc == JSON_SET_SKIP) goto skip;
        if (rc == JSON_SET_FAIL) goto fail;
        jsonSetApply(&d, &plan);
        jsonSetPlanFree(&plan);
        jsonKeyStore(key, before, d);
        jsonStatsOnUpdate(orig_size, jsonDocMemUsage(d), json_len);
    }
    jsonWritePublish(ctx, "json.set", argv[1]);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;

skip:
    ValkeyModule_ReplyWithNull(ctx);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;

fail:
    ValkeyModule_ReplyWithError(ctx, err);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.MERGE key path json
 * ------------------------------------------------------------------------- */

/* Merges the object `patch` into the value at `loc`, in place, with the
 * result of the module's merge_values(). (A patch that is not an object
 * replaces the value, which the caller does.)
 *   - The patch turns a value that is not an object into {} first.
 *   - A null member removes the member of that name.
 *   - An object member merges into an object member of that name. Onto a
 *     member of another kind it is copied as it is, nulls included. This
 *     differs from RFC 7396 and is what the module does.
 *   - Any other member replaces, or is added at the end. A new member that is
 *     an object is added without its null members, at every level.
 * `patch` belongs to another document, so it stays valid throughout. */
static void jsonMergeAt(jsonDoc *d,
                        const jsonStep *loc,
                        uint32_t n,
                        const jsonRef *patch,
                        const jsonVal *empty_object) {
    jsonStep steps_stack[JSON_INLINE_STEPS], *steps;
    jsonRef target;
    jsonWalk w;

    if (!jsonDocResolve(*d, loc, (int)n, &target)) return;
    if (jsonRefType(&target) != JSON_OBJECT) jsonSet(d, loc, (int)n, empty_object);
    if (jsonRefCount(patch) == 0) return;

    steps = jsonScratch(steps_stack, sizeof(steps_stack), ((size_t)n + jsonRefDepth(patch) + 2) * sizeof(jsonStep));
    if (n > 0) memcpy(steps, loc, n * sizeof(jsonStep));
    jsonWalkInit(&w);
    jsonWalkPush(&w, patch)->aux = n;
    while (w.n > 0) {
        jsonWalkFrame *fr = &w.f[w.n - 1];
        uint32_t tn = fr->aux; /* steps[0..tn) names the object this frame merges into. */
        jsonRef pv, object, child;
        jsonStep pstep, cstep;
        const char *name = NULL;
        uint32_t name_len = 0;
        jsonType pt;

        if (!jsonIterNext(&fr->it, &pv, &pstep, &name, &name_len)) {
            w.n--;
            continue;
        }
        if (!jsonDocResolve(*d, steps, (int)tn, &object)) continue;
        pt = jsonRefType(&pv);
        if (jsonObjectGet(&object, name, name_len, &child, &cstep)) {
            steps[tn] = cstep;
            if (pt == JSON_NULL) {
                jsonDelete(d, steps, (int)tn + 1);
            } else if (pt == JSON_OBJECT && jsonRefType(&child) == JSON_OBJECT) {
                if (jsonRefCount(&pv) > 0) jsonWalkPush(&w, &pv)->aux = tn + 1;
            } else {
                jsonVal *copy = jsonValFromRef(&pv);
                jsonSet(d, steps, (int)tn + 1, copy);
                jsonValFree(copy);
            }
        } else if (pt == JSON_OBJECT) {
            jsonObjectAdd(d, steps, (int)tn, name, name_len, empty_object);
            if (jsonRefCount(&pv) > 0 && jsonDocResolve(*d, steps, (int)tn, &object) &&
                jsonObjectGet(&object, name, name_len, &child, &cstep)) {
                steps[tn] = cstep;
                jsonWalkPush(&w, &pv)->aux = tn + 1;
            }
        } else if (pt != JSON_NULL) {
            jsonVal *copy = jsonValFromRef(&pv);
            jsonObjectAdd(d, steps, (int)tn, name, name_len, copy);
            jsonValFree(copy);
        }
    }
    jsonWalkFree(&w);
    jsonScratchFree(steps, steps_stack);
}

static int jsonMergeCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t path_len, json_len, orig_size, limit;
    const char *path, *json, *err = NULL;
    uint32_t order_stack[JSON_INLINE_MATCHES], *order;
    uint8_t *nested = NULL;
    ValkeyModuleKey *key;
    jsonKeyState ks;
    jsonQuery q;
    jsonVal *v;
    jsonDoc d, before;
    int is_root, patch_is_object;

    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    path = jsonArgPath(argv[2], &path_len);
    json = ValkeyModule_StringPtrLen(argv[3], &json_len);
    key = jsonOpenWrite(ctx, argv[1]);
    if (key == NULL) return VALKEYMODULE_OK;
    ks = jsonKeyCheck(key);
    is_root = jsonIsRootPath(path, path_len);
    if (ks == JSON_KEY_WRONGTYPE) {
        err = JSON_MSG_NOT_DOC_KEY;
    } else if (ks == JSON_KEY_MISSING && !is_root) {
        err = JSON_MSG_NEW_KEY_NOT_ROOT;
    }
    if (err != NULL) goto fail;

    if (ks == JSON_KEY_MISSING) {
        /* A new key takes the value as it is. Nothing is merged, so its null
         * members stay. */
        v = jsonParseDocument(ctx, json, json_len, &err);
        if (v == NULL) goto fail;
        jsonKeySetDocument(key, v);
        goto done;
    }

    /* Check. */
    d = before = jsonKeyDoc(key);
    orig_size = jsonDocMemUsage(d);
    jsonQueryRun(&q, is_root ? "." : path, path_len, d, JSON_PATH_INSERT);
    err = jsonQueryError(&q, 0);
    if (err == NULL && !jsonInsertNamesValid(ctx, &q.m)) err = JSON_MSG_PARSE;
    v = err == NULL ? jsonCmdParse(ctx, json, json_len, &err) : NULL;
    if (v != NULL && (q.m.count > 0 || q.m.insert_count > 0) &&
        jsonDepthExceeded(ctx, jsonMatchesMaxDepth(&q.m), jsonValDepth(v))) {
        err = JSON_MSG_DEPTH_LIMIT;
    }
    nested = err == NULL ? jsonNestedMatches(&q.m) : NULL; /* Q2: the outermost match wins. */
    limit = err == NULL ? jsonCmdSizeLimit(ctx) : 0;
    patch_is_object = v != NULL && jsonValType(v) == JSON_OBJECT;
    if (limit > 0) {
        /* A patch that is an object merges into what is there. Any other
         * patch replaces it. */
        size_t places = q.m.insert_count, removed = 0;
        for (uint32_t i = 0; i < q.m.count; i++) {
            if (nested != NULL && nested[i]) continue;
            if (!patch_is_object) removed += jsonRefMemUsage(&q.m.items[i].ref);
            places++;
        }
        if (jsonSizeExceeded(limit, orig_size, places * jsonValSizeEstimate(v), removed)) err = JSON_MSG_SIZE_LIMIT;
    }
    if (err != NULL) {
        if (nested != NULL) ValkeyModule_Free(nested);
        jsonValFree(v);
        jsonQueryFree(&q);
        goto fail;
    }

    /* Apply. A place the path adds gets the patch as it is, as in the module. */
    jsonApplyInserts(&d, &q.m, v);
    order = jsonApplyOrder(&q.m, order_stack);
    if (!patch_is_object) {
        /* Nothing to merge: the patch replaces each value. */
        for (uint32_t k = 0; k < q.m.count; k++) {
            const jsonMatch *mt = &q.m.items[order[k]];
            if (nested != NULL && nested[order[k]]) continue;
            jsonSet(&d, mt->loc, (int)mt->depth, v);
        }
        jsonValFree(v);
    } else if (q.m.count > 0) {
        /* The patch is walked member by member, which needs a document. */
        jsonVal *empty_object = jsonValEmptyObject();
        jsonDoc patch_doc = jsonValToDoc(v);
        jsonRef patch;
        jsonDocRoot(patch_doc, &patch);
        for (uint32_t k = 0; k < q.m.count; k++) {
            const jsonMatch *mt = &q.m.items[order[k]];
            if (nested != NULL && nested[order[k]]) continue;
            jsonMergeAt(&d, mt->loc, mt->depth, &patch, empty_object);
        }
        jsonValFree(empty_object);
        jsonDocFree(patch_doc);
    } else {
        jsonValFree(v);
    }
    jsonScratchFree(order, order_stack);
    if (nested != NULL) ValkeyModule_Free(nested);
    jsonQueryFree(&q);
    jsonKeyStore(key, before, d);
    jsonStatsOnUpdate(orig_size, jsonDocMemUsage(d), json_len);

done:
    jsonWritePublish(ctx, "json.merge", argv[1]);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;

fail:
    ValkeyModule_ReplyWithError(ctx, err);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.MSET key path json [key path json ...]
 *
 * All or nothing (design 8.4): every triple is checked against the keyspace
 * as it was before the command, then every triple is applied. A key named
 * more than once is the one case where a later triple depends on an earlier
 * one. Its triples are run in order on a staged document, and the staged
 * document replaces the key's value only if all of them succeed.
 * ------------------------------------------------------------------------- */

#define JSON_MSET_NONE UINT32_MAX

typedef struct jsonMsetItem {
    ValkeyModuleKey *key; /* Opened by the first triple that names the key. */
    uint32_t first;       /* That triple. */
    uint32_t next;        /* The next triple naming the same key, or JSON_MSET_NONE. */
    int is_root;
    const char *path, *json;
    size_t path_len, json_len;
    jsonVal *val;      /* Root triple: the parsed document. */
    jsonSetPlan *plan; /* Sub-path triple: what to change. */
    jsonDoc staged;    /* First triple of a key named more than once: its new document. */
    int staged_root;   /* A triple replaced the whole staged document. */
} jsonMsetItem;

static void jsonMsetFree(jsonMsetItem *items, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        jsonValFree(items[i].val);
        if (items[i].staged != 0) jsonDocFree(items[i].staged);
        if (items[i].plan != NULL) {
            jsonSetPlanFree(items[i].plan);
            ValkeyModule_Free(items[i].plan);
        }
        if (items[i].first == i) ValkeyModule_CloseKey(items[i].key);
    }
    ValkeyModule_Free(items);
}

/* Links the triples that name the same key. Returns 0 if a key could not be
 * opened. */
static int jsonMsetOpenKeys(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, jsonMsetItem *items, uint32_t n) {
    uint32_t mask = 16, *table;

    while (mask < n * 2) mask *= 2;
    table = ValkeyModule_Calloc(mask, sizeof(uint32_t)); /* Triple index plus one, 0 for empty. */
    mask--;
    for (uint32_t i = 0; i < n; i++) {
        ValkeyModuleString *name = argv[i * 3 + 1];
        size_t len;
        const unsigned char *s = (const unsigned char *)ValkeyModule_StringPtrLen(name, &len);
        uint64_t h = 14695981039346656037ull;
        uint32_t slot;

        for (size_t k = 0; k < len; k++) h = (h ^ s[k]) * 1099511628211ull;
        slot = (uint32_t)(h ^ (h >> 32)) & mask;
        while (table[slot] != 0 && ValkeyModule_StringCompare(argv[(table[slot] - 1) * 3 + 1], name) != 0) {
            slot = (slot + 1) & mask;
        }
        items[i].next = JSON_MSET_NONE;
        if (table[slot] != 0) {
            uint32_t last = table[slot] - 1;
            items[i].first = last;
            items[i].key = items[last].key;
            while (items[last].next != JSON_MSET_NONE) last = items[last].next;
            items[last].next = i;
            continue;
        }
        table[slot] = i + 1;
        items[i].first = i;
        items[i].key = ValkeyModule_OpenKey(ctx, name, VALKEYMODULE_READ | VALKEYMODULE_WRITE);
        if (items[i].key == NULL) {
            items[i].first = JSON_MSET_NONE; /* Nothing to close. */
            ValkeyModule_Free(table);
            return 0;
        }
    }
    ValkeyModule_Free(table);
    return 1;
}

/* Runs the triples of one key, in order, on a staged document. Returns the
 * staged document, or 0 with *err set. *has_root says whether a triple
 * replaced the whole document, which discards the TTL as JSON.SET does. */
static jsonDoc jsonMsetStage(ValkeyModuleCtx *ctx,
                             jsonMsetItem *items,
                             uint32_t first,
                             int *has_root,
                             const char **err) {
    jsonDoc staged = 0;

    *has_root = 0;
    for (uint32_t i = first; i != JSON_MSET_NONE; i = items[i].next) {
        jsonMsetItem *it = &items[i];
        jsonSetPlan plan;
        jsonVal *v;

        if (it->is_root) {
            if (staged != 0) jsonDocFree(staged);
            staged = jsonValToDoc(it->val);
            it->val = NULL;
            *has_root = 1;
            continue;
        }
        if (staged == 0) staged = jsonDocCopy(jsonKeyDoc(it->key));
        /* The plan made against the stored document does not hold for the
         * staged one. Check again, as a JSON.SET on the staged document. */
        v = it->plan->val;
        it->plan->val = NULL;
        jsonSetPlanFree(it->plan);
        ValkeyModule_Free(it->plan);
        it->plan = NULL;
        if (jsonSetCheck(ctx, staged, it->path, it->path_len, it->json, it->json_len, v, 0, &plan, err) !=
            JSON_SET_GO) {
            jsonDocFree(staged);
            return 0;
        }
        jsonSetApply(&staged, &plan);
        jsonSetPlanFree(&plan);
    }
    return staged;
}

static int jsonMsetCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    uint32_t n = (uint32_t)(argc - 1) / 3;
    const char *err = NULL;
    jsonMsetItem *items;

    if (argc < 4 || (argc - 1) % 3 != 0) return ValkeyModule_WrongArity(ctx);
    items = ValkeyModule_Calloc(n, sizeof(jsonMsetItem));

    /* 1. Open every key. */
    if (!jsonMsetOpenKeys(ctx, argv, items, n)) {
        err = JSON_MSG_KEY_OPEN;
        goto fail;
    }

    /* 2. Check every triple against the keyspace as it is. */
    for (uint32_t i = 0; i < n; i++) {
        jsonMsetItem *it = &items[i];
        jsonKeyState ks = jsonKeyCheck(it->key);

        it->path = jsonArgPath(argv[i * 3 + 2], &it->path_len);
        it->json = ValkeyModule_StringPtrLen(argv[i * 3 + 3], &it->json_len);
        it->is_root = jsonIsRootPath(it->path, it->path_len);
        if (ks == JSON_KEY_WRONGTYPE) {
            err = JSON_MSG_NOT_DOC_KEY;
        } else if (ks == JSON_KEY_MISSING && !it->is_root) {
            err = JSON_MSG_COMMAND_SYNTAX; /* Not the text JSON.SET uses. */
        } else if (it->is_root) {
            it->val = jsonParseDocument(ctx, it->json, it->json_len, &err);
        } else {
            it->plan = ValkeyModule_Alloc(sizeof(jsonSetPlan));
            if (jsonSetCheck(ctx, jsonKeyDoc(it->key), it->path, it->path_len, it->json, it->json_len, NULL, 0,
                             it->plan, &err) != JSON_SET_GO) {
                ValkeyModule_Free(it->plan);
                it->plan = NULL;
            }
        }
        if (err != NULL) goto fail;
    }

    /* A key named more than once is staged in full before any key changes. */
    for (uint32_t i = 0; i < n; i++) {
        jsonMsetItem *it = &items[i];
        if (it->first != i || it->next == JSON_MSET_NONE) continue;
        it->staged = jsonMsetStage(ctx, items, i, &it->staged_root, &err);
        if (it->staged == 0) goto fail;
    }

    /* 3. Apply, key by key in argument order. This cannot fail. */
    for (uint32_t i = 0; i < n; i++) {
        jsonMsetItem *it = &items[i];
        if (it->first != i) continue; /* Applied with the first triple of its key. */
        if (it->staged != 0) {
            size_t size = jsonDocMemUsage(it->staged), total_json_len = 0;
            for (uint32_t k = i; k != JSON_MSET_NONE; k = items[k].next) total_json_len += items[k].json_len;
            if (it->staged_root) {
                ValkeyModule_ModuleTypeSetValue(it->key, jsonModuleType, jsonValueNew(it->staged));
                jsonStatsOnInsert(1, 0, size, size);
            } else {
                jsonDoc old = jsonKeyDoc(it->key);
                size_t orig_size = jsonDocMemUsage(old);
                jsonKeyStoreDoc(it->key, it->staged);
                jsonDocFree(old);
                jsonStatsOnUpdate(orig_size, size, total_json_len);
            }
            it->staged = 0;
        } else if (it->is_root) {
            jsonKeySetDocument(it->key, it->val);
            it->val = NULL;
        } else {
            jsonDoc d, before;
            size_t orig_size;
            d = before = jsonKeyDoc(it->key);
            orig_size = jsonDocMemUsage(d);
            jsonSetApply(&d, it->plan);
            jsonKeyStore(it->key, before, d);
            jsonStatsOnUpdate(orig_size, jsonDocMemUsage(d), it->json_len);
        }
    }

    /* 4. Replicate, and one event per triple in argument order. */
    ValkeyModule_ReplicateVerbatim(ctx);
    for (uint32_t i = 0; i < n; i++) {
        ValkeyModule_NotifyKeyspaceEvent(ctx, VALKEYMODULE_NOTIFY_GENERIC, "json.mset", argv[i * 3 + 1]);
    }
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    jsonMsetFree(items, n);
    return VALKEYMODULE_OK;

fail:
    ValkeyModule_ReplyWithError(ctx, err);
    jsonMsetFree(items, n);
    return VALKEYMODULE_OK;
}

/* ----------------------------------------------------------------------------
 * JSON.DEBUG MEMORY | FIELDS | DEPTH | MAX-DEPTH-KEY | MAX-SIZE-KEY | HELP
 *
 * The key table subcommands and TEST-SHARED-API of the module are gone (B8).
 * ------------------------------------------------------------------------- */

/* The memory of a whole document: the document word, which holds a scalar
 * document entirely, and what it points to. */
static size_t jsonDocFootprint(jsonDoc d) {
    return sizeof(jsonDoc) + jsonDocMemUsage(d);
}

/* JSON.DEBUG MEMORY key [path] and JSON.DEBUG FIELDS key [path]. */
static int jsonDebugValueCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int fields) {
    size_t path_len = 1;
    const char *path = ".";
    ValkeyModuleKey *key;
    jsonQuery q;
    jsonDoc d;

    if (argc != 3 && argc != 4) return ValkeyModule_WrongArity(ctx);
    if (argc == 4) path = jsonArgPath(argv[3], &path_len);

    if (!fields && argc == 3) {
        /* No path: the size the document is accounted with. */
        key = ValkeyModule_OpenKey(ctx, argv[2], VALKEYMODULE_READ);
        if (!jsonReplyKeyState(ctx, jsonKeyCheck(key), NULL)) {
            ValkeyModule_ReplyWithLongLong(ctx, (long long)jsonDocFootprint(jsonKeyDoc(key)));
        }
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    if (!jsonReadQuery(ctx, argv[2], path, path_len, NULL, &key, &q)) return VALKEYMODULE_OK;
    d = jsonKeyDoc(key);
    if (!q.v2 && q.m.count == 0) {
        ValkeyModule_ReplyWithError(ctx, JSON_MSG_PATH_NOT_EXIST);
    } else {
        /* A legacy path replies for its first value only. */
        uint32_t count = q.v2 ? q.m.count : 1;
        if (q.v2) ValkeyModule_ReplyWithArray(ctx, count);
        for (uint32_t i = 0; i < count; i++) {
            const jsonRef *r = &q.m.items[i].ref;
            long long n;
            if (fields) {
                /* A container does not count itself. */
                n = (long long)jsonRefFields(r) - (jsonIsContainer(r) ? 1 : 0);
            } else {
                n = (long long)(q.m.items[i].depth == 0 ? jsonDocFootprint(d) : jsonRefMemUsage(r));
            }
            ValkeyModule_ReplyWithLongLong(ctx, n);
        }
    }
    jsonReadDone(key, &q);
    return VALKEYMODULE_OK;
}

typedef struct jsonScanMax {
    int by_depth;
    unsigned long long max;
    ValkeyModuleString *name;
} jsonScanMax;

static void jsonScanMaxCallback(ValkeyModuleCtx *ctx, ValkeyModuleString *name, ValkeyModuleKey *key, void *privdata) {
    jsonScanMax *s = privdata;
    unsigned long long v;
    jsonDoc d;
    VALKEYMODULE_NOT_USED(ctx);

    if (ValkeyModule_ModuleTypeGetType(key) != jsonModuleType) return;
    d = jsonKeyDoc(key);
    if (s->by_depth) {
        jsonRef root;
        jsonDocRoot(d, &root);
        v = jsonValuePathDepth(&root);
    } else {
        v = jsonDocFootprint(d);
    }
    if (v > s->max) {
        s->max = v;
        if (s->name != NULL) ValkeyModule_FreeString(NULL, s->name);
        s->name = ValkeyModule_CreateStringFromString(NULL, name);
    }
}

/* JSON.DEBUG MAX-DEPTH-KEY and MAX-SIZE-KEY: scans the whole database. For
 * development only. */
static int jsonDebugMaxKeyCommand(ValkeyModuleCtx *ctx, int by_depth) {
    jsonScanMax s = {by_depth, 0, NULL};
    ValkeyModuleScanCursor *cursor = ValkeyModule_ScanCursorCreate();

    while (ValkeyModule_Scan(ctx, cursor, jsonScanMaxCallback, &s)) {
    }
    ValkeyModule_ScanCursorDestroy(cursor);
    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithLongLong(ctx, (long long)s.max);
    /* The key name is a bulk string. The module sends it as a simple string,
     * and a name with a line break in it then splits into several replies. */
    if (s.name != NULL) {
        ValkeyModule_ReplyWithString(ctx, s.name);
        ValkeyModule_FreeString(NULL, s.name);
    } else {
        ValkeyModule_ReplyWithStringBuffer(ctx, "", 0);
    }
    return VALKEYMODULE_OK;
}

static int jsonDebugCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    static const char *const help[] = {
        ("JSON.DEBUG MEMORY <key> [path] - report memory size (bytes). "
         "Without path reports document size without keys. "
         "With path reports size including keys"),
        "JSON.DEBUG DEPTH <key> - report the maximum path depth of the JSON document.",
        ("JSON.DEBUG FIELDS <key> [path] - report number of fields in the "
         "JSON element. Path defaults to root if not provided."),
        "JSON.DEBUG HELP - print help message.",
        "------- DANGER, LONG RUNNING COMMANDS, DON'T USE ON PRODUCTION SYSTEM --------",
        "JSON.DEBUG MAX-DEPTH-KEY - Find JSON key with maximum depth",
        "JSON.DEBUG MAX-SIZE-KEY  - Find JSON key with largest memory size",
    };
    int keys_request = ValkeyModule_IsKeysPositionRequest(ctx);
    const char *sub;

    if (argc < 2) return keys_request ? VALKEYMODULE_ERR : ValkeyModule_WrongArity(ctx);
    sub = ValkeyModule_StringPtrLen(argv[1], NULL);

    if (!strcasecmp(sub, "MEMORY") || !strcasecmp(sub, "FIELDS") || !strcasecmp(sub, "DEPTH")) {
        if (keys_request) {
            if (argc < 3) return VALKEYMODULE_ERR;
            ValkeyModule_KeyAtPos(ctx, 2);
            return VALKEYMODULE_OK;
        }
        if (!strcasecmp(sub, "DEPTH")) {
            ValkeyModuleKey *key;
            if (argc != 3) return ValkeyModule_WrongArity(ctx);
            key = ValkeyModule_OpenKey(ctx, argv[2], VALKEYMODULE_READ);
            if (!jsonReplyKeyState(ctx, jsonKeyCheck(key), NULL)) {
                jsonRef root;
                jsonDocRoot(jsonKeyDoc(key), &root);
                ValkeyModule_ReplyWithLongLong(ctx, jsonValuePathDepth(&root));
            }
            ValkeyModule_CloseKey(key);
            return VALKEYMODULE_OK;
        }
        return jsonDebugValueCommand(ctx, argv, argc, !strcasecmp(sub, "FIELDS"));
    }

    if (keys_request) return VALKEYMODULE_ERR;
    if (!strcasecmp(sub, "MAX-DEPTH-KEY") || !strcasecmp(sub, "MAX-SIZE-KEY")) {
        if (argc != 2) return ValkeyModule_WrongArity(ctx);
        return jsonDebugMaxKeyCommand(ctx, !strcasecmp(sub, "MAX-DEPTH-KEY"));
    }
    if (!strcasecmp(sub, "HELP")) {
        if (argc != 2) return ValkeyModule_WrongArity(ctx);
        ValkeyModule_ReplyWithArray(ctx, sizeof(help) / sizeof(help[0]));
        for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); i++) ValkeyModule_ReplyWithSimpleString(ctx, help[i]);
        return VALKEYMODULE_OK;
    }
    return ValkeyModule_ReplyWithError(ctx, JSON_MSG_UNKNOWN_SUBCOMMAND);
}

/* ----------------------------------------------------------------------------
 * The C functions exported to other modules (json_api.h)
 *
 * The external module's src/json/json_api.cc. Another module resolves these by
 * name from the server binary, so they are global symbols. Everything they
 * hand back is allocated with ValkeyModule_Alloc and freed by the caller with
 * ValkeyModule_Free. An error is -1.
 * ------------------------------------------------------------------------- */

#define JSON_API __attribute__((visibility("default")))

JSON_API int is_json_key(ValkeyModuleCtx *ctx, ValkeyModuleKey *key) {
    VALKEYMODULE_NOT_USED(ctx);
    return jsonKeyCheck(key) == JSON_KEY_DOC;
}

JSON_API int is_json_key2(ValkeyModuleCtx *ctx, ValkeyModuleString *keystr) {
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, keystr, VALKEYMODULE_READ);
    int is_json = is_json_key(ctx, key);
    ValkeyModule_CloseKey(key);
    return is_json;
}

/* The document of a key given by name. Returns 0 if the key is not a JSON
 * document. The key is closed again: the document is used right away, on the
 * main thread, before anything can change it. */
static int jsonApiDoc(ValkeyModuleCtx *ctx, const char *keyname, size_t key_len, jsonDoc *d) {
    ValkeyModuleString *name = ValkeyModule_CreateString(ctx, keyname, key_len);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, name, VALKEYMODULE_READ);
    int is_json = is_json_key(ctx, key);
    if (is_json) *d = jsonKeyDoc(key);
    ValkeyModule_CloseKey(key);
    ValkeyModule_FreeString(ctx, name);
    return is_json;
}

static char *jsonApiString(const char *s, size_t n) {
    char *copy = ValkeyModule_Alloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

/* The text of JSON.GET for one path, or NULL if the command would fail. These
 * reads are not counted in the statistics, as in the module. */
static char *jsonApiValue(jsonDoc d, const char *path, size_t *len) {
    char stack[JSON_REPLY_STACK], *value = NULL;
    jsonBuf out;
    jsonQuery q;

    jsonBufInit(&out, stack, sizeof(stack));
    jsonQueryRun(&q, path, strlen(path), d, JSON_PATH_READ);
    if (jsonAppendGetValue(&out, &q, NULL, 0) == NULL) {
        value = jsonApiString(out.p, out.len);
        *len = out.len;
    }
    jsonQueryFree(&q);
    jsonBufFree(&out);
    return value;
}

/* The JSON.TYPE name of the first value a path selects, or NULL if the path
 * fails or selects nothing. */
static char *jsonApiType(jsonDoc d, const char *path, size_t *len) {
    char *type = NULL;
    jsonQuery q;

    jsonQueryRun(&q, path, strlen(path), d, JSON_PATH_READ);
    if (jsonQueryError(&q, 1) == NULL && q.m.count > 0) {
        const char *name = jsonTypeName(&q.m.items[0].ref);
        *len = strlen(name);
        type = jsonApiString(name, *len);
    }
    jsonQueryFree(&q);
    return type;
}

JSON_API int get_json_value_type(ValkeyModuleCtx *ctx,
                                 const char *keyname,
                                 const size_t key_len,
                                 const char *path,
                                 char **type,
                                 size_t *len) {
    jsonDoc d;

    *type = NULL;
    *len = 0;
    if (!jsonApiDoc(ctx, keyname, key_len, &d)) return -1;
    *type = jsonApiType(d, path, len);
    return *type != NULL ? 0 : -1;
}

JSON_API int get_json_value(ValkeyModuleCtx *ctx,
                            const char *keyname,
                            const size_t key_len,
                            const char *path,
                            char **value,
                            size_t *len) {
    jsonDoc d;

    *value = NULL;
    *len = 0;
    if (!jsonApiDoc(ctx, keyname, key_len, &d)) return -1;
    *value = jsonApiValue(d, path, len);
    return *value != NULL ? 0 : -1;
}

JSON_API int get_json_values_and_types(ValkeyModuleCtx *ctx,
                                       const char *keyname,
                                       const size_t key_len,
                                       const char **paths,
                                       const int num_paths,
                                       char ***values,
                                       size_t **lengths,
                                       char ***types,
                                       size_t **type_lengths) {
    size_t n = num_paths > 0 ? (size_t)num_paths : 0;
    jsonDoc d;

    ValkeyModule_Assert(values != NULL);
    ValkeyModule_Assert(lengths != NULL);
    *values = NULL;
    *lengths = NULL;
    if (types != NULL) *types = NULL;
    if (type_lengths != NULL) *type_lengths = NULL;
    if (!jsonApiDoc(ctx, keyname, key_len, &d)) return -1;

    /* A path that fails leaves NULL and 0 in its place. The call succeeds. */
    *values = ValkeyModule_Calloc(n, sizeof(char *));
    *lengths = ValkeyModule_Calloc(n, sizeof(size_t));
    for (size_t i = 0; i < n; i++) (*values)[i] = jsonApiValue(d, paths[i], &(*lengths)[i]);
    if (types != NULL) {
        ValkeyModule_Assert(type_lengths != NULL);
        *types = ValkeyModule_Calloc(n, sizeof(char *));
        *type_lengths = ValkeyModule_Calloc(n, sizeof(size_t));
        for (size_t i = 0; i < n; i++) (*types)[i] = jsonApiType(d, paths[i], &(*type_lengths)[i]);
    }
    return 0;
}

/* ----------------------------------------------------------------------------
 * Registration
 *
 * Names, flags, key positions, ACL categories, arity and key specifications
 * are the external module's (ValkeyModule_OnLoad in src/json/json.cc). The
 * "json" ACL category itself is created by the module glue.
 * ------------------------------------------------------------------------- */

#define JSON_FLAGS_READ "fast readonly"
#define JSON_FLAGS_SLOW_WRITE_DENY "write deny-oom"
#define JSON_FLAGS_FAST_WRITE "fast write"
#define JSON_FLAGS_FAST_WRITE_DENY "fast write deny-oom"
#define JSON_FLAGS_DEBUG "readonly getkeys-api"

#define JSON_ACL_READ "json read fast"
#define JSON_ACL_SLOW_WRITE "json write slow"
#define JSON_ACL_FAST_WRITE "json write fast"
#define JSON_ACL_DEBUG "json read slow"

#define JSON_KS_UPDATE (VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE)
#define JSON_KS_INSERT (VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_INSERT)
#define JSON_KS_DELETE (VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_DELETE)
#define JSON_KS_ACCESS_UPDATE (JSON_KS_UPDATE | VALKEYMODULE_CMD_KEY_ACCESS)
#define JSON_KS_ACCESS_DELETE (JSON_KS_DELETE | VALKEYMODULE_CMD_KEY_ACCESS)
#define JSON_KS_READ VALKEYMODULE_CMD_KEY_RO
#define JSON_KS_READ_ACCESS (VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS)

typedef struct jsonCommandDef {
    const char *name;
    ValkeyModuleCmdFunc func;
    const char *flags;
    int first_key, last_key, key_step;
    const char *acl;
    int arity;
    uint64_t key_flags;       /* Key specification: flags, */
    int ks_last_key, ks_step; /* and the range after the first key. */
} jsonCommandDef;

static const jsonCommandDef jsonCommands[] = {
    {"JSON.SET", jsonSetCommand, JSON_FLAGS_SLOW_WRITE_DENY, 1, 1, 1, JSON_ACL_SLOW_WRITE, -4, JSON_KS_UPDATE, 0, 1},
    {"JSON.MERGE", jsonMergeCommand, JSON_FLAGS_SLOW_WRITE_DENY, 1, 1, 1, JSON_ACL_SLOW_WRITE, 4, JSON_KS_UPDATE, 0, 1},
    {"JSON.MSET", jsonMsetCommand, JSON_FLAGS_SLOW_WRITE_DENY, 1, -3, 3, JSON_ACL_SLOW_WRITE, -4, JSON_KS_UPDATE, -3,
     3},
    {"JSON.GET", jsonGetCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ_ACCESS, 0, 1},
    {"JSON.MGET", jsonMgetCommand, JSON_FLAGS_READ, 1, -2, 1, JSON_ACL_READ, -3, JSON_KS_READ_ACCESS, -2, 1},
    {"JSON.DEL", jsonDelCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, -2, JSON_KS_DELETE, 0, 1},
    {"JSON.FORGET", jsonDelCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, -2, JSON_KS_DELETE, 0, 1},
    {"JSON.NUMINCRBY", jsonNumincrbyCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, 4,
     JSON_KS_ACCESS_UPDATE, 0, 1},
    {"JSON.NUMMULTBY", jsonNummultbyCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, 4,
     JSON_KS_ACCESS_UPDATE, 0, 1},
    {"JSON.STRLEN", jsonStrlenCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ, 0, 1},
    {"JSON.STRAPPEND", jsonStrappendCommand, JSON_FLAGS_FAST_WRITE_DENY, 1, 1, 1, JSON_ACL_FAST_WRITE, -3,
     JSON_KS_INSERT, 0, 1},
    {"JSON.TOGGLE", jsonToggleCommand, JSON_FLAGS_FAST_WRITE_DENY, 1, 1, 1, JSON_ACL_FAST_WRITE, -2,
     JSON_KS_ACCESS_UPDATE, 0, 1},
    {"JSON.OBJLEN", jsonObjlenCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ, 0, 1},
    {"JSON.OBJKEYS", jsonObjkeysCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ_ACCESS, 0, 1},
    {"JSON.ARRLEN", jsonArrlenCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ, 0, 1},
    {"JSON.ARRAPPEND", jsonArrappendCommand, JSON_FLAGS_FAST_WRITE_DENY, 1, 1, 1, JSON_ACL_FAST_WRITE, -4,
     JSON_KS_INSERT, 0, 1},
    {"JSON.ARRPOP", jsonArrpopCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, -2, JSON_KS_ACCESS_DELETE,
     0, 1},
    {"JSON.ARRINSERT", jsonArrinsertCommand, JSON_FLAGS_FAST_WRITE_DENY, 1, 1, 1, JSON_ACL_FAST_WRITE, -5,
     JSON_KS_INSERT, 0, 1},
    {"JSON.ARRTRIM", jsonArrtrimCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, 5, JSON_KS_DELETE, 0, 1},
    {"JSON.CLEAR", jsonClearCommand, JSON_FLAGS_FAST_WRITE, 1, 1, 1, JSON_ACL_FAST_WRITE, -2, JSON_KS_DELETE, 0, 1},
    {"JSON.ARRINDEX", jsonArrindexCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -4, JSON_KS_READ_ACCESS, 0, 1},
    {"JSON.TYPE", jsonTypeCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ_ACCESS, 0, 1},
    {"JSON.RESP", jsonRespCommand, JSON_FLAGS_READ, 1, 1, 1, JSON_ACL_READ, -2, JSON_KS_READ_ACCESS, 0, 1},
};

/* The subcommands of JSON.DEBUG. A subcommand with a key has it at position 2. */
static const struct {
    const char *name;
    int has_key;
    int arity;
} jsonDebugSubcommands[] = {
    {"MEMORY", 1, -3},
    {"DEPTH", 1, 3},
    {"FIELDS", 1, -3},
    {"HELP", 0, 2},
    {"MAX-DEPTH-KEY", 0, 2},
    {"MAX-SIZE-KEY", 0, 2},
};

/* Sets arity and, when key_pos is not 0, one key specification: keys start at
 * argument key_pos and run to last_key (relative, 0 for a single key) in
 * steps of `step`. */
static int jsonSetCommandInfo(ValkeyModuleCtx *ctx,
                              const char *name,
                              int arity,
                              int key_pos,
                              uint64_t key_flags,
                              int last_key,
                              int step) {
    ValkeyModuleCommand *cmd = ValkeyModule_GetCommand(ctx, name);
    ValkeyModuleCommandInfo info;
    ValkeyModuleCommandKeySpec specs[2]; /* The second, all zero, ends the list. */

    if (cmd == NULL) {
        ValkeyModule_Log(ctx, "warning", "Failed to get command '%s'", name);
        return VALKEYMODULE_ERR;
    }
    memset(&info, 0, sizeof(info));
    memset(specs, 0, sizeof(specs));
    info.version = VALKEYMODULE_COMMAND_INFO_VERSION;
    info.arity = arity;
    if (key_pos != 0) {
        specs[0].flags = key_flags;
        specs[0].begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX;
        specs[0].bs.index.pos = key_pos;
        specs[0].find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE;
        specs[0].fk.range.lastkey = last_key;
        specs[0].fk.range.keystep = step;
        specs[0].fk.range.limit = 0;
        info.key_specs = specs;
    }
    if (ValkeyModule_SetCommandInfo(cmd, &info) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to set command info for %s", name);
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}

int jsonRegisterCommands(ValkeyModuleCtx *ctx) {
    ValkeyModuleCommand *debug;

    for (size_t i = 0; i < sizeof(jsonCommands) / sizeof(jsonCommands[0]); i++) {
        const jsonCommandDef *c = &jsonCommands[i];
        if (ValkeyModule_CreateCommand(ctx, c->name, c->func, c->flags, c->first_key, c->last_key, c->key_step) ==
            VALKEYMODULE_ERR) {
            ValkeyModule_Log(ctx, "warning", "Failed to create command %s.", c->name);
            return VALKEYMODULE_ERR;
        }
        if (ValkeyModule_SetCommandACLCategories(ValkeyModule_GetCommand(ctx, c->name), c->acl) == VALKEYMODULE_ERR) {
            ValkeyModule_Log(ctx, "warning", "Failed to set command category for %s.", c->name);
            return VALKEYMODULE_ERR;
        }
    }

    /* JSON.DEBUG is a container command: it has no handler of its own. */
    if (ValkeyModule_CreateCommand(ctx, "JSON.DEBUG", NULL, JSON_FLAGS_DEBUG, 0, 0, 0) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to create command JSON.DEBUG.");
        return VALKEYMODULE_ERR;
    }
    debug = ValkeyModule_GetCommand(ctx, "JSON.DEBUG");
    if (ValkeyModule_SetCommandACLCategories(debug, JSON_ACL_DEBUG) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to set command category for JSON.DEBUG.");
        return VALKEYMODULE_ERR;
    }
    for (size_t i = 0; i < sizeof(jsonDebugSubcommands) / sizeof(jsonDebugSubcommands[0]); i++) {
        int pos = jsonDebugSubcommands[i].has_key ? 2 : 0;
        if (ValkeyModule_CreateSubcommand(debug, jsonDebugSubcommands[i].name, jsonDebugCommand, "", pos, pos,
                                          pos ? 1 : 0) == VALKEYMODULE_ERR) {
            ValkeyModule_Log(ctx, "warning", "Failed to create subcommand %s for command JSON.DEBUG.",
                             jsonDebugSubcommands[i].name);
            return VALKEYMODULE_ERR;
        }
    }

    for (size_t i = 0; i < sizeof(jsonCommands) / sizeof(jsonCommands[0]); i++) {
        const jsonCommandDef *c = &jsonCommands[i];
        int rc = jsonSetCommandInfo(ctx, c->name, c->arity, 1, c->key_flags, c->ks_last_key, c->ks_step);
        if (rc == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    }
    if (jsonSetCommandInfo(ctx, "JSON.DEBUG", -2, 0, 0, 0, 0) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    for (size_t i = 0; i < sizeof(jsonDebugSubcommands) / sizeof(jsonDebugSubcommands[0]); i++) {
        char name[64] = "JSON.DEBUG|";
        strcat(name, jsonDebugSubcommands[i].name);
        if (jsonSetCommandInfo(ctx, name, jsonDebugSubcommands[i].arity, jsonDebugSubcommands[i].has_key ? 2 : 0,
                               JSON_KS_READ_ACCESS, 0, 1) == VALKEYMODULE_ERR) {
            return VALKEYMODULE_ERR;
        }
    }
    return VALKEYMODULE_OK;
}
