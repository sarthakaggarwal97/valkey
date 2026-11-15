/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_path.c -- path compiler and evaluator of the built-in JSON module.
 *
 * The specification is the selector of the external valkey-json module
 * (src/json/selector.cc there). That selector is a recursive-descent parser
 * that evaluates while it parses: it re-reads the rest of the path text for
 * every value a wildcard, slice, filter or recursive descent produces, and it
 * stops reading a branch as soon as the branch has no value left. Three
 * consequences shape this file.
 *
 *   1. A malformed path is an error only if evaluation reaches the malformed
 *      part with a live value. So the compiler never fails on syntax: it
 *      compiles what is well formed and ends the program with an error step,
 *      and the evaluator reports the error when it gets there.
 *   2. The lexer state the module is in when it starts an element decides how
 *      the element reads. The compiler below is a port of the module's lexer
 *      and parser, function by function, with every "process" call replaced by
 *      "emit a step". Oddities of the original (where spaces are skipped, what
 *      a name scan covers) are kept because they decide which paths work.
 *   3. After a step that forks (wildcard, slice, union, filter on an array,
 *      recursive descent) the module re-enters its top rule, which accepts a
 *      leading "$" or "."; after a step that does not, it re-enters the rule
 *      below it. A filter does either, depending on the value. Each step
 *      therefore names its continuation, and a filter names two.
 *
 * The program is a flat array of steps linked by index. The evaluator runs it
 * with an explicit stack of frames, one per fork in progress, so it does not
 * recurse on document depth or path length. The module's limits are kept as
 * counters with the module's arithmetic, so the same inputs are rejected. */

#include "json_path.h"
#include "json_num.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define JSON_PATH_MAX_QUERY (128 * 1024) /* The module's max query string size. */
#define JSON_PATH_MAX_NESTING 200        /* The module's parser recursion depth. */
#define JSON_PATH_MAX_DESCENT_TOKENS 20  /* The module's recursive descent token limit. */

/* Two bounds the module does not have, where it has no bound at all.
 *
 * A recursive descent recurses once per level in the module, outside its
 * recursion counter, and the process dies of stack overflow on a document
 * that is deep enough (between 5,000 and 20,000 levels in the reference
 * build). Here a descent goes at most this many levels below the value it
 * starts from and then fails with the recursion limit error. */
#define JSON_PATH_MAX_DESCENT_DEPTH 4096
/* A union that names one element twice evaluates the rest of the path twice,
 * so a chain of such unions doubles the work at every step. A read may spend
 * this much work on repeats, plus this much per value in the document, and
 * then fails. A write is not affected: it skips a repeat where it occurs. */
#define JSON_PATH_REPEAT_FLOOR 100000
#define JSON_PATH_REPEAT_PER_VALUE 8

static void *pathAlloc(size_t n) {
    return jsonEngineAllocator()->alloc(n);
}
static void *pathRealloc(void *p, size_t n) {
    return jsonEngineAllocator()->realloc(p, n);
}
static void pathFree(void *p) {
    if (p) jsonEngineAllocator()->free(p);
}

/* ----------------------------------------------------------------------------
 * Errors
 * ------------------------------------------------------------------------- */

const char *jsonPathErrMsg(jsonPathErr e) {
    switch (e) {
    case JSON_PATH_OK: return "";
    case JSON_PATH_ERR_SYNTAX: return "SYNTAXERR Invalid JSON path";
    case JSON_PATH_ERR_TOO_LONG: return "LIMIT Query string size limit is exceeded";
    case JSON_PATH_ERR_TOO_DEEP: return "LIMIT Parser recursion depth is exceeded";
    case JSON_PATH_ERR_TOO_MANY_DESCENTS:
        return "LIMIT Total number of recursive descent tokens in the query string exceeds the limit";
    case JSON_PATH_ERR_NOT_EXIST: return "NONEXISTENT JSON path does not exist";
    case JSON_PATH_ERR_INDEX_RANGE: return "OUTOFBOUNDARIES Array index is out of bounds";
    case JSON_PATH_ERR_NOT_ARRAY: return "WRONGTYPE JSON element is not an array";
    case JSON_PATH_ERR_NOT_OBJECT: return "WRONGTYPE JSON element is not an object";
    case JSON_PATH_ERR_INSERT_ARRAY: return "ERROR Cannot insert a member into a non-object value";
    case JSON_PATH_ERR_INSERT_PARENT: return "NONEXISTENT Parent element does not exist";
    case JSON_PATH_ERR_OTHER: return "ERR Invalid JSON path";
    case JSON_PATH_ERR_MEMBER_NAME: return "SYNTAXERR Invalid object member name";
    case JSON_PATH_ERR_NUMBER: return "SYNTAXERR Invalid number";
    case JSON_PATH_ERR_IDENTIFIER: return "SYNTAXERR Invalid identifier";
    case JSON_PATH_ERR_DOT_SEQUENCE: return "SYNTAXERR Invalid dot sequence";
    case JSON_PATH_ERR_EMPTY_EXPR: return "SYNTAXERR Expression token cannot be empty";
    case JSON_PATH_ERR_INDEX_NOT_NUMBER: return "SYNTAXERR Array index is not a number";
    case JSON_PATH_ERR_STEP_ZERO: return "SYNTAXERR Step in the slice cannot be zero";
    case JSON_PATH_ERR_WILDCARD: return "ERR Invalid use of wildcard";
    case JSON_PATH_ERR_DOLLAR: return "SYNTAXERR Dollar sign cannot apply to non-root element";
    case JSON_PATH_ERR_JSON_PARSE: return "SYNTAXERR Failed to parse JSON string due to syntax error";
    case JSON_PATH_ERR_TOO_MANY_REPEATS: return "LIMIT Total number of repeated matches in the query result exceeds the limit";
    }
    return "";
}

int jsonPathErrIsSyntax(jsonPathErr e) {
    switch (e) {
    case JSON_PATH_ERR_SYNTAX:
    case JSON_PATH_ERR_MEMBER_NAME:
    case JSON_PATH_ERR_NUMBER:
    case JSON_PATH_ERR_IDENTIFIER:
    case JSON_PATH_ERR_EMPTY_EXPR:
    case JSON_PATH_ERR_INDEX_NOT_NUMBER:
    case JSON_PATH_ERR_STEP_ZERO:
    case JSON_PATH_ERR_INSERT_PARENT:
    case JSON_PATH_ERR_TOO_DEEP:
    case JSON_PATH_ERR_TOO_MANY_DESCENTS:
    case JSON_PATH_ERR_TOO_MANY_REPEATS:
    case JSON_PATH_ERR_TOO_LONG: return 1;
    default: return 0;
    }
}

/* ----------------------------------------------------------------------------
 * The compiled form
 * ------------------------------------------------------------------------- */

#define NO_STEP UINT32_MAX

typedef enum {
    ST_END = 0, /* Select the current value. */
    ST_ERR,     /* Report an error. */
    ST_DOLLAR,  /* "$" where the top rule is re-entered. */
    ST_MEMBER,  /* One member by name. */
    ST_INDEX,   /* One array element. */
    ST_SLICE,
    ST_IUNION, /* [1,2] */
    ST_NUNION, /* ['a','b'] */
    ST_WILD,
    ST_DESCENT,
    ST_FILTER
} stepKind;

#define SF_INSERTABLE (1 << 0) /* ST_MEMBER: a missing member may be inserted here. */
#define SF_NEED_ARRAY (1 << 1) /* ST_ERR: on a non-array the error is NOT_ARRAY instead. */
#define SF_DOT_SEQ (1 << 2)    /* ST_DESCENT: followed by another dot. */
#define SF_END_SIZE (1 << 3)   /* ST_SLICE: the end is the array length. */
#define SF_WILD_KEY (1 << 4)   /* ST_FILTER: written "*[?(...)]", which requires an array. */
#define SF_TERMINAL (1 << 5)   /* ST_FILTER: the program ends in an error. */
#define SF_SUP_SAME (1 << 6)   /* ST_FILTER: both continuations are the same step. */
#define SF_NO_REPEAT (1 << 7)  /* ST_IUNION: no two entries can name one element, whatever the array. */

typedef struct pathStep {
    uint8_t kind;
    uint8_t flags;
    uint8_t rd;        /* ".." tokens the module's lexer reads while it consumes this step. */
    uint8_t err;       /* ST_ERR. */
    uint32_t next;     /* The step that continues the branch. */
    uint32_t next_sup; /* ST_FILTER: the continuation when the value is an array or a scalar. */
    uint32_t ekey;     /* Compile time: the lexer state this step was compiled from. */
    union {
        uint32_t name; /* ST_MEMBER: index into names. */
        int64_t index; /* ST_INDEX. */
        struct {
            int64_t start, end, step;
        } sl; /* ST_SLICE. */
        struct {
            uint32_t off, count;
        } list; /* ST_IUNION: ints. ST_NUNION: names. */
        struct {
            uint32_t prog, nprog;
        } filter; /* ST_FILTER: instructions. */
    } u;
} pathStep;

/* A member name. Once it is found in the key table the path holds a reference
 * on its key, so the ID stays that name's for as long as the path has it. */
typedef struct pathName {
    uint32_t off, len; /* Decoded bytes in strs. */
    uint32_t keyid;    /* 0 until the name is found in the key table. */
    uint32_t repeat;   /* In a union of names: an earlier entry of the union is the same name. */
} pathName;

/* A filter is a straight-line program. The module evaluates every factor of a
 * filter while it parses it, with no short circuit, so the order of
 * instructions is the order of the text. Each leaf pushes a list of array
 * positions and AND / OR combine the top two. */
typedef enum {
    FI_ERR = 0,
    FI_PATHVAL,
    FI_CMP,
    FI_ATTR,
    FI_AND,
    FI_OR
} filterKind;
typedef enum {
    OP_EQ = 0,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE
} cmpOp;
typedef enum {
    LHS_SELF = 0,
    LHS_MEMBER,
    LHS_MEMBER_INDEX,
    LHS_CONTAINS
} cmpLhs;
typedef enum {
    CV_NULL = 0,
    CV_FALSE,
    CV_TRUE,
    CV_I64,
    CV_DBL,
    CV_STR,
    CV_REG,
    CV_OTHER
} cmpKind;

typedef struct filterIns {
    uint8_t kind;
    uint8_t op;
    uint8_t lhs;
    uint8_t err;     /* FI_ERR. */
    uint8_t vkind;   /* FI_CMP: kind of the right-hand value. CV_REG is the last FI_PATHVAL. */
    uint8_t may_dup; /* FI_PATHVAL: the sub-path can produce duplicates. */
    uint16_t need;   /* Deepest nesting the module's parser has reached by this instruction. */
    uint16_t vdepth; /* FI_PATHVAL: nesting at the point the sub-path is evaluated. */
    uint32_t name;   /* Member name. For LHS_SELF, the empty name. */
    int64_t index;   /* LHS_MEMBER_INDEX. */
    union {
        int64_t i;
        double d;
        struct {
            uint32_t off, len;
        } s;
        uint32_t entry; /* FI_PATHVAL: first step of the sub-path. */
    } v;
} filterIns;

#define PATH_INLINE_STEPS 8
#define PATH_INLINE_NAMES 6
#define PATH_INLINE_STRS 96

struct jsonPath {
    pathStep *steps;
    pathName *names;
    char *strs;
    filterIns *ins;
    int64_t *ints;
    uint32_t nsteps, capsteps;
    uint32_t nnames, capnames;
    uint32_t nstrs, capstrs;
    uint32_t nins, capins;
    uint32_t nints, capints;
    uint32_t entry;
    uint32_t unresolved; /* Names not yet found in the key table. */
    uint32_t nsing;      /* Singular path: number of member and index steps. */
    uint8_t rd_init;     /* ".." tokens read by the lexer's first pull. */
    uint8_t v2_static;   /* The path text starts with "$". */
    uint8_t pooled;      /* Lives in a cache entry: jsonPathFree gives it back instead of freeing it. */
    uint8_t v2, root, singular, may_dup;
    pathStep inline_steps[PATH_INLINE_STEPS];
    pathName inline_names[PATH_INLINE_NAMES];
    char inline_strs[PATH_INLINE_STRS];
};

/* Makes room for one more element of a growable array that may still be the
 * inline one. */
static void *growArray(void *arr, void *inline_arr, uint32_t used, uint32_t *cap, uint32_t need, size_t elem) {
    if (used + need <= *cap) return arr;
    uint32_t ncap = *cap ? *cap * 2 : 8;
    while (ncap < used + need) ncap *= 2;
    void *n;
    if (arr == inline_arr || arr == NULL) {
        n = pathAlloc((size_t)ncap * elem);
        if (used) memcpy(n, arr, (size_t)used * elem);
    } else {
        n = pathRealloc(arr, (size_t)ncap * elem);
    }
    *cap = ncap;
    return n;
}

static uint32_t newStep(jsonPath *p, stepKind kind) {
    p->steps = growArray(p->steps, p->inline_steps, p->nsteps, &p->capsteps, 1, sizeof(pathStep));
    pathStep *s = &p->steps[p->nsteps];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->next = s->next_sup = NO_STEP;
    s->ekey = UINT32_MAX;
    return p->nsteps++;
}

static uint32_t errStep(jsonPath *p, jsonPathErr e, int flags) {
    uint32_t si = newStep(p, ST_ERR);
    p->steps[si].err = (uint8_t)e;
    p->steps[si].flags = (uint8_t)flags;
    return si;
}

/* Reserves n bytes in strs and returns their offset. */
static uint32_t strReserve(jsonPath *p, size_t n) {
    p->strs = growArray(p->strs, p->inline_strs, p->nstrs, &p->capstrs, (uint32_t)n + 1, 1);
    return p->nstrs;
}

static uint32_t addName(jsonPath *p, uint32_t off, uint32_t len) {
    p->names = growArray(p->names, p->inline_names, p->nnames, &p->capnames, 1, sizeof(pathName));
    pathName *nm = &p->names[p->nnames];
    nm->off = off;
    nm->len = len;
    nm->repeat = 0;
    nm->keyid = 0; /* Looked up when the path is first evaluated. */
    p->unresolved++;
    return p->nnames++;
}

static uint32_t addNameBytes(jsonPath *p, const char *s, size_t len) {
    uint32_t off = strReserve(p, len);
    if (len) memcpy(p->strs + off, s, len);
    p->nstrs += (uint32_t)len;
    return addName(p, off, (uint32_t)len);
}

static filterIns *newIns(jsonPath *p, filterKind kind) {
    p->ins = growArray(p->ins, NULL, p->nins, &p->capins, 1, sizeof(filterIns));
    filterIns *in = &p->ins[p->nins++];
    memset(in, 0, sizeof(*in));
    in->kind = kind;
    return in;
}

static void addInt(jsonPath *p, int64_t v) {
    p->ints = growArray(p->ints, NULL, p->nints, &p->capints, 1, sizeof(int64_t));
    p->ints[p->nints++] = v;
}

/* ----------------------------------------------------------------------------
 * Lexer: a port of the module's Lexer
 * ------------------------------------------------------------------------- */

typedef enum {
    T_UNKNOWN = 0,
    T_DOLLAR,
    T_DOT,
    T_DOTDOT,
    T_WILDCARD,
    T_COLON,
    T_COMMA,
    T_AT,
    T_QMARK,
    T_LBRACKET,
    T_RBRACKET,
    T_LPAREN,
    T_RPAREN,
    T_SQUOTE,
    T_DQUOTE,
    T_PLUS,
    T_MINUS,
    T_DIV,
    T_PCT,
    T_EQ,
    T_NE,
    T_GT,
    T_LT,
    T_GE,
    T_LE,
    T_NOT,
    T_ASSIGN,
    T_ALPHA,
    T_DIGIT,
    T_SPACE,
    T_AND,
    T_OR,
    T_SPECIAL,
    T_END
} tokType;

/* `type` is the current token and `p` the position after it. `p` can be
 * further on than the end of the token: skipping spaces while the current
 * token is not a space moves `p` only. `ts` is where the text of the current
 * token starts. At the end of the path it keeps the value of the token before,
 * as the module's token keeps its stale string view, and the scans that start
 * from it rely on that. */
typedef struct lexer {
    const char *s;
    size_t n;
    size_t p;
    size_t ts;
    uint32_t rd; /* ".." tokens read so far. */
    uint8_t type;
} lexer;

static inline char lexAt(const lexer *lx, size_t i) {
    return i < lx->n ? lx->s[i] : '\0';
}

static inline int isDigitC(char c) {
    return c >= '0' && c <= '9';
}
static inline int isAlphaC(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static tokType lexPeek(const lexer *lx) {
    char c = lexAt(lx, lx->p), d = lexAt(lx, lx->p + 1);
    switch (c) {
    case '\0': return T_END;
    case '$': return T_DOLLAR;
    case '.': return d == '.' ? T_DOTDOT : T_DOT;
    case '*': return T_WILDCARD;
    case ':': return T_COLON;
    case ',': return T_COMMA;
    case '?': return T_QMARK;
    case '@': return T_AT;
    case '[': return T_LBRACKET;
    case ']': return T_RBRACKET;
    case '(': return T_LPAREN;
    case ')': return T_RPAREN;
    case '\'': return T_SQUOTE;
    case '"': return T_DQUOTE;
    case '+': return T_PLUS;
    case '-': return T_MINUS;
    case '/': return T_DIV;
    case '%': return T_PCT;
    case ' ': return T_SPACE;
    case '&': return d == '&' ? T_AND : T_SPECIAL;
    case '|': return d == '|' ? T_OR : T_SPECIAL;
    case '=': return d == '=' ? T_EQ : T_ASSIGN;
    case '!': return d == '=' ? T_NE : T_NOT;
    case '>': return d == '=' ? T_GE : T_GT;
    case '<': return d == '=' ? T_LE : T_LT;
    default: return isDigitC(c) ? T_DIGIT : isAlphaC(c) ? T_ALPHA
                                                        : T_SPECIAL;
    }
}

static void lexNext(lexer *lx, int skip_space) {
    for (;;) {
        lx->type = (uint8_t)lexPeek(lx);
        switch (lx->type) {
        case T_END: return;
        case T_DOTDOT:
            lx->rd++;
            /* fall through */
        case T_NE:
        case T_GE:
        case T_LE:
        case T_EQ:
        case T_AND:
        case T_OR:
            lx->ts = lx->p;
            lx->p += 2;
            return;
        case T_SPACE:
            if (skip_space) {
                while (lexAt(lx, lx->p) == ' ') lx->p++;
                skip_space = 0;
                continue;
            }
            /* fall through */
        default:
            lx->ts = lx->p;
            lx->p++;
            return;
        }
    }
}

static int lexMatch(lexer *lx, tokType type, int skip_space) {
    if (skip_space && lx->type == T_SPACE) {
        while (lexAt(lx, lx->p) == ' ') lx->p++;
        lexNext(lx, 0);
        skip_space = 0;
    }
    if (lx->type == type) {
        lexNext(lx, skip_space);
        return 1;
    }
    return 0;
}

static void lexSkipSpaces(lexer *lx) {
    if (lx->type == T_SPACE) {
        lexNext(lx, 1);
    } else {
        while (lexAt(lx, lx->p) == ' ') lx->p++;
    }
}

/* Returns 0 and the value, or 1 if the current token does not start an
 * integer. A value beyond int64 saturates: it stays larger (or smaller) than
 * any array index, which is the only thing such a value can mean. The module
 * lets it overflow instead, so there "[18446744073709551617]" is "[1]". */
static int lexInteger(lexer *lx, int64_t *val) {
    int neg = 0;
    if (lx->type != T_DIGIT && lx->type != T_PLUS && lx->type != T_MINUS) return 1;
    if (lx->type != T_DIGIT) {
        neg = lx->type == T_MINUS;
        lexNext(lx, 0);
        if (lx->type != T_DIGIT) return 1;
    }
    const uint64_t limit = (uint64_t)INT64_MAX + (neg ? 1 : 0);
    uint64_t v = (uint64_t)(lexAt(lx, lx->ts) - '0');
    while (isDigitC(lexAt(lx, lx->p))) {
        const uint64_t d = (uint64_t)(lexAt(lx, lx->p) - '0');
        v = v > (limit - d) / 10 ? limit : v * 10 + d;
        lx->p++;
    }
    *val = (int64_t)(neg ? (uint64_t)0 - v : v);
    lexNext(lx, 0);
    return 0;
}

/* 1 for the characters that end an unquoted member name: . [ ] ( ) < > = ! ' " space | & and NUL. */
static const uint8_t name_terminator[256] = {
    [0] = 1,
    ['.'] = 1,
    ['['] = 1,
    [']'] = 1,
    ['('] = 1,
    [')'] = 1,
    ['<'] = 1,
    ['>'] = 1,
    ['='] = 1,
    ['!'] = 1,
    ['\''] = 1,
    ['"'] = 1,
    [' '] = 1,
    ['|'] = 1,
    ['&'] = 1,
};

static inline int isNameTerminator(char c) {
    return name_terminator[(unsigned char)c];
}

/* An unquoted member name runs from the start of the current token to the
 * next terminator. Returns 0 and the name's place in the path text, or 1. */
static int lexUnquotedName(lexer *lx, size_t *off, size_t *len) {
    if (isNameTerminator(lexAt(lx, lx->ts))) return 1;
    size_t span = 0;
    while (!isNameTerminator(lexAt(lx, lx->p + span))) span++;
    *off = lx->ts;
    *len = 1 + span;
    lx->p += span;
    lexNext(lx, 0);
    return 0;
}

/* ----------------------------------------------------------------------------
 * String and number literals
 * ------------------------------------------------------------------------- */

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (unsigned)(c - 'A' + 10);
        } else {
            return 0;
        }
    }
    *out = v;
    return 1;
}

/* Decodes the JSON string literal that is the whole of v[0..n), with the rules
 * of the module's JSON parser: the standard escapes, surrogate pairs, no
 * unescaped control characters, no UTF-8 validation, white space allowed after
 * the closing quote. Appends the decoded bytes to strs. Returns 0 if the text
 * is not one string literal. */
static int decodeJsonString(jsonPath *p, const char *v, size_t n, uint32_t *off, uint32_t *len) {
    if (n < 2 || v[0] != '"') return 0;
    uint32_t start = strReserve(p, n);
    char *out = p->strs + start;
    size_t o = 0, i = 1;
    for (;;) {
        if (i >= n) return 0;
        unsigned char c = (unsigned char)v[i++];
        if (c == '"') break;
        if (c < 0x20) return 0;
        if (c != '\\') {
            out[o++] = (char)c;
            continue;
        }
        if (i >= n) return 0;
        char e = v[i++];
        switch (e) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (n - i < 4 || !hex4(v + i, &cp)) return 0;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                if (cp > 0xDBFF) return 0;
                if (n - i < 6 || v[i] != '\\' || v[i + 1] != 'u' || !hex4(v + i + 2, &lo)) return 0;
                if (lo < 0xDC00 || lo > 0xDFFF) return 0;
                i += 6;
                cp = (((cp - 0xD800) << 10) | (lo - 0xDC00)) + 0x10000;
            }
            if (cp < 0x80) {
                out[o++] = (char)cp;
            } else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | (cp >> 18));
                out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: return 0;
        }
    }
    for (; i < n; i++) {
        if (v[i] != ' ' && v[i] != '\t' && v[i] != '\n' && v[i] != '\r') return 0;
    }
    *off = start;
    *len = (uint32_t)o;
    p->nstrs += (uint32_t)o;
    return 1;
}

/* Scans a double-quoted string from the current token, as the module does: up
 * to the first quote that is not preceded by a backslash, then decoded as
 * JSON. Returns 0 or JSON_PATH_ERR_JSON_PARSE. */
static jsonPathErr lexDoubleQuoted(lexer *lx, jsonPath *p, uint32_t *off, uint32_t *len) {
    size_t start = lx->ts, vlen = 1;
    char prev = 0;
    int have_prev = 0;
    while (lexAt(lx, lx->p) != '\0') {
        char c = lexAt(lx, lx->p);
        if (c == '"' && (!have_prev || prev != '\\')) {
            lx->p++;
            vlen++;
            break;
        }
        prev = c;
        have_prev = 1;
        lx->p++;
        vlen++;
    }
    if (!decodeJsonString(p, lx->s + start, vlen, off, len)) return JSON_PATH_ERR_JSON_PARSE;
    lexNext(lx, 0);
    return JSON_PATH_OK;
}

/* Finds the end of a single-quoted string that starts after the current
 * token. Leaves the lexer after the closing quote. */
static void lexSingleQuotedSpan(lexer *lx, size_t *start, size_t *len) {
    char prev = 0;
    int have_prev = 0;
    *start = lx->p;
    *len = 0;
    while (lexAt(lx, lx->p) != '\0') {
        char c = lexAt(lx, lx->p);
        if (c == '\'' && (!have_prev || prev != '\\')) {
            lx->p++;
            break;
        }
        prev = c;
        have_prev = 1;
        lx->p++;
        (*len)++;
    }
}

/* A single-quoted member name. A backslash escapes \ t b f n r and the quote.
 * Any other backslash is kept. */
static void lexSingleQuotedName(lexer *lx, jsonPath *p, uint32_t *off, uint32_t *len) {
    size_t start, n;
    lexSingleQuotedSpan(lx, &start, &n);
    uint32_t o = strReserve(p, n);
    char *out = p->strs + o;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = lx->s[start + i];
        if (c == '\\' && i + 1 < n) {
            static const char from[] = "\\tbfnr'", to[] = "\\\t\b\f\n\r'";
            const char *hit = strchr(from, lx->s[start + i + 1]);
            if (hit) {
                out[w++] = to[hit - from];
                i++;
                continue;
            }
        }
        out[w++] = c;
    }
    *off = o;
    *len = (uint32_t)w;
    p->nstrs += (uint32_t)w;
    lexNext(lx, 0);
}

/* A single-quoted filter value. The module rewrites it as a double-quoted
 * JSON string (a double quote gains a backslash, a backslash before a single
 * quote is dropped) and parses that. */
static jsonPathErr lexSingleQuotedValue(lexer *lx, jsonPath *p, uint32_t *off, uint32_t *len) {
    size_t start, n;
    lexSingleQuotedSpan(lx, &start, &n);
    char small[2 * 128 + 3], *tmp = small;
    if (n * 2 + 3 > sizeof(small)) tmp = pathAlloc(n * 2 + 3);
    size_t w = 0;
    tmp[w++] = '"';
    for (size_t i = 0; i < n; i++) {
        char c = lx->s[start + i];
        if (c == '"') {
            tmp[w++] = '\\';
            tmp[w++] = '"';
        } else if (c == '\\') {
            if (lexAt(lx, start + i + 1) != '\'') tmp[w++] = c;
        } else {
            tmp[w++] = c;
        }
    }
    tmp[w++] = '"';
    int ok = decodeJsonString(p, tmp, w, off, len);
    if (tmp != small) pathFree(tmp);
    if (!ok) return JSON_PATH_ERR_JSON_PARSE;
    lexNext(lx, 0);
    return JSON_PATH_OK;
}

/* Classifies JSON number text the way the module's parser stores it: an
 * integer that fits int64, or a double. Integer text beyond int64 is a double
 * there, also when it would fit uint64. Returns 0 if the text is not a JSON
 * number or is too large for a double. */
static int parseNumberLiteral(const char *s, size_t n, uint8_t *kind, int64_t *iv, double *dv) {
    size_t i = 0;
    int neg = 0, is_int = 1;
    if (i < n && s[i] == '-') {
        neg = 1;
        i++;
    }
    size_t digits = i;
    if (i < n && s[i] == '0') {
        i++;
    } else if (i < n && s[i] >= '1' && s[i] <= '9') {
        while (i < n && isDigitC(s[i])) i++;
    } else {
        return 0;
    }
    size_t digits_end = i;
    if (i < n && s[i] == '.') {
        is_int = 0;
        i++;
        if (i >= n || !isDigitC(s[i])) return 0;
        while (i < n && isDigitC(s[i])) i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        is_int = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= n || !isDigitC(s[i])) return 0;
        while (i < n && isDigitC(s[i])) i++;
    }
    if (i != n) return 0;
    if (is_int) {
        uint64_t v = 0, limit = (uint64_t)INT64_MAX + (neg ? 1 : 0);
        int fits = 1;
        for (size_t k = digits; k < digits_end && fits; k++) {
            uint64_t d = (uint64_t)(s[k] - '0');
            if (v > (limit - d) / 10) fits = 0;
            v = v * 10 + d;
        }
        if (fits) {
            *kind = CV_I64;
            *iv = (int64_t)(neg ? (uint64_t)0 - v : v);
            return 1;
        }
    }
    /* The engine's reader: correctly rounded, and linear in the length of the text. */
    int overflow = 0;
    double d = jsonDoubleParse(s, n, &overflow);
    if (overflow || isinf(d)) return 0;
    *kind = CV_DBL;
    *dv = d;
    return 1;
}

/* ----------------------------------------------------------------------------
 * Compiler: a port of the module's parser that emits steps
 * ------------------------------------------------------------------------- */

#define CTX_REL 0 /* The module's parseRelativePath is about to run. */
#define CTX_SUP 1 /* The module's parseSupportedPath is about to run. */

typedef struct pendingAlt {
    uint32_t step; /* The filter step whose next_sup is still to be compiled. */
    lexer lx;
} pendingAlt;

typedef struct compiler {
    jsonPath *path;
    lexer lx;
    lexer alt;         /* After a filter step: the lexer its array and scalar continuation starts from. */
    uint32_t fd, fmax; /* Filter nesting, as the module's recursion counter sees it. */
    uint32_t first;    /* First step of this program. */
    uint32_t *memo;    /* Lexer state and context to step, once continuations diverge. */
    pendingAlt *pend;
    uint32_t npend, cappend;
} compiler;

static uint32_t compileProgram(compiler *c, int ctx, uint8_t *may_dup);

static int filterExpr(compiler *c);

static void filterErr(compiler *c, jsonPathErr e) {
    filterIns *in = newIns(c->path, FI_ERR);
    in->err = (uint8_t)e;
    in->need = (uint16_t)(c->fmax > UINT16_MAX ? UINT16_MAX : c->fmax);
}

static filterIns *filterIns_(compiler *c, filterKind kind) {
    filterIns *in = newIns(c->path, kind);
    in->need = (uint16_t)(c->fmax > UINT16_MAX ? UINT16_MAX : c->fmax);
    return in;
}

/* The module counts one level of recursion per parse function. Past the limit
 * the path can only fail, so the compiler stops there too, which also bounds
 * its own recursion. */
static int filterEnter(compiler *c) {
    c->fd++;
    if (c->fd > c->fmax) c->fmax = c->fd;
    if (c->fd > JSON_PATH_MAX_NESTING) {
        filterErr(c, JSON_PATH_ERR_TOO_DEEP);
        c->fd--;
        return 0;
    }
    return 1;
}

typedef struct litVal {
    uint8_t kind;
    union {
        int64_t i;
        double d;
        struct {
            uint32_t off, len;
        } s;
    } u;
} litVal;

/* The module's Lexer::scanPathValue: the extent of a "$..." path used as a
 * filter value. Returns 0 if a bracket holds something other than a quoted
 * name or digits. */
static int lexPathValue(lexer *lx, size_t *start, size_t *len) {
    int in_brackets = 0, in_quotes = 0;
    char quote = '"';
    *start = lx->ts;
    *len = 1;
    while (lexAt(lx, lx->p) != '\0') {
        char c = lexAt(lx, lx->p);
        if (!in_brackets) {
            if (c == '[') {
                in_brackets = 1;
            } else if (strchr("]()<>=!'\" |&", c) != NULL) {
                break;
            }
        } else if (!in_quotes) {
            if (c == '"' || c == '\'') {
                in_quotes = 1;
                quote = c;
            } else if (c == ']') {
                in_brackets = 0;
            } else if (strchr("-+0123456789", c) == NULL) {
                return 0;
            }
        } else {
            if (c == '\\' && lexAt(lx, lx->p + 1) == quote) {
                lx->p++;
                (*len)++;
            } else if (c == quote) {
                in_quotes = 0;
            }
        }
        lx->p++;
        (*len)++;
    }
    lexNext(lx, 0);
    return 1;
}

/* The module's parseComparisonValue. Returns 1, or 0 after emitting the
 * error. */
static int filterValue(compiler *c, litVal *v) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    jsonPathErr e = JSON_PATH_OK;
    if (!filterEnter(c)) return 0;
    if (lx->type == T_DOLLAR) {
        size_t start, len;
        if (!lexPathValue(lx, &start, &len)) {
            e = JSON_PATH_ERR_SYNTAX;
        } else {
            compiler sub;
            uint8_t may_dup;
            memset(&sub, 0, sizeof(sub));
            sub.path = p;
            sub.lx.s = lx->s + start;
            sub.lx.n = len;
            lexNext(&sub.lx, 0);
            uint32_t entry = compileProgram(&sub, CTX_SUP, &may_dup);
            filterIns *in = filterIns_(c, FI_PATHVAL);
            in->v.entry = entry;
            in->may_dup = may_dup;
            in->vdepth = (uint16_t)c->fd;
            v->kind = CV_REG;
        }
    } else if (lx->type == T_DQUOTE) {
        e = lexDoubleQuoted(lx, p, &v->u.s.off, &v->u.s.len);
        v->kind = CV_STR;
    } else if (lx->type == T_SQUOTE) {
        e = lexSingleQuotedValue(lx, p, &v->u.s.off, &v->u.s.len);
        v->kind = CV_STR;
    } else if (lx->type == T_ALPHA && strchr("ntf", lexAt(lx, lx->ts)) != NULL) {
        /* An identifier: the run of letters and digits from the token. */
        size_t start = lx->ts, len = 1;
        while (isDigitC(lexAt(lx, lx->p)) || isAlphaC(lexAt(lx, lx->p))) {
            lx->p++;
            len++;
        }
        lexNext(lx, 0);
        const char *id = lx->s + start;
        if (len == 4 && memcmp(id, "null", 4) == 0) {
            v->kind = CV_NULL;
        } else if (len == 4 && memcmp(id, "true", 4) == 0 && id[0] == 't') {
            v->kind = CV_TRUE;
        } else if (len == 5 && memcmp(id, "false", 5) == 0) {
            v->kind = CV_FALSE;
        } else {
            e = JSON_PATH_ERR_IDENTIFIER;
        }
    } else {
        /* A number: the run of number characters from the token. */
        char first = lexAt(lx, lx->ts);
        if (first == '\0' || strchr("+-0123456789.Ee", first) == NULL) {
            e = JSON_PATH_ERR_NUMBER;
        } else {
            size_t start = lx->ts, span = 0;
            while (lexAt(lx, lx->p + span) != '\0' && strchr("+-0123456789.Ee", lexAt(lx, lx->p + span)) != NULL) span++;
            lx->p += span;
            lexNext(lx, 0);
            if (!parseNumberLiteral(lx->s + start, 1 + span, &v->kind, &v->u.i, &v->u.d)) {
                e = JSON_PATH_ERR_JSON_PARSE;
            }
        }
    }
    c->fd--;
    if (e != JSON_PATH_OK) {
        filterErr(c, e);
        return 0;
    }
    return 1;
}

static int isCmpTok(uint8_t t) {
    return t == T_EQ || t == T_NE || t == T_LT || t == T_LE || t == T_GT || t == T_GE;
}

/* The module's parseComparisonOp. */
static int filterOp(compiler *c, uint8_t *op) {
    lexer *lx = &c->lx;
    lexSkipSpaces(lx);
    switch (lx->type) {
    case T_EQ: *op = OP_EQ; break;
    case T_NE: *op = OP_NE; break;
    case T_LT: *op = OP_LT; break;
    case T_LE: *op = OP_LE; break;
    case T_GT: *op = OP_GT; break;
    case T_GE: *op = OP_GE; break;
    default: filterErr(c, JSON_PATH_ERR_SYNTAX); return 0;
    }
    lexSkipSpaces(lx);
    lexNext(lx, 1);
    return 1;
}

static uint8_t swapOp(uint8_t op) {
    switch (op) {
    case OP_GT: return OP_LT;
    case OP_LT: return OP_GT;
    case OP_GE: return OP_LE;
    case OP_LE: return OP_GE;
    default: return op;
    }
}

/* A quoted member name at the current token. */
static jsonPathErr quotedName(compiler *c, uint32_t *name) {
    lexer *lx = &c->lx;
    uint32_t off, len;
    if (lx->type == T_DQUOTE) {
        jsonPathErr e = lexDoubleQuoted(lx, c->path, &off, &len);
        if (e != JSON_PATH_OK) return e;
    } else if (lx->type == T_SQUOTE) {
        lexSingleQuotedName(lx, c->path, &off, &len);
    } else {
        return JSON_PATH_ERR_SYNTAX;
    }
    *name = addName(c->path, off, len);
    return JSON_PATH_OK;
}

/* The module's parseMemberName, for a filter. */
static int filterMemberName(compiler *c, uint32_t *name) {
    lexer *lx = &c->lx;
    jsonPathErr e;
    int bracketed;
    if (lexMatch(lx, T_DOT, 0)) {
        bracketed = lexMatch(lx, T_LBRACKET, 0);
    } else if (lexMatch(lx, T_LBRACKET, 0)) {
        bracketed = 1;
    } else {
        filterErr(c, JSON_PATH_ERR_SYNTAX);
        return 0;
    }
    if (bracketed) {
        lexSkipSpaces(lx);
        e = quotedName(c, name);
        if (e == JSON_PATH_OK && !lexMatch(lx, T_RBRACKET, 1)) e = JSON_PATH_ERR_SYNTAX;
    } else {
        size_t off, len;
        if (lexUnquotedName(lx, &off, &len)) {
            e = JSON_PATH_ERR_MEMBER_NAME;
        } else {
            *name = addNameBytes(c->path, lx->s + off, len);
            e = JSON_PATH_OK;
        }
    }
    if (e != JSON_PATH_OK) {
        filterErr(c, e);
        return 0;
    }
    return 1;
}

static void filterCmp(compiler *c, cmpLhs lhs, uint32_t name, int64_t index, uint8_t op, const litVal *v) {
    filterIns *in = filterIns_(c, FI_CMP);
    in->lhs = (uint8_t)lhs;
    in->name = name;
    in->index = index;
    in->op = op;
    in->vkind = v->kind;
    switch (v->kind) {
    case CV_I64: in->v.i = v->u.i; break;
    case CV_DBL: in->v.d = v->u.d; break;
    case CV_STR:
        in->v.s.off = v->u.s.off;
        in->v.s.len = v->u.s.len;
        break;
    default: break;
    }
}

/* The module's parseFactor. Returns 1, or 0 after emitting an error. */
static int filterFactor(compiler *c) {
    lexer *lx = &c->lx;
    litVal v;
    uint8_t op = OP_EQ;
    uint32_t name = 0;
    int64_t index = 0;
    int ok = 0;
    if (!filterEnter(c)) return 0;
    lexSkipSpaces(lx);
    if (lx->type == T_LPAREN) {
        lexNext(lx, 1);
        if (!filterExpr(c)) goto out;
        if (!lexMatch(lx, T_RPAREN, 1)) {
            filterErr(c, JSON_PATH_ERR_SYNTAX);
            goto out;
        }
    } else if (lexMatch(lx, T_AT, 0)) {
        if (lx->type == T_DOT || lx->type == T_LBRACKET) {
            if (!filterMemberName(c, &name)) goto out;
            lexSkipSpaces(lx);
            if (isCmpTok(lx->type)) {
                if (!filterOp(c, &op) || !filterValue(c, &v)) goto out;
                filterCmp(c, LHS_MEMBER, name, 0, op, &v);
            } else if (lx->type == T_LBRACKET) {
                lexNext(lx, 1);
                if (lx->type == T_QMARK) {
                    /* @.name[?(@ op value)]: some element of the member satisfies the comparison. */
                    lexNext(lx, 1);
                    if (!lexMatch(lx, T_LPAREN, 0)) {
                        filterErr(c, JSON_PATH_ERR_SYNTAX);
                        goto out;
                    }
                    if (lx->type == T_AT) {
                        lexNext(lx, 1);
                        if (!filterOp(c, &op) || !filterValue(c, &v)) goto out;
                    } else {
                        if (!filterValue(c, &v) || !filterOp(c, &op)) goto out;
                        op = swapOp(op);
                        if (!lexMatch(lx, T_AT, 0)) {
                            filterErr(c, JSON_PATH_ERR_SYNTAX);
                            goto out;
                        }
                    }
                    filterCmp(c, LHS_CONTAINS, name, 0, op, &v);
                    if (!lexMatch(lx, T_RPAREN, 1) || !lexMatch(lx, T_RBRACKET, 0)) {
                        filterErr(c, JSON_PATH_ERR_SYNTAX);
                        goto out;
                    }
                } else {
                    /* @.name[index] op value */
                    if (lexInteger(lx, &index)) {
                        filterErr(c, JSON_PATH_ERR_INDEX_NOT_NUMBER);
                        goto out;
                    }
                    if (!lexMatch(lx, T_RBRACKET, 0)) {
                        filterErr(c, JSON_PATH_ERR_SYNTAX);
                        goto out;
                    }
                    if (!filterOp(c, &op) || !filterValue(c, &v)) goto out;
                    filterCmp(c, LHS_MEMBER_INDEX, name, index, op, &v);
                }
            } else {
                filterIns_(c, FI_ATTR)->name = name;
            }
        } else {
            if (!filterOp(c, &op) || !filterValue(c, &v)) goto out;
            filterCmp(c, LHS_SELF, addNameBytes(c->path, "", 0), 0, op, &v);
        }
    } else {
        /* The value is on the left: value op @[.name[[index]]]. The spaces
         * skipped above follow the value's first character, so they belong to
         * the value: scan it from that character. (The module scans past them,
         * which drops them from a string and reads "5 0" as 5.) */
        if (lx->type != T_DOLLAR) lx->p = lx->ts + 1;
        if (!filterValue(c, &v)) goto out;
        lexSkipSpaces(lx);
        if (!filterOp(c, &op)) goto out;
        op = swapOp(op);
        if (!lexMatch(lx, T_AT, 0)) {
            filterErr(c, JSON_PATH_ERR_SYNTAX);
            goto out;
        }
        lexSkipSpaces(lx);
        if (lx->type == T_RPAREN || lx->type == T_AND || lx->type == T_OR) {
            filterCmp(c, LHS_SELF, addNameBytes(c->path, "", 0), 0, op, &v);
        } else {
            if (!filterMemberName(c, &name)) goto out;
            if (lx->type == T_LBRACKET) {
                lexNext(lx, 1);
                if (lexInteger(lx, &index)) {
                    filterErr(c, JSON_PATH_ERR_INDEX_NOT_NUMBER);
                    goto out;
                }
                if (!lexMatch(lx, T_RBRACKET, 0)) {
                    filterErr(c, JSON_PATH_ERR_SYNTAX);
                    goto out;
                }
                filterCmp(c, LHS_MEMBER_INDEX, name, index, op, &v);
            } else {
                filterCmp(c, LHS_MEMBER, name, 0, op, &v);
            }
        }
    }
    ok = 1;
out:
    c->fd--;
    return ok;
}

/* The module's parseTerm. */
static int filterTerm(compiler *c) {
    int ok = 0;
    if (!filterEnter(c)) return 0;
    if (!filterFactor(c)) goto out;
    while (lexMatch(&c->lx, T_AND, 1)) {
        if (!filterFactor(c)) goto out;
        filterIns_(c, FI_AND);
    }
    ok = 1;
out:
    c->fd--;
    return ok;
}

/* The module's parseFilterExpr. */
static int filterExpr(compiler *c) {
    int ok = 0;
    if (!filterEnter(c)) return 0;
    lexSkipSpaces(&c->lx);
    if (!filterTerm(c)) goto out;
    while (lexMatch(&c->lx, T_OR, 1)) {
        if (!filterTerm(c)) goto out;
        filterIns_(c, FI_OR);
    }
    lexSkipSpaces(&c->lx);
    ok = 1;
out:
    c->fd--;
    return ok;
}

/* Compiles a filter from just after its "(". The three spellings differ in
 * where spaces may follow the closing tokens. */
static uint32_t compileFilterBody(compiler *c, int rparen_skip, int rbracket_skip, int flags) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    uint32_t prog = p->nins;
    c->fd = c->fmax = 0;
    int ok = filterExpr(c);
    if (ok && (!lexMatch(lx, T_RPAREN, rparen_skip) || !lexMatch(lx, T_RBRACKET, rbracket_skip))) {
        filterErr(c, JSON_PATH_ERR_SYNTAX);
        ok = 0;
    }
    uint32_t si = newStep(p, ST_FILTER);
    p->steps[si].u.filter.prog = prog;
    p->steps[si].u.filter.nprog = p->nins - prog;
    p->steps[si].flags = (uint8_t)(flags | (ok ? 0 : SF_TERMINAL));
    return si;
}

static uint32_t finishSlice(compiler *c, int64_t start, int64_t end, int flags, int64_t step) {
    if (!lexMatch(&c->lx, T_RBRACKET, 1)) return errStep(c->path, JSON_PATH_ERR_SYNTAX, SF_NEED_ARRAY);
    if (step == 0) return errStep(c->path, JSON_PATH_ERR_STEP_ZERO, SF_NEED_ARRAY);
    uint32_t si = newStep(c->path, ST_SLICE);
    pathStep *s = &c->path->steps[si];
    s->flags = (uint8_t)flags;
    s->u.sl.start = start;
    s->u.sl.end = end;
    s->u.sl.step = step;
    return si;
}

/* The module's parseStep. */
static uint32_t compileSliceStep(compiler *c, int64_t start, int64_t end, int flags) {
    int64_t step;
    lexSkipSpaces(&c->lx);
    if (c->lx.type == T_RBRACKET) return finishSlice(c, start, end, flags, 1);
    if (lexInteger(&c->lx, &step)) return errStep(c->path, JSON_PATH_ERR_INDEX_NOT_NUMBER, SF_NEED_ARRAY);
    return finishSlice(c, start, end, flags, step);
}

/* The module's parseEndAndStep. */
static uint32_t compileSliceEnd(compiler *c, int64_t start) {
    int64_t end;
    if (lexInteger(&c->lx, &end)) return errStep(c->path, JSON_PATH_ERR_INDEX_NOT_NUMBER, SF_NEED_ARRAY);
    lexSkipSpaces(&c->lx);
    if (c->lx.type == T_COLON) {
        lexNext(&c->lx, 0);
        return compileSliceStep(c, start, end, 0);
    }
    return finishSlice(c, start, end, 0, 1);
}

/* The module's parseUnionOfIndexes. */
static uint32_t compileIndexUnion(compiler *c, int64_t first) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    uint32_t off = p->nints;
    int comma = 0;
    addInt(p, first);
    lexSkipSpaces(lx);
    while (lx->type != T_RBRACKET) {
        if (lx->type == T_COMMA) {
            if (comma) return errStep(p, JSON_PATH_ERR_SYNTAX, SF_NEED_ARRAY);
            comma = 1;
            lexNext(lx, 1);
        } else {
            int64_t v;
            if (!comma) return errStep(p, JSON_PATH_ERR_SYNTAX, SF_NEED_ARRAY);
            comma = 0;
            lexSkipSpaces(lx);
            if (lexInteger(lx, &v)) return errStep(p, JSON_PATH_ERR_INDEX_NOT_NUMBER, SF_NEED_ARRAY);
            addInt(p, v);
        }
        lexSkipSpaces(lx);
    }
    if (comma || !lexMatch(lx, T_RBRACKET, 1)) return errStep(p, JSON_PATH_ERR_SYNTAX, SF_NEED_ARRAY);
    uint32_t si = newStep(p, ST_IUNION);
    const uint32_t count = p->nints - off;
    p->steps[si].u.list.off = off;
    p->steps[si].u.list.count = count;
    /* Entries that all count from the same end and strictly rise or strictly
     * fall name different elements in every array. Anything else is checked
     * against the array when the step runs. */
    const int64_t *v = p->ints + off;
    int rising = 1, falling = 1;
    for (uint32_t i = 1; i < count; i++) {
        if ((v[i] < 0) != (v[0] < 0)) rising = falling = 0;
        if (v[i] <= v[i - 1]) rising = 0;
        if (v[i] >= v[i - 1]) falling = 0;
    }
    if (rising || falling) p->steps[si].flags |= SF_NO_REPEAT;
    return si;
}

/* The module's parseIndexExpr. */
static uint32_t compileIndexExpr(compiler *c) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    int64_t start;
    switch (lx->type) {
    case T_END: return errStep(p, JSON_PATH_ERR_EMPTY_EXPR, 0);
    case T_QMARK:
        lexNext(lx, 0);
        if (!lexMatch(lx, T_LPAREN, 0)) return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
        return compileFilterBody(c, 0, 1, 0);
    case T_COLON:
        lexNext(lx, 1);
        if (lx->type == T_RBRACKET) return finishSlice(c, 0, 0, SF_END_SIZE, 1);
        if (lx->type == T_COLON) {
            lexNext(lx, 1);
            return compileSliceStep(c, 0, 0, SF_END_SIZE);
        }
        return compileSliceEnd(c, 0);
    case T_COMMA: return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
    default: break;
    }
    if (lexInteger(lx, &start)) return errStep(p, JSON_PATH_ERR_INDEX_NOT_NUMBER, 0);
    lexSkipSpaces(lx);
    if (lx->type == T_COLON) {
        lexNext(lx, 1);
        lexSkipSpaces(lx);
        if (lx->type == T_RBRACKET) return finishSlice(c, start, 0, SF_END_SIZE, 1);
        if (lx->type == T_COLON) {
            lexNext(lx, 0);
            return compileSliceStep(c, start, 0, SF_END_SIZE);
        }
        return compileSliceEnd(c, start);
    }
    if (lx->type == T_COMMA) return compileIndexUnion(c, start);
    if (!lexMatch(lx, T_RBRACKET, 1)) return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
    uint32_t si = newStep(p, ST_INDEX);
    p->steps[si].u.index = start;
    return si;
}

static uint32_t memberStep(compiler *c, uint32_t name) {
    uint32_t si = newStep(c->path, ST_MEMBER);
    c->path->steps[si].u.name = name;
    /* The module allows an insert when its lexer has nothing after the token
     * that follows the name, which is not quite "the name is the last step". */
    if (lexAt(&c->lx, c->lx.p) == '\0') c->path->steps[si].flags |= SF_INSERTABLE;
    return si;
}

/* Marks each name of a union that an earlier entry of the union already
 * names. Linear in the size of the union. */
static void markRepeatedNames(jsonPath *p, uint32_t first, uint32_t count) {
    uint32_t cap = 16;
    while (cap < count * 2) cap *= 2;
    uint32_t small[64], *tab = cap <= 64 ? small : pathAlloc((size_t)cap * sizeof(uint32_t));
    memset(tab, 0, (size_t)cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; i++) {
        pathName *nm = &p->names[first + i];
        const char *bytes = p->strs + nm->off;
        uint64_t h = 1469598103934665603ULL ^ nm->len;
        for (uint32_t k = 0; k < nm->len; k++) h = (h ^ (unsigned char)bytes[k]) * 1099511628211ULL;
        uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (cap - 1);
        for (; tab[slot]; slot = (slot + 1) & (cap - 1)) {
            const pathName *other = &p->names[tab[slot] - 1];
            if (other->len == nm->len && memcmp(p->strs + other->off, bytes, nm->len) == 0) {
                nm->repeat = 1;
                break;
            }
        }
        if (!nm->repeat) tab[slot] = first + i + 1;
    }
    if (tab != small) pathFree(tab);
}

/* The module's parseNameInBrackets. */
static uint32_t compileBracketNames(compiler *c) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    uint32_t first = p->nnames, name, count = 0;
    jsonPathErr e = quotedName(c, &name);
    if (e != JSON_PATH_OK) return errStep(p, e, 0);
    first = name;
    count = 1;
    while (lexMatch(lx, T_COMMA, 1)) {
        /* Skip spaces before the name only. The module also skips the spaces
         * that follow the opening quote, which loses them from the name. */
        if (lx->type == T_SPACE) lexSkipSpaces(lx);
        e = quotedName(c, &name);
        if (e != JSON_PATH_OK) return errStep(p, e, 0);
        count++;
    }
    if (!lexMatch(lx, T_RBRACKET, 1)) return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
    if (count == 1) return memberStep(c, first);
    markRepeatedNames(p, first, count);
    uint32_t si = newStep(p, ST_NUNION);
    p->steps[si].u.list.off = first;
    p->steps[si].u.list.count = count;
    return si;
}

/* The module's parseBracketPathElement. On return the lexer is where the
 * children of a forking step continue from. A step that does not fork
 * continues after the spaces that follow, and so does a filter on an object,
 * while a filter on an array or a scalar does not: c->alt keeps that state. */
static uint32_t compileBracket(compiler *c) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    uint32_t si;
    if (!lexMatch(lx, T_LBRACKET, 1)) return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
    if (lx->type == T_WILDCARD) {
        lexMatch(lx, T_WILDCARD, 1);
        if (!lexMatch(lx, T_RBRACKET, 1)) return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
        if (lx->type == T_LBRACKET && lexPeek(lx) == T_QMARK) {
            /* "[*][?(...)]" is the filter alone, applied to the current value. */
            lexMatch(lx, T_LBRACKET, 1);
            lexSkipSpaces(lx);
            if (!lexMatch(lx, T_QMARK, 0) || !lexMatch(lx, T_LPAREN, 0)) return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
            si = compileFilterBody(c, 1, 1, 0);
        } else {
            si = newStep(p, ST_WILD);
        }
    } else if (lx->type == T_SQUOTE || lx->type == T_DQUOTE) {
        si = compileBracketNames(c);
    } else {
        si = compileIndexExpr(c);
    }
    uint8_t kind = p->steps[si].kind;
    if (kind == ST_ERR || (kind == ST_FILTER && (p->steps[si].flags & SF_TERMINAL))) return si;
    if (kind == ST_MEMBER || kind == ST_INDEX) {
        lexSkipSpaces(lx);
    } else if (kind == ST_FILTER) {
        c->alt = *lx;
        lexSkipSpaces(lx);
    } else {
        /* The module skips the spaces after the children have run. The only
         * thing left to observe is a ".." the lexer may read while doing so. */
        lexer t = *lx;
        lexSkipSpaces(&t);
        lx->rd = t.rd;
    }
    return si;
}

/* The module's parseKey. */
static uint32_t compileKey(compiler *c) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    size_t off, len;
    if (lexMatch(lx, T_WILDCARD, 0)) {
        if (lx->type == T_DOT) lexNext(lx, 0);
        if (lx->type == T_LBRACKET && lexPeek(lx) == T_QMARK) {
            /* "*[?(...)]": the filter alone, and the value must be an array. */
            if (!lexMatch(lx, T_LBRACKET, 0) || !lexMatch(lx, T_QMARK, 0) || !lexMatch(lx, T_LPAREN, 1)) {
                return errStep(p, JSON_PATH_ERR_SYNTAX, 0);
            }
            uint32_t si = compileFilterBody(c, 1, 0, SF_WILD_KEY);
            c->alt = *lx;
            return si;
        }
        return newStep(p, ST_WILD);
    }
    if (lexUnquotedName(lx, &off, &len)) return errStep(p, JSON_PATH_ERR_MEMBER_NAME, 0);
    return memberStep(c, addNameBytes(p, lx->s + off, len));
}

/* Compiles the element at the lexer and returns its step. *ctx_after is the
 * context the continuation is read in. */
static uint32_t compileElement(compiler *c, int ctx, int *ctx_after) {
    lexer *lx = &c->lx;
    jsonPath *p = c->path;
    uint32_t rd0 = lx->rd, si;
    *ctx_after = CTX_REL;
    if (lx->type == T_END) return newStep(p, ST_END);
    if (ctx == CTX_SUP) {
        if (lx->type == T_DOLLAR) {
            lexMatch(lx, T_DOLLAR, 0);
            si = newStep(p, ST_DOLLAR);
            goto done;
        }
        if (lx->type == T_DOT) {
            lexMatch(lx, T_DOT, 0);
            if (lx->type == T_END) return newStep(p, ST_END);
        }
    }
    switch (lx->type) {
    case T_DOTDOT:
        lexMatch(lx, T_DOTDOT, 0);
        si = newStep(p, ST_DESCENT);
        if (lx->type == T_DOTDOT || lx->type == T_DOT) p->steps[si].flags |= SF_DOT_SEQ;
        break;
    case T_DOT:
        lexMatch(lx, T_DOT, 0);
        si = lx->type == T_LBRACKET ? compileBracket(c) : compileKey(c);
        break;
    case T_LBRACKET: si = compileBracket(c); break;
    default: si = compileKey(c); break;
    }
done:
    switch (p->steps[si].kind) {
    case ST_SLICE:
    case ST_IUNION:
    case ST_NUNION:
    case ST_WILD:
    case ST_DESCENT: *ctx_after = CTX_SUP; break;
    default: break;
    }
    p->steps[si].rd = (uint8_t)(lx->rd - rd0 > 255 ? 255 : lx->rd - rd0);
    return si;
}

/* The two contexts read the same thing unless the next token is "$", or is a
 * "." that ends the path or has spaces skipped after it. */
static int normalCtx(const lexer *lx, int ctx) {
    if (ctx != CTX_SUP || lx->type == T_DOLLAR) return ctx;
    if (lx->type == T_DOT && (lexAt(lx, lx->p) == '\0' || lx->p != lx->ts + 1)) return ctx;
    return CTX_REL;
}

/* Identifies a lexer state together with a context. For one token start `p`
 * has two possible values: just after the token, or after the spaces that
 * follow it. */
static uint32_t lexKey(const lexer *lx, int ctx) {
    size_t ts = lx->type == T_END ? lx->n : lx->ts;
    size_t tlen = 1;
    switch (lx->type) {
    case T_END: tlen = 0; break;
    case T_DOTDOT:
    case T_NE:
    case T_GE:
    case T_LE:
    case T_EQ:
    case T_AND:
    case T_OR: tlen = 2; break;
    default: break;
    }
    int advanced = lx->type != T_END && lx->p != ts + tlen;
    return (uint32_t)(ts * 4 + (size_t)advanced * 2 + (size_t)ctx);
}

static uint32_t compileChain(compiler *c, int ctx) {
    jsonPath *p = c->path;
    uint32_t first = NO_STEP, prev = NO_STEP;
    for (;;) {
        int ctx_after;
        ctx = normalCtx(&c->lx, ctx);
        uint32_t key = lexKey(&c->lx, ctx);
        uint32_t si = c->memo ? c->memo[key] : NO_STEP;
        int fresh = si == NO_STEP;
        if (fresh) {
            si = compileElement(c, ctx, &ctx_after);
            p->steps[si].ekey = key;
            if (c->memo) c->memo[key] = si;
        }
        if (prev == NO_STEP) {
            first = si;
        } else {
            p->steps[prev].next = si;
            if (p->steps[prev].flags & SF_SUP_SAME && p->steps[prev].kind == ST_FILTER) p->steps[prev].next_sup = si;
        }
        if (!fresh) break;
        pathStep *s = &p->steps[si];
        if (s->kind == ST_END || s->kind == ST_ERR) break;
        if (s->kind == ST_FILTER) {
            if (s->flags & SF_TERMINAL) break;
            int actx = normalCtx(&c->alt, CTX_SUP);
            if (lexKey(&c->alt, actx) == lexKey(&c->lx, normalCtx(&c->lx, CTX_REL))) {
                s->flags |= SF_SUP_SAME;
            } else {
                c->pend = growArray(c->pend, NULL, c->npend, &c->cappend, 1, sizeof(pendingAlt));
                c->pend[c->npend].step = si;
                c->pend[c->npend].lx = c->alt;
                c->npend++;
            }
        }
        prev = si;
        ctx = ctx_after;
    }
    return first;
}

/* Compiles the path the lexer is at the start of. Returns its first step. */
static uint32_t compileProgram(compiler *c, int ctx, uint8_t *may_dup) {
    jsonPath *p = c->path;
    c->first = p->nsteps;
    uint32_t entry = compileChain(c, ctx);
    while (c->npend) {
        /* A filter whose two continuations differ. Chains can meet again, so
         * from here on steps are shared by the lexer state they start from,
         * which keeps the program linear in the path length. */
        pendingAlt pa = c->pend[--c->npend];
        if (!c->memo) {
            size_t slots = (c->lx.n + 1) * 4;
            c->memo = pathAlloc(slots * sizeof(uint32_t));
            for (size_t i = 0; i < slots; i++) c->memo[i] = NO_STEP;
            for (uint32_t i = c->first; i < p->nsteps; i++) {
                if (p->steps[i].ekey != UINT32_MAX) c->memo[p->steps[i].ekey] = i;
            }
        }
        c->lx = pa.lx;
        uint32_t chain = compileChain(c, CTX_SUP);
        p->steps[pa.step].next_sup = chain;
    }
    pathFree(c->memo);
    pathFree(c->pend);
    c->memo = NULL;
    c->pend = NULL;
    /* Which programs can select one value twice: see dedupe() below. */
    int unions = 0, filters = 0, descents = 0;
    for (uint32_t i = c->first; i < p->nsteps; i++) {
        uint8_t k = p->steps[i].kind;
        if (p->steps[i].ekey == UINT32_MAX) continue;
        p->steps[i].ekey = UINT32_MAX; /* Not this program's concern any more. */
        if (k == ST_IUNION || k == ST_NUNION) unions++;
        if (k == ST_FILTER) filters++;
        if (k == ST_DESCENT) descents++;
    }
    *may_dup = unions || descents > 1 || (descents && filters);
    return entry;
}

static void pathInit(jsonPath *p) {
    memset(p, 0, offsetof(jsonPath, inline_steps));
    p->steps = p->inline_steps;
    p->capsteps = PATH_INLINE_STEPS;
    p->names = p->inline_names;
    p->capnames = PATH_INLINE_NAMES;
    p->strs = p->inline_strs;
    p->capstrs = PATH_INLINE_STRS;
}

/* Drops the references the path holds on its names' keys. */
static void pathReleaseKeys(jsonPath *p) {
    for (uint32_t i = 0; i < p->nnames; i++) {
        if (p->names[i].keyid) jsonKeyRelease(p->names[i].keyid);
        p->names[i].keyid = 0;
    }
}

static void pathFreeArrays(jsonPath *p) {
    if (p->steps != p->inline_steps) pathFree(p->steps);
    if (p->names != p->inline_names) pathFree(p->names);
    if (p->strs != p->inline_strs) pathFree(p->strs);
    pathFree(p->ins);
    pathFree(p->ints);
}

/* The bytes the direct compiler below takes as part of a member name:
 * letters, digits, "_", "-" and everything outside ASCII. */
static inline int isSimpleNameChar(char c) {
    return isAlphaC(c) || isDigitC(c) || c == '_' || c == '-' || (unsigned char)c >= 0x80;
}

/* The paths clients send most are plain: an optional "$", then ".name" and
 * "[digits]" elements, with a bare name allowed first in a legacy path. Such
 * a path is compiled here in one pass, to the same program the full compiler
 * makes of it (the tests compare the two on every path they have). Returns 0,
 * having done nothing, for any other path. */
static int compileSimple(jsonPath *p, const char *s, size_t n) {
    const size_t start = n && s[0] == '$';
    size_t i = start;
    int bare = !start; /* A name without its dot may come next. */
    if (n == 0) return 0;
    while (i < n) {
        const char c = s[i];
        if (c == '[') {
            const size_t digits = ++i;
            while (i < n && isDigitC(s[i])) i++;
            if (i == digits || i - digits > 18 || i == n || s[i] != ']') return 0;
            i++;
        } else if (c == '.' || bare) {
            const size_t name = i += c == '.';
            while (i < n && isSimpleNameChar(s[i])) i++;
            if (i == name) return 0;
        } else {
            return 0;
        }
        bare = 0;
    }

    uint32_t prev = NO_STEP, si = NO_STEP, count = 0;
    if (start) prev = newStep(p, ST_DOLLAR);
    for (i = start; i < n; count++) {
        if (s[i] == '[') {
            int64_t v = 0;
            for (i++; s[i] != ']'; i++) v = v * 10 + (s[i] - '0');
            i++;
            si = newStep(p, ST_INDEX);
            p->steps[si].u.index = v;
        } else {
            const size_t name = i += s[i] == '.';
            while (i < n && isSimpleNameChar(s[i])) i++;
            const uint32_t nm = addNameBytes(p, s + name, i - name);
            si = newStep(p, ST_MEMBER);
            p->steps[si].u.name = nm;
            if (i == n) p->steps[si].flags = SF_INSERTABLE;
        }
        if (prev != NO_STEP) p->steps[prev].next = si;
        prev = si;
    }
    si = newStep(p, ST_END);
    if (prev != NO_STEP) p->steps[prev].next = si;
    p->entry = 0;
    p->rd_init = 0;
    p->may_dup = 0;
    p->singular = 1;
    p->nsing = count;
    return 1;
}

/* Compiles into a path that is new or was compiled before. Arrays that have
 * grown keep their size, so recompiling into a pooled path does not allocate
 * once the pool has seen paths of that shape. */
static void compileInto(jsonPath *p, const char *s, size_t n, int direct) {
    pathReleaseKeys(p);
    p->nsteps = p->nnames = p->nstrs = p->nins = p->nints = 0;
    p->unresolved = 0;
    p->singular = 0;
    p->nsing = 0;
    p->v2 = p->v2_static = n > 0 && s[0] == '$';
    p->root = n == 1 && (s[0] == '$' || s[0] == '.');
    if (direct && compileSimple(p, s, n)) return;

    compiler c;
    memset(&c, 0, sizeof(c));
    c.path = p;
    c.lx.s = s;
    c.lx.n = n;
    lexNext(&c.lx, 0);
    p->rd_init = (uint8_t)c.lx.rd;
    p->entry = compileProgram(&c, CTX_SUP, &p->may_dup);

    /* A singular path: an optional "$", then member and index steps only. */
    uint32_t si = p->entry, count = 0;
    if (p->steps[si].kind == ST_DOLLAR) si = p->steps[si].next;
    while (p->steps[si].kind == ST_MEMBER || p->steps[si].kind == ST_INDEX) {
        si = p->steps[si].next;
        count++;
    }
    if (p->steps[si].kind == ST_END && !p->rd_init) {
        p->singular = 1;
        p->nsing = count;
    }
}

/* ----------------------------------------------------------------------------
 * Cache of compiled paths
 *
 * Clients send the same few paths over and over, and a command compiles its
 * path, uses it and frees it. The cache keeps the compiled form of recent
 * short paths, keyed by the path bytes, so that the common command neither
 * parses nor allocates: jsonPathCompile hands out a cached path and
 * jsonPathFree hands it back.
 *
 * Its entries are also the pool that a short path nobody has seen is compiled
 * into, replacing the entry least recently used. An entry that is handed out
 * is never handed out again or replaced until it comes back, so two live
 * compilations of one string are two entries. When every entry is out, and
 * for a path longer than PATH_CACHE_MAX_LEN, the path is compiled on the heap
 * as if there were no cache.
 *
 * A cached path keeps the references it holds on the keys of its names, which
 * is what makes its key IDs still right the next time it is used. So a name
 * used by a cached path stays in the key table after the last document with
 * it has gone, until the entry is replaced or jsonPathCacheClear is called. The
 * engine's parser keeps recent keys the same way.
 *
 * Memory: at most PATH_CACHE_ENTRIES entries of about 1 KB, plus what the
 * step arrays of a path of PATH_CACHE_MAX_LEN bytes can grow to. Main thread
 * only, like everything else here.
 * ------------------------------------------------------------------------- */

#define PATH_CACHE_ENTRIES 256
#define PATH_CACHE_BUCKETS 512
#define PATH_CACHE_MAX_LEN 128

typedef struct cacheEntry {
    jsonPath path; /* First, so that a pooled path and its entry have one address. */
    uint32_t hash, len;
    uint16_t chain; /* The next entry of the bucket, as index + 1. */
    uint16_t index; /* Of this entry. */
    uint8_t linked; /* In a bucket: `key` is valid. */
    uint8_t in_use; /* Handed out and not yet given back. */
    uint8_t recent; /* Used since the eviction hand last passed. */
    char key[PATH_CACHE_MAX_LEN];
} cacheEntry;

static struct {
    cacheEntry *entries[PATH_CACHE_ENTRIES];
    uint16_t heads[PATH_CACHE_BUCKETS];
    uint32_t count, hand;
} pathCache;

static inline uint32_t cacheHash(const char *s, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ n, w;
    for (; n >= 8; s += 8, n -= 8) {
        memcpy(&w, s, 8);
        h = (h ^ w) * 0xD6E8FEB86659FD93ULL;
        h ^= h >> 32;
    }
    for (w = 0; n; n--) w = (w << 8) | (unsigned char)s[n - 1];
    h ^= w;
    /* A full mix at the end: paths often differ only in their last bytes. */
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

static void cacheUnlink(cacheEntry *e) {
    uint16_t *link = &pathCache.heads[e->hash & (PATH_CACHE_BUCKETS - 1)];
    while (*link != e->index + 1) link = &pathCache.entries[*link - 1]->chain;
    *link = e->chain;
    e->linked = 0;
}

/* An entry to compile into: a new one while there is room, else the first
 * entry the hand finds that is neither out nor recently used. NULL if every
 * entry is out. */
static cacheEntry *cacheVictim(void) {
    if (pathCache.count < PATH_CACHE_ENTRIES) {
        cacheEntry *e = pathAlloc(sizeof(*e));
        pathInit(&e->path);
        e->path.pooled = 1;
        e->index = (uint16_t)pathCache.count;
        e->linked = e->in_use = e->recent = 0;
        pathCache.entries[pathCache.count++] = e;
        return e;
    }
    for (uint32_t tries = 0; tries < 2 * PATH_CACHE_ENTRIES; tries++) {
        cacheEntry *e = pathCache.entries[pathCache.hand];
        pathCache.hand = (pathCache.hand + 1) % PATH_CACHE_ENTRIES;
        if (e->in_use) continue;
        if (e->recent) {
            e->recent = 0;
            continue;
        }
        if (e->linked) cacheUnlink(e);
        return e;
    }
    return NULL;
}

void jsonPathCacheClear(void) {
    for (uint32_t i = 0; i < pathCache.count; i++) {
        cacheEntry *e = pathCache.entries[i];
        if (e->in_use) {
            /* Still out: it becomes an ordinary heap path, freed by jsonPathFree. */
            e->path.pooled = 0;
        } else {
            pathReleaseKeys(&e->path);
            pathFreeArrays(&e->path);
            pathFree(e);
        }
    }
    memset(&pathCache, 0, sizeof(pathCache));
}

jsonPath *jsonPathCompile(const char *s, size_t n, jsonPathErr *err) {
    uint32_t hash = 0;
    uint16_t *head = NULL;
    if (err) *err = JSON_PATH_OK;
    if (n <= PATH_CACHE_MAX_LEN) {
        /* A cached key holds no NUL byte, so a hit on all n bytes also shows
         * that the path does not end early. */
        hash = cacheHash(s, n);
        head = &pathCache.heads[hash & (PATH_CACHE_BUCKETS - 1)];
        for (uint32_t i = *head; i; i = pathCache.entries[i - 1]->chain) {
            cacheEntry *e = pathCache.entries[i - 1];
            if (e->hash != hash || e->len != n || e->in_use || memcmp(e->key, s, n) != 0) continue;
            e->in_use = e->recent = 1;
            e->path.v2 = e->path.v2_static; /* jsonPathForceV2 was for one use. */
            return &e->path;
        }
    }
    const char *nul = n ? memchr(s, '\0', n) : NULL;
    if (nul) return jsonPathCompile(s, (size_t)(nul - s), err);
    if (head) {
        cacheEntry *e = cacheVictim();
        if (e) {
            compileInto(&e->path, s, n, 1);
            if (n) memcpy(e->key, s, n);
            e->hash = hash;
            e->len = (uint32_t)n;
            e->chain = *head;
            *head = (uint16_t)(e->index + 1);
            e->linked = e->in_use = 1;
            e->recent = 0; /* A second use earns the second chance, not the first. */
            return &e->path;
        }
    }
    if (n > JSON_PATH_MAX_QUERY) {
        if (err) *err = JSON_PATH_ERR_TOO_LONG;
        return NULL;
    }
    jsonPath *p = pathAlloc(sizeof(*p));
    pathInit(p);
    compileInto(p, s, n, 1);
    return p;
}

void jsonPathFree(jsonPath *p) {
    if (!p) return;
    if (p->pooled) {
        ((cacheEntry *)p)->in_use = 0;
        return;
    }
    pathReleaseKeys(p);
    pathFreeArrays(p);
    pathFree(p);
}

int jsonPathIsV2(const jsonPath *p) {
    return p->v2;
}
void jsonPathForceV2(jsonPath *p) {
    p->v2 = 1;
}
int jsonPathIsRoot(const jsonPath *p) {
    return p->root;
}
int jsonPathIsSingular(const jsonPath *p) {
    return p->singular;
}

/* ----------------------------------------------------------------------------
 * Results
 * ------------------------------------------------------------------------- */

/* Locators live in chunks that never move, so a jsonMatch can point at one. */
typedef struct stepChunk {
    struct stepChunk *next;
    uint32_t used, cap;
    jsonStep steps[];
} stepChunk;

static jsonStep *matchSteps(jsonMatches *m, uint32_t n) {
    stepChunk *c = m->arena;
    if (!c || c->cap - c->used < n) {
        uint32_t cap = c ? c->cap * 2 : 128;
        if (cap > 65536) cap = 65536;
        if (cap < n) cap = n;
        stepChunk *nc = pathAlloc(sizeof(*nc) + (size_t)cap * sizeof(jsonStep));
        nc->next = c;
        nc->used = 0;
        nc->cap = cap;
        m->arena = nc;
        c = nc;
    }
    jsonStep *r = c->steps + c->used;
    c->used += n;
    return r;
}

void jsonMatchesInit(jsonMatches *m) {
    m->items = m->inline_items;
    m->count = 0;
    m->inserts = &m->inline_insert;
    m->insert_count = 0;
    m->cap = JSON_MATCHES_INLINE;
    m->insert_cap = 1;
    m->arena = NULL;
    m->max_depth = 0;
    m->raw_err = JSON_PATH_OK;
    m->v2 = 0;
}

void jsonMatchesFree(jsonMatches *m) {
    if (m->items != m->inline_items) pathFree(m->items);
    if (m->inserts != &m->inline_insert) pathFree(m->inserts);
    for (stepChunk *c = m->arena, *next; c; c = next) {
        next = c->next;
        pathFree(c);
    }
    jsonMatchesInit(m);
}

jsonPathErr jsonMatchesRawErr(const jsonMatches *m) {
    return (jsonPathErr)m->raw_err;
}
int jsonMatchesIsV2(const jsonMatches *m) {
    return m->v2;
}
uint32_t jsonMatchesMaxDepth(const jsonMatches *m) {
    return m->max_depth;
}

static void addInsert(jsonMatches *m, const jsonPath *p, const jsonStep *loc, uint32_t depth, uint32_t name) {
    if (m->insert_count == m->insert_cap) {
        m->inserts = growArray(m->inserts, &m->inline_insert, m->insert_count, &m->insert_cap, 1, sizeof(jsonInsert));
    }
    jsonInsert *in = &m->inserts[m->insert_count++];
    in->parent_depth = depth;
    in->name = p->strs + p->names[name].off;
    in->name_len = p->names[name].len;
    in->parent = m->inline_steps;
    if (depth) {
        jsonStep *l = matchSteps(m, depth);
        memcpy(l, loc, (size_t)depth * sizeof(jsonStep));
        in->parent = l;
    }
}

static int locEqual(const jsonStep *a, const jsonStep *b, uint32_t depth) {
    for (uint32_t i = 0; i < depth; i++) {
        if (a[i].keyid != b[i].keyid) return 0;
        if (!a[i].keyid && a[i].pos != b[i].pos) return 0;
    }
    return 1;
}

static uint64_t locHash(const jsonStep *loc, uint32_t depth) {
    uint64_t h = 1469598103934665603ULL ^ depth;
    for (uint32_t i = 0; i < depth; i++) {
        h = (h ^ (loc[i].keyid ? loc[i].keyid : ~(uint64_t)loc[i].pos)) * 1099511628211ULL;
    }
    return h ^ (h >> 29);
}

/* Drops every element of [from, count) whose locator already occurs earlier
 * in the array, keeping first occurrences and their order. Elements before
 * `from` are known to be distinct and are already in the table. Works on
 * jsonMatch and jsonInsert arrays alike: loc_off and depth_off say where the
 * locator and its length are. Returns the new count. */
static uint32_t uniqueLocs(void *base,
                           size_t stride,
                           size_t loc_off,
                           size_t depth_off,
                           uint32_t from,
                           uint32_t count,
                           uint32_t **tab,
                           uint32_t *tab_cap) {
#define LOC_OF(i) (*(const jsonStep **)((char *)base + (size_t)(i) * stride + loc_off))
#define DEPTH_OF(i) (*(const uint32_t *)((char *)base + (size_t)(i) * stride + depth_off))
    if ((uint64_t)count * 2 > *tab_cap) {
        uint32_t cap = *tab_cap ? *tab_cap : 64;
        while ((uint64_t)count * 2 > cap) cap *= 2;
        pathFree(*tab);
        *tab = pathAlloc((size_t)cap * sizeof(uint32_t));
        memset(*tab, 0, (size_t)cap * sizeof(uint32_t));
        *tab_cap = cap;
        for (uint32_t i = 0; i < from; i++) {
            uint32_t slot = (uint32_t)locHash(LOC_OF(i), DEPTH_OF(i)) & (cap - 1);
            while ((*tab)[slot]) slot = (slot + 1) & (cap - 1);
            (*tab)[slot] = i + 1;
        }
    }
    uint32_t mask = *tab_cap - 1, w = from;
    for (uint32_t j = from; j < count; j++) {
        const jsonStep *loc = LOC_OF(j);
        uint32_t depth = DEPTH_OF(j);
        uint32_t slot = (uint32_t)locHash(loc, depth) & mask, seen = 0;
        while ((*tab)[slot]) {
            uint32_t k = (*tab)[slot] - 1;
            if (DEPTH_OF(k) == depth && locEqual(LOC_OF(k), loc, depth)) {
                seen = 1;
                break;
            }
            slot = (slot + 1) & mask;
        }
        if (seen) continue;
        if (w != j) memcpy((char *)base + (size_t)w * stride, (char *)base + (size_t)j * stride, stride);
        (*tab)[slot] = ++w;
    }
    return w;
#undef LOC_OF
#undef DEPTH_OF
}

/* 1 if each locator of the array comes strictly after the one before it in
 * document order, which shows without a table that no two are the same. */
static int locsAscend(const void *base, size_t stride, size_t loc_off, size_t depth_off, uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        const char *x = (const char *)base + (size_t)(i - 1) * stride, *y = x + stride;
        const jsonStep *a = *(const jsonStep *const *)(x + loc_off), *b = *(const jsonStep *const *)(y + loc_off);
        const uint32_t na = *(const uint32_t *)(x + depth_off), nb = *(const uint32_t *)(y + depth_off);
        uint32_t k = 0, common = na < nb ? na : nb;
        if (a == b) k = common; /* Nested matches share their steps. */
        while (k < common && a[k].pos == b[k].pos) k++;
        if (k < common ? a[k].pos > b[k].pos : na >= nb) return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 * Evaluator state
 * ------------------------------------------------------------------------- */

typedef enum {
    M_READ = 0,
    M_INSERT,
    M_DELETE
} evalMode;

/* Scratch memory for filter results. Allocation is a stack: a frame remembers
 * the position it started at and everything after it goes when it pops. */
typedef struct scratchChunk {
    struct scratchChunk *next;
    size_t used, cap;
} scratchChunk;

typedef struct scratchPos {
    scratchChunk *chunk;
    size_t used;
} scratchPos;

typedef enum {
    FR_WILD = 1,
    FR_SLICE,
    FR_IUNION,
    FR_NUNION,
    FR_FILTER_ARR,
    FR_FILTER_SCALAR,
    FR_DESCENT
} frameKind;

/* One fork in progress: the value being fanned out and where its children
 * continue. `cdepth` is the value of the module's recursion counter while a
 * child runs, and `pextra` is how far the module's path depth is ahead of the
 * locator depth (a filter on a scalar counts a level without descending). */
typedef struct evalFrame {
    uint8_t kind;
    uint8_t started; /* FR_DESCENT: the value itself has been evaluated. */
    uint8_t top;     /* FR_DESCENT: the value the descent began at. */
    uint8_t dup;     /* The value was reached through a repeated union entry. */
    uint8_t norep;   /* FR_IUNION: the entries cannot repeat. */
    uint32_t level;  /* FR_DESCENT: levels below the value the descent began at. */
    uint32_t next;
    uint32_t depth;
    uint32_t pextra;
    uint32_t cdepth;
    jsonRef node;
    scratchPos mark;
    union {
        jsonIter it;
        struct {
            int64_t i, end, step, n;
        } sl;
        struct {
            uint32_t off, i, n, size;
            uint64_t *seen; /* FR_IUNION with many entries: the elements already named. */
        } list;
        struct {
            const uint32_t *v;
            uint32_t i, n;
        } fl;
    } u;
} evalFrame;

typedef struct branch {
    jsonRef node;
    uint32_t depth, pextra, cdepth, step;
    uint32_t dup; /* Reached through a repeated union entry: its work is counted. */
} branch;

/* The value of a "$..." filter operand, kept for the rest of the evaluation.
 * It depends on the document and on the recursion depth the filter runs at,
 * and on nothing else. */
typedef struct operandCache {
    uint8_t known;
    uint8_t rc;
    uint32_t cdepth;
    jsonRef ref;
} operandCache;

typedef struct cmpVal {
    uint8_t kind;
    union {
        int64_t i;
        double d;
    } n;
    const char *s;
    uint32_t slen;
} cmpVal;

#define EVAL_INLINE_LOC 32
#define EVAL_INLINE_FRAMES 6

typedef struct evalState {
    const jsonPath *P;
    jsonMatches *out;
    jsonRef root;
    uint8_t mode, v2, rdflag, may_dup;
    uint8_t distinct;  /* A write mode: every value at most once. */
    uint8_t rep_sized; /* rep_cap already takes the size of the document into account. */
    uint64_t rep_work; /* Work done on values reached through repeated union entries. */
    uint64_t rep_cap;
    void *mark_chunk; /* The locator arena as it was when the first `unique` matches were stored. */
    uint32_t mark_used;
    jsonStep *last; /* The locator stored last, when it is still the newest thing in the arena. */
    uint32_t last_len;
    uint32_t shared;    /* Leading steps of `loc` known to equal those of `last`. */
    operandCache *ops;  /* One slot per filter instruction, allocated when first needed. */
    uint32_t rd;        /* The module lexer's count of ".." tokens read. */
    uint32_t max_depth; /* The module's maxPathDepth. */
    uint32_t unique;    /* Matches before this index are distinct and in `tab`. */
    uint32_t *tab, tab_cap;
    jsonStep *loc; /* Locator of the current branch. */
    uint32_t loc_cap;
    evalFrame *frames;
    uint32_t nframes, cap_frames;
    scratchChunk *sc_head, *sc_cur;
    jsonRef reg_ref; /* Value of the last "$..." filter operand. */
    cmpVal reg;
    jsonStep loc_inline[EVAL_INLINE_LOC];
    evalFrame frames_inline[EVAL_INLINE_FRAMES];
} evalState;

static void stateInit(evalState *E, const jsonPath *p, jsonMatches *out, evalMode mode) {
    E->P = p;
    E->out = out;
    E->mode = (uint8_t)mode;
    E->v2 = E->rdflag = E->may_dup = E->distinct = E->rep_sized = 0;
    E->rep_work = 0;
    E->rep_cap = JSON_PATH_REPEAT_FLOOR;
    E->last = NULL;
    E->last_len = E->shared = 0;
    E->mark_chunk = NULL;
    E->mark_used = 0;
    E->ops = NULL;
    E->rd = E->max_depth = E->unique = 0;
    E->tab = NULL;
    E->tab_cap = 0;
    E->loc = E->loc_inline;
    E->loc_cap = EVAL_INLINE_LOC;
    E->frames = E->frames_inline;
    E->nframes = 0;
    E->cap_frames = EVAL_INLINE_FRAMES;
    E->sc_head = E->sc_cur = NULL;
}

static void stateFree(evalState *E) {
    if (E->loc != E->loc_inline) pathFree(E->loc);
    if (E->frames != E->frames_inline) pathFree(E->frames);
    pathFree(E->tab);
    pathFree(E->ops);
    for (scratchChunk *c = E->sc_head, *next; c; c = next) {
        next = c->next;
        pathFree(c);
    }
}

static inline jsonStep *locSlot(evalState *E, uint32_t depth) {
    if (depth >= E->loc_cap) {
        E->loc = growArray(E->loc, E->loc_inline, E->loc_cap, &E->loc_cap, depth + 1 - E->loc_cap, sizeof(jsonStep));
    }
    if (depth < E->shared) E->shared = depth; /* The step is about to be rewritten. */
    return &E->loc[depth];
}

/* Charges work done on a value that was reached through a repeated union
 * entry. The allowance grows with the document, whose size is only looked up
 * once the fixed part is spent. */
static jsonPathErr chargeRepeat(evalState *E, uint64_t units) {
    E->rep_work += units;
    if (E->rep_work <= E->rep_cap) return JSON_PATH_OK;
    if (!E->rep_sized) {
        E->rep_sized = 1;
        E->rep_cap = JSON_PATH_REPEAT_FLOOR + JSON_PATH_REPEAT_PER_VALUE * jsonRefFields(&E->root);
        if (E->rep_work <= E->rep_cap) return JSON_PATH_OK;
    }
    return JSON_PATH_ERR_TOO_MANY_REPEATS;
}

static inline void noteDepth(evalState *E, uint32_t d) {
    if (d > E->max_depth) E->max_depth = d;
}

static scratchPos scratchMark(const evalState *E) {
    scratchPos pos = {E->sc_cur, E->sc_cur ? E->sc_cur->used : 0};
    return pos;
}

static void scratchRelease(evalState *E, scratchPos pos) {
    if (pos.chunk) {
        E->sc_cur = pos.chunk;
        pos.chunk->used = pos.used;
    } else if (E->sc_head) {
        E->sc_cur = E->sc_head;
        E->sc_head->used = 0;
    }
}

static void *scratchAlloc(evalState *E, size_t n) {
    n = (n + 7) & ~(size_t)7;
    scratchChunk *c = E->sc_cur;
    while (!c || c->cap - c->used < n) {
        scratchChunk *nx = c ? c->next : E->sc_head;
        if (!nx) {
            size_t cap = c ? c->cap * 2 : 4096;
            if (cap < n) cap = n;
            nx = pathAlloc(sizeof(*nx) + cap);
            nx->next = NULL;
            nx->cap = cap;
            if (c) {
                c->next = nx;
            } else {
                E->sc_head = nx;
            }
        }
        nx->used = 0;
        c = nx;
    }
    E->sc_cur = c;
    void *r = (char *)(c + 1) + c->used;
    c->used += n;
    return r;
}

/* The member of object `obj` called by the path's name number `name`. A name
 * that is not in the key table is in no document. */
static inline int memberGet(const jsonPath *P, uint32_t name, const jsonRef *obj, jsonRef *out, jsonStep *step) {
    const uint32_t keyid = P->names[name].keyid;
    return keyid && jsonObjectGetById(obj, keyid, out, step);
}

/* Records a match of the current branch. Its locator is stored once. A match
 * that lies inside the match stored just before it (which is the order a
 * recursive descent produces them in) extends that locator in place and both
 * point at the same steps, so a chain of nested matches costs its length and
 * not the square of it. */
static void addMatch(evalState *E, const jsonRef *ref, uint32_t depth) {
    jsonMatches *m = E->out;
    if (m->count == m->cap) {
        m->items = growArray(m->items, m->inline_items, m->count, &m->cap, 1, sizeof(jsonMatch));
    }
    jsonMatch *it = &m->items[m->count++];
    it->ref = *ref;
    it->depth = depth;
    it->loc = m->inline_steps;
    if (!depth) return;
    stepChunk *c = m->arena;
    const uint32_t have = E->last_len;
    /* `shared` steps are known to be those of the last locator. The steps
     * after them were handed out for rewriting, often with the value they
     * already had, so they are compared. That costs no more than the copy it
     * may save. */
    if (E->last && c && depth >= have && E->last + have == c->steps + c->used && c->cap - c->used >= depth - have &&
        (E->shared >= have ||
         memcmp(E->last + E->shared, E->loc + E->shared, (size_t)(have - E->shared) * sizeof(jsonStep)) == 0)) {
        memcpy(E->last + have, E->loc + have, (size_t)(depth - have) * sizeof(jsonStep));
        c->used += depth - have;
    } else {
        E->last = matchSteps(m, depth);
        memcpy(E->last, E->loc, (size_t)depth * sizeof(jsonStep));
    }
    E->last_len = E->shared = depth;
    it->loc = E->last;
}

static evalFrame *framePush(evalState *E, frameKind kind, const branch *b, uint32_t next) {
    if (E->nframes == E->cap_frames) {
        E->frames = growArray(E->frames, E->frames_inline, E->nframes, &E->cap_frames, 1, sizeof(evalFrame));
    }
    evalFrame *f = &E->frames[E->nframes++];
    f->kind = (uint8_t)kind;
    f->started = f->top = f->norep = 0;
    f->dup = b->dup != 0;
    f->level = 0;
    f->next = next;
    f->depth = b->depth;
    f->pextra = b->pextra;
    f->cdepth = b->cdepth + 2; /* The module's evalMember, then eval. */
    f->node = b->node;
    return f;
}

/* The module's Selector::dedupe: every match so far is made distinct. Only a
 * path with a union, with two recursive descents, or with a recursive descent
 * and a filter can select a value twice, so other paths skip the work. */
static void dedupe(evalState *E) {
    jsonMatches *m = E->out;
    if (!E->may_dup || m->count == E->unique) return;
    const uint32_t before = E->unique;
    m->count = uniqueLocs(m->items, sizeof(jsonMatch), offsetof(jsonMatch, loc), offsetof(jsonMatch, depth), E->unique,
                          m->count, &E->tab, &E->tab_cap);
    E->unique = m->count;
    stepChunk *head = m->arena;
    if (m->count == before && before > 0) {
        /* Everything since the last time was a repeat, as it is when a second
         * recursive descent goes over ground the first one covered. Their
         * locators go back to the arena, so repeats cost time but no memory. */
        while (head && head != E->mark_chunk) {
            stepChunk *older = head->next;
            pathFree(head);
            head = older;
        }
        m->arena = head;
        if (head) head->used = E->mark_used;
        E->last = NULL;
        E->last_len = E->shared = 0;
    }
    E->mark_chunk = head;
    E->mark_used = head ? head->used : 0;
}

/* ----------------------------------------------------------------------------
 * Filters
 * ------------------------------------------------------------------------- */

/* A number is an integer to the module only if it has integer syntax and fits
 * int64. Everything else, including integer text beyond int64, is a double. */
static void numberOf(const jsonRef *r, jsonType t, cmpVal *v) {
    if (t == JSON_INT) {
        v->kind = CV_I64;
        jsonRefInt(r, &v->n.i);
    } else {
        v->kind = CV_DBL;
        v->n.d = jsonRefDouble(r);
    }
}

static inline int cmpResult(int c, uint8_t op) {
    switch (op) {
    case OP_EQ: return c == 0;
    case OP_NE: return c != 0;
    case OP_LT: return c < 0;
    case OP_LE: return c <= 0;
    case OP_GT: return c > 0;
    default: return c >= 0;
    }
}

/* The module's Selector::evalOp. Values of different types never compare,
 * not even with !=. Numbers compare as doubles if either side is one. */
static int evalOp(const jsonRef *v, uint8_t op, const cmpVal *cv) {
    jsonType t = jsonRefType(v);
    switch (cv->kind) {
    case CV_NULL: return t == JSON_NULL && op == OP_EQ;
    case CV_FALSE:
    case CV_TRUE:
        if (t != JSON_FALSE && t != JSON_TRUE) return 0;
        return cmpResult((t == JSON_TRUE) - (cv->kind == CV_TRUE), op);
    case CV_STR: {
        if (t != JSON_STRING) return 0;
        uint32_t len;
        const char *s = jsonRefString(v, &len);
        uint32_t min = len < cv->slen ? len : cv->slen;
        int c = min ? memcmp(s, cv->s, min) : 0;
        if (!c) c = (len > cv->slen) - (len < cv->slen);
        return cmpResult(c, op);
    }
    case CV_I64:
    case CV_DBL: {
        if (t != JSON_INT && t != JSON_DOUBLE && t != JSON_RAWNUM) return 0;
        cmpVal a;
        numberOf(v, t, &a);
        if (a.kind == CV_DBL || cv->kind == CV_DBL) {
            double x = a.kind == CV_DBL ? a.n.d : (double)a.n.i;
            double y = cv->kind == CV_DBL ? cv->n.d : (double)cv->n.i;
            switch (op) {
            case OP_EQ: return x <= y && x >= y;
            case OP_NE: return x < y || x > y;
            case OP_LT: return x < y;
            case OP_LE: return x <= y;
            case OP_GT: return x > y;
            default: return x >= y;
            }
        }
        return cmpResult((a.n.i > cv->n.i) - (a.n.i < cv->n.i), op);
    }
    default: return 0;
    }
}

static void valueOfRef(const jsonRef *r, cmpVal *v) {
    jsonType t = jsonRefType(r);
    switch (t) {
    case JSON_NULL: v->kind = CV_NULL; break;
    case JSON_FALSE: v->kind = CV_FALSE; break;
    case JSON_TRUE: v->kind = CV_TRUE; break;
    case JSON_STRING:
        v->kind = CV_STR;
        v->s = jsonRefString(r, &v->slen);
        break;
    case JSON_INT:
    case JSON_DOUBLE:
    case JSON_RAWNUM: numberOf(r, t, v); break;
    default: v->kind = CV_OTHER; break;
    }
}

static jsonPathErr evalGeneral(evalState *E, uint32_t entry, uint32_t cdepth);

/* A "$..." path used as a filter operand: the module runs a second selector
 * over the whole document. It must select exactly one scalar. Such a path
 * cannot hold a filter, so this is the only level of nesting. */
static jsonPathErr evalOperandPath(evalState *E, const filterIns *in, uint32_t cdepth) {
    evalState N;
    jsonMatches tmp;
    jsonMatchesInit(&tmp);
    stateInit(&N, E->P, &tmp, M_READ);
    N.root = E->root;
    N.may_dup = in->may_dup;
    jsonPathErr rc = evalGeneral(&N, in->v.entry, cdepth + in->vdepth + 1);
    if (rc == JSON_PATH_OK) {
        if (tmp.count != 1) {
            rc = JSON_PATH_ERR_SYNTAX;
        } else {
            E->reg_ref = tmp.items[0].ref;
            valueOfRef(&E->reg_ref, &E->reg);
            if (E->reg.kind == CV_OTHER) rc = JSON_PATH_ERR_SYNTAX;
        }
    }
    stateFree(&N);
    jsonMatchesFree(&tmp);
    return rc;
}

static inline int memberOf(const evalState *E, uint32_t name, const jsonRef *v, jsonRef *out) {
    return jsonRefType(v) == JSON_OBJECT && memberGet(E->P, name, v, out, NULL);
}

/* One comparison against one candidate value. */
static int cmpCandidate(const evalState *E, const filterIns *in, const jsonRef *v, const cmpVal *cv, int self) {
    jsonRef m, e;
    if (self) return evalOp(v, in->op, cv);
    if (!memberOf(E, in->name, v, &m)) return 0;
    if (in->lhs == LHS_MEMBER) return evalOp(&m, in->op, cv);
    if (jsonRefType(&m) != JSON_ARRAY) return 0;
    if (in->lhs == LHS_MEMBER_INDEX) {
        int64_t idx = in->index, n = jsonRefCount(&m);
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n || !jsonArrayGet(&m, (uint32_t)idx, &e)) return 0;
        return evalOp(&e, in->op, cv);
    }
    jsonIter it;
    jsonStep st;
    jsonIterInit(&it, &m);
    while (jsonIterNext(&it, &e, &st, NULL, NULL)) {
        if (evalOp(&e, in->op, cv)) return 1;
    }
    return 0;
}

typedef struct posList {
    uint32_t *v;
    uint32_t n;
    scratchPos end; /* The scratch position just after the list. */
} posList;

/* Runs a filter program against one value. The result is a list of positions:
 * element indexes for an array, and for anything else an empty list or the
 * single position 0. The order is the order the module produces, which after
 * an "||" is not ascending. */
static jsonPathErr runFilter(evalState *E, const pathStep *s, const branch *b, jsonType t, posList *res) {
    const jsonPath *P = E->P;
    const jsonRef *node = &b->node;
    uint32_t count = t == JSON_ARRAY ? jsonRefCount(node) : 1;
    if (b->dup) {
        jsonPathErr rc = chargeRepeat(E, ((uint64_t)count + 1) * s->u.filter.nprog);
        if (rc != JSON_PATH_OK) return rc;
    }
    posList *st = scratchAlloc(E, sizeof(posList) * (s->u.filter.nprog + 1));
    uint32_t sp = 0;
    for (uint32_t pc = 0; pc < s->u.filter.nprog; pc++) {
        const filterIns *in = &P->ins[s->u.filter.prog + pc];
        if (b->cdepth + in->need > JSON_PATH_MAX_NESTING) return JSON_PATH_ERR_TOO_DEEP;
        switch (in->kind) {
        case FI_ERR: return (jsonPathErr)in->err;
        case FI_PATHVAL: {
            /* The operand is the same for every value this step filters:
             * evaluate it once per evaluation, not once per value. */
            if (!E->ops) {
                E->ops = pathAlloc((size_t)P->nins * sizeof(operandCache));
                memset(E->ops, 0, (size_t)P->nins * sizeof(operandCache));
            }
            operandCache *oc = &E->ops[s->u.filter.prog + pc];
            if (!oc->known || oc->cdepth != b->cdepth) {
                oc->rc = (uint8_t)evalOperandPath(E, in, b->cdepth);
                oc->ref = E->reg_ref;
                oc->cdepth = b->cdepth;
                oc->known = 1;
            }
            if (oc->rc != JSON_PATH_OK) return (jsonPathErr)oc->rc;
            E->reg_ref = oc->ref;
            valueOfRef(&E->reg_ref, &E->reg);
            break;
        }
        case FI_ATTR:
        case FI_CMP: {
            uint32_t n = 0;
            uint32_t *v = scratchAlloc(E, sizeof(uint32_t) * (count ? count : 1));
            cmpVal cv;
            jsonRef m;
            int self = in->kind == FI_CMP && in->lhs == LHS_SELF;
            if (in->kind == FI_CMP) {
                if (in->vkind == CV_REG) {
                    cv = E->reg;
                } else {
                    cv.kind = in->vkind;
                    cv.n.i = in->v.i; /* Or the double: the two share their storage. */
                    if (in->vkind == CV_STR) {
                        cv.s = P->strs + in->v.s.off;
                        cv.slen = in->v.s.len;
                    }
                }
            }
            if (t == JSON_ARRAY) {
                jsonIter it;
                jsonRef e;
                jsonStep est;
                uint32_t i = 0;
                jsonIterInit(&it, node);
                for (; jsonIterNext(&it, &e, &est, NULL, NULL); i++) {
                    if (in->kind == FI_ATTR ? memberOf(E, in->name, &e, &m) : cmpCandidate(E, in, &e, &cv, self)) {
                        v[n++] = i;
                    }
                }
            } else if (t == JSON_OBJECT) {
                /* A comparison of an object itself looks up the member with the empty name. */
                if (in->kind == FI_ATTR) {
                    if (memberOf(E, in->name, node, &m)) v[n++] = 0;
                } else if (in->lhs == LHS_SELF || in->lhs == LHS_MEMBER) {
                    if (memberOf(E, in->name, node, &m) && evalOp(&m, in->op, &cv)) v[n++] = 0;
                }
            } else if (in->kind == FI_ATTR) {
                return JSON_PATH_ERR_SYNTAX;
            } else if (self) {
                if (evalOp(node, in->op, &cv)) v[n++] = 0;
            }
            st[sp].v = v;
            st[sp].end = scratchMark(E);
            st[sp++].n = n;
            break;
        }
        default: {
            /* AND keeps the left list's order. OR appends what the right list adds. */
            posList *l = &st[sp - 2], *r = &st[sp - 1];
            uint64_t *bits = scratchAlloc(E, sizeof(uint64_t) * (count / 64 + 1));
            memset(bits, 0, sizeof(uint64_t) * (count / 64 + 1));
            posList *mark = in->kind == FI_AND ? r : l;
            for (uint32_t i = 0; i < mark->n; i++) bits[mark->v[i] >> 6] |= 1ULL << (mark->v[i] & 63);
            if (in->kind == FI_AND) {
                uint32_t w = 0;
                for (uint32_t i = 0; i < l->n; i++) {
                    if (bits[l->v[i] >> 6] >> (l->v[i] & 63) & 1) l->v[w++] = l->v[i];
                }
                l->n = w;
            } else {
                for (uint32_t i = 0; i < r->n; i++) {
                    if (!(bits[r->v[i] >> 6] >> (r->v[i] & 63) & 1)) l->v[l->n++] = r->v[i];
                }
            }
            /* The right list and the bits are done with: the scratch a filter
             * holds is its nesting times the array, not its length times it. */
            scratchRelease(E, l->end);
            sp--;
            break;
        }
        }
    }
    *res = st[0];
    return JSON_PATH_OK;
}

/* ----------------------------------------------------------------------------
 * Evaluator
 * ------------------------------------------------------------------------- */

static inline void pushDescent(evalState *E, const branch *b, uint32_t next, uint32_t cdepth, int top, uint32_t level) {
    evalFrame *f = framePush(E, FR_DESCENT, b, next);
    f->cdepth = cdepth;
    f->top = (uint8_t)top;
    f->level = level;
    jsonIterInit(&f->u.it, &f->node);
}

/* Runs one branch from its step until it selects a value, dies, forks (a
 * frame is pushed) or fails. */
static jsonPathErr runChain(evalState *E, branch *b) {
    const jsonPath *P = E->P;
    for (;;) {
        const pathStep *s = &P->steps[b->step];
        jsonType t = jsonRefType(&b->node);
        switch (s->kind) {
        case ST_END: addMatch(E, &b->node, b->depth); return JSON_PATH_OK;
        case ST_ERR:
            if ((s->flags & SF_NEED_ARRAY) && t != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
            return (jsonPathErr)s->err;
        case ST_DOLLAR:
            E->rd += s->rd;
            if (b->depth != 0) return JSON_PATH_ERR_DOLLAR;
            E->v2 = 1;
            break;
        case ST_MEMBER: {
            jsonRef child;
            E->rd += s->rd;
            if (t != JSON_OBJECT) return E->mode == M_READ ? JSON_PATH_OK : JSON_PATH_ERR_INSERT_ARRAY;
            if (!memberGet(P, s->u.name, &b->node, &child, locSlot(E, b->depth))) {
                if (E->mode == M_INSERT && !E->rdflag) {
                    if (!(s->flags & SF_INSERTABLE)) return JSON_PATH_ERR_NOT_EXIST;
                    addInsert(E->out, P, E->loc, b->depth, s->u.name);
                    noteDepth(E, b->depth + b->pextra + 1);
                }
                return JSON_PATH_OK;
            }
            b->node = child;
            b->depth++;
            noteDepth(E, b->depth + b->pextra);
            break;
        }
        case ST_INDEX: {
            jsonRef child;
            E->rd += s->rd;
            if (t != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
            int64_t idx = s->u.index, n = jsonRefCount(&b->node);
            if (idx < 0) idx += n;
            if (idx < 0 || idx >= n || !jsonArrayGet(&b->node, (uint32_t)idx, &child)) return JSON_PATH_ERR_INDEX_RANGE;
            jsonStep *slot = locSlot(E, b->depth);
            slot->pos = (uint32_t)idx;
            slot->keyid = 0;
            b->node = child;
            b->depth++;
            noteDepth(E, b->depth + b->pextra);
            break;
        }
        case ST_SLICE: {
            if (t != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
            E->rd += s->rd;
            int64_t n = jsonRefCount(&b->node), start = s->u.sl.start, end = (s->flags & SF_END_SIZE) ? n : s->u.sl.end;
            if (start < 0) start += n;
            if (end < 0) end += n;
            start = start < 0 ? 0 : start > n ? n
                                              : start;
            end = end < 0 ? 0 : end > n ? n
                                        : end;
            /* The module keeps the index in an int and adds the 64-bit step
             * to it, so what a step does there is decided by its low 32 bits.
             * When those have the sign of the step, the slice is a slice with
             * that step: 4294967297 is 1 and -4294967298 is -2, and that is
             * kept. When they are zero or have the other sign (2147483648,
             * 4294967295, 4294967296, 2^63 - 1), the module loops for ever or
             * walks the array in the wrong direction. Such a step is taken at
             * its true size, which is past either end of any array, so the
             * slice is its first element alone. The index itself never wraps
             * here, so every slice visits each element at most once and ends
             * within the array's length. */
            const int64_t step = s->u.sl.step;
            const int32_t low = (int32_t)(uint32_t)(uint64_t)step;
            evalFrame *f = framePush(E, FR_SLICE, b, s->next);
            f->u.sl.i = start;
            f->u.sl.end = end;
            f->u.sl.step = step > 0 ? (low > 0 ? low : n + 1) : (low < 0 ? low : -(n + 1));
            f->u.sl.n = n;
            return JSON_PATH_OK;
        }
        case ST_IUNION: {
            if (t != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
            E->rd += s->rd;
            if (b->dup && s->u.list.count > 16) {
                /* Going through a long list is work even when little of it is in the value. */
                jsonPathErr rc = chargeRepeat(E, s->u.list.count);
                if (rc != JSON_PATH_OK) return rc;
            }
            evalFrame *f = framePush(E, FR_IUNION, b, s->next);
            f->u.list.off = s->u.list.off;
            f->u.list.n = s->u.list.count;
            f->u.list.i = 0;
            f->u.list.size = jsonRefCount(&b->node);
            f->u.list.seen = NULL;
            f->norep = (s->flags & SF_NO_REPEAT) != 0;
            if (!f->norep && f->u.list.n > 16 && f->u.list.size) {
                /* Too many entries to compare each with the ones before it. */
                const size_t words = f->u.list.size / 64 + 1;
                f->mark = scratchMark(E);
                f->u.list.seen = scratchAlloc(E, words * sizeof(uint64_t));
                memset(f->u.list.seen, 0, words * sizeof(uint64_t));
            }
            return JSON_PATH_OK;
        }
        case ST_NUNION: {
            E->rd += s->rd;
            if (t != JSON_OBJECT) return E->mode == M_READ ? JSON_PATH_OK : JSON_PATH_ERR_INSERT_ARRAY;
            if (b->dup && s->u.list.count > 16) {
                /* Going through a long list is work even when little of it is in the value. */
                jsonPathErr rc = chargeRepeat(E, s->u.list.count);
                if (rc != JSON_PATH_OK) return rc;
            }
            evalFrame *f = framePush(E, FR_NUNION, b, s->next);
            f->u.list.off = s->u.list.off;
            f->u.list.n = s->u.list.count;
            f->u.list.i = 0;
            f->u.list.seen = NULL;
            return JSON_PATH_OK;
        }
        case ST_WILD: {
            E->rd += s->rd;
            if (t != JSON_OBJECT && t != JSON_ARRAY) return E->v2 ? JSON_PATH_ERR_WILDCARD : JSON_PATH_ERR_SYNTAX;
            evalFrame *f = framePush(E, FR_WILD, b, s->next);
            jsonIterInit(&f->u.it, &f->node);
            return JSON_PATH_OK;
        }
        case ST_DESCENT:
            E->rdflag = 1;
            E->rd += s->rd;
            if (E->rd > JSON_PATH_MAX_DESCENT_TOKENS) return JSON_PATH_ERR_TOO_MANY_DESCENTS;
            if (s->flags & SF_DOT_SEQ) return JSON_PATH_ERR_DOT_SEQUENCE;
            if (t != JSON_OBJECT && t != JSON_ARRAY) {
                dedupe(E);
                return JSON_PATH_OK;
            }
            pushDescent(E, b, s->next, b->cdepth + 1, 1, 0);
            return JSON_PATH_OK;
        case ST_FILTER: {
            if ((s->flags & SF_WILD_KEY) && t != JSON_ARRAY) return JSON_PATH_ERR_SYNTAX;
            scratchPos mark = scratchMark(E);
            posList res = {NULL, 0, {NULL, 0}};
            jsonPathErr rc = runFilter(E, s, b, t, &res);
            if (rc != JSON_PATH_OK) {
                scratchRelease(E, mark);
                return rc;
            }
            E->rd += s->rd;
            if (t == JSON_ARRAY && res.n) {
                evalFrame *f = framePush(E, FR_FILTER_ARR, b, s->next_sup);
                f->mark = mark;
                f->u.fl.v = res.v;
                f->u.fl.n = res.n;
                f->u.fl.i = 0;
                return JSON_PATH_OK;
            }
            scratchRelease(E, mark);
            if (!res.n || t == JSON_ARRAY) return JSON_PATH_OK;
            if (t != JSON_OBJECT) {
                /* A scalar that passes continues as a child of itself. */
                framePush(E, FR_FILTER_SCALAR, b, s->next_sup);
                return JSON_PATH_OK;
            }
            break;
        }
        default: return JSON_PATH_ERR_OTHER;
        }
        b->step = s->next;
    }
}

/* Hands the child of the top frame to the caller as the next branch. */
static jsonPathErr enterChild(evalState *E, const evalFrame *f, branch *b, const jsonRef *child, int *have, int repeat) {
    if (f->cdepth > JSON_PATH_MAX_NESTING) return JSON_PATH_ERR_TOO_DEEP;
    b->node = *child;
    b->depth = f->depth + 1;
    b->pextra = f->pextra;
    b->cdepth = f->cdepth;
    b->step = f->next;
    b->dup = f->dup || repeat;
    noteDepth(E, b->depth + b->pextra);
    *have = 1;
    return b->dup ? chargeRepeat(E, 1) : JSON_PATH_OK;
}

static jsonPathErr enterElement(evalState *E, evalFrame *f, branch *b, uint32_t idx, int *have, int repeat) {
    jsonRef child;
    jsonStep *slot = locSlot(E, f->depth);
    if (!jsonArrayGet(&f->node, idx, &child)) return JSON_PATH_ERR_INDEX_RANGE;
    slot->pos = idx;
    slot->keyid = 0;
    return enterChild(E, f, b, &child, have, repeat);
}

/* Advances the top frame: produces its next child as a branch, or pops it. */
static jsonPathErr advance(evalState *E, branch *b, int *have) {
    evalFrame *f = &E->frames[E->nframes - 1];
    jsonRef child;
    switch (f->kind) {
    case FR_WILD:
        if (jsonIterNext(&f->u.it, &child, locSlot(E, f->depth), NULL, NULL)) return enterChild(E, f, b, &child, have, 0);
        break;
    case FR_SLICE:
        while (f->u.sl.step > 0 ? f->u.sl.i < f->u.sl.end : f->u.sl.i > f->u.sl.end) {
            const int64_t i = f->u.sl.i;
            f->u.sl.i += f->u.sl.step;
            /* A descending slice may start one past the end: an index error, which a slice ignores. */
            if (i < 0 || i >= f->u.sl.n) continue;
            return enterElement(E, f, b, (uint32_t)i, have, 0);
        }
        break;
    case FR_IUNION:
        while (f->u.list.i < f->u.list.n) {
            const int64_t *ints = E->P->ints + f->u.list.off;
            const uint32_t at = f->u.list.i++;
            int64_t idx = ints[at], n = f->u.list.size;
            int repeat = 0;
            if (idx < 0) idx += n;
            /* Out of range is skipped, except on an empty array, where the
             * module's unsigned arithmetic lets it through to an index error. */
            if (idx < 0 || (n > 0 && idx > n - 1)) continue;
            if (idx >= n) return JSON_PATH_ERR_INDEX_RANGE;
            /* Has an earlier entry named this element? A read evaluates it
             * again, as the module does, and counts the work. A write must
             * not see a value twice and skips it here. */
            if (f->u.list.seen) {
                repeat = f->u.list.seen[idx >> 6] >> (idx & 63) & 1;
                f->u.list.seen[idx >> 6] |= 1ULL << (idx & 63);
            } else if (!f->norep) {
                for (uint32_t k = 0; k < at && !repeat; k++) repeat = (ints[k] < 0 ? ints[k] + n : ints[k]) == idx;
            }
            if (repeat && E->distinct) continue;
            return enterElement(E, f, b, (uint32_t)idx, have, repeat);
        }
        if (f->u.list.seen) scratchRelease(E, f->mark);
        break;
    case FR_NUNION:
        while (f->u.list.i < f->u.list.n) {
            uint32_t name = f->u.list.off + f->u.list.i++;
            int repeat = E->P->names[name].repeat != 0;
            if (repeat && E->distinct) continue;
            if (memberGet(E->P, name, &f->node, &child, locSlot(E, f->depth))) {
                return enterChild(E, f, b, &child, have, repeat);
            }
        }
        break;
    case FR_FILTER_ARR:
        if (f->u.fl.i < f->u.fl.n) return enterElement(E, f, b, f->u.fl.v[f->u.fl.i++], have, 0);
        scratchRelease(E, f->mark);
        break;
    case FR_FILTER_SCALAR:
        if (!f->started) {
            f->started = 1;
            if (f->cdepth > JSON_PATH_MAX_NESTING) return JSON_PATH_ERR_TOO_DEEP;
            b->node = f->node;
            b->depth = f->depth;
            b->pextra = f->pextra + 1;
            b->cdepth = f->cdepth;
            b->step = f->next;
            b->dup = f->dup;
            noteDepth(E, b->depth + b->pextra);
            *have = 1;
            return f->dup ? chargeRepeat(E, 1) : JSON_PATH_OK;
        }
        break;
    case FR_DESCENT:
        /* The module's recursiveSearch: the value itself first, then its
         * children in document order. Scalars count for the path depth. */
        if (!f->started) {
            f->started = 1;
            if (f->cdepth > JSON_PATH_MAX_NESTING) return JSON_PATH_ERR_TOO_DEEP;
            b->node = f->node;
            b->depth = f->depth;
            b->pextra = f->pextra;
            b->cdepth = f->cdepth;
            b->step = f->next;
            b->dup = f->dup;
            *have = 1;
            return f->dup ? chargeRepeat(E, 1) : JSON_PATH_OK;
        }
        if (jsonIterNext(&f->u.it, &child, locSlot(E, f->depth), NULL, NULL)) {
            jsonType t = jsonRefType(&child);
            noteDepth(E, f->depth + 1 + f->pextra);
            if (f->dup) {
                jsonPathErr rc = chargeRepeat(E, 1);
                if (rc != JSON_PATH_OK) return rc;
            }
            if (t == JSON_OBJECT || t == JSON_ARRAY) {
                if (f->level >= JSON_PATH_MAX_DESCENT_DEPTH) return JSON_PATH_ERR_TOO_DEEP;
                branch cb = {child, f->depth + 1, f->pextra, 0, 0, f->dup};
                pushDescent(E, &cb, f->next, f->cdepth, 0, f->level + 1);
            }
            return JSON_PATH_OK;
        }
        if (f->top) {
            E->nframes--;
            dedupe(E);
            return JSON_PATH_OK;
        }
        break;
    default: break;
    }
    E->nframes--;
    return JSON_PATH_OK;
}

/* Evaluates the program at `entry` from the root. Returns what the module's
 * Selector::eval returns: an error that is not a syntax error has only ended
 * the branch that met it, unless nothing above that branch absorbed it. */
static jsonPathErr evalGeneral(evalState *E, uint32_t entry, uint32_t cdepth) {
    branch b = {E->root, 0, 0, cdepth, entry, 0};
    int have = 1;
    if (cdepth > JSON_PATH_MAX_NESTING) return JSON_PATH_ERR_TOO_DEEP;
    for (;;) {
        jsonPathErr rc;
        if (have) {
            have = 0;
            rc = runChain(E, &b);
        } else {
            if (!E->nframes) return JSON_PATH_OK;
            rc = advance(E, &b, &have);
        }
        if (rc != JSON_PATH_OK) {
            if (jsonPathErrIsSyntax(rc)) return rc;
            /* Every fork absorbs the error and goes on to its next child,
             * except a union of indexes, which gives up and passes it on. */
            while (E->nframes && E->frames[E->nframes - 1].kind == FR_IUNION) {
                evalFrame *u = &E->frames[--E->nframes];
                if (u->u.list.seen) scratchRelease(E, u->mark);
            }
            if (!E->nframes) return rc;
            have = 0;
        }
    }
}

/* A path of member and index steps: no state, no allocation unless the path
 * is longer than the inline locator. Each step is one engine call when it
 * finds its child: the engine's lookups already answer "not an object" and
 * "not an array" with a miss, so the type is only asked for after a miss. */
static jsonPathErr evalSingular(const jsonPath *p, jsonDoc d, evalMode mode, jsonMatches *out) {
    jsonRef node, child;
    jsonStep *loc = out->inline_steps;
    uint32_t depth = 0;
    const pathStep *steps = p->steps, *s = &steps[p->entry];
    const pathName *names = p->names;
    if (p->nsing > JSON_MATCHES_INLINE_STEPS) loc = matchSteps(out, p->nsing);
    jsonDocRoot(d, &node);
    if (s->kind == ST_DOLLAR) s = &steps[s->next];
    for (; s->kind != ST_END; s = &steps[s->next], node = child, depth++) {
        if (s->kind == ST_MEMBER) {
            const pathName *nm = &names[s->u.name];
            if (nm->keyid && jsonObjectGetById(&node, nm->keyid, &child, &loc[depth])) continue;
            out->max_depth = depth;
            if (jsonRefType(&node) != JSON_OBJECT) return mode == M_READ ? JSON_PATH_OK : JSON_PATH_ERR_INSERT_ARRAY;
            if (mode != M_INSERT) return JSON_PATH_OK;
            if (!(s->flags & SF_INSERTABLE)) return JSON_PATH_ERR_NOT_EXIST;
            jsonInsert *in = &out->inserts[0];
            in->parent = loc;
            in->parent_depth = depth;
            in->name = p->strs + nm->off;
            in->name_len = nm->len;
            out->insert_count = 1;
            out->max_depth = depth + 1;
            return JSON_PATH_OK;
        }
        int64_t idx = s->u.index;
        if (idx < 0 || idx > UINT32_MAX || !jsonArrayGet(&node, (uint32_t)idx, &child)) {
            out->max_depth = depth;
            if (jsonRefType(&node) != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
            if (idx < 0) idx += jsonRefCount(&node);
            if (idx < 0 || idx > UINT32_MAX || !jsonArrayGet(&node, (uint32_t)idx, &child)) return JSON_PATH_ERR_INDEX_RANGE;
        }
        loc[depth].pos = (uint32_t)idx;
        loc[depth].keyid = 0;
    }
    out->items[0].ref = node;
    out->items[0].loc = loc;
    out->items[0].depth = depth;
    out->count = 1;
    out->max_depth = depth;
    return JSON_PATH_OK;
}

jsonPathErr jsonPathEval(const jsonPath *p, jsonDoc d, jsonPathMode mode, jsonMatches *out) {
    if (out->arena || out->items != out->inline_items || out->inserts != &out->inline_insert) jsonMatchesFree(out);
    out->count = out->insert_count = 0;
    out->max_depth = 0;
    out->raw_err = JSON_PATH_OK;
    out->v2 = p->v2;
    if (!d) return JSON_PATH_OK;
    if (p->unresolved) {
        /* Names that were in no document at the last look may be in one by now. */
        jsonPath *w = (jsonPath *)p;
        for (uint32_t i = 0; i < w->nnames; i++) {
            if (w->names[i].keyid) continue;
            w->names[i].keyid = jsonKeyFind(w->strs + w->names[i].off, w->names[i].len);
            if (w->names[i].keyid) w->unresolved--;
        }
    }
    evalMode em = mode == JSON_PATH_INSERT ? M_INSERT : mode == JSON_PATH_DELETE ? M_DELETE
                                                                                 : M_READ;
    jsonPathErr raw;
    if (p->singular) {
        raw = evalSingular(p, d, em, out);
    } else {
        evalState E;
        stateInit(&E, p, out, em);
        jsonDocRoot(d, &E.root);
        E.v2 = p->v2;
        E.rd = p->rd_init;
        E.may_dup = p->may_dup;
        E.distinct = mode != JSON_PATH_READ;
        raw = evalGeneral(&E, p->entry, 1);
        if (mode != JSON_PATH_READ) {
            /* A write must never be handed one value twice. Which paths can
             * select a value twice is known (see dedupe), but the guarantee
             * does not rest on that: any result that is not plainly in
             * document order is made distinct as well. */
            if (out->count > 1 && (E.may_dup || !locsAscend(out->items, sizeof(jsonMatch), offsetof(jsonMatch, loc),
                                                            offsetof(jsonMatch, depth), out->count))) {
                E.may_dup = 1;
                dedupe(&E);
            }
            if (out->insert_count > 1 && (E.may_dup || !locsAscend(out->inserts, sizeof(jsonInsert), offsetof(jsonInsert, parent),
                                                                   offsetof(jsonInsert, parent_depth), out->insert_count))) {
                uint32_t *tab = NULL, cap = 0;
                out->insert_count = uniqueLocs(out->inserts, sizeof(jsonInsert), offsetof(jsonInsert, parent),
                                               offsetof(jsonInsert, parent_depth), 0, out->insert_count, &tab, &cap);
                pathFree(tab);
            }
        }
        out->max_depth = E.max_depth;
        out->v2 = E.v2;
        stateFree(&E);
    }
    out->raw_err = (uint8_t)raw;
    if (raw != JSON_PATH_OK &&
        (jsonPathErrIsSyntax(raw) || !out->v2 || mode == JSON_PATH_INSERT || mode == JSON_PATH_DELETE)) {
        out->count = out->insert_count = 0;
        return raw;
    }
    if (!out->v2 && out->count == 0 && mode != JSON_PATH_INSERT) return JSON_PATH_ERR_NOT_EXIST;
    return JSON_PATH_OK;
}

/* 1 if match a has to be applied before match b. */
static int applyBefore(const jsonMatches *m, uint32_t a, uint32_t b) {
    const jsonMatch *x = &m->items[a], *y = &m->items[b];
    if (x->depth != y->depth) return x->depth > y->depth;
    for (uint32_t i = 0; i < x->depth; i++) {
        if (x->loc[i].pos != y->loc[i].pos) return x->loc[i].pos > y->loc[i].pos;
    }
    return a < b;
}

static void applySift(const jsonMatches *m, uint32_t *order, uint32_t root, uint32_t end) {
    for (;;) {
        uint32_t kid = 2 * root + 1;
        if (kid >= end) return;
        if (kid + 1 < end && applyBefore(m, order[kid], order[kid + 1])) kid++;
        if (!applyBefore(m, order[root], order[kid])) return;
        uint32_t tmp = order[root];
        order[root] = order[kid];
        order[kid] = tmp;
        root = kid;
    }
}

/* Heap sort: no allocation, and the order is total so stability is moot. */
void jsonMatchesApplyOrder(const jsonMatches *m, uint32_t *order) {
    uint32_t n = m->count;
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = n / 2; i-- > 0;) applySift(m, order, i, n);
    for (uint32_t end = n; end-- > 1;) {
        uint32_t tmp = order[0];
        order[0] = order[end];
        order[end] = tmp;
        applySift(m, order, 0, end);
    }
}

int jsonMatchIsAncestor(const jsonMatch *a, const jsonMatch *b) {
    return a->depth < b->depth && locEqual(a->loc, b->loc, a->depth);
}

#ifdef JSON_PATH_DEBUG
/* ----------------------------------------------------------------------------
 * A text form of a compiled path, for the tests
 * ------------------------------------------------------------------------- */

#include <stdio.h>

typedef struct dumpBuf {
    char *buf;
    size_t len, cap;
} dumpBuf;

static void dumpf(dumpBuf *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void dumpf(dumpBuf *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (d->len < d->cap) {
        int n = vsnprintf(d->buf + d->len, d->cap - d->len, fmt, ap);
        if (n > 0) d->len += (size_t)n < d->cap - d->len ? (size_t)n : d->cap - d->len - 1;
    }
    va_end(ap);
}

static void dumpBytes(dumpBuf *d, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20) {
            dumpf(d, "\\x%02x", (unsigned char)s[i]);
        } else {
            dumpf(d, "%c", s[i]);
        }
    }
}

static void dumpName(dumpBuf *d, const jsonPath *p, uint32_t name) {
    dumpBytes(d, p->strs + p->names[name].off, p->names[name].len);
}

static void dumpChain(dumpBuf *d, const jsonPath *p, uint32_t si, int depth);

static void dumpValue(dumpBuf *d, const jsonPath *p, const filterIns *in) {
    switch (in->vkind) {
    case CV_NULL: dumpf(d, "null"); break;
    case CV_FALSE: dumpf(d, "false"); break;
    case CV_TRUE: dumpf(d, "true"); break;
    case CV_I64: dumpf(d, "%lld", (long long)in->v.i); break;
    case CV_DBL: dumpf(d, "%.17g", in->v.d); break;
    case CV_STR:
        dumpf(d, "'");
        dumpBytes(d, p->strs + in->v.s.off, in->v.s.len);
        dumpf(d, "'");
        break;
    default: dumpf(d, "REG"); break;
    }
}

static void dumpChain(dumpBuf *d, const jsonPath *p, uint32_t si, int depth) {
    static const char *ops[] = {"==", "!=", "<", "<=", ">", ">="};
    for (int guard = 0; si != NO_STEP && guard < 4096 && d->len + 64 < d->cap; guard++) {
        const pathStep *s = &p->steps[si];
        if (guard) dumpf(d, " ");
        switch (s->kind) {
        case ST_END: dumpf(d, "END"); return;
        case ST_ERR: dumpf(d, "ERR(%d%s)", s->err, (s->flags & SF_NEED_ARRAY) ? ",arr" : ""); return;
        case ST_DOLLAR: dumpf(d, "$"); break;
        case ST_MEMBER:
            dumpf(d, "m:");
            dumpName(d, p, s->u.name);
            if (s->flags & SF_INSERTABLE) dumpf(d, "!");
            break;
        case ST_INDEX: dumpf(d, "i:%lld", (long long)s->u.index); break;
        case ST_SLICE:
            dumpf(d, "s:%lld:", (long long)s->u.sl.start);
            if (s->flags & SF_END_SIZE) {
                dumpf(d, "E");
            } else {
                dumpf(d, "%lld", (long long)s->u.sl.end);
            }
            dumpf(d, ":%lld", (long long)s->u.sl.step);
            break;
        case ST_IUNION:
            dumpf(d, "u:");
            for (uint32_t i = 0; i < s->u.list.count; i++) {
                dumpf(d, "%s%lld", i ? "," : "", (long long)p->ints[s->u.list.off + i]);
            }
            break;
        case ST_NUNION:
            dumpf(d, "n:");
            for (uint32_t i = 0; i < s->u.list.count; i++) {
                if (i) dumpf(d, ",");
                dumpName(d, p, s->u.list.off + i);
            }
            break;
        case ST_WILD: dumpf(d, "*"); break;
        case ST_DESCENT: dumpf(d, "..%s", (s->flags & SF_DOT_SEQ) ? "!" : ""); break;
        case ST_FILTER:
            dumpf(d, "f%s{", (s->flags & SF_WILD_KEY) ? "w" : "");
            for (uint32_t i = 0; i < s->u.filter.nprog; i++) {
                const filterIns *in = &p->ins[s->u.filter.prog + i];
                if (i) dumpf(d, " ");
                switch (in->kind) {
                case FI_ERR: dumpf(d, "err(%d)", in->err); break;
                case FI_PATHVAL:
                    dumpf(d, "path(");
                    if (depth < 4) dumpChain(d, p, in->v.entry, depth + 1);
                    dumpf(d, ")");
                    break;
                case FI_ATTR:
                    dumpf(d, "attr(");
                    dumpName(d, p, in->name);
                    dumpf(d, ")");
                    break;
                case FI_CMP:
                    dumpf(d, "cmp(");
                    if (in->lhs == LHS_SELF) {
                        dumpf(d, "@");
                    } else {
                        dumpName(d, p, in->name);
                        if (in->lhs == LHS_MEMBER_INDEX) dumpf(d, "[%lld]", (long long)in->index);
                        if (in->lhs == LHS_CONTAINS) dumpf(d, "[?]");
                    }
                    dumpf(d, "%s", ops[in->op]);
                    dumpValue(d, p, in);
                    dumpf(d, ")");
                    break;
                case FI_AND: dumpf(d, "and"); break;
                default: dumpf(d, "or"); break;
                }
                dumpf(d, "/%u", in->need);
            }
            dumpf(d, "}");
            if (s->flags & SF_TERMINAL) return;
            if (s->next_sup != s->next) {
                dumpf(d, "<");
                if (depth < 4) dumpChain(d, p, s->next_sup, depth + 1);
                dumpf(d, ">");
            }
            break;
        default: dumpf(d, "?"); break;
        }
        if (s->rd) dumpf(d, "+%u", s->rd);
        si = s->next;
    }
}

/* Compiles with the full compiler only and outside the cache, so that a test
 * can compare its program with the one jsonPathCompile produces. */
jsonPath *jsonPathDebugCompileFull(const char *s, size_t n) {
    const char *nul = n ? memchr(s, '\0', n) : NULL;
    if (nul) n = (size_t)(nul - s);
    if (n > JSON_PATH_MAX_QUERY) return NULL;
    jsonPath *p = pathAlloc(sizeof(*p));
    pathInit(p);
    compileInto(p, s, n, 0);
    return p;
}

/* Writes the program as text: steps in order, separated by spaces. */
size_t jsonPathDebugDump(const jsonPath *p, char *buf, size_t cap) {
    dumpBuf d = {buf, 0, cap};
    if (cap) buf[0] = '\0';
    if (p->rd_init) dumpf(&d, "+%u ", p->rd_init);
    dumpChain(&d, p, p->entry, 0);
    return d.len;
}
#endif /* JSON_PATH_DEBUG */
