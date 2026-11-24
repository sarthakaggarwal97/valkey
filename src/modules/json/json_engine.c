/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_engine.c -- storage engine of the built-in JSON module.
 *
 * The byte formats (the packed format, spine words and heap nodes) are
 * described at the top of json_engine_format.h. This file is laid out as:
 *
 *   1. Engine state and memory
 *   2. Key table: member names interned to 32-bit IDs, reference counted
 *   3. Tapes: the flat intermediate form every value passes through
 *   4. Tokeniser: JSON text to tape
 *   5. Emitter: tape to document. The only code that decides packed or spine
 *   6. References and iteration: reading a document in place
 *   7. Document to tape: copying a value out of a document
 *   8. Serialiser
 *   9. Whole-value queries: equality, depth, fields, memory
 *  10. Whole-document operations
 *  11. Changing a document: in place, splice, re-emit, and spine slots
 *  12. Active defrag
 *
 * Nothing here recurses on document depth. Walks keep an explicit stack that
 * starts in the caller's frame and moves to the heap when it has to.
 *
 * Threads. All of it runs on the main thread except jsonDocFree, which may
 * run on a lazy-free thread, and jsonSerialize, which may run on a save
 * thread. Those two touch no engine state except the key table, in the ways
 * section 2 allows. */

#include "json_engine.h"
#include "json_engine_format.h"
#include "json_engine_stats.h"
#include "json_num.h"

#include <float.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef JSON_ENGINE_DEBUG
#include <assert.h>
#define jsonAssert(x) assert(x)
#else
#define jsonAssert(x) ((void)0)
#endif

/* ============================================================================
 * 1. Engine state and memory
 * ========================================================================= */

#define CAP_BYTES_DEFAULT 4096
#define CAP_BYTES_MAX PK_LEN16_MAX
#define CAP_ENTRIES_DEFAULT 128
#define CAP_ENTRIES_MAX PK_LEN16_MAX

/* Key table geometry. Segment s holds 64 << s slots, so 26 segments cover
 * every ID below 2^32 - 64. */
#define KT_SEG0_BITS 6
#define KT_SEG0_SIZE ((uint32_t)1 << KT_SEG0_BITS)
#define KT_MAX_SEGS 26
#define KT_FREE SIZE_MAX                    /* Count of a slot that holds no key. */
#define KT_HT_MIN 64                        /* Smallest hash index. */
#define KT_LEN_MASK 0x7FFFFFFFu             /* keyInfo.len_flags: the length. */
#define KT_NOESC 0x80000000u                /* keyInfo.len_flags: no escaping needed. */
#define KT_DEFAULT_K0 0x0706050403020100ull /* Hash key until jsonEngineSetHashSeed. */
#define KT_DEFAULT_K1 0x0f0e0d0c0b0a0908ull
#define KT_SWEEP_BUDGET 4096 /* Slots examined per jsonKeyTableSweep call. */

#define KEY_CACHE_SLOTS 256
#define KEY_CACHE_MAX_KEYS 256 /* Keys remembered per object. */
#define TAPE_POOL_MAX 4

/* A key's name and, in the same allocation, its serialised form: the name
 * quoted and escaped as the writer escapes it, then ':'. A name that needs
 * no escaping is stored once, inside its serialised form ("name":), and
 * `bytes` points after the opening quote. Otherwise the name comes first and
 * the serialised form after it. */
typedef struct keyInfo {
    const uint8_t *bytes; /* The name. */
    uint32_t len_flags;
    uint32_t hash;
    uint32_t ser_len; /* Bytes of the serialised form. */
} keyInfo;

typedef struct keySeg {
    _Atomic size_t *cnt; /* Reference counts. KT_FREE marks a free slot. */
    keyInfo *info;       /* Read-only while the count is above zero. */
    uint64_t *freemap;   /* One bit per slot, set when the slot is free. */
    uint32_t nfree;
    uint32_t hint; /* No free bit in the freemap words below this one. */
} keySeg;

/* The keys of the last object parsed at one nesting position (design 6.1).
 * The cache holds no reference on them: an ID it remembers may have been
 * freed or given to another name since, so it is checked before it is used. */
typedef struct keyCacheSlot {
    uint32_t pos;
    uint32_t gen; /* 0 when the slot is empty. */
    uint32_t n;
    uint32_t cap;
    uint32_t *ids;
} keyCacheSlot;

static struct {
    jsonAllocator alloc;
    pthread_t main_thread;
    uint32_t cap_bytes;
    uint32_t cap_entries;
    uint64_t hash_k0, hash_k1; /* SipHash key of the key table. */

    /* Key table. Everything but the counts is main-thread only. */
    keySeg seg[KT_MAX_SEGS];
    uint32_t kt_nseg;
    uint32_t kt_first_free; /* No free slot in the segments below this one. */
    uint32_t kt_live;       /* Slots that hold a key, including those at count zero. */
    uint32_t *kt_ht;        /* Open addressing over key IDs, 0 = empty. */
    uint32_t kt_htcap;
    size_t kt_bytes;
    _Atomic size_t kt_live_bytes; /* Name storage of the keys that are referenced. Updated from any thread. */
    atomic_int kt_dirty;          /* A count reached zero off the main thread. */
    int kt_sweeping;
    uint32_t kt_sweep_pos;

    keyCacheSlot kcache[KEY_CACHE_SLOTS];
    uint32_t kcache_gen;

    jsonVal *pool[TAPE_POOL_MAX];
    int npool;

    uint64_t writes[JSON_WRITE_KINDS]; /* Changes made, by the primitive used. */
} E;

static inline void *engAlloc(size_t n) {
    return E.alloc.alloc(n);
}

static inline void engFree(void *p) {
    if (p) E.alloc.free(p);
}

/* Grows (or creates) a scratch array. */
static void *engGrow(void *p, size_t n) {
    return p ? E.alloc.realloc(p, n) : E.alloc.alloc(n);
}

/* Sizes. Where size_t has 64 bits nothing the engine counts brings a size
 * near its limit. Where it has 32 bits, a count times an element size or a
 * buffer that keeps doubling can pass it, and wrapping around would give a
 * small allocation that is then written as a large one. Such a size is more
 * than the address space, so the engine stops there, as it would when the
 * allocation failed. Sizes are computed in 64 bits and go through engSize,
 * which is a plain conversion on a 64-bit target. sizeSat is for estimates,
 * which say "too much" instead. */
#if SIZE_MAX < UINT64_MAX
static inline size_t engSize(uint64_t n) {
    if (JSON_UNLIKELY(n > SIZE_MAX)) __builtin_trap();
    return (size_t)n;
}
static inline size_t sizeSat(uint64_t n) {
    return n > SIZE_MAX ? SIZE_MAX : (size_t)n;
}
#else
static inline size_t engSize(uint64_t n) {
    return (size_t)n;
}
static inline size_t sizeSat(uint64_t n) {
    return (size_t)n;
}
#endif

/* A computed double rounded to 64 bits. Where doubles are evaluated in a
 * wider format (x87: FLT_EVAL_METHOD 2) a value can keep excess precision
 * in a register, and then two numbers that are equal as doubles compare
 * unequal: (double)9007199254740993 would differ from 9007199254740992.0.
 * Going through memory rounds it. Nothing to do elsewhere. */
#if defined(FLT_EVAL_METHOD) && (FLT_EVAL_METHOD == 0 || FLT_EVAL_METHOD == 1)
#define JSON_DOUBLE_EXACT 1
static inline double doubleRound(double d) {
    return d;
}
#else
#define JSON_DOUBLE_EXACT 0
static inline double doubleRound(double d) {
    volatile double v = d;
    return v;
}
#endif

static int engOnMainThread(void) {
    return pthread_equal(pthread_self(), E.main_thread);
}

/* A stack of fixed-size frames that starts in caller memory. Used by every
 * walk so that no walk recurses and none shares scratch state. */
typedef struct walkStack {
    void *base;
    uint32_t n, cap;
    uint32_t frame_size;
    int heap;
} walkStack;

static inline void walkStackInit(walkStack *s, void *inline_frames, uint32_t cap, uint32_t frame_size) {
    s->base = inline_frames;
    s->n = 0;
    s->cap = cap;
    s->frame_size = frame_size;
    s->heap = 0;
}

static void walkStackGrow(walkStack *s) {
    const size_t bytes = (size_t)s->cap * s->frame_size; /* Allocated already: it fits. */
    void *nb = engAlloc(engSize((uint64_t)bytes * 2));
    memcpy(nb, s->base, bytes);
    if (s->heap) engFree(s->base);
    s->base = nb;
    s->cap *= 2;
    s->heap = 1;
}

static inline void *walkStackPush(walkStack *s) {
    if (JSON_UNLIKELY(s->n == s->cap)) walkStackGrow(s);
    return (uint8_t *)s->base + (size_t)s->n++ * s->frame_size;
}

static inline void *walkStackTop(const walkStack *s) {
    return (uint8_t *)s->base + (size_t)(s->n - 1) * s->frame_size;
}

static inline void walkStackFree(walkStack *s) {
    if (s->heap) engFree(s->base);
}

/* Copy without a libc call for the short lengths that dominate JSON strings.
 * Never reads or writes outside [0, n): up to 64 bytes go as two fixed-size
 * moves that overlap inside the range. A compiler that cannot see how small
 * n is assumes it can be anything up to 64, so a stack buffer whose content
 * is copied with a length the compiler cannot bound is given
 * COPY_SMALL_SPAN bytes. */
#define COPY_SMALL_SPAN 64
static JSON_INLINE void copySmall(void *dv, const void *sv, size_t n) {
    uint8_t *d = dv;
    const uint8_t *s = sv;
    if (n >= 16) {
        if (n <= 32) {
            memcpy(d, s, 16);
            memcpy(d + n - 16, s + n - 16, 16);
        } else if (n <= 64) {
            memcpy(d, s, 32);
            memcpy(d + n - 32, s + n - 32, 32);
        } else {
            memcpy(d, s, n);
        }
    } else if (n >= 8) {
        memcpy(d, s, 8);
        memcpy(d + n - 8, s + n - 8, 8);
    } else if (n >= 4) {
        memcpy(d, s, 4);
        memcpy(d + n - 4, s + n - 4, 4);
    } else if (n) {
        d[0] = s[0];
        d[n >> 1] = s[n >> 1];
        d[n - 1] = s[n - 1];
    }
}

/* Equality of two n-byte strings without a libc call for n <= 16. */
static inline int equalSmall(const void *av, const void *bv, size_t n) {
    const uint8_t *a = av, *b = bv;
    if (n > 16) return memcmp(a, b, n) == 0;
    if (n >= 8) return ((loadU64(a) ^ loadU64(b)) | (loadU64(a + n - 8) ^ loadU64(b + n - 8))) == 0;
    if (n >= 4) return ((loadU32(a) ^ loadU32(b)) | (loadU32(a + n - 4) ^ loadU32(b + n - 4))) == 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* 1 if a string needs no escaping when written as JSON: no control byte,
 * quote or backslash (see serPutString for the writer's rules). */
static int bytesNoEscape(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] == '"' || s[i] == '\\') return 0;
    }
    return 1;
}

/* What the module's writer does with a byte: 0 = copy it, 'u' = \u00XX,
 * anything else = a backslash and that character. Control characters, the
 * quote and the backslash are escaped. '/', 0x7F and bytes of 0x80 and above
 * (UTF-8, valid or not) are copied. */
static inline char serEscapeOf(uint8_t c) {
    if (c >= 0x20) return (c == '"' || c == '\\') ? (char)c : 0;
    switch (c) {
    case '\b': return 'b';
    case '\t': return 't';
    case '\n': return 'n';
    case '\f': return 'f';
    case '\r': return 'r';
    default: return 'u';
    }
}

/* Writes n bytes escaped as the module's writer escapes them, at most 6
 * bytes each, and returns the end. */
static char *escapeBytes(char *o, const uint8_t *s, size_t n) {
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = s[i];
        const char esc = serEscapeOf(c);
        if (JSON_LIKELY(!esc)) {
            *o++ = (char)c;
            continue;
        }
        *o++ = '\\';
        *o++ = esc;
        if (esc == 'u') {
            *o++ = '0';
            *o++ = '0';
            *o++ = HEX[c >> 4];
            *o++ = HEX[c & 15];
        }
    }
    return o;
}

static uint64_t escapedLength(const uint8_t *s, size_t n) {
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        const char esc = serEscapeOf(s[i]);
        total += !esc ? 1 : (esc == 'u' ? 6 : 2);
    }
    return total;
}

void jsonEngineInit(const jsonAllocator *a) {
    memset(&E, 0, sizeof(E));
    E.alloc = *a;
    E.main_thread = pthread_self();
    E.cap_bytes = CAP_BYTES_DEFAULT;
    E.cap_entries = CAP_ENTRIES_DEFAULT;
    E.hash_k0 = KT_DEFAULT_K0;
    E.hash_k1 = KT_DEFAULT_K1;
    atomic_init(&E.kt_dirty, 0);
    atomic_init(&E.kt_live_bytes, 0);
}

const jsonAllocator *jsonEngineAllocator(void) {
    return &E.alloc;
}

void jsonEngineSetHashSeed(const uint8_t seed[16]) {
    /* SipHash reads its key as two little-endian words. */
    E.hash_k0 = loadU64LE(seed);
    E.hash_k1 = loadU64LE(seed + 8);
}

/* A cap of 0 is allowed: nothing is packed then, not even an empty container. */
void jsonEngineSetCaps(uint32_t max_packed_bytes, uint32_t max_packed_entries) {
    if (max_packed_bytes > CAP_BYTES_MAX) max_packed_bytes = CAP_BYTES_MAX;
    if (max_packed_entries > CAP_ENTRIES_MAX) max_packed_entries = CAP_ENTRIES_MAX;
    E.cap_bytes = max_packed_bytes;
    E.cap_entries = max_packed_entries;
}

/* ============================================================================
 * 2. Key table (design 5.5)
 *
 * Slots live in segments that double in size and never move, so a background
 * reader can go from an ID to the key bytes while the main thread interns
 * other keys. The hash index, the free maps and the slot contents are
 * written by the main thread only. Other threads only decrement counts.
 *
 * A count that reaches zero on the main thread frees the slot at once. One
 * that reaches zero elsewhere leaves the slot live at zero and sets
 * kt_dirty. jsonKeyTableSweep frees such slots later. A slot at zero is
 * still in the hash index, so interning its name again revives it.
 *
 * kt_live_bytes is the name storage of the keys whose count is above zero.
 * It follows the counts at once, on whichever thread changes them, so it
 * does not wait for the sweep the way the memory itself does.
 * ========================================================================= */

static void *ktAlloc(size_t n) {
    void *p = engAlloc(n);
    E.kt_bytes += E.alloc.usable(p);
    return p;
}

static void ktFree(void *p) {
    if (!p) return;
    E.kt_bytes -= E.alloc.usable(p);
    E.alloc.free(p);
}

static inline uint32_t ktSegSize(uint32_t seg) {
    return KT_SEG0_SIZE << seg;
}

/* The first ID of a segment. */
static inline uint32_t ktSegBase(uint32_t seg) {
    return (KT_SEG0_SIZE << seg) - KT_SEG0_SIZE;
}

static JSON_INLINE void ktLocate(uint32_t id, uint32_t *seg, uint32_t *off) {
    const uint32_t x = id + KT_SEG0_SIZE;
    const uint32_t lg = 31 - (uint32_t)__builtin_clz(x);
    *seg = lg - KT_SEG0_BITS;
    *off = x - ((uint32_t)1 << lg);
}

static JSON_INLINE const keyInfo *ktInfo(uint32_t id) {
    uint32_t seg, off;
    ktLocate(id, &seg, &off);
    return &E.seg[seg].info[off];
}

static inline _Atomic size_t *ktCount(uint32_t id) {
    uint32_t seg, off;
    ktLocate(id, &seg, &off);
    return &E.seg[seg].cnt[off];
}

static JSON_INLINE uint32_t ktInfoLen(const keyInfo *ki) {
    return ki->len_flags & KT_LEN_MASK;
}

/* The serialised form of a key: `"name":`. */
static JSON_INLINE const uint8_t *ktInfoSer(const keyInfo *ki) {
    return (ki->len_flags & KT_NOESC) ? ki->bytes - 1 : ki->bytes + ktInfoLen(ki);
}

/* The allocation that holds a key's name and serialised form. */
static inline void *ktInfoBlock(const keyInfo *ki) {
    return (void *)(uintptr_t)((ki->len_flags & KT_NOESC) ? ki->bytes - 1 : ki->bytes);
}

#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND(v0, v1, v2, v3) \
    do {                          \
        v0 += v1;                 \
        v1 = SIP_ROTL(v1, 13);    \
        v1 ^= v0;                 \
        v0 = SIP_ROTL(v0, 32);    \
        v2 += v3;                 \
        v3 = SIP_ROTL(v3, 16);    \
        v3 ^= v2;                 \
        v0 += v3;                 \
        v3 = SIP_ROTL(v3, 21);    \
        v3 ^= v0;                 \
        v2 += v1;                 \
        v1 = SIP_ROTL(v1, 17);    \
        v1 ^= v2;                 \
        v2 = SIP_ROTL(v2, 32);    \
    } while (0)

/* SipHash-1-2 of a member name, folded to 32 bits. The message is read as
 * little-endian words on every target, as SipHash defines it: read in native
 * order on a big-endian one, the last bytes of a name would land in the
 * byte that holds its length, and names that differ only there would
 * collide whatever the seed. */
static uint32_t ktHash(const uint8_t *in, size_t len) {
    const uint64_t k0 = E.hash_k0, k1 = E.hash_k1;
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    const uint8_t *end = in + (len & ~(size_t)7);
    uint64_t b = (uint64_t)len << 56, m, tail = 0;
    for (; in != end; in += 8) {
        m = loadU64LE(in);
        v3 ^= m;
        SIP_ROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    memcpy(&tail, in, len & 7);
    if (JSON_BIG_ENDIAN) tail = __builtin_bswap64(tail); /* The first byte lowest. */
    b |= tail;
    v3 ^= b;
    SIP_ROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    const uint64_t h = v0 ^ v1 ^ v2 ^ v3;
    return (uint32_t)h ^ (uint32_t)(h >> 32);
}

static void ktAddSegment(void) {
    const uint32_t s = E.kt_nseg;
    if (s == KT_MAX_SEGS) __builtin_trap(); /* 2^32 live keys: unreachable in practice. */
    const size_t n = ktSegSize(s), words = n / 64;
    keySeg *g = &E.seg[s];
    g->cnt = ktAlloc(engSize((uint64_t)n * sizeof(*g->cnt)));
    g->info = ktAlloc(engSize((uint64_t)n * sizeof(*g->info)));
    g->freemap = ktAlloc(words * sizeof(*g->freemap));
    for (size_t i = 0; i < n; i++) atomic_init(&g->cnt[i], KT_FREE);
    memset(g->freemap, 0xFF, words * sizeof(*g->freemap));
    g->nfree = (uint32_t)n;
    g->hint = 0;
    if (s == 0) {
        /* ID 0 means "no key" and is never handed out. */
        g->freemap[0] &= ~(uint64_t)1;
        g->nfree--;
    }
    E.kt_nseg = s + 1;
}

static void ktDropSegment(void) {
    keySeg *g = &E.seg[--E.kt_nseg];
    ktFree(g->cnt);
    ktFree(g->info);
    ktFree(g->freemap);
    memset(g, 0, sizeof(*g));
    if (E.kt_first_free > E.kt_nseg) E.kt_first_free = E.kt_nseg;
}

/* The smallest free ID, so that keys interned early get one-byte IDs. */
static uint32_t ktNewId(void) {
    uint32_t s = E.kt_first_free;
    while (s < E.kt_nseg && E.seg[s].nfree == 0) s++;
    E.kt_first_free = s;
    if (s == E.kt_nseg) ktAddSegment();
    keySeg *g = &E.seg[s];
    uint32_t w = g->hint;
    while (g->freemap[w] == 0) w++;
    g->hint = w;
    const uint32_t bit = (uint32_t)__builtin_ctzll(g->freemap[w]);
    g->freemap[w] &= ~((uint64_t)1 << bit);
    g->nfree--;
    return ktSegBase(s) + w * 64 + bit;
}

static void ktHtResize(uint32_t ncap) {
    uint32_t *nh = ktAlloc(engSize((uint64_t)ncap * sizeof(*nh)));
    memset(nh, 0, (size_t)ncap * sizeof(*nh));
    for (uint32_t i = 0; i < E.kt_htcap; i++) {
        const uint32_t id = E.kt_ht[i];
        if (!id) continue;
        uint32_t j = ktInfo(id)->hash & (ncap - 1);
        while (nh[j]) j = (j + 1) & (ncap - 1);
        nh[j] = id;
    }
    ktFree(E.kt_ht);
    E.kt_ht = nh;
    E.kt_htcap = ncap;
}

/* Backward-shift deletion: no tombstones in the index. */
static void ktHtRemove(uint32_t id, uint32_t hash) {
    const uint32_t mask = E.kt_htcap - 1;
    uint32_t i = hash & mask;
    while (E.kt_ht[i] != id) i = (i + 1) & mask;
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        const uint32_t other = E.kt_ht[j];
        if (!other) break;
        const uint32_t home = ktInfo(other)->hash & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            E.kt_ht[i] = other;
            i = j;
        }
    }
    E.kt_ht[i] = 0;
}

/* The ID of a name, or 0. Takes no reference. */
static uint32_t ktLookup(const uint8_t *s, uint32_t len, uint32_t hash) {
    if (!E.kt_htcap) return 0;
    const uint32_t mask = E.kt_htcap - 1;
    uint32_t i = hash & mask, id;
    while ((id = E.kt_ht[i]) != 0) {
        const keyInfo *ki = ktInfo(id);
        if (ki->hash == hash && ktInfoLen(ki) == len && equalSmall(ki->bytes, s, len)) return id;
        i = (i + 1) & mask;
    }
    return 0;
}

/* The bytes a referenced key adds to kt_live_bytes: its name storage. */
static inline size_t ktNameBytes(const keyInfo *ki) {
    return E.alloc.usable(ktInfoBlock(ki));
}

static inline void ktIncr(uint32_t id) {
    uint32_t seg, off;
    ktLocate(id, &seg, &off);
    if (JSON_LIKELY(atomic_fetch_add_explicit(&E.seg[seg].cnt[off], 1, memory_order_relaxed) != 0)) return;
    /* Revived: its last reference was dropped on another thread and the
     * sweep has not reached it yet. */
    atomic_fetch_add_explicit(&E.kt_live_bytes, ktNameBytes(&E.seg[seg].info[off]), memory_order_relaxed);
}

/* 1 if an ID names a key now. Main thread only. */
static inline int ktIsKey(uint32_t id) {
    return id < ktSegBase(E.kt_nseg) && atomic_load_explicit(ktCount(id), memory_order_relaxed) != KT_FREE;
}

/* Finds or creates a key and takes one reference. */
static uint32_t ktIntern(const uint8_t *s, uint32_t len, int noesc) {
    const uint32_t hash = ktHash(s, len);
    uint32_t id = ktLookup(s, len, hash);
    if (id) {
        ktIncr(id);
        return id;
    }
    if (((uint64_t)E.kt_live + 1) * 2 > E.kt_htcap) ktHtResize(E.kt_htcap ? E.kt_htcap * 2 : KT_HT_MIN);
    id = ktNewId();
    uint32_t seg, off;
    ktLocate(id, &seg, &off);
    /* The name and its serialised form, written before the count is
     * published: a save thread that reaches the key through a document
     * reads both only while the count is above zero. */
    keyInfo *ki = &E.seg[seg].info[off];
    if (noesc || bytesNoEscape(s, len)) {
        uint8_t *block = ktAlloc((size_t)len + 3); /* "name": */
        block[0] = '"';
        memcpy(block + 1, s, len);
        block[1 + len] = '"';
        block[2 + len] = ':';
        ki->bytes = block + 1;
        ki->len_flags = len | KT_NOESC;
        ki->ser_len = len + 3;
    } else {
        const uint64_t esc_len = escapedLength(s, len) + 3;
        /* Names are limited far below this. */
        if (esc_len > UINT32_MAX) __builtin_trap();
        uint8_t *block = ktAlloc(engSize((uint64_t)len + esc_len)); /* name, then "escaped name": */
        memcpy(block, s, len);
        char *o = (char *)block + len;
        *o++ = '"';
        o = escapeBytes(o, s, len);
        *o++ = '"';
        *o++ = ':';
        ki->bytes = block;
        ki->len_flags = len;
        ki->ser_len = (uint32_t)esc_len;
    }
    ki->hash = hash;
    atomic_store_explicit(&E.seg[seg].cnt[off], 1, memory_order_release);
    atomic_fetch_add_explicit(&E.kt_live_bytes, ktNameBytes(ki), memory_order_relaxed);
    const uint32_t mask = E.kt_htcap - 1;
    uint32_t i = hash & mask;
    while (E.kt_ht[i]) i = (i + 1) & mask;
    E.kt_ht[i] = id;
    E.kt_live++;
    return id;
}

/* Frees the slot of a key whose count is zero. Main thread only.
 *
 * The top segment is released when it is entirely free. The design releases
 * it on that condition alone. Here the segment below must also be at least
 * half free, so that a table sitting at a segment boundary does not allocate
 * and release a segment on every intern and release. */
static void ktRelease(uint32_t id) {
    uint32_t seg, off;
    ktLocate(id, &seg, &off);
    keySeg *g = &E.seg[seg];
    keyInfo *ki = &g->info[off];
    ktHtRemove(id, ki->hash);
    ktFree(ktInfoBlock(ki));
    ki->bytes = NULL;
    atomic_store_explicit(&g->cnt[off], KT_FREE, memory_order_relaxed);
    g->freemap[off / 64] |= (uint64_t)1 << (off % 64);
    g->nfree++;
    if (off / 64 < g->hint) g->hint = off / 64;
    if (seg < E.kt_first_free) E.kt_first_free = seg;
    E.kt_live--;
    while (E.kt_nseg > 1) {
        const uint32_t top = E.kt_nseg - 1;
        if (E.seg[top].nfree != ktSegSize(top)) break;
        if ((uint64_t)E.seg[top - 1].nfree * 2 < ktSegSize(top - 1)) break;
        ktDropSegment();
    }
    if (E.kt_htcap > KT_HT_MIN && (uint64_t)E.kt_live * 8 < E.kt_htcap) ktHtResize(E.kt_htcap / 2);
}

/* Drops one reference. `on_main` says whether the caller may free the slot. */
static inline void ktDecr(uint32_t id, int on_main) {
    uint32_t seg, off;
    ktLocate(id, &seg, &off);
    _Atomic size_t *cnt = &E.seg[seg].cnt[off];
    const keyInfo *ki = &E.seg[seg].info[off];
    if (on_main) {
        if (JSON_LIKELY(atomic_fetch_sub_explicit(cnt, 1, memory_order_acq_rel) != 1)) return;
        atomic_fetch_sub_explicit(&E.kt_live_bytes, ktNameBytes(ki), memory_order_relaxed);
        ktRelease(id);
        return;
    }
    /* Off the main thread the size is read first: once the count is zero the
     * main thread may free the name at any moment. */
    const size_t bytes = ktNameBytes(ki);
    if (JSON_LIKELY(atomic_fetch_sub_explicit(cnt, 1, memory_order_acq_rel) != 1)) return;
    atomic_fetch_sub_explicit(&E.kt_live_bytes, bytes, memory_order_relaxed);
    atomic_store_explicit(&E.kt_dirty, 1, memory_order_release);
}

int jsonKeyTableSweep(void) {
    if (!E.kt_sweeping) {
        if (!atomic_exchange_explicit(&E.kt_dirty, 0, memory_order_acq_rel)) return 0;
        E.kt_sweeping = 1;
        E.kt_sweep_pos = 1;
    }
    uint32_t id = E.kt_sweep_pos;
    for (uint32_t budget = KT_SWEEP_BUDGET; budget && id < ktSegBase(E.kt_nseg); budget--, id++) {
        /* A count is zero only for a key dropped off the main thread and not
         * revived since. Only the main thread raises a count from zero. */
        if (atomic_load_explicit(ktCount(id), memory_order_acquire) == 0) ktRelease(id);
    }
    E.kt_sweep_pos = id;
    if (id < ktSegBase(E.kt_nseg)) return 1;
    E.kt_sweeping = 0;
    return atomic_load_explicit(&E.kt_dirty, memory_order_acquire) != 0;
}

uint32_t jsonKeyFind(const char *name, size_t len) {
    if (len > KT_LEN_MASK) return 0;
    const uint32_t id = ktLookup((const uint8_t *)name, (uint32_t)len, ktHash((const uint8_t *)name, len));
    /* A slot at zero is a name no document uses any more. */
    if (!id || atomic_load_explicit(ktCount(id), memory_order_acquire) == 0) return 0;
    ktIncr(id);
    return id;
}

void jsonKeyRelease(uint32_t keyid) {
    if (keyid) ktDecr(keyid, 1);
}

const char *jsonKeyName(uint32_t keyid, uint32_t *len) {
    const keyInfo *ki = ktInfo(keyid);
    *len = ktInfoLen(ki);
    return (const char *)ki->bytes;
}

void jsonEngineGetStats(jsonEngineStats *s) {
    s->keytable_bytes = E.kt_bytes;
    s->keytable_keys = E.kt_live;
    s->keytable_live_bytes = atomic_load_explicit(&E.kt_live_bytes, memory_order_relaxed);
}

/* Predicted-key cache. A nesting position is identified by a hash of the
 * path of member indexes that leads to it. All elements of an array share
 * one position. */
static inline uint32_t keyCachePos(uint32_t parent_pos, uint32_t index_in_parent) {
    return ((parent_pos ^ (index_in_parent + 0x9E3779B9u + (parent_pos << 6) + (parent_pos >> 2))) * 0x85EBCA6Bu) | 1;
}

static inline uint32_t keyCacheSlotOf(uint32_t pos) {
    return (pos >> 16) & (KEY_CACHE_SLOTS - 1);
}

/* Remembers the keys of an object that was just closed. */
static void keyCacheStore(uint32_t slot_no, uint32_t pos, const uint64_t *tape, const uint32_t *key_entries, uint32_t n);

static void keyCacheClear(void) {
    for (int i = 0; i < KEY_CACHE_SLOTS; i++) {
        keyCacheSlot *c = &E.kcache[i];
        ktFree(c->ids);
        memset(c, 0, sizeof(*c));
    }
}

static void tapePoolClear(void);

size_t jsonEngineShutdown(void) {
    keyCacheClear();
    tapePoolClear();
    /* Free what was dropped off the main thread, then count what is left. */
    for (uint32_t id = 1; id < ktSegBase(E.kt_nseg); id++) {
        if (atomic_load_explicit(ktCount(id), memory_order_acquire) == 0) ktRelease(id);
    }
    const size_t leaked = E.kt_live;
    for (uint32_t id = 1; id < ktSegBase(E.kt_nseg); id++) {
        if (atomic_load_explicit(ktCount(id), memory_order_acquire) == KT_FREE) continue;
        ktFree(ktInfoBlock(ktInfo(id)));
    }
    while (E.kt_nseg) ktDropSegment();
    ktFree(E.kt_ht);
    const jsonAllocator a = E.alloc;
    const uint64_t k0 = E.hash_k0, k1 = E.hash_k1;
    jsonEngineInit(&a);
    E.hash_k0 = k0;
    E.hash_k1 = k1;
    return leaked;
}

/* ============================================================================
 * 3. Tapes (design 6.1, 6.2, 6.3)
 *
 * A tape is a flat array of 8-byte entries plus an arena of string bytes. A
 * jsonVal is a tape that holds one complete value. Values reach a document
 * only through a tape: the tokeniser and the document walker (section 7)
 * write one, the emitter (section 5) reads it.
 *
 * Entry layouts. The type is in bits 0..3.
 *
 *   NULL, FALSE, TRUE  one entry.
 *   INT       one entry: the value in bits 8..63 (fits 56 bits signed).
 *   INT64     two entries: type, then the value.
 *   DBL       two entries: type, then the IEEE bits.
 *   STR       one entry: arena offset in bits 32..63, length in bits 8..31,
 *             TF_FLAG = no escaping needed. With TF_BIG the length does not
 *             fit 24 bits and is in a second entry.
 *   RAWNUM    as STR: the text of a number kept verbatim.
 *   DEC       two entries: a decimal. Type and, in bits 8..15, its scale;
 *             then its digits as an integer.
 *   KEYID     one entry: key ID in bits 32..63. TF_FLAG = this entry holds
 *             one reference on the key.
 *   ARR, OBJ  three entries, filled when the container closes:
 *               [0] type, TF_WIDE, typed element kind in bits 8..15, scale
 *                   of a typed decimal array in bits 16..23, count in bits
 *                   32..63
 *               [1] entries in the whole subtree in bits 32..63, exact
 *                   packed size in bits 0..31 or TAPE_NOFIT32
 *               [2] bytes the emitter requests if the subtree does not fit a
 *                   packed node, else 0
 *
 * Key references (design 6.3): every KEYID entry with TF_FLAG holds one
 * reference. Emitting moves it into the document. Discarding a tape releases
 * the references of entries that were not emitted.
 * ========================================================================= */

enum {
    T_NULL = 0,
    T_FALSE,
    T_TRUE,
    T_INT,
    T_INT64,
    T_DBL,
    T_STR,
    T_RAWNUM,
    T_DEC,
    T_KEYID,
    T_ARR,
    T_OBJ
};

#define T_TYPE_MASK 15
_Static_assert((int)T_NULL == (int)PK_NULL && (int)T_FALSE == (int)PK_FALSE && (int)T_TRUE == (int)PK_TRUE,
               "tape and packed literals are numbered alike");
#define TF_FLAG 0x10
#define TF_BIG 0x20  /* STR, RAWNUM: length in the next entry. */
#define TF_WIDE 0x20 /* ARR, OBJ: 16-bit size and count fields. */
#define T_LEN_SHIFT 8
#define T_LEN_MAX 0xFFFFFFu
#define T_HIGH_SHIFT 32
#define T_EKIND_SHIFT 8
#define T_SCALE_SHIFT 16
#define T_INLINE_INT_MIN (-((int64_t)1 << 55))
#define T_INLINE_INT_MAX (((int64_t)1 << 55) - 1)
#define T_CONTAINER_ENTRIES 3

#define TAPE_NOFIT ((uint64_t)1 << 40) /* Added to a frame's packed size by a child that cannot be packed. */
#define TAPE_NOFIT32 0xFFFFFFFFu
/* Tape indexes and arena offsets are 32 bits, and a text can need three tape
 * entries per byte ("[[[["). 512 MB of text keeps the entry count below 2^31,
 * so a text from a client is held to that. A trusted text is a document some
 * server stored. It may have grown past 512 MB one change at a time and must
 * still load, so it is taken up to what the tape holds: TAPE_MAX_TEXT bytes
 * and TAPE_MAX_ENTRIES entries (see tapeEntriesFit).
 * The limits are the same where size_t has 32 bits: the format holds such a
 * text there too, and a text whose tape would not fit the address space
 * stops at engSize like any allocation that fails, rather than refusing
 * texts that do fit, such as 100 MB of long strings. */
#define TAPE_MAX_INPUT ((size_t)1 << 29)
#define TAPE_MAX_TEXT JSON_STRING_MAX /* Longest single string, and longest trusted text. */
#define TAPE_MAX_ENTRIES ((uint64_t)1 << 31)
#define TAPE_KEEP_ENTRIES 8192      /* Buffers larger than these are released */
#define TAPE_KEEP_ARENA (64 * 1024) /* when a tape returns to the pool. */
#define TAPE_KEEP_STACK 1024

/* Kinds of element seen in an open array. */
#define KIND_INT 1
#define KIND_DBL 2
#define KIND_OTHER 4
#define KIND_DEC 8  /* A decimal. */
#define KIND_STR 16 /* A string short enough for a string array. */
#define KIND_LIT 32 /* null, false or true. */

#define SCALE_NONE 0     /* No decimal element seen yet. */
#define SCALE_MIXED 0xFF /* The decimal elements have different scales. */

/* One open container while a tape is being written. Frame 0 stands for the
 * position of the root value and is never closed. */
typedef struct tapeFrame {
    uint64_t psize;     /* Packed bytes of the keys and values so far. */
    int64_t imin, imax; /* Range of the integer elements, or of the digits of the decimal ones. */
    uint32_t open;      /* Tape index of the opening entry. */
    uint32_t count;     /* Members or elements. */
    uint32_t kbase;     /* Where this object's keys start on the key stack. */
    uint32_t pos;       /* Nesting position, for the key cache. */
    uint32_t kc_gen;    /* Key cache generation predicted from, 0 = none. */
    uint8_t is_obj;
    uint8_t vinc;     /* What a value adds to count: 1 in an array, 0 in an object. */
    uint8_t kinds;    /* KIND_* of the elements. */
    uint8_t from_doc; /* Copied from a document: its member names are unique. */
    uint8_t scale;    /* Scale the decimal elements share, SCALE_NONE or SCALE_MIXED. */
    uint8_t str8;     /* A string element is longer than an inline word holds. */
    uint8_t kc_slot;
    uint8_t kc_miss; /* A key differed from the prediction. */
} tapeFrame;

/* One spine container being filled by the emitter. */
typedef struct emitFrame {
    uint8_t *node;
    uint32_t i, end; /* Tape range of the children still to emit. */
    uint32_t n;      /* Children emitted. */
} emitFrame;

struct jsonVal {
    uint64_t *tape;
    uint32_t tlen, tcap;
    uint8_t *arena;
    size_t alen, acap;
    tapeFrame *stk;
    uint32_t sp, scap;
    uint32_t *kstk; /* Tape indexes of the keys of the open objects. */
    uint32_t klen, kcap;
    uint32_t *scratch; /* Hash set for duplicate detection. */
    uint32_t scratch_cap;
    emitFrame *estk;
    uint32_t ecap;
    uint32_t depth;     /* Deepest nesting written. */
    uint32_t max_depth; /* Limit while tokenising, 0 = none. */
    int trusted;
};

static void tapeInitRootFrame(jsonVal *P) {
    tapeFrame *f = &P->stk[0];
    memset(f, 0, sizeof(*f));
    f->vinc = 1;
    f->pos = 1;
    f->imin = INT64_MAX;
    f->imax = INT64_MIN;
    P->sp = 1;
}

/* Takes an empty tape from the pool. */
static jsonVal *tapeAcquire(void) {
    jsonVal *P;
    if (E.npool) {
        P = E.pool[--E.npool];
    } else {
        P = engAlloc(sizeof(*P));
        memset(P, 0, sizeof(*P));
    }
    if (!P->scap) {
        P->scap = 32;
        P->stk = engAlloc(sizeof(tapeFrame) * P->scap);
    }
    if (!P->acap) {
        /* Never without an arena: a string entry points into it even when
         * it has no bytes, and an empty string at a NULL arena would hand
         * memcpy a null pointer (undefined, if harmless, and glibc's
         * nonnull attribute makes UBSan stop there). */
        P->acap = 1024;
        P->arena = engAlloc(P->acap);
    }
    P->tlen = 0;
    P->alen = 0;
    P->klen = 0;
    P->depth = 0;
    P->max_depth = 0;
    P->trusted = 1;
    tapeInitRootFrame(P);
    return P;
}

static void tapeDestroy(jsonVal *P) {
    engFree(P->tape);
    engFree(P->arena);
    engFree(P->stk);
    engFree(P->kstk);
    engFree(P->scratch);
    engFree(P->estk);
    engFree(P);
}

/* Returns a tape whose entries hold no references any more. */
static void tapeRelease(jsonVal *P) {
    if (E.npool == TAPE_POOL_MAX) {
        tapeDestroy(P);
        return;
    }
    /* Trim after a large value so that an idle tape stays small. */
    if (P->tcap > TAPE_KEEP_ENTRIES) {
        engFree(P->tape);
        P->tape = NULL;
        P->tcap = 0;
    }
    if (P->acap > TAPE_KEEP_ARENA) {
        engFree(P->arena);
        P->arena = NULL;
        P->acap = 0;
    }
    if (P->kcap > TAPE_KEEP_ENTRIES) {
        engFree(P->kstk);
        P->kstk = NULL;
        P->kcap = 0;
    }
    if (P->scratch_cap > TAPE_KEEP_ENTRIES) {
        engFree(P->scratch);
        P->scratch = NULL;
        P->scratch_cap = 0;
    }
    if (P->scap > TAPE_KEEP_STACK) {
        engFree(P->stk);
        P->stk = NULL;
        P->scap = 0;
    }
    if (P->ecap > TAPE_KEEP_STACK) {
        engFree(P->estk);
        P->estk = NULL;
        P->ecap = 0;
    }
    E.pool[E.npool++] = P;
}

static void tapePoolClear(void) {
    while (E.npool) tapeDestroy(E.pool[--E.npool]);
}

static inline void tapeNeed(jsonVal *P, uint32_t n) {
    const uint64_t need = (uint64_t)P->tlen + n;
    if (JSON_LIKELY(need <= P->tcap)) return;
    /* More entries than a 32-bit index reaches. No text gets here (see
     * jsonValParse), only a document of billions of values being copied to a
     * tape. The engine stops, as it does when an allocation fails. */
    if (JSON_UNLIKELY(need > TAPE_MAX_ENTRIES)) __builtin_trap();
    uint32_t nc = P->tcap ? P->tcap * 2 : 256;
    while (nc < need) nc *= 2;
    P->tape = engGrow(P->tape, engSize((uint64_t)nc * sizeof(*P->tape)));
    P->tcap = nc;
}

/* Makes room for n more arena bytes. The tokeniser reserves the whole input
 * once, so that a string can be unescaped at the arena tail in place. */
static void arenaReserve(jsonVal *P, size_t n) {
    if ((uint64_t)P->alen + n <= P->acap) return;
    uint64_t nc = P->acap ? (uint64_t)P->acap * 2 : 1024;
    while (nc < (uint64_t)P->alen + n) nc *= 2;
    P->arena = engGrow(P->arena, engSize(nc));
    P->acap = (size_t)nc;
}

static inline tapeFrame *tapeTop(jsonVal *P) {
    return &P->stk[P->sp - 1];
}

static inline unsigned tapeType(uint64_t e) {
    return (unsigned)(e & T_TYPE_MASK);
}

static inline uint32_t tapeHigh(uint64_t e) {
    return (uint32_t)(e >> T_HIGH_SHIFT);
}

/* The bytes of a STR or RAWNUM entry. */
static inline const uint8_t *tapeText(const jsonVal *P, uint32_t idx, uint32_t *len) {
    const uint64_t e = P->tape[idx];
    *len = (e & TF_BIG) ? (uint32_t)P->tape[idx + 1] : (uint32_t)(e >> T_LEN_SHIFT) & T_LEN_MAX;
    return P->arena + tapeHigh(e);
}

static inline int64_t tapeDecMantissa(const uint64_t *t, uint32_t idx) {
    return (int64_t)t[idx + 1];
}

static inline uint32_t tapeDecScale(uint64_t e) {
    return (uint32_t)(e >> T_LEN_SHIFT) & 0xFF;
}

static inline int64_t tapeInt(const uint64_t *t, uint32_t idx) {
    return tapeType(t[idx]) == T_INT ? (int64_t)t[idx] >> T_LEN_SHIFT : (int64_t)t[idx + 1];
}

/* Entries one tape item occupies, with a container counted as its three
 * header entries only. */
static inline uint32_t tapeEntryWidth(uint64_t e) {
    switch (tapeType(e)) {
    case T_INT64:
    case T_DBL:
    case T_DEC: return 2;
    case T_STR:
    case T_RAWNUM: return (e & TF_BIG) ? 2 : 1;
    case T_ARR:
    case T_OBJ: return T_CONTAINER_ENTRIES;
    default: return 1;
    }
}

/* Entries of the whole value at idx. Containers must be closed. */
static inline uint32_t tapeSpan(const uint64_t *t, uint32_t idx) {
    const unsigned type = tapeType(t[idx]);
    if (type == T_ARR || type == T_OBJ) return tapeHigh(t[idx + 1]);
    return tapeEntryWidth(t[idx]);
}

static inline int tapeIsContainer(uint64_t e) {
    return tapeType(e) >= T_ARR;
}

static inline int tapeFits(const uint64_t *t, uint32_t idx) {
    return (uint32_t)t[idx + 1] != TAPE_NOFIT32;
}

static inline uint32_t tapeCount(uint64_t e) {
    return tapeHigh(e);
}

static inline uint32_t tapeEkind(uint64_t e) {
    return (uint32_t)(e >> T_EKIND_SHIFT) & 0xFF;
}

static inline uint8_t tapeScale(uint64_t e) {
    return (uint8_t)(e >> T_SCALE_SHIFT);
}

static inline uint32_t packedIntSize(int64_t v) {
    if ((uint64_t)v <= PK_INT0_MAX) return 1;
    if (v == (int8_t)v) return 2;
    if (v == (int16_t)v) return 3;
    if (v == (int32_t)v) return 5;
    return 9;
}

/* A decimal: the tag and its digits in 1, 2, 4 or 8 bytes, or the long form
 * that a scale beyond the tags takes. */
static inline uint32_t packedDecSize(int64_t m, uint32_t scale) {
    if (scale > PK_DEC_SCALE_MAX) return PK_DECX_SIZE;
    if (m == (int8_t)m) return 2;
    if (m == (int16_t)m) return 3;
    if (m == (int32_t)m) return 5;
    return 9;
}

static inline uint64_t packedStrSize(uint32_t len) {
    if (len <= PK_SSTR_MAX) return 1 + (uint64_t)len;
    return len <= PK_LEN16_MAX ? 3 + (uint64_t)len : TAPE_NOFIT;
}

static inline uint64_t packedRawSize(uint32_t len) {
    if (len <= PK_RAW8_MAX) return 2 + (uint64_t)len;
    return len <= PK_LEN16_MAX ? 3 + (uint64_t)len : TAPE_NOFIT;
}

/* Exact packed size of the value at idx, or TAPE_NOFIT. */
static uint64_t tapePackedSize(const jsonVal *P, uint32_t idx) {
    const uint64_t *t = P->tape;
    const uint64_t e = t[idx];
    uint32_t len;
    switch (tapeType(e)) {
    case T_NULL:
    case T_FALSE:
    case T_TRUE: return 1;
    case T_INT:
    case T_INT64: return packedIntSize(tapeInt(t, idx));
    case T_DBL: return 9;
    case T_DEC: return packedDecSize(tapeDecMantissa(t, idx), tapeDecScale(e));
    case T_STR:
        tapeText(P, idx, &len);
        return packedStrSize(len);
    case T_RAWNUM:
        tapeText(P, idx, &len);
        return packedRawSize(len);
    default: return tapeFits(t, idx) ? (uint32_t)t[idx + 1] : TAPE_NOFIT;
    }
}

/* ---- Writing values. Each adds one value at the current position. ---- */

static inline void tapeCountValue(tapeFrame *f, uint64_t psize, uint8_t kind) {
    f->psize += psize;
    f->kinds |= kind;
    f->count += f->vinc;
}

static inline void tapeSimple(jsonVal *P, int type) {
    tapeNeed(P, 1);
    P->tape[P->tlen++] = (uint64_t)type;
    tapeCountValue(tapeTop(P), 1, KIND_LIT);
}

static inline void tapeInt64(jsonVal *P, int64_t v) {
    tapeNeed(P, 2);
    if (JSON_LIKELY(v >= T_INLINE_INT_MIN && v <= T_INLINE_INT_MAX)) {
        P->tape[P->tlen++] = ((uint64_t)v << T_LEN_SHIFT) | T_INT;
    } else {
        P->tape[P->tlen++] = T_INT64;
        P->tape[P->tlen++] = (uint64_t)v;
    }
    tapeFrame *f = tapeTop(P);
    tapeCountValue(f, packedIntSize(v), KIND_INT);
    if (v < f->imin) f->imin = v;
    if (v > f->imax) f->imax = v;
}

static inline void tapeDouble(jsonVal *P, double d) {
    tapeNeed(P, 2);
    P->tape[P->tlen++] = T_DBL;
    P->tape[P->tlen++] = doubleBits(d);
    tapeCountValue(tapeTop(P), 9, KIND_DBL);
}

/* Appends bytes to the arena and returns their offset. The bytes may already
 * sit at the arena tail, where the tokeniser unescapes. The caller reserved
 * the room. */
static inline uint32_t arenaPut(jsonVal *P, const uint8_t *s, uint32_t len) {
    const uint32_t off = (uint32_t)P->alen;
    jsonAssert(P->arena != NULL && P->alen + len <= P->acap);
    if (s != P->arena + P->alen) copySmall(P->arena + P->alen, s, len);
    P->alen += len;
    return off;
}

/* Writes a STR or RAWNUM entry. */
static inline void tapeTextEntry(jsonVal *P, unsigned type_and_flags, const uint8_t *s, uint32_t len) {
    tapeNeed(P, 2);
    const uint64_t off = (uint64_t)arenaPut(P, s, len) << T_HIGH_SHIFT;
    if (JSON_LIKELY(len <= T_LEN_MAX)) {
        P->tape[P->tlen++] = off | ((uint64_t)len << T_LEN_SHIFT) | type_and_flags;
    } else {
        P->tape[P->tlen++] = off | TF_BIG | type_and_flags;
        P->tape[P->tlen++] = len;
    }
}

static inline void tapeString(jsonVal *P, const uint8_t *s, uint32_t len, int noesc) {
    tapeTextEntry(P, T_STR | (noesc ? TF_FLAG : 0), s, len);
    tapeFrame *f = tapeTop(P);
    tapeCountValue(f, packedStrSize(len), len <= EK_STR_MAX ? KIND_STR : KIND_OTHER);
    if (len > W_STR_MAX) f->str8 = 1;
}

static inline void tapeRawNum(jsonVal *P, const uint8_t *s, uint32_t len) {
    tapeTextEntry(P, T_RAWNUM, s, len);
    tapeCountValue(tapeTop(P), packedRawSize(len), KIND_OTHER);
}

/* A decimal: the integer its digits spell, and its number of fraction digits. */
static inline void tapeDecimal(jsonVal *P, int64_t mantissa, uint32_t scale) {
    tapeNeed(P, 2);
    P->tape[P->tlen++] = ((uint64_t)scale << T_LEN_SHIFT) | T_DEC;
    P->tape[P->tlen++] = (uint64_t)mantissa;
    tapeFrame *f = tapeTop(P);
    tapeCountValue(f, packedDecSize(mantissa, scale), KIND_DEC);
    if (mantissa < f->imin) f->imin = mantissa;
    if (mantissa > f->imax) f->imax = mantissa;
    if (f->scale == SCALE_NONE) {
        f->scale = (uint8_t)scale;
    } else if (f->scale != scale) {
        f->scale = SCALE_MIXED;
    }
}

static inline void keyStackPush(jsonVal *P, uint32_t idx) {
    if (JSON_UNLIKELY(P->klen == P->kcap)) {
        P->kcap = P->kcap ? P->kcap * 2 : 64;
        P->kstk = engGrow(P->kstk, engSize((uint64_t)P->kcap * sizeof(*P->kstk)));
    }
    P->kstk[P->klen++] = idx;
}

/* Writes the key entry of the next member. The entry takes over one
 * reference the caller holds on the key. */
static inline void tapeKeyEntry(jsonVal *P, tapeFrame *f, uint32_t id) {
    tapeNeed(P, 1);
    keyStackPush(P, P->tlen);
    P->tape[P->tlen++] = ((uint64_t)id << T_HIGH_SHIFT) | TF_FLAG | T_KEYID;
    f->psize += varintSize(id);
    f->count++;
}

/* A member name read from JSON text. Tries the key predicted for this
 * position before the hash lookup. */
static inline void tapeKey(jsonVal *P, const uint8_t *s, uint32_t len, int noesc) {
    tapeFrame *f = tapeTop(P);
    if (f->kc_gen) {
        const keyCacheSlot *c = &E.kcache[f->kc_slot];
        if (c->gen == f->kc_gen && f->count < c->n) {
            const uint32_t id = c->ids[f->count];
            const keyInfo *ki = ktIsKey(id) ? ktInfo(id) : NULL;
            if (ki && ktInfoLen(ki) == len && equalSmall(ki->bytes, s, len)) {
                ktIncr(id);
                tapeKeyEntry(P, f, id);
                return;
            }
        }
        if (f->count < KEY_CACHE_MAX_KEYS) f->kc_miss = 1;
    }
    tapeKeyEntry(P, f, ktIntern(s, len, noesc));
}

/* A member name copied from a document: a new reference (design 6.3). */
static inline void tapeKeyId(jsonVal *P, uint32_t id) {
    ktIncr(id);
    tapeKeyEntry(P, tapeTop(P), id);
}

/* Opens a container. `from_doc` says it is copied from a document, where
 * names are already unique. */
static inline tapeFrame *tapeOpen(jsonVal *P, int is_obj, int from_doc) {
    tapeNeed(P, T_CONTAINER_ENTRIES);
    const uint32_t idx = P->tlen;
    P->tape[idx] = is_obj ? T_OBJ : T_ARR;
    P->tape[idx + 1] = 0;
    P->tape[idx + 2] = 0;
    P->tlen += T_CONTAINER_ENTRIES;
    if (JSON_UNLIKELY(P->sp == P->scap)) {
        P->scap *= 2;
        P->stk = engGrow(P->stk, engSize((uint64_t)sizeof(tapeFrame) * P->scap));
    }
    const tapeFrame *par = &P->stk[P->sp - 1];
    tapeFrame *f = &P->stk[P->sp++];
    if (P->sp - 1 > P->depth) P->depth = P->sp - 1;
    f->psize = 0;
    f->imin = INT64_MAX;
    f->imax = INT64_MIN;
    f->open = idx;
    f->count = 0;
    f->kbase = P->klen;
    f->kc_gen = 0;
    f->is_obj = (uint8_t)is_obj;
    f->vinc = (uint8_t)!is_obj;
    f->kinds = 0;
    f->from_doc = (uint8_t)from_doc;
    f->scale = SCALE_NONE;
    f->str8 = 0;
    f->kc_slot = 0;
    f->kc_miss = 0;
    f->pos = 0;
    if (!from_doc) {
        f->pos = keyCachePos(par->pos, par->is_obj ? par->count : 0xFFFFu);
        if (is_obj) {
            const uint32_t slot = keyCacheSlotOf(f->pos);
            f->kc_slot = (uint8_t)slot;
            if (E.kcache[slot].pos == f->pos && E.kcache[slot].gen) {
                f->kc_gen = E.kcache[slot].gen;
            } else {
                f->kc_miss = 1;
            }
        }
    }
    return f;
}

/* Releases the references held by tape entries [from, to). Works on open
 * containers too: it steps entry by entry. */
static void tapeDropRefs(jsonVal *P, uint32_t from, uint32_t to) {
    uint64_t *t = P->tape;
    for (uint32_t i = from; i < to;) {
        const uint64_t e = t[i];
        if (tapeType(e) == T_KEYID && (e & TF_FLAG)) {
            ktDecr(tapeHigh(e), 1);
            t[i] = e & ~(uint64_t)TF_FLAG;
        }
        i += tapeEntryWidth(e);
    }
}

static void keyCacheStore(uint32_t slot_no, uint32_t pos, const uint64_t *tape, const uint32_t *key_entries, uint32_t n) {
    keyCacheSlot *c = &E.kcache[slot_no];
    if (n > c->cap) {
        ktFree(c->ids);
        c->cap = n * 2;
        c->ids = ktAlloc((size_t)c->cap * sizeof(*c->ids));
    }
    for (uint32_t i = 0; i < n; i++) c->ids[i] = tapeHigh(tape[key_entries[i]]);
    c->n = n;
    c->pos = pos;
    c->gen = ++E.kcache_gen;
    if (c->gen == 0) c->gen = ++E.kcache_gen; /* 0 means empty. */
}

/* ---- Closing an object: duplicate member names ---- */

/* An empty hash set for n entries. *cap is its size, a power of two. */
static uint32_t *tapeScratch(jsonVal *P, uint32_t n, uint32_t *cap) {
    uint32_t need = 16;
    while (need < n * 2) need *= 2;
    if (need > P->scratch_cap) {
        engFree(P->scratch);
        P->scratch = engAlloc(engSize((uint64_t)need * sizeof(*P->scratch)));
        P->scratch_cap = need;
    }
    memset(P->scratch, 0, (size_t)need * sizeof(*P->scratch));
    *cap = need;
    return P->scratch;
}

static inline uint32_t scratchHome(uint32_t keyid, uint32_t cap) {
    return ((keyid * OBJECT_HASH_MULT) >> 7) & (cap - 1);
}

/* 1 if two of the n keys listed in `ks` are the same. */
static int keysHaveDuplicate(jsonVal *P, const uint32_t *ks, uint32_t n) {
    const uint64_t *t = P->tape;
    if (n < 2) return 0;
    /* Nearly every object has only small IDs: a bitmap on the stack. */
    uint64_t seen[4] = {0, 0, 0, 0};
    uint32_t i;
    for (i = 0; i < n; i++) {
        const uint32_t id = tapeHigh(t[ks[i]]);
        if (JSON_UNLIKELY(id >= 256)) break;
        const uint64_t bit = (uint64_t)1 << (id & 63);
        if (seen[id >> 6] & bit) return 1;
        seen[id >> 6] |= bit;
    }
    if (i == n) return 0;
    /* A hash set of key list positions, stored plus one. */
    uint32_t cap;
    uint32_t *set = tapeScratch(P, n, &cap);
    for (i = 0; i < n; i++) {
        const uint32_t id = tapeHigh(t[ks[i]]);
        uint32_t x = scratchHome(id, cap), s;
        while ((s = set[x]) != 0) {
            if (tapeHigh(t[ks[s - 1]]) == id) return 1;
            x = (x + 1) & (cap - 1);
        }
        set[x] = i + 1;
    }
    return 0;
}

/* Resolves duplicate names as the module does: the later value takes the
 * earlier member's position. The superseded value and the later key entry
 * give up their references, and the object's entries are rewritten without
 * them. */
static void tapeDedupe(jsonVal *P, tapeFrame *f) {
    const uint32_t n = P->klen - f->kbase;
    uint32_t *ks = P->kstk + f->kbase;
    uint64_t *t = P->tape;
    uint32_t cap;
    uint32_t *set = tapeScratch(P, n, &cap);
    /* first[i]: tape index of the i-th distinct key. value[i]: its value. */
    uint32_t *first = engAlloc(engSize((uint64_t)n * 2 * sizeof(*first)));
    uint32_t *value = first + n;
    uint32_t distinct = 0;
    for (uint32_t j = 0; j < n; j++) {
        const uint32_t kj = ks[j], id = tapeHigh(t[kj]);
        uint32_t x = scratchHome(id, cap), s;
        while ((s = set[x]) != 0 && tapeHigh(t[first[s - 1]]) != id) x = (x + 1) & (cap - 1);
        if (s) {
            const uint32_t old = value[s - 1];
            tapeDropRefs(P, old, old + tapeSpan(t, old));
            tapeDropRefs(P, kj, kj + 1);
            value[s - 1] = kj + 1;
        } else {
            first[distinct] = kj;
            value[distinct] = kj + 1;
            set[x] = ++distinct;
        }
    }
    const uint32_t base = f->open + T_CONTAINER_ENTRIES;
    uint64_t *tmp = engAlloc(engSize((uint64_t)(P->tlen - base + 1) * sizeof(*tmp)));
    uint32_t m = 0;
    f->count = distinct;
    f->psize = 0;
    for (uint32_t i = 0; i < distinct; i++) {
        const uint32_t span = tapeSpan(t, value[i]);
        f->psize += varintSize(tapeHigh(t[first[i]])) + tapePackedSize(P, value[i]);
        ks[i] = base + m;
        tmp[m++] = t[first[i]];
        memcpy(tmp + m, t + value[i], (size_t)span * sizeof(*tmp));
        m += span;
    }
    memcpy(t + base, tmp, (size_t)m * sizeof(*tmp));
    P->tlen = base + m;
    P->klen = f->kbase + distinct;
    engFree(tmp);
    engFree(first);
}

/* ---- Closing a container (design 6.1 step 2) ---- */

static inline uint32_t objectIndexLg(uint32_t count) {
    uint32_t lg = OBJECT_MIN_LG;
    while (((uint64_t)1 << lg) * 3 < (uint64_t)count * 4) lg++; /* Load at most 0.75. */
    return lg;
}

static inline uint32_t objectIndexWidth(uint32_t cap) {
    if (cap < 0xFF) return 1;
    return cap < 0xFFFF ? 2 : 4;
}

/* Node sizes are 64-bit: see engSize. */
static inline uint64_t objectNodeSize(uint32_t cap) {
    return SPINE_HDR + (uint64_t)cap * OBJECT_ENTRY + ((uint64_t)1 << objectIndexLg(cap)) * objectIndexWidth(cap);
}

static inline uint64_t strNodeSize(uint32_t len) {
    return (len <= STR_SHORT_MAX ? STR_SHORT_HDR : STR_LONG_HDR) + (uint64_t)len;
}

static inline int doubleFitsWord(uint64_t bits) {
    const unsigned tag = (unsigned)(bits >> 61); /* The low three bits after the rotation. */
    return bits == 0 || bits == (uint64_t)1 << 63 || tag == 1 || tag == 5;
}

/* Bytes the emitter requests for the value at idx when it is a child of a
 * spine container, not counting its slot. */
static uint64_t tapeSpineChildBytes(const jsonVal *P, uint32_t idx) {
    const uint64_t *t = P->tape;
    const uint64_t e = t[idx];
    uint32_t len;
    switch (tapeType(e)) {
    case T_INT:
    case T_INT64: {
        const int64_t v = tapeInt(t, idx);
        return v >= W_INT_MIN && v <= W_INT_MAX ? 0 : BOXNUM_SIZE;
    }
    case T_DBL: return doubleFitsWord(t[idx + 1]) ? 0 : BOXNUM_SIZE;
    case T_DEC: return wordDecFits(tapeDecMantissa(t, idx), tapeDecScale(e)) ? 0 : BOXNUM_SIZE;
    case T_STR:
    case T_RAWNUM:
        tapeText(P, idx, &len);
        return len <= W_STR_MAX ? 0 : strNodeSize(len);
    case T_ARR:
    case T_OBJ: return tapeFits(t, idx) ? PACKED_HDR + (uint64_t)(uint32_t)t[idx + 1] : t[idx + 2];
    default: return 0; /* Literals are held in the word. */
    }
}

/* Bytes the emitter requests for a closed container that does not fit a
 * packed node. */
static uint64_t tapeSpineBytes(const jsonVal *P, uint32_t open, uint32_t end, int is_obj, uint32_t count, uint32_t ekind) {
    const uint64_t *t = P->tape;
    if (ekind) return TARRAY_HDR + (uint64_t)count * EK_WIDTH[ekind];
    uint64_t bytes = is_obj ? objectNodeSize(count) : SPINE_HDR + (uint64_t)count * sizeof(uint64_t);
    for (uint32_t i = open + T_CONTAINER_ENTRIES; i < end; i += tapeSpan(t, i)) {
        if (tapeType(t[i]) != T_KEYID) bytes += tapeSpineChildBytes(P, i);
    }
    return bytes;
}

static inline uint32_t intRangeEkind(int64_t lo, int64_t hi) {
    if (lo >= INT8_MIN && hi <= INT8_MAX) return EK_I8;
    if (lo >= INT16_MIN && hi <= INT16_MAX) return EK_I16;
    if (lo >= INT32_MIN && hi <= INT32_MAX) return EK_I32;
    return EK_I64;
}

/* Closes the innermost container: resolves duplicates, chooses a typed
 * array, and writes the count and sizes to the opening entry. */
static void tapeClose(jsonVal *P) {
    tapeFrame *f = tapeTop(P);
    uint32_t ekind = EK_NONE, wide;
    uint8_t scale = 0;
    uint64_t total;
    if (f->is_obj) {
        if (!f->from_doc) {
            if (keysHaveDuplicate(P, P->kstk + f->kbase, P->klen - f->kbase)) tapeDedupe(P, f);
            const keyCacheSlot *c = &E.kcache[f->kc_slot];
            const uint32_t remember = f->count < KEY_CACHE_MAX_KEYS ? f->count : KEY_CACHE_MAX_KEYS;
            if (f->kc_miss || c->gen != f->kc_gen || c->n != remember) {
                keyCacheStore(f->kc_slot, f->pos, P->tape, P->kstk + f->kbase, remember);
            }
        }
        P->klen = f->kbase;
        wide = f->psize > PK_NARROW_MAX || f->count > PK_NARROW_MAX;
        total = f->psize + (wide ? PK_HDR_WIDE : PK_HDR_NARROW);
    } else {
        /* Numbers of one kind make a typed array: integers, doubles, or
         * decimals that share a scale. */
        if (f->count && f->kinds == KIND_INT) {
            ekind = intRangeEkind(f->imin, f->imax);
        } else if (f->count && f->kinds == KIND_DBL) {
            ekind = EK_F64;
        } else if (f->count && f->kinds == KIND_DEC && f->scale != SCALE_MIXED) {
            ekind = EK_DEC_BASE + intRangeEkind(f->imin, f->imax);
            scale = f->scale;
        }
        if (ekind) {
            wide = f->count > PK_NARROW_MAX;
            total = (uint64_t)f->count * EK_WIDTH[ekind] + (wide ? 3 : 2) + (scale != 0);
        } else {
            wide = f->psize > PK_NARROW_MAX || f->count > PK_NARROW_MAX;
            total = f->psize + (wide ? PK_HDR_WIDE : PK_HDR_NARROW);
        }
    }
    const int fits = total <= E.cap_bytes && f->count <= E.cap_entries;
    /* Short strings that will not be packed go into one node, 16 bytes each.
     * That only pays when some of them would not fit a word anyway. */
    if (!fits && !f->is_obj && !ekind && f->count && f->kinds == KIND_STR && f->str8) ekind = EK_STR;
    /* Likewise an array of only null, false and true: a byte each. */
    if (!fits && !f->is_obj && !ekind && f->count && f->kinds == KIND_LIT) ekind = EK_LIT;
    const uint32_t open = f->open, count = f->count;
    uint64_t *t = P->tape;
    t[open] = (t[open] & T_TYPE_MASK) | TF_FLAG | (wide ? TF_WIDE : 0) | ((uint64_t)ekind << T_EKIND_SHIFT) |
              ((uint64_t)scale << T_SCALE_SHIFT) | ((uint64_t)count << T_HIGH_SHIFT);
    t[open + 1] = ((uint64_t)(P->tlen - open) << T_HIGH_SHIFT) | (fits ? (uint32_t)total : TAPE_NOFIT32);
    t[open + 2] = fits ? 0 : tapeSpineBytes(P, open, P->tlen, f->is_obj, count, ekind);
    P->sp--;
    tapeCountValue(tapeTop(P), fits ? total : TAPE_NOFIT, KIND_OTHER);
}

/* ============================================================================
 * 4. Tokeniser (design 6.1 step 1)
 * ========================================================================= */

#define UTF8_BOM "\xEF\xBB\xBF"
#define UTF8_BOM_LEN 3
#define SWAR_ONES 0x0101010101010101ull
#define SWAR_HIGHS 0x8080808080808080ull

#define NUM_FAST_DIGITS 15      /* A decimal of at most this many digits is the shortest text of its double. */
#define NUM_PRINT_DIGITS 17     /* No canonical text has more significant digits. */
#define NUM_PLAIN_ZEROS 5       /* Canonical text is plain decimal down to 1e-6: "0." and at most five zeros. */
#define NUM_PLAIN_INT_DIGITS 21 /* ...and up to, not including, 1e21. */
#define NUM_SAFE_INT_DIGITS 308 /* An integer of this many digits is below the largest double. */

/* Advances over string bytes that need no attention: everything but '"',
 * '\\' and control bytes, and, with `stop_high`, bytes of 0x80 and above.
 * Eight bytes at a time: a byte is flagged when subtracting makes it borrow.
 * A borrow can flag a higher byte falsely, but never a lower one, and only
 * the lowest flag is used. */
static inline const uint8_t *scanPlain(const uint8_t *p, const uint8_t *end, int stop_high) {
    const uint64_t high = stop_high ? SWAR_HIGHS : 0;
    while (end - p >= 8) {
        const uint64_t w = loadU64LE(p); /* So that the first byte is the lowest: see above. */
        const uint64_t quote = w ^ (SWAR_ONES * '"'), bslash = w ^ (SWAR_ONES * '\\');
        const uint64_t low = ((w - SWAR_ONES * 0x20) | (quote - SWAR_ONES) | (bslash - SWAR_ONES)) & ~w & SWAR_HIGHS;
        const uint64_t m = low | (w & high);
        if (m) return p + (__builtin_ctzll(m) >> 3);
        p += 8;
    }
    while (p < end) {
        const uint8_t c = *p;
        if (c == '"' || c == '\\' || c < 0x20 || (c >= 0x80 && stop_high)) break;
        p++;
    }
    return p;
}

static inline int isContinuation(uint8_t c) {
    return (c & 0xC0) == 0x80;
}

/* Length of the valid UTF-8 sequence at p (2 to 4), or 0. Rejects overlong
 * forms, surrogates and code points above U+10FFFF. */
static int utf8SequenceLen(const uint8_t *p, const uint8_t *end) {
    const uint8_t c = p[0];
    if (c >= 0xC2 && c <= 0xDF) return (end - p >= 2 && isContinuation(p[1])) ? 2 : 0;
    if ((c & 0xF0) == 0xE0) {
        if (end - p < 3 || !isContinuation(p[1]) || !isContinuation(p[2])) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;
        if (c == 0xED && p[1] >= 0xA0) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (end - p < 4 || !isContinuation(p[1]) || !isContinuation(p[2]) || !isContinuation(p[3])) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

static inline int hex4(const uint8_t *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t c = p[i], lower = c | 0x20;
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (lower >= 'a' && lower <= 'f') {
            digit = (uint32_t)(lower - 'a' + 10);
        } else {
            return 0;
        }
        v = (v << 4) | digit;
    }
    *out = v;
    return 1;
}

static inline uint8_t *utf8Encode(uint8_t *d, uint32_t cp) {
    if (cp < 0x80) {
        *d++ = (uint8_t)cp;
    } else if (cp < 0x800) {
        *d++ = (uint8_t)(0xC0 | (cp >> 6));
        *d++ = (uint8_t)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        *d++ = (uint8_t)(0xE0 | (cp >> 12));
        *d++ = (uint8_t)(0x80 | ((cp >> 6) & 63));
        *d++ = (uint8_t)(0x80 | (cp & 63));
    } else {
        *d++ = (uint8_t)(0xF0 | (cp >> 18));
        *d++ = (uint8_t)(0x80 | ((cp >> 12) & 63));
        *d++ = (uint8_t)(0x80 | ((cp >> 6) & 63));
        *d++ = (uint8_t)(0x80 | (cp & 63));
    }
    return d;
}

/* Decodes the escape at *pp (which points at the backslash) to *dp. Sets
 * *needs_escape if the decoded character has to be escaped on output. */
static int lexEscape(const uint8_t **pp, const uint8_t *end, uint8_t **dp, int *needs_escape) {
    const uint8_t *p = *pp;
    uint8_t *d = *dp;
    uint32_t cp, low;
    if (end - p < 2) return 0;
    const uint8_t c = p[1];
    p += 2;
    switch (c) {
    case '"': cp = '"'; break;
    case '\\': cp = '\\'; break;
    case '/': cp = '/'; break;
    case 'b': cp = '\b'; break;
    case 'f': cp = '\f'; break;
    case 'n': cp = '\n'; break;
    case 'r': cp = '\r'; break;
    case 't': cp = '\t'; break;
    case 'u':
        if (end - p < 4 || !hex4(p, &cp)) return 0;
        p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* A high surrogate must be followed by an escaped low one. */
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u' || !hex4(p + 2, &low)) return 0;
            if (low < 0xDC00 || low > 0xDFFF) return 0;
            p += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return 0;
        }
        break;
    default: return 0;
    }
    if (cp < 0x20 || cp == '"' || cp == '\\') *needs_escape = 1;
    *dp = utf8Encode(d, cp);
    *pp = p;
    return 1;
}

/* Reads a string. *pp points after the opening quote and is left after the
 * closing one, or at the error. On success (*sp, *slen) are the unescaped
 * bytes: inside the input when it has no escapes, else at the arena tail,
 * not yet committed. */
static jsonErr lexString(jsonVal *P, const uint8_t **pp, const uint8_t *end, const uint8_t **sp, uint32_t *slen, int *noesc) {
    const int validate = !P->trusted;
    const uint8_t *p = *pp, *s = p;
    jsonErr err = JSON_ERR_SYNTAX;
    for (;;) {
        p = scanPlain(p, end, validate);
        if (JSON_UNLIKELY(p >= end)) goto fail;
        const uint8_t c = *p;
        if (JSON_LIKELY(c == '"')) {
            *sp = s;
            *slen = (uint32_t)(p - s);
            *noesc = 1;
            *pp = p + 1;
            return JSON_OK;
        }
        if (c == '\\') break;
        if (c < 0x80) goto fail; /* A control byte. */
        const int n = utf8SequenceLen(p, end);
        if (!n) {
            err = JSON_ERR_UTF8;
            goto fail;
        }
        p += n;
    }
    /* There is an escape: unescape into the arena tail. The output is never
     * longer than the input, which the arena was reserved for. */
    uint8_t *d0 = P->arena + P->alen, *d = d0;
    int needs_escape = 0;
    memcpy(d, s, (size_t)(p - s));
    d += p - s;
    for (;;) {
        const uint8_t *q = scanPlain(p, end, validate);
        memcpy(d, p, (size_t)(q - p));
        d += q - p;
        p = q;
        if (p >= end) goto fail;
        const uint8_t c = *p;
        if (c == '"') break;
        if (c == '\\') {
            if (!lexEscape(&p, end, &d, &needs_escape)) goto fail;
            continue;
        }
        if (c < 0x80) goto fail;
        const int n = utf8SequenceLen(p, end);
        if (!n) {
            err = JSON_ERR_UTF8;
            goto fail;
        }
        memcpy(d, p, (size_t)n);
        d += n;
        p += n;
    }
    *sp = d0;
    *slen = (uint32_t)(d - d0);
    *noesc = !needs_escape;
    *pp = p + 1;
    return JSON_OK;
fail:
    *pp = p;
    return err;
}

/* One number as it appears in JSON text. */
typedef struct numToken {
    const uint8_t *s;  /* The whole token. */
    const uint8_t *ip; /* Integer digits. */
    const uint8_t *fp; /* Fraction digits, when nfrac is not 0. */
    uint64_t v;        /* Value of the integer digits, unless they overflow. */
    uint32_t len, nint, nfrac;
    uint8_t neg, isint, overflow, hasexp;
} numToken;

static inline int isDigit(uint8_t c) {
    return (unsigned)(c - '0') < 10;
}

/* Value of a run of 20 digits. Returns 1 if it does not fit 64 bits. */
static int digitsOverflow(const uint8_t *d, const uint8_t *end, uint64_t *out) {
    uint64_t v = 0;
    for (; d < end; d++) {
        if (__builtin_mul_overflow(v, (uint64_t)10, &v) || __builtin_add_overflow(v, (uint64_t)(*d - '0'), &v)) return 1;
    }
    *out = v;
    return 0;
}

/* Scans the JSON number at p, p < end. Returns the byte after it, or NULL
 * if there is no well-formed number there. */
static const uint8_t *lexNumberToken(const uint8_t *p, const uint8_t *end, numToken *k) {
    uint64_t v = 0;
    k->s = p;
    k->neg = 0;
    k->overflow = 0;
    if (*p == '-') {
        k->neg = 1;
        if (++p >= end) return NULL;
    }
    k->ip = p;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        const uint8_t *d0 = p;
        do {
            v = v * 10 + (uint64_t)(*p - '0');
            p++;
        } while (p < end && isDigit(*p));
        const size_t digits = (size_t)(p - d0);
        if (JSON_UNLIKELY(digits > 19)) k->overflow = digits > 20 || digitsOverflow(d0, p, &v);
    } else {
        return NULL;
    }
    k->v = v;
    k->nint = (uint32_t)(p - k->ip);
    k->isint = 1;
    k->hasexp = 0;
    k->nfrac = 0;
    k->fp = NULL;
    if (p < end && *p == '.') {
        k->isint = 0;
        p++;
        if (p >= end || !isDigit(*p)) return NULL;
        k->fp = p;
        do {
            p++;
        } while (p < end && isDigit(*p));
        k->nfrac = (uint32_t)(p - k->fp);
    }
    if (p < end && (*p | 0x20) == 'e') {
        k->isint = 0;
        k->hasexp = 1;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || !isDigit(*p)) return NULL;
        do {
            p++;
        } while (p < end && isDigit(*p));
    }
    k->len = (uint32_t)(p - k->s);
    return p;
}

typedef enum {
    NUM_INT,    /* Integer syntax within int64. */
    NUM_DEC,    /* Plain notation with a fraction and at most 18 digits: kept as scaled digits. */
    NUM_DOUBLE, /* The text is the canonical text of its double. */
    NUM_RAW     /* Anything else: the text is kept. */
} numClass;

static numClass classifyNumber(const numToken *k, int64_t *iv, uint32_t *scale, double *dv, int *overflow) {
    const char *text = (const char *)k->s;
    *overflow = 0;
    if (k->isint) {
        if (!k->overflow) {
            if (!k->neg && k->v <= (uint64_t)INT64_MAX) {
                *iv = (int64_t)k->v;
                return NUM_INT;
            }
            if (k->neg && k->v <= (uint64_t)1 << 63) {
                *iv = (int64_t)(0 - k->v); /* "-0" is the integer 0, as in the module. */
                return NUM_INT;
            }
        }
        if (k->nint > NUM_SAFE_INT_DIGITS) (void)jsonDoubleParse(text, k->len, overflow);
        return NUM_RAW;
    }
    /* Digits of the text from its first non-zero digit to its last digit. */
    const int int_zero = k->nint == 1 && k->ip[0] == '0';
    uint32_t zeros = 0;
    if (int_zero) {
        while (zeros < k->nfrac && k->fp[zeros] == '0') zeros++;
    }
    const uint32_t text_digits = int_zero ? k->nfrac - zeros : k->nint + k->nfrac;
    if (!k->hasexp && k->nfrac <= DEC_SCALE_MAX && text_digits <= DEC_DIGITS_MAX) {
        /* A decimal. Its digits, read as one integer, are below 10^18, and
         * with the scale they give back the text exactly. No conversion to a
         * double and no printing is needed. */
        uint64_t m = k->v;
        for (uint32_t i = 0; i < k->nfrac; i++) m = m * 10 + (uint64_t)(k->fp[i] - '0');
        if (m != 0 || !k->neg) {
            *iv = k->neg ? -(int64_t)m : (int64_t)m;
            *scale = k->nfrac;
            return NUM_DEC;
        }
        /* A negative zero has no integer to hold its sign. The rules below
         * keep "-0.0" as a double and "-0.00" as text. */
    }
    /* Significant digits. They differ from the above only for a whole value
     * written as "DIGITS000.0", whose trailing zeros and ".0" are layout. */
    const int whole = !k->hasexp && k->nfrac == 1 && k->fp[0] == '0';
    uint32_t sig = text_digits;
    if (whole) {
        sig = k->nint;
        while (sig > 0 && k->ip[sig - 1] == '0') sig--;
    }
    if (!k->hasexp && sig <= NUM_FAST_DIGITS) {
        /* No printing needed. A decimal of at most 15 digits is the shortest
         * text of its double, so the text is canonical if it is also laid
         * out canonically: plain notation is right for its magnitude, and
         * the fraction has no trailing zero other than a lone ".0". */
        *dv = jsonDoubleParse(text, k->len, overflow);
        const int trailing_zero = k->nfrac >= 2 && k->fp[k->nfrac - 1] == '0';
        const int plain = whole ? k->nint <= NUM_PLAIN_INT_DIGITS : (!int_zero || zeros <= NUM_PLAIN_ZEROS);
        return (!trailing_zero && plain) ? NUM_DOUBLE : NUM_RAW;
    }
    if (sig <= NUM_PRINT_DIGITS) {
        char buf[JSON_DOUBLE_BUF];
        *dv = jsonDoubleParse(text, k->len, overflow);
        if (*overflow) return NUM_RAW;
        const int n = jsonDoublePrint(*dv, buf);
        return ((uint32_t)n == k->len && memcmp(buf, text, k->len) == 0) ? NUM_DOUBLE : NUM_RAW;
    }
    if (k->hasexp || k->nint > NUM_SAFE_INT_DIGITS) (void)jsonDoubleParse(text, k->len, overflow);
    return NUM_RAW;
}

/* How a double that was computed, not read, is stored: as the number its
 * canonical text would be read as. That is a decimal when the text is short
 * plain notation ("20.0", "98.6"), else the double itself. */
static numClass classifyDouble(double d, int64_t *iv, uint32_t *scale) {
    char buf[JSON_DOUBLE_BUF];
    numToken k;
    double dv;
    int overflow;
    const int n = jsonDoublePrint(d, buf);
    if (!lexNumberToken((const uint8_t *)buf, (const uint8_t *)buf + n, &k)) return NUM_DOUBLE; /* Not finite. */
    return classifyNumber(&k, iv, scale, &dv, &overflow) == NUM_DEC ? NUM_DEC : NUM_DOUBLE;
}

/* Writes a number at the current position. */
static jsonErr tapeNumber(jsonVal *P, const numToken *k) {
    int64_t iv = 0;
    double dv = 0;
    uint32_t scale = 0;
    int overflow;
    switch (classifyNumber(k, &iv, &scale, &dv, &overflow)) {
    case NUM_INT: tapeInt64(P, iv); return JSON_OK;
    case NUM_DEC: tapeDecimal(P, iv, scale); return JSON_OK;
    case NUM_DOUBLE: tapeDouble(P, dv); return JSON_OK;
    default:
        if (overflow && !P->trusted) return JSON_ERR_NUM_RANGE;
        tapeRawNum(P, k->s, k->len);
        return JSON_OK;
    }
}

/* Writes a computed double at the current position. */
static void tapeComputedDouble(jsonVal *P, double d) {
    int64_t iv;
    uint32_t scale;
    if (classifyDouble(d, &iv, &scale) == NUM_DEC) {
        tapeDecimal(P, iv, scale);
    } else {
        tapeDouble(P, d);
    }
}

static inline const uint8_t *skipSpace(const uint8_t *p, const uint8_t *end) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

/* Reads `"name" :` and writes the key. */
static jsonErr lexMemberName(jsonVal *P, const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = skipSpace(*pp, end), *s;
    uint32_t slen;
    int noesc;
    jsonErr err = JSON_ERR_SYNTAX;
    if (JSON_UNLIKELY(p >= end || *p != '"')) goto fail;
    p++;
    if (JSON_UNLIKELY((err = lexString(P, &p, end, &s, &slen, &noesc)) != JSON_OK)) goto fail;
    if (JSON_UNLIKELY(slen > KT_LEN_MASK)) {
        err = JSON_ERR_TOO_BIG;
        goto fail;
    }
    tapeKey(P, s, slen, noesc);
    p = skipSpace(p, end);
    err = JSON_ERR_SYNTAX;
    if (JSON_UNLIKELY(p >= end || *p != ':')) goto fail;
    *pp = p + 1;
    return JSON_OK;
fail:
    *pp = p;
    return err;
}

static inline int lexLiteral(const uint8_t *p, const uint8_t *end, const char *word, size_t n) {
    return (size_t)(end - p) >= n && memcmp(p, word, n) == 0;
}

/* Reads one value that is not a container and writes it. */
static jsonErr lexScalar(jsonVal *P, const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp, *s;
    uint32_t slen;
    int noesc;
    jsonErr err;
    numToken k;
    switch (*p) {
    case '"':
        p++;
        err = lexString(P, &p, end, &s, &slen, &noesc);
        *pp = p;
        if (JSON_UNLIKELY(err != JSON_OK)) return err;
        tapeString(P, s, slen, noesc);
        return JSON_OK;
    case 't':
        if (!lexLiteral(p, end, "true", 4)) return JSON_ERR_SYNTAX;
        *pp = p + 4;
        tapeSimple(P, T_TRUE);
        return JSON_OK;
    case 'f':
        if (!lexLiteral(p, end, "false", 5)) return JSON_ERR_SYNTAX;
        *pp = p + 5;
        tapeSimple(P, T_FALSE);
        return JSON_OK;
    case 'n':
        if (!lexLiteral(p, end, "null", 4)) return JSON_ERR_SYNTAX;
        *pp = p + 4;
        tapeSimple(P, T_NULL);
        return JSON_OK;
    default:
        p = lexNumberToken(p, end, &k);
        if (JSON_UNLIKELY(!p)) return JSON_ERR_SYNTAX;
        *pp = p;
        return tapeNumber(P, &k);
    }
}

/* Tokenises one JSON text onto an empty tape. On failure *errp is where. */
static jsonErr tapeTokenise(jsonVal *P, const uint8_t *p, const uint8_t *end, const uint8_t **errp) {
    jsonErr err;
    for (;;) {
        /* A value is expected here. */
        err = JSON_ERR_SYNTAX;
        p = skipSpace(p, end);
        if (JSON_UNLIKELY(p >= end)) goto fail;
        if (*p == '{' || *p == '[') {
            const int is_obj = *p == '{';
            if (JSON_UNLIKELY(P->max_depth && P->sp > P->max_depth)) {
                err = JSON_ERR_DEPTH;
                goto fail;
            }
            p++;
            tapeOpen(P, is_obj, 0);
            p = skipSpace(p, end);
            if (!(p < end && *p == (is_obj ? '}' : ']'))) {
                /* Not empty: the first member name, then the first value. */
                if (is_obj && (err = lexMemberName(P, &p, end)) != JSON_OK) goto fail;
                continue;
            }
            p++;
            tapeClose(P);
        } else if (JSON_UNLIKELY((err = lexScalar(P, &p, end)) != JSON_OK)) {
            goto fail;
        }
        /* A value just ended. Close every container that ends here, then
         * step to the next value. */
        for (;;) {
            p = skipSpace(p, end);
            err = JSON_ERR_SYNTAX;
            if (P->sp == 1) {
                if (p != end) goto fail;
                return JSON_OK;
            }
            if (JSON_UNLIKELY(p >= end)) goto fail;
            const int is_obj = tapeTop(P)->is_obj;
            if (*p == ',') {
                p++;
                if (is_obj && (err = lexMemberName(P, &p, end)) != JSON_OK) goto fail;
                break;
            }
            if (*p != (is_obj ? '}' : ']')) goto fail;
            p++;
            tapeClose(P);
        }
    }
fail:
    *errp = p;
    return err;
}

/* ============================================================================
 * 5. Emitter (design 6.1 step 3)
 *
 * Writes the final encoding of a closed tape value, top-down, with one
 * exact-size allocation per node. The close step already decided, for every
 * container, whether it fits a packed node: here that decision is only read.
 *
 * Emitting moves each key entry's reference into the document, so a tape is
 * emitted at most once and is reset afterwards without releasing anything.
 * ========================================================================= */

/* Totals of the nodes allocated for one document. */
typedef struct nodeAcct {
    size_t bytes;
    size_t nodes;
} nodeAcct;

/* Allocates a node. `acct` is NULL when the caller does not need totals. */
static inline uint8_t *nodeAlloc(uint64_t n, nodeAcct *acct) {
    uint8_t *node = engAlloc(engSize(n));
    jsonAssert(((uintptr_t)node & W_TAG_MASK) == 0); /* See the spine word tags. */
    if (acct) {
        acct->bytes += E.alloc.usable(node);
        acct->nodes++;
    }
    return node;
}

/* ---- Packed bytes ---- */

static inline uint8_t *packInt(uint8_t *o, int64_t v) {
    if ((uint64_t)v <= PK_INT0_MAX) {
        *o++ = (uint8_t)(PK_INT0 + v);
    } else if (v == (int8_t)v) {
        *o++ = PK_I8;
        *o++ = (uint8_t)v;
    } else if (v == (int16_t)v) {
        *o++ = PK_I16;
        storeU16(o, (uint16_t)v);
        o += 2;
    } else if (v == (int32_t)v) {
        *o++ = PK_I32;
        storeU32(o, (uint32_t)v);
        o += 4;
    } else {
        *o++ = PK_I64;
        storeU64(o, (uint64_t)v);
        o += 8;
    }
    return o;
}

static inline uint8_t *packDouble(uint8_t *o, uint64_t bits) {
    *o++ = PK_F64;
    storeU64(o, bits);
    return o + 8;
}

static inline uint8_t *packString(uint8_t *o, const uint8_t *s, uint32_t len, int noesc) {
    if (len <= PK_SSTR_MAX) {
        *o++ = (uint8_t)((noesc ? PK_SSTR_NE : PK_SSTR) | len);
    } else {
        *o++ = noesc ? PK_STR16_NE : PK_STR16;
        storeU16(o, (uint16_t)len);
        o += 2;
    }
    copySmall(o, s, len);
    return o + len;
}

static inline uint8_t *packRaw(uint8_t *o, const uint8_t *s, uint32_t len) {
    if (len <= PK_RAW8_MAX) {
        *o++ = PK_RAW8;
        *o++ = (uint8_t)len;
    } else {
        *o++ = PK_RAW16;
        storeU16(o, (uint16_t)len);
        o += 2;
    }
    copySmall(o, s, len);
    return o + len;
}

/* Header of a plain array or object whose whole encoding takes `total` bytes. */
static inline uint8_t *packHeader(uint8_t *o, uint8_t narrow_tag, int wide, uint32_t total, uint32_t count) {
    if (wide) {
        o[0] = (uint8_t)(narrow_tag + 1);
        storeU16(o + 1, (uint16_t)(total - PK_HDR_WIDE));
        storeU16(o + 3, (uint16_t)count);
        return o + PK_HDR_WIDE;
    }
    o[0] = narrow_tag;
    o[1] = (uint8_t)(total - PK_HDR_NARROW);
    o[2] = (uint8_t)count;
    return o + PK_HDR_NARROW;
}

/* Stores one integer element of a typed array, or the digits of a decimal one. */
static inline void ekStoreInt(uint8_t *p, uint32_t ekind, int64_t v) {
    switch (ekIntKind(ekind)) {
    case EK_I8: *p = (uint8_t)v; break;
    case EK_I16: storeU16(p, (uint16_t)v); break;
    case EK_I32: storeU32(p, (uint32_t)v); break;
    default: storeU64(p, (uint64_t)v);
    }
}

static inline uint8_t *packDecimal(uint8_t *o, int64_t m, uint32_t scale) {
    if (scale > PK_DEC_SCALE_MAX) {
        o[0] = PK_DECX;
        o[1] = (uint8_t)scale;
        storeU64(o + 2, (uint64_t)m);
        return o + PK_DECX_SIZE;
    }
    const uint32_t size = packedDecSize(m, scale), width = size - 1;
    const uint32_t lg = width == 1 ? 0 : width == 2 ? 1
                                     : width == 4   ? 2
                                                    : 3;
    *o = (uint8_t)(PK_DEC + (scale - 1) * 4 + lg);
    ekStoreInt(o + 1, EK_I8 + lg, m);
    return o + size;
}

/* Raw elements of a numeric typed array from `count` entries at tape[j]. */
static uint8_t *packTypedElements(uint8_t *o, const uint64_t *t, uint32_t j, uint32_t count, uint32_t ekind) {
    const uint32_t width = EK_WIDTH[ekind];
    for (uint32_t c = 0; c < count; c++, o += width) {
        if (ekind == EK_F64) {
            storeU64(o, t[j + 1]);
            j += 2;
        } else if (ekIsDec(ekind)) {
            ekStoreInt(o, ekind, tapeDecMantissa(t, j));
            j += 2;
        } else {
            ekStoreInt(o, ekind, tapeInt(t, j));
            j += tapeEntryWidth(t[j]);
        }
    }
    return o;
}

static uint8_t *packTypedArray(uint8_t *o, const uint64_t *t, uint32_t idx) {
    const uint64_t e = t[idx];
    const uint32_t count = tapeCount(e), ekind = tapeEkind(e);
    const int wide = (e & TF_WIDE) != 0;
    *o++ = (uint8_t)(PK_TARR + (ekind - 1) * 2 + (uint32_t)wide);
    if (wide) {
        storeU16(o, (uint16_t)count);
        o += 2;
    } else {
        *o++ = (uint8_t)count;
    }
    if (ekIsDec(ekind)) *o++ = tapeScale(e);
    return packTypedElements(o, t, idx + T_CONTAINER_ENTRIES, count, ekind);
}

/* Writes the packed encoding of the value at tape[idx] and returns its end.
 * The tape is in document order and every container knows its size, so this
 * is one pass with no stack. */
static uint8_t *packValue(const jsonVal *P, uint32_t idx, uint8_t *o) {
    const uint64_t *t = P->tape;
    const uint32_t end = idx + tapeSpan(t, idx);
    const uint8_t *s;
    uint32_t len;
    for (uint32_t i = idx; i < end;) {
        const uint64_t e = t[i];
        switch (tapeType(e)) {
        case T_NULL: *o++ = PK_NULL; break;
        case T_FALSE: *o++ = PK_FALSE; break;
        case T_TRUE: *o++ = PK_TRUE; break;
        case T_INT:
        case T_INT64: o = packInt(o, tapeInt(t, i)); break;
        case T_DBL: o = packDouble(o, t[i + 1]); break;
        case T_STR:
            s = tapeText(P, i, &len);
            o = packString(o, s, len, (e & TF_FLAG) != 0);
            break;
        case T_DEC: o = packDecimal(o, tapeDecMantissa(t, i), tapeDecScale(e)); break;
        case T_RAWNUM:
            s = tapeText(P, i, &len);
            o = packRaw(o, s, len);
            break;
        case T_KEYID: o = varintPut(o, tapeHigh(e)); break;
        case T_ARR:
            if (tapeEkind(e)) {
                o = packTypedArray(o, t, i);
                i += tapeSpan(t, i);
                continue;
            }
            o = packHeader(o, PK_ARR8, (e & TF_WIDE) != 0, (uint32_t)t[i + 1], tapeCount(e));
            break;
        default: /* T_OBJ */
            o = packHeader(o, PK_OBJ8, (e & TF_WIDE) != 0, (uint32_t)t[i + 1], tapeCount(e));
        }
        i += tapeEntryWidth(e);
    }
    return o;
}

/* ---- Nodes ---- */

static uint64_t emitPacked(const jsonVal *P, uint32_t idx, nodeAcct *acct) {
    const uint32_t size = (uint32_t)P->tape[idx + 1];
    uint8_t *node = nodeAlloc(PACKED_HDR + (size_t)size, acct);
    node[0] = NK_PACKED;
    storeU16(node + 1, (uint16_t)size);
    uint8_t *end = packValue(P, idx, node + PACKED_HDR);
    jsonAssert((size_t)(end - node) == PACKED_HDR + (size_t)size);
    (void)end;
    return nodeWord(node);
}

/* A string or raw number as a spine word. */
static uint64_t emitText(const uint8_t *s, uint32_t len, int noesc, int rawnum, nodeAcct *acct) {
    if (len <= W_STR_MAX) {
        uint64_t w = 0;
        memcpy((uint8_t *)&w + W_STR_AT, s, len);
        return w | W_STR | ((uint64_t)len << W_STR_LEN_SHIFT) | (noesc ? W_STR_NOESC : 0) | (rawnum ? W_STR_RAWNUM : 0);
    }
    const uint8_t flags = (uint8_t)(NK_STR | (noesc ? STR_NOESC : 0) | (rawnum ? STR_RAWNUM : 0));
    uint8_t *node = nodeAlloc(strNodeSize(len), acct);
    if (len <= STR_SHORT_MAX) {
        node[0] = flags;
        node[1] = (uint8_t)len;
        memcpy(node + STR_SHORT_HDR, s, len);
    } else {
        node[0] = flags | STR_LONG;
        storeU32(node + 1, len);
        memcpy(node + STR_LONG_HDR, s, len);
    }
    return nodeWord(node);
}

static uint64_t emitBoxed(uint8_t sub, uint64_t bits, nodeAcct *acct) {
    uint8_t *node = nodeAlloc(BOXNUM_SIZE, acct);
    memset(node, 0, BOXNUM_VALUE);
    node[0] = NK_BOXNUM;
    node[1] = sub;
    storeU64(node + BOXNUM_VALUE, bits);
    return nodeWord(node);
}

static inline uint64_t emitInt(int64_t v, nodeAcct *acct) {
    if (v >= W_INT_MIN && v <= W_INT_MAX) return ((uint64_t)v << W_INT_SHIFT) | W_INT;
    return emitBoxed(BOXNUM_INT, (uint64_t)v, acct);
}

static inline uint64_t emitDouble(uint64_t bits, nodeAcct *acct) {
    if (bits == 0) return W_PZERO;
    if (bits == (uint64_t)1 << 63) return W_NZERO;
    const uint64_t w = (bits << 3) | (bits >> 61);
    return wordIsDouble(w) ? w : emitBoxed(BOXNUM_DOUBLE, bits, acct);
}

/* A decimal: in the word when it holds one, else boxed. */
static inline uint64_t emitDecimal(int64_t mantissa, uint32_t scale, nodeAcct *acct) {
    if (JSON_LIKELY(wordDecFits(mantissa, scale))) return wordDec(mantissa, scale);
    const uint64_t w = emitBoxed(BOXNUM_DEC, (uint64_t)mantissa, acct);
    wordNode(w)[BOXNUM_SCALE] = (uint8_t)scale;
    return w;
}

static uint64_t emitScalar(const jsonVal *P, uint32_t idx, nodeAcct *acct) {
    const uint64_t *t = P->tape;
    const uint64_t e = t[idx];
    const uint8_t *s;
    uint32_t len;
    switch (tapeType(e)) {
    case T_NULL: return W_NULL;
    case T_FALSE: return W_FALSE;
    case T_TRUE: return W_TRUE;
    case T_INT:
    case T_INT64: return emitInt(tapeInt(t, idx), acct);
    case T_DBL: return emitDouble(t[idx + 1], acct);
    case T_STR:
        s = tapeText(P, idx, &len);
        return emitText(s, len, (e & TF_FLAG) != 0, 0, acct);
    case T_DEC: return emitDecimal(tapeDecMantissa(t, idx), tapeDecScale(e), acct);
    default: /* T_RAWNUM */
        s = tapeText(P, idx, &len);
        return emitText(s, len, 1, 1, acct);
    }
}

/* One element of a string array. */
static inline void ekStoreString(uint8_t *p, const uint8_t *s, uint32_t len, int noesc) {
    memset(p, 0, EK_STR_SIZE);
    p[0] = (uint8_t)(len | (noesc ? EK_STR_NOESC : 0));
    memcpy(p + 1, s, len);
}

static uint64_t emitTypedArray(const jsonVal *P, uint32_t idx, nodeAcct *acct) {
    const uint64_t *t = P->tape;
    const uint32_t count = tapeCount(t[idx]), ekind = tapeEkind(t[idx]);
    uint8_t *node = nodeAlloc(TARRAY_HDR + (uint64_t)count * EK_WIDTH[ekind], acct);
    memset(node, 0, TARRAY_HDR);
    node[0] = NK_TARRAY;
    node[TARRAY_EKIND] = (uint8_t)ekind;
    node[TARRAY_SCALE] = tapeScale(t[idx]);
    storeU32(node + SPINE_LEN, count);
    storeU32(node + SPINE_CAP, count);
    if (ekind == EK_LIT) {
        /* One entry each, and the tape numbers the three as the packed tags do. */
        for (uint32_t c = 0; c < count; c++) node[TARRAY_HDR + c] = (uint8_t)tapeType(t[idx + T_CONTAINER_ENTRIES + c]);
        return nodeWord(node);
    }
    if (ekind != EK_STR) {
        packTypedElements(node + TARRAY_HDR, t, idx + T_CONTAINER_ENTRIES, count, ekind);
        return nodeWord(node);
    }
    uint32_t j = idx + T_CONTAINER_ENTRIES, len;
    for (uint32_t c = 0; c < count; c++, j++) {
        const uint8_t *s = tapeText(P, j, &len); /* At most 15 bytes: one entry each. */
        ekStoreString(node + TARRAY_HDR + (size_t)c * EK_STR_SIZE, s, len, (t[j] & TF_FLAG) != 0);
    }
    return nodeWord(node);
}

static inline void objectIndexSet(uint8_t *index, uint32_t width, uint32_t i, uint32_t v) {
    if (width == 1) {
        index[i] = (uint8_t)v;
    } else if (width == 2) {
        storeU16(index + (size_t)i * 2, (uint16_t)v);
    } else {
        storeU32(index + (size_t)i * 4, v);
    }
}

static void objectIndexInsert(uint8_t *node, uint32_t keyid, uint32_t entry) {
    uint8_t *index = node + (objectIndex(node) - node);
    const uint32_t width = node[OBJECT_IDXW], lg = node[OBJECT_LG], mask = ((uint32_t)1 << lg) - 1;
    uint32_t x = objectHome(keyid, lg);
    while (objectIndexGet(index, width, x) != OBJECT_NO_ENTRY) x = (x + 1) & mask;
    objectIndexSet(index, width, x, entry);
}

/* An empty ARRAY or OBJECT node sized for the container at tape[idx], with
 * its length already set: the emitter fills every slot. */
static uint8_t *emitSpineNode(const jsonVal *P, uint32_t idx, nodeAcct *acct) {
    const uint64_t e = P->tape[idx];
    const uint32_t count = tapeCount(e);
    uint8_t *node;
    if (tapeType(e) == T_ARR) {
        node = nodeAlloc(SPINE_HDR + (uint64_t)count * sizeof(uint64_t), acct);
        memset(node, 0, SPINE_HDR);
        node[0] = NK_ARRAY;
    } else {
        const uint32_t lg = objectIndexLg(count), width = objectIndexWidth(count);
        node = nodeAlloc(objectNodeSize(count), acct);
        memset(node, 0, SPINE_HDR);
        node[0] = NK_OBJECT;
        node[OBJECT_IDXW] = (uint8_t)width;
        node[OBJECT_LG] = (uint8_t)lg;
        storeU32(node + OBJECT_LIVE, count);
        memset(node + SPINE_HDR + (size_t)count * OBJECT_ENTRY, 0xFF, (size_t)((uint64_t)1 << lg) * width);
    }
    storeU32(node + SPINE_LEN, count);
    storeU32(node + SPINE_CAP, count);
    return node;
}

/* 1 if the value at idx has to become an ARRAY or OBJECT node. */
static inline int tapeNeedsSpine(const uint64_t *t, uint32_t idx) {
    return tapeIsContainer(t[idx]) && !tapeFits(t, idx) && !tapeEkind(t[idx]);
}

/* Emits a value that is a single word or a single node. */
static uint64_t emitLeaf(const jsonVal *P, uint32_t idx, nodeAcct *acct) {
    const uint64_t *t = P->tape;
    if (!tapeIsContainer(t[idx])) return emitScalar(P, idx, acct);
    if (tapeFits(t, idx)) return emitPacked(P, idx, acct);
    return emitTypedArray(P, idx, acct);
}

static void emitPush(jsonVal *P, uint32_t *sp, uint8_t *node, uint32_t idx) {
    if (*sp == P->ecap) {
        P->ecap = P->ecap ? P->ecap * 2 : 16;
        P->estk = engGrow(P->estk, engSize((uint64_t)sizeof(emitFrame) * P->ecap));
    }
    emitFrame *f = &P->estk[(*sp)++];
    f->node = node;
    f->i = idx + T_CONTAINER_ENTRIES;
    f->end = idx + tapeSpan(P->tape, idx);
    f->n = 0;
}

/* Emits a container that does not fit the caps: ARRAY and OBJECT nodes whose
 * children are words, packed nodes, typed arrays or further spine nodes. */
static uint64_t emitSpine(jsonVal *P, uint32_t root_idx, nodeAcct *acct) {
    const uint64_t *t = P->tape;
    uint8_t *root = emitSpineNode(P, root_idx, acct);
    uint32_t sp = 0;
    emitPush(P, &sp, root, root_idx);
    while (sp) {
        emitFrame *f = &P->estk[sp - 1];
        if (f->i == f->end) {
            sp--;
            continue;
        }
        uint8_t *node = f->node;
        const int is_obj = nodeKind(node) == NK_OBJECT;
        const uint32_t keyid = is_obj ? tapeHigh(t[f->i++]) : 0;
        const uint32_t vi = f->i, n = f->n++;
        f->i += tapeSpan(t, vi);
        uint8_t *child = tapeNeedsSpine(t, vi) ? emitSpineNode(P, vi, acct) : NULL;
        const uint64_t w = child ? nodeWord(child) : emitLeaf(P, vi, acct);
        if (is_obj) {
            /* The key entry's reference moves into the node. */
            nodeWords(node)[(size_t)n * 2] = wordKey(keyid);
            nodeWords(node)[(size_t)n * 2 + 1] = w;
            objectIndexInsert(node, keyid, n);
        } else {
            nodeWords(node)[n] = w;
        }
        if (child) emitPush(P, &sp, child, vi);
    }
    return nodeWord(root);
}

/* Emits the whole tape as a document. */
static jsonDoc emitRoot(jsonVal *P) {
    if (!tapeNeedsSpine(P->tape, 0)) return emitLeaf(P, 0, NULL);
    nodeAcct acct = {0, 0};
    const uint64_t w = emitSpine(P, 0, &acct);
    storeU64(wordNode(w) + SPINE_NODES, acct.nodes);
    storeU64(wordNode(w) + SPINE_BYTES, acct.bytes);
    return w;
}

/* ---- Values ---- */

static jsonVal *tapeFail(jsonVal *P, jsonErr code, size_t off, jsonErr *err, size_t *err_off) {
    if (P) {
        tapeDropRefs(P, 0, P->tlen);
        tapeRelease(P);
    }
    if (err) *err = code;
    if (err_off) *err_off = off;
    return NULL;
}

/* 1 if the tape of a text is sure to stay within TAPE_MAX_ENTRIES. A value or
 * a member name is the first thing in the text or follows one of '[', '{',
 * ',' and ':', and takes at most T_CONTAINER_ENTRIES entries, so that many
 * times the number of those bytes is a bound. Such a byte inside a string is
 * counted too, which only makes the bound larger. */
static int tapeEntriesFit(const uint8_t *s, size_t n) {
    uint64_t starts = 1;
    for (size_t i = 0; i < n; i++) starts += (s[i] == '[') | (s[i] == '{') | (s[i] == ',') | (s[i] == ':');
    return starts * T_CONTAINER_ENTRIES <= TAPE_MAX_ENTRIES;
}

jsonVal *jsonValParse(const char *s, size_t n, int flags, uint32_t max_depth, jsonErr *err, size_t *err_off) {
    if (n > TAPE_MAX_INPUT &&
        (!(flags & JSON_PARSE_TRUSTED) || n > TAPE_MAX_TEXT || !tapeEntriesFit((const uint8_t *)s, n))) {
        return tapeFail(NULL, JSON_ERR_TOO_BIG, 0, err, err_off);
    }
    jsonVal *P = tapeAcquire();
    const uint8_t *text = (const uint8_t *)s, *errp = text, *start = text;
    P->trusted = (flags & JSON_PARSE_TRUSTED) != 0;
    P->max_depth = max_depth;
    arenaReserve(P, n + 16);
    /* One UTF-8 byte order mark before the text is skipped, as the module's
     * reader skips it. Anywhere else it is an error like any stray byte. */
    if (n >= UTF8_BOM_LEN && memcmp(text, UTF8_BOM, UTF8_BOM_LEN) == 0) start += UTF8_BOM_LEN;
    const jsonErr code = tapeTokenise(P, start, text + n, &errp);
    if (code != JSON_OK) return tapeFail(P, code, (size_t)(errp - text), err, err_off);
    if (err) *err = JSON_OK;
    return P;
}

jsonVal *jsonValNull(void) {
    jsonVal *P = tapeAcquire();
    tapeSimple(P, T_NULL);
    return P;
}

jsonVal *jsonValBool(int v) {
    jsonVal *P = tapeAcquire();
    tapeSimple(P, v ? T_TRUE : T_FALSE);
    return P;
}

jsonVal *jsonValInt(int64_t v) {
    jsonVal *P = tapeAcquire();
    tapeInt64(P, v);
    return P;
}

jsonVal *jsonValDouble(double d) {
    jsonVal *P = tapeAcquire();
    tapeComputedDouble(P, d);
    return P;
}

jsonVal *jsonValNumText(const char *s, size_t n) {
    jsonVal *P = tapeAcquire();
    const uint8_t *text = (const uint8_t *)s;
    numToken k;
    const uint8_t *end = n && n <= TAPE_MAX_INPUT ? lexNumberToken(text, text + n, &k) : NULL;
    if (end != text + n) return tapeFail(P, JSON_ERR_SYNTAX, 0, NULL, NULL); /* Not a JSON number. */
    arenaReserve(P, n);
    tapeNumber(P, &k); /* A trusted tape: no range error. */
    return P;
}

jsonVal *jsonValString(const char *s, size_t n) {
    if (n > TAPE_MAX_TEXT) return NULL;
    jsonVal *P = tapeAcquire();
    arenaReserve(P, n);
    tapeString(P, (const uint8_t *)s, (uint32_t)n, bytesNoEscape((const uint8_t *)s, n));
    return P;
}

jsonVal *jsonValEmptyArray(void) {
    jsonVal *P = tapeAcquire();
    tapeOpen(P, 0, 1);
    tapeClose(P);
    return P;
}

jsonVal *jsonValEmptyObject(void) {
    jsonVal *P = tapeAcquire();
    tapeOpen(P, 1, 1);
    tapeClose(P);
    return P;
}

jsonType jsonValType(const jsonVal *v) {
    const uint64_t e = v->tape[0];
    switch (tapeType(e)) {
    case T_NULL: return JSON_NULL;
    case T_FALSE: return JSON_FALSE;
    case T_TRUE: return JSON_TRUE;
    case T_INT:
    case T_INT64: return JSON_INT;
    case T_DBL: return JSON_DOUBLE;
    case T_STR: return JSON_STRING;
    case T_ARR: return JSON_ARRAY;
    case T_OBJ: return JSON_OBJECT;
    default: return JSON_RAWNUM; /* T_RAWNUM, T_DEC. */
    }
}

uint32_t jsonValDepth(const jsonVal *v) {
    return v->depth;
}

size_t jsonValSizeEstimate(const jsonVal *v) {
    const uint64_t *t = v->tape;
    if (!tapeIsContainer(t[0])) {
        const uint64_t packed = tapePackedSize(v, 0);
        return packed < TAPE_NOFIT ? (size_t)packed : sizeSat(tapeSpineChildBytes(v, 0));
    }
    return tapeFits(t, 0) ? PACKED_HDR + (size_t)(uint32_t)t[1] : sizeSat(t[2]);
}

jsonDoc jsonValToDoc(jsonVal *v) {
    const jsonDoc d = emitRoot(v);
    v->tlen = 0; /* Every reference moved into the document. */
    tapeRelease(v);
    return d;
}

void jsonValFree(jsonVal *v) {
    if (!v) return;
    tapeDropRefs(v, 0, v->tlen);
    tapeRelease(v);
}

/* ============================================================================
 * 6. References and iteration
 *
 * A jsonRef names one value where it lies. Its private fields:
 *
 *   REF_PACKED  p = the packed value. w = the word of its PACKED node when
 *               the value is the whole node, else 0.
 *   REF_WORD    w = a spine word that is not a pointer to a PACKED node.
 *   REF_ELEM    p = one raw element of a typed array. b = element kind.
 *               sub = the scale of a decimal array.
 *
 * In all three, a = REF_IN_SLOT when the value occupies a slot of a spine
 * container (for jsonRefMemUsage).
 *
 * A decimal is reported as JSON_RAWNUM: a number that has a text of its own.
 * Unlike other raw numbers it holds no text, so the engine asks refDecimal
 * before it asks for the bytes of a raw number.
 * ========================================================================= */

enum {
    REF_NONE = 0,
    REF_PACKED,
    REF_WORD,
    REF_ELEM
};

#define REF_IN_SLOT 1

static inline void refFromPacked(jsonRef *r, const uint8_t *p) {
    r->p = p;
    r->w = 0;
    r->a = 0;
    r->b = 0;
    r->kind = REF_PACKED;
    r->sub = 0;
}

static inline void refFromWord(jsonRef *r, uint64_t w, uint32_t flags) {
    r->w = w;
    r->a = flags;
    r->b = 0;
    r->sub = 0;
    if (wordTag(w) == W_PTR && nodeKind(wordNode(w)) == NK_PACKED) {
        r->p = wordNode(w) + PACKED_HDR;
        r->kind = REF_PACKED;
    } else {
        r->p = NULL;
        r->kind = REF_WORD;
    }
}

static inline void refFromElem(jsonRef *r, const uint8_t *p, uint32_t ekind, uint8_t scale) {
    r->p = p;
    r->w = 0;
    r->a = 0;
    r->b = ekind;
    r->kind = REF_ELEM;
    r->sub = scale;
}

void jsonDocRoot(jsonDoc d, jsonRef *out) {
    if (d) {
        refFromWord(out, d, 0);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

/* The type of a packed value, by table and without a branch: which tag comes
 * next is decided by the data, true or false, number or string. The tags
 * below PK_INT0 have an entry each, and the rest one for each 32 tags. */
_Static_assert(PK_INT0 == 32 && PK_SSTR == 64 && PK_DEC == 192, "packedType reads the top three bits of a tag");
static inline jsonType packedType(uint8_t tag) {
    static const uint8_t TYPES[PK_INT0 + 8] = {
        [PK_NULL] = JSON_NULL,
        [PK_FALSE] = JSON_FALSE,
        [PK_TRUE] = JSON_TRUE,
        [PK_I8] = JSON_INT,
        [PK_I16] = JSON_INT,
        [PK_I32] = JSON_INT,
        [PK_I64] = JSON_INT,
        [PK_F64] = JSON_DOUBLE,
        [PK_STR16] = JSON_STRING,
        [PK_STR16_NE] = JSON_STRING,
        [PK_ARR8] = JSON_ARRAY,
        [PK_ARR16] = JSON_ARRAY,
        [PK_OBJ8] = JSON_OBJECT,
        [PK_OBJ16] = JSON_OBJECT,
        /* Typed arrays. */
        [PK_TARR] = JSON_ARRAY,
        [PK_TARR + 1] = JSON_ARRAY,
        [PK_TARR + 2] = JSON_ARRAY,
        [PK_TARR + 3] = JSON_ARRAY,
        [PK_TARR + 4] = JSON_ARRAY,
        [PK_TARR + 5] = JSON_ARRAY,
        [PK_TARR + 6] = JSON_ARRAY,
        [PK_TARR + 7] = JSON_ARRAY,
        [PK_TARR + 8] = JSON_ARRAY,
        [PK_TARR + 9] = JSON_ARRAY,
        [PK_TARR + 10] = JSON_ARRAY,
        [PK_TARR + 11] = JSON_ARRAY,
        [PK_TARR + 12] = JSON_ARRAY,
        [PK_TARR + 13] = JSON_ARRAY,
        [PK_TARR + 14] = JSON_ARRAY,
        [PK_TARR + 15] = JSON_ARRAY,
        [PK_TARR + 16] = JSON_ARRAY,
        [PK_TARR + 17] = JSON_ARRAY,
        /* PK_INT0, the two ranges of short strings, then decimals and number text. */
        [PK_INT0 + 1] = JSON_INT,
        [PK_INT0 + 2] = JSON_STRING,
        [PK_INT0 + 3] = JSON_STRING,
        [PK_INT0 + 4] = JSON_STRING,
        [PK_INT0 + 5] = JSON_STRING,
        [PK_INT0 + 6] = JSON_RAWNUM,
        [PK_INT0 + 7] = JSON_RAWNUM,
    };
    return (jsonType)TYPES[tag < PK_INT0 ? tag : PK_INT0 + (tag >> 5)];
}

static jsonType wordType(uint64_t w) {
    switch (wordTag(w)) {
    case W_PTR: {
        const uint8_t *node = wordNode(w);
        switch (nodeKind(node)) {
        case NK_STR: return (node[0] & STR_RAWNUM) ? JSON_RAWNUM : JSON_STRING;
        case NK_BOXNUM: return node[1] == BOXNUM_INT ? JSON_INT : node[1] == BOXNUM_DEC ? JSON_RAWNUM
                                                                                        : JSON_DOUBLE;
        case NK_OBJECT: return JSON_OBJECT;
        default: return JSON_ARRAY;
        }
    }
    case W_INT: return JSON_INT;
    case W_STR: return (w & W_STR_RAWNUM) ? JSON_RAWNUM : JSON_STRING;
    case W_SPECIAL:
        switch (w & W_SPECIAL_MASK) {
        case W_NULL: return JSON_NULL;
        case W_FALSE: return JSON_FALSE;
        case W_TRUE: return JSON_TRUE;
        default: return JSON_DOUBLE; /* +0.0 or -0.0 */
        }
    case W_DEC_POS:
    case W_DEC_NEG: return JSON_RAWNUM;
    default: return JSON_DOUBLE;
    }
}

static jsonType elemType(uint32_t ekind, const uint8_t *elem) {
    if (ekIsInt(ekind)) return JSON_INT;
    if (ekind == EK_LIT) return (jsonType)*elem; /* PK_NULL, PK_FALSE and PK_TRUE are numbered as the types. */
    if (ekind == EK_F64) return JSON_DOUBLE;
    return ekind == EK_STR ? JSON_STRING : JSON_RAWNUM;
}

jsonType jsonRefType(const jsonRef *r) {
    switch (r->kind) {
    case REF_PACKED: return packedType(*r->p);
    case REF_WORD: return wordType(r->w);
    case REF_ELEM: return elemType(r->b, r->p);
    default: return JSON_NULL;
    }
}

/* The heap node of a REF_WORD, or NULL when the value is in the word. */
static inline const uint8_t *refNode(const jsonRef *r) {
    return (r->kind == REF_WORD && wordTag(r->w) == W_PTR) ? wordNode(r->w) : NULL;
}

uint32_t jsonRefCount(const jsonRef *r) {
    if (r->kind == REF_PACKED) {
        const uint8_t tag = *r->p;
        const uint8_t *end;
        uint32_t count, ekind;
        uint8_t scale;
        if (pkIsNested(tag)) {
            pkOpen(r->p, &end, &count);
            return count;
        }
        if (pkIsTyped(tag)) {
            pkTypedOpen(r->p, &ekind, &count, &scale);
            return count;
        }
        return 0;
    }
    const uint8_t *node = refNode(r);
    if (!node) return 0;
    switch (nodeKind(node)) {
    case NK_ARRAY:
    case NK_TARRAY: return nodeLen(node);
    case NK_OBJECT: return loadU32(node + OBJECT_LIVE);
    default: return 0;
    }
}

int jsonRefInt(const jsonRef *r, int64_t *out) {
    const uint8_t *node;
    switch (r->kind) {
    case REF_PACKED:
        if (!pkIsInt(*r->p)) return 0;
        *out = pkInt(r->p);
        return 1;
    case REF_WORD:
        if (wordTag(r->w) == W_INT) {
            *out = (int64_t)r->w >> W_INT_SHIFT;
            return 1;
        }
        node = refNode(r);
        if (!node || nodeKind(node) != NK_BOXNUM || node[1] != BOXNUM_INT) return 0;
        *out = (int64_t)loadU64(node + BOXNUM_VALUE);
        return 1;
    case REF_ELEM:
        if (!ekIsInt(r->b)) return 0;
        *out = ekInt(r->p, r->b);
        return 1;
    default: return 0;
    }
}

/* 1 if the value is a decimal, with its digits and scale. */
static int refDecimal(const jsonRef *r, int64_t *mantissa, uint32_t *scale) {
    const uint8_t *node;
    switch (r->kind) {
    case REF_PACKED:
        if (!pkIsDec(*r->p)) return 0;
        *mantissa = pkDecimal(r->p, scale);
        return 1;
    case REF_WORD:
        if (wordIsDec(r->w)) {
            *mantissa = wordDecimal(r->w, scale);
            return 1;
        }
        node = refNode(r);
        if (!node || nodeKind(node) != NK_BOXNUM || node[1] != BOXNUM_DEC) return 0;
        *mantissa = (int64_t)loadU64(node + BOXNUM_VALUE);
        *scale = node[BOXNUM_SCALE];
        return 1;
    case REF_ELEM:
        if (!ekIsDec(r->b)) return 0;
        *mantissa = ekInt(r->p, r->b);
        *scale = r->sub;
        return 1;
    default: return 0;
    }
}

/* The bytes of a string, or of a raw number that is not a decimal. */
static const uint8_t *refText(const jsonRef *r, uint32_t *len) {
    if (r->kind == REF_PACKED) return pkText(r->p, len);
    if (r->kind == REF_ELEM) {
        *len = r->p[0] & EK_STR_LEN_MASK;
        return r->p + 1;
    }
    if (wordTag(r->w) == W_STR) {
        *len = (uint32_t)(r->w >> W_STR_LEN_SHIFT) & W_STR_MAX;
        return wordStrBytes(&r->w);
    }
    return strNodeBytes(wordNode(r->w), len);
}

/* 1 if a string value is known to need no escaping on output. */
static int refNoEscape(const jsonRef *r) {
    if (r->kind == REF_PACKED) return (*r->p >= PK_SSTR_NE && *r->p < PK_DEC) || *r->p == PK_STR16_NE;
    if (r->kind == REF_ELEM) return (r->p[0] & EK_STR_NOESC) != 0;
    if (wordTag(r->w) == W_STR) return (r->w & W_STR_NOESC) != 0;
    return (wordNode(r->w)[0] & STR_NOESC) != 0;
}

/* The value of a JSON_DOUBLE. */
static double refDoubleValue(const jsonRef *r) {
    if (r->kind == REF_PACKED) return loadF64(r->p + 1);
    if (r->kind == REF_ELEM) return loadF64(r->p);
    if (wordIsDouble(r->w)) return wordDouble(r->w);
    if (wordTag(r->w) == W_SPECIAL) return (r->w & W_SPECIAL_MASK) == W_NZERO ? -0.0 : 0.0;
    return loadF64(wordNode(r->w) + BOXNUM_VALUE);
}

/* The value of a decimal beyond the reach of one exact division. Out of
 * line, so that the common case stays a leaf. */
static __attribute__((noinline)) double decimalValueLong(int64_t mantissa, uint32_t scale) {
    return jsonDoubleFromDecimal(decMagnitude(mantissa), -(int)scale, mantissa < 0);
}

/* The value of a decimal, correctly rounded. When its digits are within
 * 2^53 and the power of ten is at most 10^22, both are exact doubles and the
 * one division rounds once. A decimal beyond that goes to the number library:
 * the division would round twice there, and can be one unit off. */
#define DEC_EXACT_SCALE_MAX 22
static inline double decimalValue(int64_t mantissa, uint32_t scale) {
    static const double POW10[DEC_EXACT_SCALE_MAX + 1] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                                          1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    /* |M| <= 2^53, in one comparison. Where doubles are evaluated in a wider
     * format the division rounds twice, once to that format and once to a
     * double, so the number library does all of them there. */
    if (JSON_DOUBLE_EXACT &&
        JSON_LIKELY((uint64_t)mantissa + ((uint64_t)1 << 53) <= (uint64_t)1 << 54 && scale <= DEC_EXACT_SCALE_MAX)) {
        return (double)mantissa / POW10[scale];
    }
    return decimalValueLong(mantissa, scale);
}

static const char DIGIT_PAIRS[201] = "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
                                     "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

static const uint64_t POW10_U64[20] = {1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
                                       100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
                                       1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
                                       1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
                                       1000000000000000000ULL, 10000000000000000000ULL};

/* The number of decimal digits of v; one for zero. 1233 / 4096 is log10(2)
 * closely enough for every bit length up to 64: t is the number of digits or
 * one less, and the comparison says which. */
static JSON_INLINE uint32_t decimalDigits(uint64_t v) {
    const uint32_t t = ((64 - (uint32_t)__builtin_clzll(v | 1)) * 1233) >> 12;
    return t + (t == 0 || v >= POW10_U64[t]);
}

/* Writes v, below 10^n, as exactly n digits with leading zeros, two digits
 * at a time. */
static JSON_INLINE void printDigits(char *o, uint64_t v, uint32_t n) {
    char *end = o + n;
    for (; n >= 2; n -= 2) {
        const uint64_t q = v / 100;
        end -= 2;
        memcpy(end, DIGIT_PAIRS + (v - q * 100) * 2, 2);
        v = q;
    }
    if (n) end[-1] = (char)('0' + v);
}

/* The same for long numbers: eight digits at a time while there are that
 * many, since the four pairs of a block do not wait for one another as the
 * steps of a single chain of divisions do. */
static JSON_INLINE void printLongDigits(char *o, uint64_t v, uint32_t n) {
    for (; n >= 8; n -= 8) {
        const uint64_t q = v / 100000000;
        const uint32_t low = (uint32_t)(v - q * 100000000);
        const uint32_t hi = low / 10000, lo = low - hi * 10000;
        const uint32_t a = hi / 100, c = lo / 100;
        char *const at = o + n - 8;
        memcpy(at, DIGIT_PAIRS + a * 2, 2);
        memcpy(at + 2, DIGIT_PAIRS + (hi - a * 100) * 2, 2);
        memcpy(at + 4, DIGIT_PAIRS + c * 2, 2);
        memcpy(at + 6, DIGIT_PAIRS + (lo - c * 100) * 2, 2);
        v = q;
    }
    printDigits(o, v, n);
}

/* Writes an unsigned integer in decimal and returns the end. */
static JSON_INLINE char *printU64(char *o, uint64_t v) {
    const uint32_t n = decimalDigits(v);
    printLongDigits(o, v, n);
    return o + n;
}

static JSON_INLINE char *printI64(char *o, int64_t v) {
    if (v < 0) {
        *o++ = '-';
        return printU64(o, 0 - (uint64_t)v);
    }
    return printU64(o, (uint64_t)v);
}

/* The text of a decimal of any scale, m being |M|. Out of line: see
 * printShortDecimal. */
_Static_assert(DEC_SCALE_MAX - DEC_DIGITS_MAX <= 16, "printLongDecimal writes the zeros as one 16-byte block");
static char *printLongDecimal(char *o, uint64_t m, uint32_t scale) {
    if (scale >= DEC_DIGITS_MAX) {
        /* Nothing before the point, and zeros between it and the digits:
         * at most 13, written as 16 within the room every caller keeps for
         * a number, of which the digits overwrite the rest. */
        *o++ = '0';
        *o++ = '.';
        memcpy(o, "0000000000000000", 16);
        o += scale - DEC_DIGITS_MAX;
        printLongDigits(o, m, DEC_DIGITS_MAX);
        return o + DEC_DIGITS_MAX;
    }
    if (m < POW10_U64[scale]) {
        /* Below 1, as nearly every full-precision coordinate or embedding
         * component is: no division. */
        *o++ = '0';
        *o++ = '.';
        printLongDigits(o, m, scale);
        return o + scale;
    }
    const uint64_t whole = m / POW10_U64[scale];
    o = printU64(o, whole);
    *o++ = '.';
    printLongDigits(o, m - whole * POW10_U64[scale], scale);
    return o + scale;
}

/* The text of a decimal with a scale below 8, m being |M|: a few instructions
 * in the loops that print prices and coordinates. */
#define DEC_SHORT_SCALES 8
static JSON_INLINE char *printShortDecimal(char *o, uint64_t m, uint32_t scale) {
    const uint64_t whole = m / POW10_U64[scale];
    const uint32_t n = decimalDigits(whole);
    printDigits(o, whole, n);
    o += n;
    *o++ = '.';
    printDigits(o, m - whole * POW10_U64[scale], scale);
    return o + scale;
}

/* Writes the text of a decimal: its digits with the point `scale` digits
 * from the right, and a zero before the point when nothing else is there. At
 * most DEC_TEXT_MAX bytes: a sign, "0." and DEC_SCALE_MAX digits. Returns the
 * end. */
#define DEC_TEXT_MAX (3 + DEC_SCALE_MAX)
static inline char *printDecimal(char *o, int64_t mantissa, uint32_t scale) {
    uint64_t m = (uint64_t)mantissa;
    if (mantissa < 0) {
        *o++ = '-';
        m = 0 - m;
    }
    return scale < DEC_SHORT_SCALES ? printShortDecimal(o, m, scale) : printLongDecimal(o, m, scale);
}

double jsonRefDouble(const jsonRef *r) {
    int64_t iv;
    uint32_t len, scale;
    int overflow;
    switch (jsonRefType(r)) {
    case JSON_INT:
        jsonRefInt(r, &iv);
        return doubleRound((double)iv);
    case JSON_DOUBLE: return refDoubleValue(r);
    case JSON_RAWNUM: {
        if (refDecimal(r, &iv, &scale)) return decimalValue(iv, scale);
        const uint8_t *s = refText(r, &len);
        return jsonDoubleParse((const char *)s, len, &overflow);
    }
    default: return 0.0;
    }
}

const char *jsonRefString(const jsonRef *r, uint32_t *len) {
    if (jsonRefType(r) != JSON_STRING) {
        *len = 0;
        return NULL;
    }
    return (const char *)refText(r, len);
}

const char *jsonRefNumText(const jsonRef *r, char *buf, uint32_t *len) {
    int64_t iv;
    uint32_t scale;
    switch (jsonRefType(r)) {
    case JSON_INT:
        jsonRefInt(r, &iv);
        *len = (uint32_t)(printI64(buf, iv) - buf);
        return buf;
    case JSON_DOUBLE:
        *len = (uint32_t)jsonDoublePrint(refDoubleValue(r), buf);
        return buf;
    case JSON_RAWNUM:
        if (!refDecimal(r, &iv, &scale)) return (const char *)refText(r, len);
        *len = (uint32_t)(printDecimal(buf, iv, scale) - buf);
        return buf;
    default:
        *len = 0;
        return buf;
    }
}

int jsonArrayGet(const jsonRef *arr, uint32_t index, jsonRef *out) {
    uint32_t count, ekind;
    uint8_t scale;
    if (arr->kind == REF_PACKED) {
        const uint8_t tag = *arr->p;
        if (pkIsArray(tag)) {
            const uint8_t *end, *p = pkOpen(arr->p, &end, &count);
            if (index >= count) return 0;
            for (uint32_t i = 0; i < index; i++) p = pkSkip(p);
            refFromPacked(out, p);
            return 1;
        }
        if (pkIsTyped(tag)) {
            const uint8_t *elems = pkTypedOpen(arr->p, &ekind, &count, &scale);
            if (index >= count) return 0;
            refFromElem(out, elems + (size_t)index * EK_WIDTH[ekind], ekind, scale);
            return 1;
        }
        return 0;
    }
    const uint8_t *node = refNode(arr);
    if (!node || index >= nodeLen(node)) return 0;
    if (nodeKind(node) == NK_ARRAY) {
        refFromWord(out, nodeWords(node)[index], REF_IN_SLOT);
        return 1;
    }
    if (nodeKind(node) == NK_TARRAY) {
        ekind = node[TARRAY_EKIND];
        refFromElem(out, tarrayData(node) + (size_t)index * EK_WIDTH[ekind], ekind, node[TARRAY_SCALE]);
        return 1;
    }
    return 0;
}

/* The entry of a member in an OBJECT node, or OBJECT_NO_ENTRY. */
static uint32_t objectFind(const uint8_t *node, uint32_t keyid) {
    const uint8_t *index = objectIndex(node);
    const uint64_t *entries = nodeWords(node), key = wordKey(keyid);
    const uint32_t width = node[OBJECT_IDXW], lg = node[OBJECT_LG], mask = ((uint32_t)1 << lg) - 1;
    for (uint32_t x = objectHome(keyid, lg);; x = (x + 1) & mask) {
        const uint32_t e = objectIndexGet(index, width, x);
        if (e == OBJECT_NO_ENTRY || entries[(size_t)e * 2] == key) return e;
    }
}

static inline int stepFill(jsonStep *step, uint32_t pos, uint32_t keyid) {
    if (step) {
        step->pos = pos;
        step->keyid = keyid;
    }
    return 1;
}

int jsonObjectGetById(const jsonRef *obj, uint32_t keyid, jsonRef *out, jsonStep *step) {
    if (!keyid) return 0;
    if (obj->kind == REF_PACKED) {
        if (!pkIsObject(*obj->p)) return 0;
        const uint8_t *end;
        uint32_t count, id, pos = 0;
        /* Keys and values are interleaved: scan, stepping over each value. */
        for (const uint8_t *p = pkOpen(obj->p, &end, &count); p < end; p = pkSkip(p), pos++) {
            p = varintGet(p, &id);
            if (id != keyid) continue;
            refFromPacked(out, p);
            return stepFill(step, pos, keyid);
        }
        return 0;
    }
    const uint8_t *node = refNode(obj);
    if (!node || nodeKind(node) != NK_OBJECT) return 0;
    const uint32_t entry = objectFind(node, keyid);
    if (entry == OBJECT_NO_ENTRY) return 0;
    refFromWord(out, nodeWords(node)[(size_t)entry * 2 + 1], REF_IN_SLOT);
    return stepFill(step, entry, keyid);
}

int jsonObjectGet(const jsonRef *obj, const char *name, size_t len, jsonRef *out, jsonStep *step) {
    if (len > KT_LEN_MASK) return 0;
    /* A name that is not in the key table is in no document. */
    const uint32_t keyid = ktLookup((const uint8_t *)name, (uint32_t)len, ktHash((const uint8_t *)name, len));
    return jsonObjectGetById(obj, keyid, out, step);
}

int jsonChildByStep(const jsonRef *container, const jsonStep *step, jsonRef *out) {
    if (step->keyid) return jsonObjectGetById(container, step->keyid, out, NULL);
    return jsonArrayGet(container, step->pos, out);
}

/* The child that a step names in the packed array or object at p, or NULL. */
static inline const uint8_t *pkChild(const uint8_t *p, const jsonStep *step) {
    const int is_obj = pkIsObject(*p);
    const uint8_t *end;
    uint32_t count, id;
    p = pkOpen(p, &end, &count);
    if (is_obj) {
        if (!step->keyid) return NULL;
        while (p < end) {
            p = varintGet(p, &id);
            if (id == step->keyid) return p;
            p = pkSkip(p);
        }
        return NULL;
    }
    if (step->keyid || step->pos >= count) return NULL;
    for (uint32_t i = 0; i < step->pos; i++) p = pkSkip(p);
    return p;
}

int jsonDocResolve(jsonDoc d, const jsonStep *loc, int n, jsonRef *out) {
    jsonRef cur, next;
    int i = 0;
    if (!d) return 0;
    jsonDocRoot(d, &cur);
    while (i < n) {
        if (cur.kind == REF_PACKED && pkIsNested(*cur.p)) {
            /* Inside one packed node a plain pointer is all the state there
             * is: take every step that stays inside it. */
            const uint8_t *p = cur.p;
            do {
                p = pkChild(p, &loc[i]);
                if (!p) return 0;
                i++;
            } while (i < n && pkIsNested(*p));
            refFromPacked(&cur, p);
            continue;
        }
        if (!jsonChildByStep(&cur, &loc[i], &next)) return 0;
        cur = next;
        i++;
    }
    *out = cur;
    return 1;
}

/* Iterator kinds and the private fields each uses:
 *
 *   IT_PACKED_ARR  p = next value, end. i = its index.
 *   IT_PACKED_OBJ  p = next key, end. i = its position.
 *   IT_ELEMS       p = first element of a typed array. i, n. a = element
 *                  kind. sub = scale of a decimal array.
 *   IT_NODE_ARR    base = the ARRAY node. i, n.
 *   IT_NODE_OBJ    base = the OBJECT node. i = next entry. n = entries used. */
enum {
    IT_NONE = 0,
    IT_PACKED_ARR,
    IT_PACKED_OBJ,
    IT_ELEMS,
    IT_NODE_ARR,
    IT_NODE_OBJ
};

void jsonIterInit(jsonIter *it, const jsonRef *container) {
    uint32_t count = 0, ekind = 0;
    uint8_t scale = 0;
    memset(it, 0, sizeof(*it));
    if (container->kind == REF_PACKED) {
        const uint8_t tag = *container->p;
        if (pkIsNested(tag)) {
            it->p = pkOpen(container->p, &it->end, &count);
            it->kind = pkIsObject(tag) ? IT_PACKED_OBJ : IT_PACKED_ARR;
        } else if (pkIsTyped(tag)) {
            it->p = pkTypedOpen(container->p, &ekind, &count, &scale);
            it->kind = IT_ELEMS;
        }
    } else {
        const uint8_t *node = refNode(container);
        if (!node) return;
        switch (nodeKind(node)) {
        case NK_ARRAY: it->kind = IT_NODE_ARR; break;
        case NK_OBJECT: it->kind = IT_NODE_OBJ; break;
        case NK_TARRAY:
            it->p = tarrayData(node);
            ekind = node[TARRAY_EKIND];
            scale = node[TARRAY_SCALE];
            it->kind = IT_ELEMS;
            break;
        default: return;
        }
        it->base = nodeWord(node);
        count = nodeLen(node);
    }
    it->n = count;
    it->a = ekind;
    it->sub = scale;
}

int jsonIterNext(jsonIter *it, jsonRef *val, jsonStep *step, const char **name, uint32_t *name_len) {
    uint32_t keyid = 0, pos = it->i;
    switch (it->kind) {
    case IT_PACKED_ARR:
        if (it->p == it->end) return 0;
        refFromPacked(val, it->p);
        it->p = pkSkip(it->p);
        break;
    case IT_PACKED_OBJ:
        if (it->p == it->end) return 0;
        it->p = varintGet(it->p, &keyid);
        refFromPacked(val, it->p);
        it->p = pkSkip(it->p);
        break;
    case IT_ELEMS:
        if (it->i == it->n) return 0;
        refFromElem(val, it->p + (size_t)it->i * EK_WIDTH[it->a], it->a, it->sub);
        break;
    case IT_NODE_ARR:
        if (it->i == it->n) return 0;
        refFromWord(val, nodeWords(wordNode(it->base))[it->i], REF_IN_SLOT);
        break;
    case IT_NODE_OBJ: {
        const uint64_t *entries = nodeWords(wordNode(it->base));
        while (it->i < it->n && entries[(size_t)it->i * 2] == 0) it->i++; /* Removed members. */
        if (it->i == it->n) return 0;
        pos = it->i;
        keyid = wordKeyId(entries[(size_t)pos * 2]);
        refFromWord(val, entries[(size_t)pos * 2 + 1], REF_IN_SLOT);
        break;
    }
    default: return 0;
    }
    it->i++;
    stepFill(step, pos, keyid);
    if (name && keyid) {
        const keyInfo *ki = ktInfo(keyid);
        *name = (const char *)ki->bytes;
        if (name_len) *name_len = ktInfoLen(ki);
    }
    return 1;
}

/* ============================================================================
 * 7. Document to tape
 *
 * Copies a value that lies in a document onto a tape. Every key copied takes
 * a new reference: the document keeps its own (design 6.3).
 * ========================================================================= */

#define WALK_INLINE_FRAMES 16

static inline int typeIsContainer(jsonType type) {
    return type >= JSON_ARRAY;
}

static void tapeAppendScalar(jsonVal *P, const jsonRef *r) {
    const uint8_t *s;
    uint32_t len, scale;
    int64_t iv;
    switch (jsonRefType(r)) {
    case JSON_NULL: tapeSimple(P, T_NULL); break;
    case JSON_FALSE: tapeSimple(P, T_FALSE); break;
    case JSON_TRUE: tapeSimple(P, T_TRUE); break;
    case JSON_INT:
        jsonRefInt(r, &iv);
        tapeInt64(P, iv);
        break;
    case JSON_DOUBLE: tapeDouble(P, refDoubleValue(r)); break;
    case JSON_RAWNUM:
        if (refDecimal(r, &iv, &scale)) {
            tapeDecimal(P, iv, scale);
            break;
        }
        s = refText(r, &len);
        arenaReserve(P, len);
        tapeRawNum(P, s, len);
        break;
    default: /* JSON_STRING */
        s = refText(r, &len);
        arenaReserve(P, len);
        tapeString(P, s, len, refNoEscape(r));
    }
}

/* Takes a new reference for every key entry of a value, after an emit moved
 * the tape's references into a document: the value can then be emitted
 * again. Together the two amount to emitting a copy (design 6.3). */
static void tapeRetainRefs(const jsonVal *v) {
    const uint64_t *t = v->tape;
    for (uint32_t i = 0; i < v->tlen; i += tapeEntryWidth(t[i])) {
        if (tapeType(t[i]) == T_KEYID) ktIncr(tapeHigh(t[i]));
    }
}

/* Appends a copy of a whole value from another tape at the current position.
 * Every key copied takes a new reference. */
static void tapeAppendTape(jsonVal *P, const jsonVal *v) {
    const uint64_t *t = v->tape;
    const uint64_t e = t[0];
    const uint8_t *text;
    uint32_t len;
    switch (tapeType(e)) {
    case T_NULL:
    case T_FALSE:
    case T_TRUE: tapeSimple(P, (int)tapeType(e)); return;
    case T_INT:
    case T_INT64: tapeInt64(P, tapeInt(t, 0)); return;
    case T_DBL: tapeDouble(P, bitsDouble(t[1])); return;
    case T_DEC: tapeDecimal(P, tapeDecMantissa(t, 0), tapeDecScale(e)); return;
    case T_STR:
        text = tapeText(v, 0, &len);
        arenaReserve(P, len);
        tapeString(P, text, len, (e & TF_FLAG) != 0);
        return;
    case T_RAWNUM:
        text = tapeText(v, 0, &len);
        arenaReserve(P, len);
        tapeRawNum(P, text, len);
        return;
    default: break;
    }
    /* A container. It is closed, so its entries are copied as they are, with
     * the arena offsets of its strings moved to where its arena lands. */
    const uint64_t base = (uint64_t)P->alen << T_HIGH_SHIFT;
    if (v->alen) {
        arenaReserve(P, v->alen);
        memcpy(P->arena + P->alen, v->arena, v->alen);
        P->alen += v->alen;
    }
    tapeNeed(P, v->tlen);
    uint64_t *out = P->tape + P->tlen;
    for (uint32_t i = 0; i < v->tlen;) {
        const uint32_t width = tapeEntryWidth(t[i]);
        const unsigned type = tapeType(t[i]);
        out[i] = t[i];
        if (type == T_STR || type == T_RAWNUM) out[i] += base;
        if (type == T_KEYID) ktIncr(tapeHigh(t[i]));
        if (width == 2) out[i + 1] = t[i + 1];
        if (width == T_CONTAINER_ENTRIES) {
            out[i + 1] = t[i + 1];
            out[i + 2] = t[i + 2];
        }
        i += width;
    }
    P->tlen += v->tlen;
    if (P->sp - 1 + v->depth > P->depth) P->depth = P->sp - 1 + v->depth;
    tapeCountValue(tapeTop(P), tapeFits(t, 0) ? (uint32_t)t[1] : TAPE_NOFIT, KIND_OTHER);
}

/* An edit applied while a value is copied to a tape (the re-emit of design
 * 8.3): in one packed container, children [at, at + ndel) are left out and
 * `nins` values are put in their place. */
typedef struct tapeEdit {
    const uint8_t *container; /* The container, by the address of its tag byte. */
    uint32_t at, ndel, nins;
    const jsonVal *const *vals;
    uint32_t keyid; /* Object: name of the new member, or 0 to keep the name of the member replaced. */
} tapeEdit;

/* One container being copied. */
typedef struct copyFrame {
    jsonIter it;
    uint32_t index; /* Children read so far. */
    int edit;       /* The edit applies to this container and is still to be made. */
} copyFrame;

static void copyPush(walkStack *stack, const jsonRef *container, const tapeEdit *ed) {
    copyFrame *f = walkStackPush(stack);
    jsonIterInit(&f->it, container);
    f->index = 0;
    f->edit = ed && container->kind == REF_PACKED && container->p == ed->container;
}

static void tapeApplyEdit(jsonVal *P, copyFrame *f, const tapeEdit *ed) {
    jsonRef dropped;
    jsonStep step;
    uint32_t keyid = ed->keyid;
    for (uint32_t k = 0; k < ed->ndel; k++) {
        jsonIterNext(&f->it, &dropped, &step, NULL, NULL);
        if (!keyid) keyid = step.keyid;
    }
    for (uint32_t k = 0; k < ed->nins; k++) {
        if (keyid) tapeKeyId(P, keyid);
        tapeAppendTape(P, ed->vals[k]);
    }
}

/* Appends a copy of the value *root at the tape's current position, with an
 * edit applied on the way if `ed` is not NULL. */
static void tapeAppendValue(jsonVal *P, const jsonRef *root, const tapeEdit *ed) {
    jsonType type = jsonRefType(root);
    if (!typeIsContainer(type)) {
        tapeAppendScalar(P, root);
        return;
    }
    copyFrame inline_frames[WALK_INLINE_FRAMES];
    walkStack stack;
    jsonRef val;
    jsonStep step;
    walkStackInit(&stack, inline_frames, WALK_INLINE_FRAMES, sizeof(copyFrame));
    tapeOpen(P, type == JSON_OBJECT, 1);
    copyPush(&stack, root, ed);
    while (stack.n) {
        copyFrame *f = walkStackTop(&stack);
        if (f->edit && f->index == ed->at) {
            tapeApplyEdit(P, f, ed);
            f->edit = 0;
        }
        if (!jsonIterNext(&f->it, &val, &step, NULL, NULL)) {
            tapeClose(P);
            stack.n--;
            continue;
        }
        f->index++;
        if (step.keyid) tapeKeyId(P, step.keyid);
        type = jsonRefType(&val);
        if (typeIsContainer(type)) {
            tapeOpen(P, type == JSON_OBJECT, 1);
            copyPush(&stack, &val, ed);
        } else {
            tapeAppendScalar(P, &val);
        }
    }
    walkStackFree(&stack);
}

jsonVal *jsonValFromRef(const jsonRef *r) {
    jsonVal *P = tapeAcquire();
    tapeAppendValue(P, r, NULL);
    return P;
}

/* ============================================================================
 * 8. Serialiser (design 6.4)
 *
 * All of its state is in the caller's frame and it only reads the document
 * and the key table, so it can run on a save thread.
 *
 * There are two walkers. Compact output of a packed value, which is nearly
 * all of the work in practice, has a loop of its own (serPackedCompact) that
 * keeps the read and write positions in registers and checks the output
 * buffer once per value. Everything else goes through the general walker:
 * spine containers, and any value when a format is given.
 *
 * Formatting reproduces the module's JSON.GET with INDENT, NEWLINE and
 * SPACE: before each array element or member name comes a comma (not for
 * the first), the newline and one indent per open container. Before a
 * closing bracket of a non-empty container comes the newline and one indent
 * per container still open. After the colon comes the space. An empty
 * container is "[]" or "{}".
 * ========================================================================= */

#define BUF_MIN_HEAP 256
#define SER_INLINE_FRAMES 32
#define SER_ESCAPE_CHUNK 256 /* String bytes escaped per buffer reservation. */
#define SER_ESCAPE_MAX 6     /* Longest escape: \uXXXX. */
#define SER_SLACK 128        /* Room the compact walker keeps ahead: a comma, then any number, any literal or a \
                              * short string that needs no escaping. */

void jsonBufInit(jsonBuf *b, char *initial, size_t cap) {
    b->p = initial;
    b->len = 0;
    b->cap = initial ? cap : 0;
    b->heap = 0;
}

void jsonBufFree(jsonBuf *b) {
    if (b->heap) engFree(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    b->heap = 0;
}

/* Makes room for n more bytes, moving to the heap if needed. */
/* Moves the buffer to `nc` bytes on the heap. */
static void bufSetCap(jsonBuf *b, size_t nc) {
    if (b->heap) {
        b->p = E.alloc.realloc(b->p, nc);
    } else {
        char *np = engAlloc(nc);
        if (b->len) memcpy(np, b->p, b->len);
        b->p = np;
        b->heap = 1;
    }
    b->cap = nc;
}

static void bufGrow(jsonBuf *b, size_t n) {
    uint64_t nc = (uint64_t)b->cap * 2 > BUF_MIN_HEAP ? (uint64_t)b->cap * 2 : BUF_MIN_HEAP;
    while (nc < (uint64_t)b->len + n) nc *= 2;
    bufSetCap(b, engSize(nc));
}

void jsonBufReserve(jsonBuf *b, size_t n) {
    if (b->cap - b->len < n) bufSetCap(b, engSize((uint64_t)b->len + n)); /* What was asked for: the caller sized it. */
}

/* Returns where the next n bytes go. The caller adds to len. */
static JSON_INLINE char *bufRoom(jsonBuf *b, size_t n) {
    if (JSON_UNLIKELY(b->cap - b->len < n)) bufGrow(b, n);
    return b->p + b->len;
}

static JSON_INLINE void bufPutc(jsonBuf *b, char c) {
    *bufRoom(b, 1) = c;
    b->len++;
}

static JSON_INLINE void bufPut(jsonBuf *b, const void *s, size_t n) {
    copySmall(bufRoom(b, n), s, n);
    b->len += n;
}

void jsonBufAppend(jsonBuf *b, const char *s, size_t n) {
    memcpy(bufRoom(b, n), s, n);
    b->len += n;
}

static void bufPutEscaped(jsonBuf *b, const uint8_t *s, size_t len) {
    while (len) {
        const size_t chunk = len < SER_ESCAPE_CHUNK ? len : SER_ESCAPE_CHUNK;
        char *o = escapeBytes(bufRoom(b, chunk * SER_ESCAPE_MAX), s, chunk);
        b->len = (size_t)(o - b->p);
        s += chunk;
        len -= chunk;
    }
}

/* Writes a JSON string literal. `noesc` says the bytes need no escaping. */
static JSON_INLINE void serPutString(jsonBuf *b, const uint8_t *s, size_t len, int noesc) {
    if (JSON_LIKELY(noesc)) {
        char *o = bufRoom(b, len + 2);
        *o++ = '"';
        copySmall(o, s, len);
        o[len] = '"';
        b->len += len + 2;
        return;
    }
    bufPutc(b, '"');
    bufPutEscaped(b, s, len);
    bufPutc(b, '"');
}

void jsonBufAppendQuoted(jsonBuf *b, const char *s, size_t n) {
    serPutString(b, (const uint8_t *)s, n, 0);
}

static inline void serPutInt(jsonBuf *b, int64_t v) {
    b->len = (size_t)(printI64(bufRoom(b, JSON_NUM_BUF), v) - b->p);
}

static inline void serPutDouble(jsonBuf *b, double d) {
    b->len += (size_t)jsonDoublePrint(d, bufRoom(b, JSON_NUM_BUF));
}

static inline void serPutDecimal(jsonBuf *b, int64_t mantissa, uint32_t scale) {
    b->len = (size_t)(printDecimal(bufRoom(b, JSON_NUM_BUF), mantissa, scale) - b->p);
}

/* Writes one element of a typed array that is not a string at o and returns
 * the end. */
static inline char *printElement(char *o, const uint8_t *elem, uint32_t ekind, uint8_t scale) {
    if (ekind == EK_LIT) {
        static const char TEXT[3][8] = {"null", "false", "true"};
        memcpy(o, TEXT[*elem], 8); /* Within the room every caller keeps for a number. */
        return o + 4 + (*elem == PK_FALSE);
    }
    if (ekind == EK_F64) return o + jsonDoublePrint(loadF64(elem), o);
    if (ekIsDec(ekind)) return printDecimal(o, ekInt(elem, ekind), scale);
    return printI64(o, ekInt(elem, ekind));
}

/* ---- Compact output of typed arrays and packed values ---- */

/* The write position of the compact walkers: a pointer into the buffer and
 * the end of its capacity, kept out of the jsonBuf while a walker runs. */
typedef struct outPos {
    char *o, *limit;
} outPos;

static JSON_INLINE outPos outBegin(const jsonBuf *b) {
    const outPos w = {b->p + b->len, b->p + b->cap};
    return w;
}

static JSON_INLINE void outEnd(jsonBuf *b, const outPos *w) {
    b->len = (size_t)(w->o - b->p);
}

static void outGrow(jsonBuf *b, outPos *w, size_t n) {
    outEnd(b, w);
    bufGrow(b, n);
    *w = outBegin(b);
}

/* Makes sure n more bytes can be written at w->o. */
static JSON_INLINE void outRoom(jsonBuf *b, outPos *w, size_t n) {
    if (JSON_UNLIKELY((size_t)(w->limit - w->o) < n)) outGrow(b, w, n);
}

/* A string through the general writer: the rare ones that need escaping. */
static void outEscapedString(jsonBuf *b, outPos *w, const uint8_t *s, size_t len) {
    outEnd(b, w);
    serPutString(b, s, len, 0);
    *w = outBegin(b);
}

static JSON_INLINE void outString(jsonBuf *b, outPos *w, const uint8_t *s, size_t len, int noesc) {
    if (JSON_UNLIKELY(!noesc)) {
        outEscapedString(b, w, s, len);
        return;
    }
    outRoom(b, w, len + SER_SLACK);
    *w->o++ = '"';
    copySmall(w->o, s, len);
    w->o += len;
    *w->o++ = '"';
}

/* A typed array, compact. One loop for each family of element, so that the
 * kind is not decided again for every element. */
static void outTyped(jsonBuf *b, outPos *w, const uint8_t *elems, uint32_t count, uint32_t ekind, uint8_t scale) {
    const uint32_t width = EK_WIDTH[ekind];
    const uint8_t *const end = elems + (size_t)count * width;
    outRoom(b, w, SER_SLACK);
    *w->o++ = '[';
    if (ekind == EK_STR) {
        for (; elems < end; elems += width) {
            outString(b, w, elems + 1, elems[0] & EK_STR_LEN_MASK, (elems[0] & EK_STR_NOESC) != 0);
            *w->o++ = ',';
        }
    } else if (ekind == EK_F64) {
        for (; elems < end; elems += width) {
            outRoom(b, w, SER_SLACK);
            w->o += jsonDoublePrint(loadF64(elems), w->o);
            *w->o++ = ',';
        }
    } else if (ekIsDec(ekind) && scale < DEC_SHORT_SCALES) {
        for (; elems < end; elems += width) {
            uint64_t m = (uint64_t)ekInt(elems, ekind);
            outRoom(b, w, SER_SLACK);
            if ((int64_t)m < 0) {
                *w->o++ = '-';
                m = 0 - m;
            }
            w->o = printShortDecimal(w->o, m, scale);
            *w->o++ = ',';
        }
    } else if (ekIsDec(ekind)) {
        for (; elems < end; elems += width) {
            outRoom(b, w, SER_SLACK);
            w->o = printDecimal(w->o, ekInt(elems, ekind), scale);
            *w->o++ = ',';
        }
    } else if (ekind == EK_LIT) {
        for (; elems < end; elems += width) {
            outRoom(b, w, SER_SLACK);
            w->o = printElement(w->o, elems, ekind, scale);
            *w->o++ = ',';
        }
    } else {
        for (; elems < end; elems += width) {
            outRoom(b, w, SER_SLACK);
            w->o = printI64(w->o, ekInt(elems, ekind));
            *w->o++ = ',';
        }
    }
    /* The bracket takes the place of the last comma. */
    if (count) w->o--;
    outRoom(b, w, 1);
    *w->o++ = ']';
}

/* One open container of a packed value being written. */
typedef struct compactFrame {
    const uint8_t *end;
    char close;
} compactFrame;

/* Writes the packed value at p, whole, in compact form. `stop` is the byte
 * after it.
 *
 * One room check per value: SER_SLACK bytes, beyond the serialised member
 * name in an object, cover the comma and any value that is written without a
 * check of its own (a number, a literal, a bracket, a short string that needs
 * no escaping). Everything longer checks for itself. */
static void serPackedCompact(jsonBuf *b, const uint8_t *p, const uint8_t *const stop) {
    static const uint8_t INT_SIZE[4] = {2, 3, 5, 9}; /* PK_I8..PK_I64, tag included. */
    compactFrame inline_frames[SER_INLINE_FRAMES], *top = NULL;
    walkStack stack;
    outPos w = outBegin(b);
    uint32_t len, count, ekind, keyid;
    uint8_t scale;
    int first = 1;  /* Nothing written yet in the innermost container: no comma. */
    int in_obj = 0; /* The innermost container is an object. */
    walkStackInit(&stack, inline_frames, SER_INLINE_FRAMES, sizeof(compactFrame));
    for (;;) {
        while (top && p == top->end) {
            outRoom(b, &w, 1);
            *w.o++ = top->close;
            first = 0;
            top = --stack.n ? walkStackTop(&stack) : NULL;
            in_obj = top && top->close == '}';
        }
        if (p == stop) break;
        if (in_obj) {
            /* The member name as the key table keeps it: "name": */
            p = varintGet(p, &keyid);
            const keyInfo *ki = ktInfo(keyid);
            const uint32_t klen = ki->ser_len;
            outRoom(b, &w, (size_t)klen + SER_SLACK);
            if (!first) *w.o++ = ',';
            copySmall(w.o, ktInfoSer(ki), klen);
            w.o += klen;
        } else {
            outRoom(b, &w, SER_SLACK);
            if (!first) *w.o++ = ',';
        }
        first = 0;
        const uint8_t tag = *p;
        if (tag >= PK_SSTR) {
            if (tag < PK_DEC) {
                len = tag & PK_SSTR_LEN_MASK;
                if (JSON_LIKELY(tag >= PK_SSTR_NE)) {
                    *w.o++ = '"'; /* At most 63 bytes: within the slack. */
                    copySmall(w.o, p + 1, len);
                    w.o += len;
                    *w.o++ = '"';
                } else {
                    outEscapedString(b, &w, p + 1, len);
                }
                p += 1 + len;
            } else if (pkIsDec(tag)) {
                /* printDecimal, with the short form always inline: prices. */
                uint32_t dscale;
                uint64_t m = (uint64_t)pkDecimal(p, &dscale);
                if ((int64_t)m < 0) {
                    *w.o++ = '-';
                    m = 0 - m;
                }
                w.o = dscale < DEC_SHORT_SCALES ? printShortDecimal(w.o, m, dscale) : printLongDecimal(w.o, m, dscale);
                p += pkDecSize(tag);
            } else {
                const uint8_t *text = pkText(p, &len);
                outRoom(b, &w, len);
                memcpy(w.o, text, len);
                w.o += len;
                p = text + len;
            }
            continue;
        }
        if (tag >= PK_INT0) {
            const unsigned v = tag - PK_INT0;
            if (v >= 10) *w.o++ = (char)('0' + v / 10);
            *w.o++ = (char)('0' + v % 10);
            p++;
            continue;
        }
        switch (tag) {
        case PK_NULL:
            memcpy(w.o, "null", 4);
            w.o += 4;
            p++;
            break;
        case PK_FALSE:
            memcpy(w.o, "false", 5);
            w.o += 5;
            p++;
            break;
        case PK_TRUE:
            memcpy(w.o, "true", 4);
            w.o += 4;
            p++;
            break;
        case PK_I8:
        case PK_I16:
        case PK_I32:
        case PK_I64:
            w.o = printI64(w.o, pkInt(p));
            p += INT_SIZE[tag - PK_I8];
            break;
        case PK_F64:
            w.o += jsonDoublePrint(loadF64(p + 1), w.o);
            p += 9;
            break;
        case PK_STR16:
        case PK_STR16_NE:
            len = loadU16(p + 1);
            outString(b, &w, p + 3, len, tag == PK_STR16_NE);
            p += 3 + len;
            break;
        case PK_ARR8:
        case PK_ARR16:
        case PK_OBJ8:
        case PK_OBJ16:
            top = walkStackPush(&stack);
            in_obj = pkIsObject(tag);
            top->close = in_obj ? '}' : ']';
            *w.o++ = in_obj ? '{' : '[';
            p = pkOpen(p, &top->end, &count);
            first = 1;
            break;
        default: {
            const uint8_t *elems = pkTypedOpen(p, &ekind, &count, &scale);
            outTyped(b, &w, elems, count, ekind, scale);
            p = elems + (size_t)count * EK_WIDTH[ekind];
        }
        }
    }
    outEnd(b, &w);
    walkStackFree(&stack);
}

/* ---- The general walker ---- */

/* One open container. */
typedef struct serFrame {
    const uint8_t *p;   /* Packed: the next key or value. Spine: the node. */
    const uint8_t *end; /* Packed: the end of the container. */
    uint32_t i;         /* Spine: the next element or entry. */
    uint8_t kind;       /* SER_* */
    uint8_t first;      /* No child written yet. */
} serFrame;

enum {
    SER_PACKED_ARR,
    SER_PACKED_OBJ,
    SER_NODE_ARR,
    SER_NODE_OBJ
};

typedef struct serializer {
    jsonBuf *out;
    const jsonFormat *fmt; /* NULL for compact output. */
    walkStack stack;       /* serFrame per open container. */
} serializer;

/* The newline, then one indent per level. */
static void serNewlineIndent(serializer *s, uint32_t level) {
    const jsonFormat *f = s->fmt;
    if (f->newline) bufPut(s->out, f->newline, f->newline_len);
    if (!f->indent) return;
    level += f->initial_level;
    for (uint32_t i = 0; i < level; i++) bufPut(s->out, f->indent, f->indent_len);
}

/* What precedes a child of the container on top of the stack. */
static inline void serChildPrefix(serializer *s, serFrame *f) {
    if (!f->first) bufPutc(s->out, ',');
    f->first = 0;
    if (JSON_UNLIKELY(s->fmt != NULL)) serNewlineIndent(s, s->stack.n);
}

/* Closes the container on top of the stack. */
static inline void serClose(serializer *s, char bracket) {
    s->stack.n--;
    if (JSON_UNLIKELY(s->fmt != NULL)) serNewlineIndent(s, s->stack.n);
    bufPutc(s->out, bracket);
}

static inline void serMemberName(serializer *s, uint32_t keyid) {
    const keyInfo *ki = ktInfo(keyid);
    bufPut(s->out, ktInfoSer(ki), ki->ser_len); /* "name": */
    if (JSON_UNLIKELY(s->fmt != NULL) && s->fmt->space) bufPut(s->out, s->fmt->space, s->fmt->space_len);
}

/* A typed array, written whole. */
static void serTyped(serializer *s, const uint8_t *elems, uint32_t count, uint32_t ekind, uint8_t scale) {
    jsonBuf *out = s->out;
    const uint32_t width = EK_WIDTH[ekind];
    if (!s->fmt) {
        outPos w = outBegin(out);
        outTyped(out, &w, elems, count, ekind, scale);
        outEnd(out, &w);
        return;
    }
    bufPutc(out, '[');
    for (uint32_t i = 0; i < count; i++, elems += width) {
        if (i) bufPutc(out, ',');
        serNewlineIndent(s, s->stack.n + 1);
        if (ekind == EK_STR) {
            serPutString(out, elems + 1, elems[0] & EK_STR_LEN_MASK, (elems[0] & EK_STR_NOESC) != 0);
        } else {
            out->len = (size_t)(printElement(bufRoom(out, JSON_NUM_BUF), elems, ekind, scale) - out->p);
        }
    }
    if (count) serNewlineIndent(s, s->stack.n);
    bufPutc(out, ']');
}

/* Writes the packed scalar or typed array at p and returns the next value. */
static const uint8_t *serPackedLeaf(serializer *s, const uint8_t *p) {
    jsonBuf *out = s->out;
    const uint8_t tag = *p;
    const uint8_t *text;
    uint32_t len;
    if (tag >= PK_SSTR) {
        if (pkIsDec(tag)) {
            uint32_t scale;
            const int64_t m = pkDecimal(p, &scale);
            serPutDecimal(out, m, scale);
            return p + pkDecSize(tag);
        }
        if (tag >= PK_RAW8) {
            text = pkText(p, &len);
            bufPut(out, text, len);
            return text + len;
        }
        len = tag & PK_SSTR_LEN_MASK;
        serPutString(out, p + 1, len, tag >= PK_SSTR_NE);
        return p + 1 + len;
    }
    switch (tag) {
    case PK_NULL: bufPut(out, "null", 4); return p + 1;
    case PK_FALSE: bufPut(out, "false", 5); return p + 1;
    case PK_TRUE: bufPut(out, "true", 4); return p + 1;
    case PK_F64:
        serPutDouble(out, loadF64(p + 1));
        return p + 9;
    case PK_STR16:
    case PK_STR16_NE:
        len = loadU16(p + 1);
        serPutString(out, p + 3, len, tag == PK_STR16_NE);
        return p + 3 + len;
    default:
        if (pkIsInt(tag)) {
            serPutInt(out, pkInt(p));
            return pkSkip(p);
        } else {
            uint32_t ekind, count;
            uint8_t scale;
            const uint8_t *elems = pkTypedOpen(p, &ekind, &count, &scale);
            serTyped(s, elems, count, ekind, scale);
            return elems + (size_t)count * EK_WIDTH[ekind];
        }
    }
}

/* Starts the packed array or object at p: writes it whole if it is empty,
 * else writes the opening bracket and pushes a frame. */
static void serOpenPacked(serializer *s, const uint8_t *p) {
    const int is_obj = pkIsObject(*p);
    const uint8_t *end;
    uint32_t count;
    const uint8_t *first = pkOpen(p, &end, &count);
    if (first == end) {
        bufPut(s->out, is_obj ? "{}" : "[]", 2);
        return;
    }
    bufPutc(s->out, is_obj ? '{' : '[');
    serFrame *f = walkStackPush(&s->stack);
    f->p = first;
    f->end = end;
    f->i = 0;
    f->kind = is_obj ? SER_PACKED_OBJ : SER_PACKED_ARR;
    f->first = 1;
}

/* Writes the packed value at p, or starts it if it is a container and a
 * format has to be followed inside it. */
static inline void serPacked(serializer *s, const uint8_t *p, const uint8_t *stop) {
    if (!s->fmt) {
        serPackedCompact(s->out, p, stop);
    } else if (pkIsNested(*p)) {
        serOpenPacked(s, p);
    } else {
        serPackedLeaf(s, p);
    }
}

static void serOpenNode(serializer *s, const uint8_t *node) {
    const int is_obj = nodeKind(node) == NK_OBJECT;
    if ((is_obj ? loadU32(node + OBJECT_LIVE) : nodeLen(node)) == 0) {
        bufPut(s->out, is_obj ? "{}" : "[]", 2);
        return;
    }
    bufPutc(s->out, is_obj ? '{' : '[');
    serFrame *f = walkStackPush(&s->stack);
    f->p = node;
    f->end = NULL;
    f->i = 0;
    f->kind = is_obj ? SER_NODE_OBJ : SER_NODE_ARR;
    f->first = 1;
}

static void serPutTextWord(jsonBuf *out, const uint8_t *text, uint32_t len, int noesc, int rawnum) {
    if (rawnum) {
        bufPut(out, text, len);
    } else {
        serPutString(out, text, len, noesc);
    }
}

/* Writes the value of a spine word, or starts it if it is a container. */
static void serWord(serializer *s, uint64_t w) {
    jsonBuf *out = s->out;
    uint32_t len;
    switch (wordTag(w)) {
    case W_PTR: {
        const uint8_t *node = wordNode(w), *text;
        switch (nodeKind(node)) {
        case NK_STR:
            text = strNodeBytes(node, &len);
            serPutTextWord(out, text, len, (node[0] & STR_NOESC) != 0, (node[0] & STR_RAWNUM) != 0);
            break;
        case NK_BOXNUM:
            if (node[1] == BOXNUM_INT) {
                serPutInt(out, (int64_t)loadU64(node + BOXNUM_VALUE));
            } else if (node[1] == BOXNUM_DEC) {
                serPutDecimal(out, (int64_t)loadU64(node + BOXNUM_VALUE), node[BOXNUM_SCALE]);
            } else {
                serPutDouble(out, loadF64(node + BOXNUM_VALUE));
            }
            break;
        case NK_PACKED: serPacked(s, node + PACKED_HDR, node + PACKED_HDR + loadU16(node + 1)); break;
        case NK_TARRAY: serTyped(s, tarrayData(node), nodeLen(node), node[TARRAY_EKIND], node[TARRAY_SCALE]); break;
        default: serOpenNode(s, node);
        }
        break;
    }
    case W_INT: serPutInt(out, (int64_t)w >> W_INT_SHIFT); break;
    case W_STR:
        len = (uint32_t)(w >> W_STR_LEN_SHIFT) & W_STR_MAX;
        serPutTextWord(out, wordStrBytes(&w), len, (w & W_STR_NOESC) != 0, (w & W_STR_RAWNUM) != 0);
        break;
    case W_SPECIAL:
        switch (w & W_SPECIAL_MASK) {
        case W_NULL: bufPut(out, "null", 4); break;
        case W_FALSE: bufPut(out, "false", 5); break;
        case W_TRUE: bufPut(out, "true", 4); break;
        case W_PZERO: bufPut(out, "0.0", 3); break;
        default: bufPut(out, "-0.0", 4);
        }
        break;
    case W_DEC_POS:
    case W_DEC_NEG: {
        uint32_t scale;
        const int64_t m = wordDecimal(w, &scale);
        serPutDecimal(out, m, scale);
        break;
    }
    default: serPutDouble(out, wordDouble(w));
    }
}

/* Writes the next child of the container on top of the stack, or closes it. */
/* Compact output of the inline decimals that follow one another in an ARRAY
 * from index i: the form an array of full-precision numbers takes. Returns
 * the index after the run. */
static uint32_t serDecimalRun(jsonBuf *b, const uint64_t *words, uint32_t i, uint32_t n, int first) {
    outPos w = outBegin(b);
    for (; i < n && wordIsDec(words[i]); i++, first = 0) {
        uint32_t scale;
        const int64_t m = wordDecimal(words[i], &scale);
        outRoom(b, &w, SER_SLACK);
        if (!first) *w.o++ = ',';
        w.o = printDecimal(w.o, m, scale);
    }
    outEnd(b, &w);
    return i;
}

/* Compact output of the PACKED nodes that follow one another in an ARRAY
 * from index i: an array of records. Returns the index after the run. */
static uint32_t serPackedRun(jsonBuf *b, const uint64_t *words, uint32_t i, uint32_t n, int first) {
    for (; i < n; i++, first = 0) {
        const uint64_t w = words[i];
        if (wordTag(w) != W_PTR || nodeKind(wordNode(w)) != NK_PACKED) break;
        const uint8_t *node = wordNode(w);
        if (!first) bufPutc(b, ',');
        serPackedCompact(b, node + PACKED_HDR, node + PACKED_HDR + loadU16(node + 1));
    }
    return i;
}

static inline void serStep(serializer *s) {
    serFrame *f = walkStackTop(&s->stack);
    uint32_t keyid;
    switch (f->kind) {
    case SER_PACKED_ARR:
    case SER_PACKED_OBJ: {
        const int is_obj = f->kind == SER_PACKED_OBJ;
        const uint8_t *p = f->p;
        if (p == f->end) {
            serClose(s, is_obj ? '}' : ']');
            return;
        }
        serChildPrefix(s, f);
        if (is_obj) {
            p = varintGet(p, &keyid);
            serMemberName(s, keyid);
        }
        if (pkIsNested(*p)) {
            f->p = pkSkip(p); /* Before the push: it can move the stack. */
            serOpenPacked(s, p);
        } else {
            f->p = serPackedLeaf(s, p);
        }
        return;
    }
    case SER_NODE_ARR: {
        const uint8_t *node = f->p;
        if (f->i == nodeLen(node)) {
            serClose(s, ']');
            return;
        }
        if (!s->fmt && wordIsDec(nodeWords(node)[f->i])) {
            f->i = serDecimalRun(s->out, nodeWords(node), f->i, nodeLen(node), f->first);
            f->first = 0;
            return;
        }
        if (!s->fmt) {
            const uint64_t w = nodeWords(node)[f->i];
            if (wordTag(w) == W_PTR && nodeKind(wordNode(w)) == NK_PACKED) {
                f->i = serPackedRun(s->out, nodeWords(node), f->i, nodeLen(node), f->first);
                f->first = 0;
                return;
            }
        }
        serChildPrefix(s, f);
        serWord(s, nodeWords(node)[f->i++]);
        return;
    }
    default: { /* SER_NODE_OBJ */
        const uint8_t *node = f->p;
        const uint64_t *entries = nodeWords(node);
        const uint32_t used = nodeLen(node);
        while (f->i < used && entries[(size_t)f->i * 2] == 0) f->i++; /* Removed members. */
        if (f->i == used) {
            serClose(s, '}');
            return;
        }
        const uint64_t *entry = entries + (size_t)f->i++ * 2;
        serChildPrefix(s, f);
        serMemberName(s, wordKeyId(entry[0]));
        serWord(s, entry[1]);
        return;
    }
    }
}

void jsonSerialize(const jsonRef *r, const jsonFormat *fmt, jsonBuf *out) {
    serFrame inline_frames[SER_INLINE_FRAMES];
    serializer s;
    s.out = out;
    s.fmt = (fmt && (fmt->indent || fmt->newline || fmt->space)) ? fmt : NULL;
    if (r->kind == REF_PACKED && !s.fmt) {
        serPackedCompact(out, r->p, pkSkip(r->p)); /* The common case needs none of the rest. */
        return;
    }
    walkStackInit(&s.stack, inline_frames, SER_INLINE_FRAMES, sizeof(serFrame));
    switch (r->kind) {
    case REF_PACKED: serPacked(&s, r->p, pkSkip(r->p)); break;
    case REF_WORD: serWord(&s, r->w); break;
    case REF_ELEM:
        if (r->b == EK_STR) {
            serPutString(out, r->p + 1, r->p[0] & EK_STR_LEN_MASK, (r->p[0] & EK_STR_NOESC) != 0);
        } else {
            out->len = (size_t)(printElement(bufRoom(out, JSON_NUM_BUF), r->p, r->b, r->sub) - out->p);
        }
        break;
    default: break;
    }
    while (s.stack.n) serStep(&s);
    walkStackFree(&s.stack);
}

/* A typed array's text is bounded per element by its kind and, for decimals
 * of one scale, by the digits of the kind and that scale. */
static size_t typedTextMax(uint32_t ekind, uint32_t count, uint32_t scale) {
    static const uint8_t PER_ELEMENT[EK_LIT + 1] = {0, 5, 7, 12, 21, 26, 3, 5, 10, 18, 2 + EK_STR_MAX * SER_ESCAPE_MAX + 1, 6};
    size_t each = PER_ELEMENT[ekind];
    /* A sign, the digits with a point among them or "0." and the scale, a comma. */
    if (ekIsDec(ekind)) each = (each > scale ? each + 1 : scale + 2) + 2;
    return 2 + (size_t)count * each;
}

size_t jsonSerializeSizeHint(const jsonRef *r) {
    uint32_t len, ekind, count;
    uint8_t scale;
    switch (r->kind) {
    case REF_PACKED: {
        if (pkIsTyped(*r->p)) {
            pkTypedOpen(r->p, &ekind, &count, &scale);
            return typedTextMax(ekind, count, scale);
        }
        /* Text is about one and a half times the packed bytes on records:
         * member names come back, numbers grow, strings stay. */
        return (size_t)(pkSkip(r->p) - r->p) * 2 + JSON_NUM_BUF;
    }
    case REF_WORD: {
        const uint8_t *node = refNode(r);
        if (!node) return JSON_NUM_BUF;
        switch (nodeKind(node)) {
        case NK_STR:
            strNodeBytes(node, &len);
            return (size_t)len + len / 8 + 2;
        case NK_TARRAY: return typedTextMax(node[TARRAY_EKIND], nodeLen(node), node[TARRAY_SCALE]);
        case NK_ARRAY:
        case NK_OBJECT: {
            /* A root knows the bytes of the whole document. Elsewhere only
             * the size of the node itself is at hand. */
            const size_t bytes = (size_t)loadU64(node + SPINE_BYTES);
            size_t hint = bytes ? bytes + bytes / 2 : (size_t)nodeLen(node) * 2 * sizeof(uint64_t);
            if (nodeKind(node) == NK_ARRAY) {
                /* Full-precision numbers are three times their word as text.
                 * The first few elements say whether this is such an array. */
                const uint64_t *words = nodeWords(node);
                const uint32_t probe = nodeLen(node) < 16 ? nodeLen(node) : 16;
                uint32_t decimals = 0;
                for (uint32_t i = 0; i < probe; i++) decimals += (uint32_t)wordIsDec(words[i]);
                if (decimals * 2 > probe) hint += (size_t)nodeLen(node) * 2 * sizeof(uint64_t);
            }
            return hint;
        }
        default: return JSON_NUM_BUF;
        }
    }
    default: return JSON_NUM_BUF;
    }
}

/* ============================================================================
 * 9. Whole-value queries
 * ========================================================================= */

static inline int typeIsNumber(jsonType type) {
    return type == JSON_INT || type == JSON_DOUBLE || type == JSON_RAWNUM;
}

/* A number as the module's comparison sees it. The module keeps a number as
 * a 64-bit integer when its text is an integer within int64, and as its text
 * otherwise; that is exactly JSON_INT against everything else here. */
typedef struct numView {
    int is_int;
    int64_t i; /* The integer, when is_int. */
    double d;  /* The value as a double, rounded to nearest. */
} numView;

static void refNumView(const jsonRef *r, jsonType type, numView *v) {
    uint32_t len, scale;
    int overflow;
    v->is_int = 0;
    v->i = 0;
    if (type == JSON_INT) {
        jsonRefInt(r, &v->i);
        v->is_int = 1;
        v->d = doubleRound((double)v->i);
    } else if (type == JSON_DOUBLE) {
        v->d = refDoubleValue(r);
    } else if (refDecimal(r, &v->i, &scale)) {
        v->d = decimalValue(v->i, scale);
    } else {
        /* Number text, an integer beyond int64 included. */
        const uint8_t *s = refText(r, &len);
        v->d = jsonDoubleParse((const char *)s, len, &overflow);
    }
}

/* Equality of two values that are not containers, as the module's
 * GenericValue::operator== defines it. null, false, true and strings equal
 * only their own kind. Numbers:
 *
 *   a \ b                  JSON_INT              decimal, JSON_DOUBLE, number text
 *   JSON_INT               the same integer      (double)a == value of b
 *   decimal, JSON_DOUBLE,  value of a ==         value of a == value of b
 *   number text            (double)b
 *
 * where the value of a number that is not a JSON_INT is its text read as a
 * double. An integer beyond int64 is number text, whether or not it fits
 * uint64. So integers within int64 are told apart exactly, and everything
 * else as far as a double can:
 *
 *   9007199254740993     != 9007199254740992      both JSON_INT
 *   9007199254740993     == 9007199254740992.0    both 2^53 as doubles
 *   9223372036854775807  == 9223372036854775808   both 2^63 as doubles
 *   18446744073709551615 == 18446744073709551616  both 2^64 as doubles
 *   -1                   != 18446744073709551615  the module has no uint64
 *   1                    == 1.0 == 1e0 == 1.00 */
static int scalarEquals(const jsonRef *a, jsonType ta, const jsonRef *b, jsonType tb) {
    if (typeIsNumber(ta) || typeIsNumber(tb)) {
        numView x, y;
        if (!typeIsNumber(ta) || !typeIsNumber(tb)) return 0;
        refNumView(a, ta, &x);
        refNumView(b, tb, &y);
        return (x.is_int && y.is_int) ? x.i == y.i : x.d == y.d;
    }
    if (ta != tb) return 0;
    if (ta == JSON_STRING) {
        uint32_t la, lb;
        const uint8_t *sa = refText(a, &la), *sb = refText(b, &lb);
        return la == lb && memcmp(sa, sb, la) == 0;
    }
    return 1;
}

/* Two containers being compared, walked in step. Member order does not
 * matter in an object, as in the module: when the two members at one
 * position have different names, the name is looked up in the other object. */
typedef struct equalsFrame {
    jsonIter ia, ib;
    jsonRef b;
    int is_obj;
} equalsFrame;

/* Compares a and b as far as it can without descending. Returns 0 if they
 * differ. Two containers of the same kind and size get a frame. */
static int equalsEnter(walkStack *stack, const jsonRef *a, const jsonRef *b) {
    const jsonType ta = jsonRefType(a), tb = jsonRefType(b);
    if (!typeIsContainer(ta) || !typeIsContainer(tb)) {
        return typeIsContainer(ta) == typeIsContainer(tb) && scalarEquals(a, ta, b, tb);
    }
    if (ta != tb || jsonRefCount(a) != jsonRefCount(b)) return 0;
    equalsFrame *f = walkStackPush(stack);
    f->is_obj = ta == JSON_OBJECT;
    f->b = *b;
    jsonIterInit(&f->ia, a);
    jsonIterInit(&f->ib, b);
    return 1;
}

int jsonRefEquals(const jsonRef *a, const jsonRef *b) {
    equalsFrame inline_frames[8];
    walkStack stack;
    jsonRef va, vb;
    jsonStep sa, sb;
    walkStackInit(&stack, inline_frames, 8, sizeof(equalsFrame));
    int equal = equalsEnter(&stack, a, b);
    while (equal && stack.n) {
        equalsFrame *f = walkStackTop(&stack);
        if (!jsonIterNext(&f->ia, &va, &sa, NULL, NULL)) {
            stack.n--;
            continue;
        }
        jsonIterNext(&f->ib, &vb, &sb, NULL, NULL); /* The counts are equal. */
        if (f->is_obj && sa.keyid != sb.keyid) equal = jsonObjectGetById(&f->b, sa.keyid, &vb, NULL);
        equal = equal && equalsEnter(&stack, &va, &vb);
    }
    walkStackFree(&stack);
    return equal;
}

/* Counts the values in *r, itself included, and measures the nesting below
 * it. */
static void refMeasure(const jsonRef *r, uint64_t *fields, uint32_t *depth) {
    jsonIter inline_frames[WALK_INLINE_FRAMES];
    walkStack stack;
    jsonRef val;
    *fields = 1;
    *depth = 0;
    if (!typeIsContainer(jsonRefType(r))) return;
    walkStackInit(&stack, inline_frames, WALK_INLINE_FRAMES, sizeof(jsonIter));
    jsonIterInit(walkStackPush(&stack), r);
    while (stack.n) {
        jsonIter *it = walkStackTop(&stack);
        if (stack.n > *depth) *depth = stack.n;
        if (it->kind == IT_ELEMS) {
            *fields += it->n; /* A typed array holds only numbers. */
            stack.n--;
            continue;
        }
        if (!jsonIterNext(it, &val, NULL, NULL, NULL)) {
            stack.n--;
            continue;
        }
        (*fields)++;
        if (typeIsContainer(jsonRefType(&val))) jsonIterInit(walkStackPush(&stack), &val);
    }
    walkStackFree(&stack);
}

uint32_t jsonRefDepth(const jsonRef *r) {
    uint64_t fields;
    uint32_t depth;
    refMeasure(r, &fields, &depth);
    return depth;
}

uint64_t jsonRefFields(const jsonRef *r) {
    uint64_t fields;
    uint32_t depth;
    refMeasure(r, &fields, &depth);
    return fields;
}

/* An ARRAY or OBJECT node being walked child by child. */
typedef struct spineFrame {
    uint8_t *node;
    uint32_t i;
} spineFrame;

static inline int nodeIsSpine(const uint8_t *node) {
    return nodeKind(node) == NK_ARRAY || nodeKind(node) == NK_OBJECT;
}

static inline void spinePush(walkStack *stack, uint8_t *node) {
    spineFrame *f = walkStackPush(stack);
    f->node = node;
    f->i = 0;
}

/* The next child word of a spine node, or 0 after the last. *keyid is the
 * member's key in an OBJECT, 0 in an ARRAY. */
static inline uint64_t spineNext(spineFrame *f, uint32_t *keyid) {
    const uint64_t *words = nodeWords(f->node);
    const uint32_t n = nodeLen(f->node);
    *keyid = 0;
    if (nodeKind(f->node) == NK_ARRAY) return f->i < n ? words[f->i++] : 0;
    while (f->i < n && words[(size_t)f->i * 2] == 0) f->i++; /* Removed members. */
    if (f->i == n) return 0;
    *keyid = wordKeyId(words[(size_t)f->i * 2]);
    return words[(size_t)f->i++ * 2 + 1];
}

/* Allocator bytes and node count of everything a word points at. */
static void wordMeasure(uint64_t w, size_t *bytes, size_t *nodes) {
    spineFrame inline_frames[WALK_INLINE_FRAMES];
    walkStack stack;
    uint32_t keyid;
    *bytes = 0;
    *nodes = 0;
    if (wordTag(w) != W_PTR) return;
    *bytes = E.alloc.usable(wordNode(w));
    *nodes = 1;
    if (!nodeIsSpine(wordNode(w))) return;
    walkStackInit(&stack, inline_frames, WALK_INLINE_FRAMES, sizeof(spineFrame));
    spinePush(&stack, wordNode(w));
    while (stack.n) {
        const uint64_t child = spineNext(walkStackTop(&stack), &keyid);
        if (!child) {
            stack.n--;
            continue;
        }
        if (wordTag(child) != W_PTR) continue;
        *bytes += E.alloc.usable(wordNode(child));
        (*nodes)++;
        if (nodeIsSpine(wordNode(child))) spinePush(&stack, wordNode(child));
    }
    walkStackFree(&stack);
}

size_t jsonRefMemUsage(const jsonRef *r) {
    const size_t slot = (r->a & REF_IN_SLOT) ? sizeof(uint64_t) : 0;
    size_t bytes, nodes;
    switch (r->kind) {
    case REF_PACKED:
        /* A whole PACKED node, or some bytes inside one. */
        if (r->w) return slot + E.alloc.usable(wordNode(r->w));
        return (size_t)(pkSkip(r->p) - r->p);
    case REF_WORD:
        wordMeasure(r->w, &bytes, &nodes);
        return slot + bytes;
    case REF_ELEM: return EK_WIDTH[r->b];
    default: return 0;
    }
}

/* ============================================================================
 * 10. Whole-document operations
 * ========================================================================= */

/* One open container of a packed value being scanned for its keys. */
typedef struct packedFrame {
    const uint8_t *end;
    int is_obj;
} packedFrame;

/* Releases the key references held by the packed value at p. */
static void packedReleaseKeys(const uint8_t *p, int on_main) {
    packedFrame inline_frames[WALK_INLINE_FRAMES];
    walkStack stack;
    uint32_t count, keyid;
    if (!pkIsNested(*p)) return; /* Scalars and typed arrays hold no keys. */
    const uint8_t *const end = pkSkip(p);
    walkStackInit(&stack, inline_frames, WALK_INLINE_FRAMES, sizeof(packedFrame));
    for (;;) {
        while (stack.n && p == ((packedFrame *)walkStackTop(&stack))->end) stack.n--;
        if (p == end) break;
        if (stack.n && ((packedFrame *)walkStackTop(&stack))->is_obj) {
            p = varintGet(p, &keyid);
            ktDecr(keyid, on_main);
        }
        if (pkIsNested(*p)) {
            packedFrame *f = walkStackPush(&stack);
            f->is_obj = pkIsObject(*p);
            p = pkOpen(p, &f->end, &count);
        } else {
            p = pkSkip(p);
        }
    }
    walkStackFree(&stack);
}

/* Frees one node. `acct`, if not NULL, has it subtracted. */
static inline void nodeFree(uint8_t *node, nodeAcct *acct) {
    if (acct) {
        acct->bytes -= E.alloc.usable(node);
        acct->nodes--;
    }
    E.alloc.free(node);
}

/* Frees a node that has no child nodes. Returns 0, and does nothing, for an
 * ARRAY or OBJECT. */
static int nodeFreeLeaf(uint8_t *node, int on_main, nodeAcct *acct) {
    if (nodeIsSpine(node)) return 0;
    if (nodeKind(node) == NK_PACKED) packedReleaseKeys(node + PACKED_HDR, on_main);
    nodeFree(node, acct);
    return 1;
}

/* Frees everything a word points at and releases its keys. */
static void wordFree(uint64_t w, int on_main, nodeAcct *acct) {
    spineFrame inline_frames[WALK_INLINE_FRAMES];
    walkStack stack;
    uint32_t keyid;
    if (!w || wordTag(w) != W_PTR) return;
    if (nodeFreeLeaf(wordNode(w), on_main, acct)) return;
    walkStackInit(&stack, inline_frames, WALK_INLINE_FRAMES, sizeof(spineFrame));
    spinePush(&stack, wordNode(w));
    while (stack.n) {
        spineFrame *f = walkStackTop(&stack);
        const uint64_t child = spineNext(f, &keyid);
        if (!child) {
            nodeFree(f->node, acct);
            stack.n--;
            continue;
        }
        if (keyid) ktDecr(keyid, on_main);
        if (wordTag(child) != W_PTR) continue;
        if (!nodeFreeLeaf(wordNode(child), on_main, acct)) spinePush(&stack, wordNode(child));
    }
    walkStackFree(&stack);
}

void jsonDocFree(jsonDoc d) {
    /* Off the main thread a key that drops to zero is left for the sweep. */
    wordFree(d, engOnMainThread(), NULL);
}

jsonDoc jsonDocCopy(jsonDoc d) {
    jsonRef root;
    if (!d) return 0;
    /* Emitted from scratch, so the copy is shaped by the current caps. */
    jsonDocRoot(d, &root);
    return jsonValToDoc(jsonValFromRef(&root));
}

static inline const uint8_t *docSpineRoot(jsonDoc d) {
    if (!d || wordTag(d) != W_PTR) return NULL;
    return nodeIsSpine(wordNode(d)) ? wordNode(d) : NULL;
}

size_t jsonDocMemUsage(jsonDoc d) {
    const uint8_t *root = docSpineRoot(d);
    if (root) return (size_t)loadU64(root + SPINE_BYTES);
    return (d && wordTag(d) == W_PTR) ? E.alloc.usable(wordNode(d)) : 0;
}

size_t jsonDocFreeEffort(jsonDoc d) {
    const uint8_t *root = docSpineRoot(d);
    return root ? (size_t)loadU64(root + SPINE_NODES) : 1;
}

const char *jsonDocEncoding(jsonDoc d) {
    if (docSpineRoot(d)) return "jtree";
    if (d && wordTag(d) == W_PTR && nodeKind(wordNode(d)) == NK_TARRAY) return "jtree";
    return "jpack";
}

/* ============================================================================
 * 11. Changing a document (design 8.3)
 *
 * A change names a value by its locator. The locator is followed through
 * spine containers, slot by slot, to the container the change is made in
 * (the "site"). What happens there depends on what the site is:
 *
 *   - A spine ARRAY, OBJECT or TARRAY node is changed directly: a slot is
 *     written, elements are moved, the node is grown or rebuilt.
 *   - A container inside a PACKED node is changed with the cheapest of three
 *     primitives. In place: the new bytes are as long as the old ones and
 *     overwrite them. Splice: the byte range is replaced, the rest of the
 *     node moved, and the size of every enclosing container patched, which
 *     needs each of those size fields to hold the new size and the node to
 *     stay within the caps. Re-emit: the node is copied to a tape with the
 *     change applied and emitted again, and the emitter decides its new form.
 *
 * Values to write arrive as tapes (jsonVal). Writing one emits a copy: the
 * tape keeps its key references and the document gets its own.
 *
 * Numbers. A value given as text keeps its text. A computed double
 * (jsonSetDouble) is stored as what its canonical text would be read as: a
 * decimal when that text is short plain notation, else the double. Either
 * way a typed array that cannot hold the new element widens within its
 * family (a larger integer or decimal width) or becomes a plain array (a
 * decimal of another scale, a number of another kind, a non-number).
 *
 * Memory accounting. The root of a spine document carries the document's
 * byte and node totals. Every node allocated, freed or resized under such a
 * root goes through `acct`, and the difference is added to the root when the
 * change is complete.
 * ========================================================================= */

#define SPINE_SHRINK_MIN 8 /* A container this small keeps its capacity when it shrinks. */

void jsonEngineGetWriteStats(uint64_t counts[JSON_WRITE_KINDS]) {
    memcpy(counts, E.writes, sizeof(E.writes));
}

typedef struct mutation {
    jsonDoc *d;
    nodeAcct delta;  /* Bytes and nodes added so far (wrapping arithmetic). */
    nodeAcct *acct;  /* &delta when the root carries totals, else NULL: nothing to track. */
    int totals_done; /* The root was emitted afresh, with its totals. */
} mutation;

static void mutBegin(mutation *m, jsonDoc *d) {
    m->d = d;
    m->delta.bytes = 0;
    m->delta.nodes = 0;
    m->acct = docSpineRoot(*d) ? &m->delta : NULL;
    m->totals_done = 0;
}

/* Brings the totals in a spine root up to date. */
static void mutEnd(mutation *m) {
    size_t bytes, nodes;
    if (m->totals_done || !docSpineRoot(*m->d)) return;
    uint8_t *root = wordNode(*m->d);
    if (m->acct) {
        bytes = (size_t)loadU64(root + SPINE_BYTES) + m->delta.bytes;
        nodes = (size_t)loadU64(root + SPINE_NODES) + m->delta.nodes;
    } else {
        /* The root just became a spine node (a typed array that had to
         * become a plain one): count what is under it. */
        wordMeasure(*m->d, &bytes, &nodes);
    }
    storeU64(root + SPINE_BYTES, bytes);
    storeU64(root + SPINE_NODES, nodes);
}

static uint8_t *nodeRealloc(uint8_t *node, uint64_t n, nodeAcct *acct) {
    if (acct) acct->bytes -= E.alloc.usable(node);
    node = E.alloc.realloc(node, engSize(n));
    jsonAssert(((uintptr_t)node & W_TAG_MASK) == 0);
    if (acct) acct->bytes += E.alloc.usable(node);
    return node;
}

static inline uint32_t growCap(uint32_t cap, uint32_t need) {
    const uint64_t grown = (uint64_t)cap + cap / 2;
    if (grown <= need) return need;
    return grown > UINT32_MAX ? UINT32_MAX : (uint32_t)grown;
}

/* Emits a copy of a value as a spine word. The value stays usable. */
static uint64_t emitCopy(const jsonVal *v, nodeAcct *acct) {
    jsonVal *scratch = (jsonVal *)(uintptr_t)v; /* Only the emitter's own stack in it is written. */
    const uint64_t w = tapeNeedsSpine(v->tape, 0) ? emitSpine(scratch, 0, acct) : emitLeaf(v, 0, acct);
    tapeRetainRefs(v);
    return w;
}

/* ---- Scalars that need no tape ---- */

typedef struct scalarVal {
    unsigned type; /* T_NULL, T_FALSE, T_TRUE, T_INT, T_DBL or T_DEC. */
    int64_t i;     /* T_INT: the value. T_DEC: the digits. */
    uint64_t bits; /* T_DBL. */
    uint32_t scale;
} scalarVal;

static int tapeRootScalar(const jsonVal *v, scalarVal *sv) {
    const uint64_t *t = v->tape;
    sv->type = tapeType(t[0]);
    switch (sv->type) {
    case T_NULL:
    case T_FALSE:
    case T_TRUE: return 1;
    case T_INT:
    case T_INT64:
        sv->type = T_INT;
        sv->i = tapeInt(t, 0);
        return 1;
    case T_DBL:
        sv->bits = t[1];
        return 1;
    case T_DEC:
        sv->i = tapeDecMantissa(t, 0);
        sv->scale = tapeDecScale(t[0]);
        return 1;
    default: return 0;
    }
}

static jsonVal *scalarToVal(const scalarVal *sv) {
    jsonVal *P = tapeAcquire();
    switch (sv->type) {
    case T_INT: tapeInt64(P, sv->i); break;
    case T_DBL: tapeDouble(P, bitsDouble(sv->bits)); break;
    case T_DEC: tapeDecimal(P, sv->i, sv->scale); break;
    default: tapeSimple(P, (int)sv->type);
    }
    return P;
}

static uint64_t scalarWord(const scalarVal *sv, nodeAcct *acct) {
    switch (sv->type) {
    case T_NULL: return W_NULL;
    case T_FALSE: return W_FALSE;
    case T_TRUE: return W_TRUE;
    case T_INT: return emitInt(sv->i, acct);
    case T_DEC: return emitDecimal(sv->i, sv->scale, acct);
    default: return emitDouble(sv->bits, acct);
    }
}

/* ---- Typed array elements ---- */

static inline int ekindHoldsInt(uint32_t ekind, int64_t v) {
    switch (ekIntKind(ekind)) {
    case EK_I8: return v == (int8_t)v;
    case EK_I16: return v == (int16_t)v;
    case EK_I32: return v == (int32_t)v;
    default: return 1;
    }
}

/* What a value needs of a typed array to be an element of it: the kind of
 * array, and for a decimal its scale. EK_NONE if no typed array holds it. */
static uint32_t valueElementKind(const jsonVal *v, uint8_t *scale) {
    const uint64_t e = v->tape[0];
    uint32_t len;
    *scale = 0;
    switch (tapeType(e)) {
    case T_INT:
    case T_INT64: {
        const int64_t i = tapeInt(v->tape, 0);
        return intRangeEkind(i, i);
    }
    case T_DBL: return EK_F64;
    case T_DEC: {
        const int64_t m = tapeDecMantissa(v->tape, 0);
        *scale = (uint8_t)tapeDecScale(e);
        return EK_DEC_BASE + intRangeEkind(m, m);
    }
    case T_STR:
        tapeText(v, 0, &len);
        return len <= EK_STR_MAX ? EK_STR : EK_NONE;
    case T_NULL:
    case T_FALSE:
    case T_TRUE: return EK_LIT;
    default: return EK_NONE;
    }
}

/* The kind a typed array of kind `ekind` and scale `scale` must have to
 * take a value as well: the same, a wider one of its family, or EK_NONE
 * when the value is of another family or scale. */
static uint32_t ekindToHold(uint32_t ekind, uint8_t scale, const jsonVal *v) {
    uint8_t vscale;
    const uint32_t need = valueElementKind(v, &vscale);
    if (need == EK_NONE || vscale != scale) return EK_NONE;
    if (ekIsInt(ekind) && ekIsInt(need)) return need > ekind ? need : ekind;
    if (ekIsDec(ekind) && ekIsDec(need)) return need > ekind ? need : ekind;
    return need == ekind ? ekind : EK_NONE;
}

/* Stores a value the array's kind holds. */
static uint8_t *ekStoreVal(uint8_t *p, uint32_t ekind, const jsonVal *v) {
    const uint64_t *t = v->tape;
    uint32_t len;
    if (ekind == EK_F64) {
        storeU64(p, t[1]);
    } else if (ekind == EK_LIT) {
        *p = (uint8_t)tapeType(t[0]);
    } else if (ekind == EK_STR) {
        const uint8_t *s = tapeText(v, 0, &len);
        ekStoreString(p, s, len, (t[0] & TF_FLAG) != 0);
    } else if (ekIsDec(ekind)) {
        ekStoreInt(p, ekind, tapeDecMantissa(t, 0));
    } else {
        ekStoreInt(p, ekind, tapeInt(t, 0));
    }
    return p + EK_WIDTH[ekind];
}

/* Overwrites one element if the scalar fits the array's kind and scale. */
static int elemSetScalar(uint8_t *p, uint32_t ekind, uint8_t scale, const scalarVal *sv) {
    if (ekind == EK_F64) {
        if (sv->type != T_DBL) return 0;
        storeU64(p, sv->bits);
        return 1;
    }
    if (ekind == EK_LIT) {
        if (sv->type > T_TRUE) return 0;
        *p = (uint8_t)sv->type;
        return 1;
    }
    if (ekIsInt(ekind) ? sv->type != T_INT : (!ekIsDec(ekind) || sv->type != T_DEC || sv->scale != scale)) return 0;
    if (!ekindHoldsInt(ekind, sv->i)) return 0;
    ekStoreInt(p, ekind, sv->i);
    return 1;
}

/* ---- Sites ---- */

#define SITE_INLINE_ANCESTORS 16

/* The container a change is made in. */
typedef struct mutSite {
    uint64_t *slot; /* Holds the word of `node`: the document word, or a slot of a spine container. */
    uint8_t *node;  /* ARRAY, OBJECT, TARRAY: the container. PACKED: the node the container is in. */
    uint32_t cont;  /* PACKED: offset in the node of the container's first byte. */
    walkStack anc;  /* PACKED: offsets (uint32_t) of the containers around it, outermost first. */
    uint32_t anc_inline[SITE_INLINE_ANCESTORS];
} mutSite;

/* Follows a locator to the container it names. */
static void siteResolve(mutation *m, const jsonStep *loc, int n, mutSite *s) {
    uint64_t *slot = m->d;
    int i = 0;
    walkStackInit(&s->anc, s->anc_inline, SITE_INLINE_ANCESTORS, sizeof(uint32_t));
    for (; i < n; i++) {
        const uint8_t *node = wordNode(*slot);
        if (nodeKind(node) == NK_ARRAY) {
            slot = &nodeWords(node)[loc[i].pos];
        } else if (nodeKind(node) == NK_OBJECT) {
            slot = &nodeWords(node)[(size_t)objectFind(node, loc[i].keyid) * 2 + 1];
        } else {
            break; /* A PACKED node: the rest of the path is inside it. */
        }
    }
    s->slot = slot;
    s->node = wordNode(*slot);
    s->cont = PACKED_HDR;
    if (nodeKind(s->node) != NK_PACKED) return;
    const uint8_t *p = s->node + PACKED_HDR;
    for (; i < n; i++) {
        *(uint32_t *)walkStackPush(&s->anc) = (uint32_t)(p - s->node);
        p = pkChild(p, &loc[i]);
    }
    s->cont = (uint32_t)(p - s->node);
}

static inline void siteFree(mutSite *s) {
    walkStackFree(&s->anc);
}

static inline void siteSetNode(mutSite *s, uint8_t *node) {
    s->node = node;
    *s->slot = nodeWord(node);
}

/* ---- Containers inside a packed node ---- */

typedef struct packedCont {
    const uint8_t *first, *end; /* Its children. */
    uint32_t count;
    uint32_t ekind; /* Element kind of a typed array, else EK_NONE. */
    uint8_t scale;  /* Typed decimal array: its scale. */
    int is_obj;
    int wide; /* 16-bit size and count fields. */
} packedCont;

static void packedContOpen(const uint8_t *c, packedCont *pc) {
    const uint8_t tag = *c;
    pc->is_obj = pkIsObject(tag);
    if (pkIsTyped(tag)) {
        pc->first = pkTypedOpen(c, &pc->ekind, &pc->count, &pc->scale);
        pc->end = pc->first + (size_t)pc->count * EK_WIDTH[pc->ekind];
        pc->wide = (tag - PK_TARR) & 1;
    } else {
        pc->first = pkOpen(c, &pc->end, &pc->count);
        pc->ekind = EK_NONE;
        pc->scale = 0;
        pc->wide = tag & 1;
    }
}

/* The bytes of elements [at, at + n) of a packed array. */
static void packedElements(const packedCont *pc, uint32_t at, uint32_t n, const uint8_t **start, const uint8_t **end) {
    const uint8_t *p = pc->first;
    if (pc->ekind) {
        *start = p + (size_t)at * EK_WIDTH[pc->ekind];
        *end = *start + (size_t)n * EK_WIDTH[pc->ekind];
        return;
    }
    if (at == pc->count) {
        *start = *end = pc->end;
        return;
    }
    for (uint32_t i = 0; i < at; i++) p = pkSkip(p);
    *start = p;
    if (at + n == pc->count) {
        *end = pc->end;
        return;
    }
    for (uint32_t i = 0; i < n; i++) p = pkSkip(p);
    *end = p;
}

/* A member of a packed object: returns its value, and its position and key. */
static const uint8_t *packedMember(const packedCont *pc, uint32_t keyid, uint32_t *at, const uint8_t **key) {
    uint32_t id, pos = 0;
    *at = 0;
    *key = NULL;
    for (const uint8_t *p = pc->first; p < pc->end; p = pkSkip(p), pos++) {
        const uint8_t *k = p;
        p = varintGet(p, &id);
        if (id != keyid) continue;
        *at = pos;
        *key = k;
        return p;
    }
    return NULL;
}

/* Releases the keys held by a run of packed values, which are members
 * (name, value) when has_names is set. */
static void packedReleaseRun(const uint8_t *p, const uint8_t *end, int has_names) {
    uint32_t keyid;
    while (p < end) {
        if (has_names) {
            p = varintGet(p, &keyid);
            ktDecr(keyid, 1);
        }
        packedReleaseKeys(p, 1);
        p = pkSkip(p);
    }
}

static inline uint32_t pkFieldMax(int wide) {
    return wide ? PK_LEN16_MAX : PK_NARROW_MAX;
}

/* 1 if the size field of the plain container at c can take `delta` more. */
static inline int pkSizeHolds(const uint8_t *c, int64_t delta) {
    const int wide = *c & 1;
    return (int64_t)(wide ? loadU16(c + 1) : c[1]) + delta <= (int64_t)pkFieldMax(wide);
}

static inline void pkSizeAdd(uint8_t *c, int64_t delta) {
    if (*c & 1) {
        storeU16(c + 1, (uint16_t)(loadU16(c + 1) + delta));
    } else {
        c[1] = (uint8_t)(c[1] + delta);
    }
}

static inline void pkCountSet(uint8_t *c, const packedCont *pc, uint32_t count) {
    uint8_t *field = c + (pc->ekind ? 1 : (pc->wide ? 3 : 2)); /* A typed array has no size field. */
    if (pc->wide) {
        storeU16(field, (uint16_t)count);
    } else {
        *field = (uint8_t)count;
    }
}

/* One change to a packed container: children [at, at + ndel) go and `nins`
 * values come in their place. */
typedef struct packedChange {
    uint32_t at, ndel;
    uint32_t start, end; /* The bytes that go, as offsets in the node. */
    int del_names;       /* Object: those bytes include the member names. */
    const jsonVal *const *vals;
    uint32_t nins;
    uint32_t keyid; /* Object: name of the new member, a reference this change consumes. Else 0. */
} packedChange;

/* Re-emit: copies the node to a tape with the change applied and emits it
 * again. The emitter decides what it becomes. */
static void packedReemit(mutation *m, mutSite *s, const packedChange *ch) {
    uint8_t *old = s->node;
    const tapeEdit ed = {old + s->cont, ch->at, ch->ndel, ch->nins, ch->vals, ch->keyid};
    nodeAcct fresh = {0, 0};
    nodeAcct *acct = m->acct ? m->acct : &fresh;
    jsonRef root;
    jsonVal *T = tapeAcquire();
    refFromPacked(&root, old + PACKED_HDR);
    tapeAppendValue(T, &root, &ed);
    const uint64_t w = tapeNeedsSpine(T->tape, 0) ? emitSpine(T, 0, acct) : emitLeaf(T, 0, acct);
    T->tlen = 0; /* Its references moved into the new nodes. */
    tapeRelease(T);
    packedReleaseKeys(old + PACKED_HDR, 1);
    nodeFree(old, m->acct);
    siteSetNode(s, wordNode(w));
    if (!m->acct && nodeIsSpine(s->node)) {
        /* A packed root became a spine root: `fresh` is all of it. */
        storeU64(s->node + SPINE_BYTES, fresh.bytes);
        storeU64(s->node + SPINE_NODES, fresh.nodes);
        m->totals_done = 1;
    }
    if (ch->keyid) ktDecr(ch->keyid, 1); /* The tape took its own reference. */
}

/* 1 if the change can be made by moving bytes inside the node: every size
 * and count field holds its new value and the node stays within the caps. */
static int packedSpliceAllowed(const mutSite *s, const packedCont *pc, uint32_t blob_len, int64_t delta, uint32_t new_count) {
    const uint8_t *node = s->node;
    const uint32_t *anc = s->anc.base;
    if (new_count > pc->count && (new_count > E.cap_entries || new_count > pkFieldMax(pc->wide))) return 0;
    if (delta <= 0) return 1; /* A field may be wider than its value needs. */
    if ((int64_t)blob_len + delta > (int64_t)E.cap_bytes) return 0;
    for (uint32_t i = 0; i < s->anc.n; i++) {
        if (!pkSizeHolds(node + anc[i], delta)) return 0;
    }
    return pc->ekind || pkSizeHolds(node + s->cont, delta);
}

/* Makes a change to a container inside a packed node, by the cheapest of in
 * place, splice and re-emit. */
static void packedEdit(mutation *m, mutSite *s, const packedCont *pc, const packedChange *ch) {
    uint8_t *node = s->node;
    const uint32_t blob_len = loadU16(node + 1), del_len = ch->end - ch->start;
    const uint32_t new_count = pc->count - ch->ndel + ch->nins;
    const uint32_t *anc = s->anc.base;
    uint64_t new_len = 0;
    for (uint32_t k = 0; k < ch->nins; k++) {
        if (pc->ekind) {
            new_len += ekindToHold(pc->ekind, pc->scale, ch->vals[k]) == pc->ekind ? EK_WIDTH[pc->ekind] : TAPE_NOFIT;
        } else {
            new_len += tapePackedSize(ch->vals[k], 0) + (ch->keyid ? varintSize(ch->keyid) : 0);
        }
    }
    const int64_t delta = (int64_t)new_len - (int64_t)del_len;
    unsigned how = JSON_WRITE_REEMIT;
    if (new_len < TAPE_NOFIT) {
        if (delta == 0 && new_count == pc->count) {
            how = JSON_WRITE_IN_PLACE;
        } else if (packedSpliceAllowed(s, pc, blob_len, delta, new_count)) {
            how = JSON_WRITE_SPLICE;
        }
    }
    E.writes[how]++;
    if (how == JSON_WRITE_REEMIT) {
        packedReemit(m, s, ch);
        return;
    }
    if (!pc->ekind) packedReleaseRun(node + ch->start, node + ch->end, ch->del_names);
    const size_t tail = PACKED_HDR + (size_t)blob_len - ch->end;
    const size_t new_size = (size_t)((int64_t)(PACKED_HDR + blob_len) + delta);
    if (delta > 0) node = nodeRealloc(node, new_size, m->acct);
    if (delta != 0) memmove(node + ch->start + new_len, node + ch->end, tail);
    uint8_t *o = node + ch->start;
    for (uint32_t k = 0; k < ch->nins; k++) {
        if (pc->ekind) {
            o = ekStoreVal(o, pc->ekind, ch->vals[k]);
            continue;
        }
        if (ch->keyid) o = varintPut(o, ch->keyid); /* The caller's reference moves into the node. */
        o = packValue(ch->vals[k], 0, o);
        tapeRetainRefs(ch->vals[k]);
    }
    if (delta < 0) node = nodeRealloc(node, new_size, m->acct);
    if (delta != 0) {
        storeU16(node + 1, (uint16_t)((int64_t)blob_len + delta));
        for (uint32_t i = 0; i < s->anc.n; i++) pkSizeAdd(node + anc[i], delta);
        if (!pc->ekind) pkSizeAdd(node + s->cont, delta);
    }
    if (new_count != pc->count) pkCountSet(node + s->cont, pc, new_count);
    siteSetNode(s, node);
}

/* Overwrites the packed scalar at p with another of exactly its size. An
 * integer, or the digits of a decimal, may be left in a field wider than
 * they need. */
static int packedScalarInPlace(uint8_t *p, const scalarVal *sv) {
    if (pkIsNested(*p) || pkIsTyped(*p)) return 0;
    const size_t size = (size_t)(pkSkip(p) - p);
    const int64_t v = sv->i;
    uint32_t lg;
    switch (sv->type) {
    case T_NULL:
    case T_FALSE:
    case T_TRUE:
        if (size != 1) return 0;
        *p = (uint8_t)sv->type; /* T_NULL, T_FALSE and T_TRUE are numbered as their packed tags. */
        return 1;
    case T_INT:
        if (size == 1) {
            if ((uint64_t)v > PK_INT0_MAX) return 0;
            *p = (uint8_t)(PK_INT0 + v);
            return 1;
        }
        JSON_FALLTHROUGH;
    case T_DEC:
        if (sv->type == T_DEC && (size == PK_DECX_SIZE || sv->scale > PK_DEC_SCALE_MAX)) {
            /* The long form of a decimal holds any decimal, and nothing else holds one of a large scale. */
            if (size != PK_DECX_SIZE) return 0;
            p[0] = PK_DECX;
            p[1] = (uint8_t)sv->scale;
            storeU64(p + 2, (uint64_t)v);
            return 1;
        }
        /* A tag and 1, 2, 4 or 8 bytes. */
        if (size != 2 && size != 3 && size != 5 && size != 9) return 0;
        lg = size == 2 ? 0 : size == 3 ? 1
                         : size == 5   ? 2
                                       : 3;
        if (!ekindHoldsInt(EK_I8 + lg, v)) return 0;
        *p = sv->type == T_INT ? (uint8_t)(PK_I8 + lg) : (uint8_t)(PK_DEC + (sv->scale - 1) * 4 + lg);
        ekStoreInt(p + 1, EK_I8 + lg, v);
        return 1;
    default:
        if (size != 9) return 0;
        packDouble(p, sv->bits);
        return 1;
    }
}

/* Tries to set a child of a packed container to a scalar in place. */
static int packedSetScalar(mutSite *s, const jsonStep *step, const scalarVal *sv) {
    packedCont pc;
    const uint8_t *child = NULL, *end = NULL;
    uint32_t at = 0;
    packedContOpen(s->node + s->cont, &pc);
    if (step->keyid) {
        child = packedMember(&pc, step->keyid, &at, &end);
    } else {
        packedElements(&pc, step->pos, 0, &child, &end);
    }
    uint8_t *p = s->node + (child - s->node);
    const int done = pc.ekind ? elemSetScalar(p, pc.ekind, pc.scale, sv) : packedScalarInPlace(p, sv);
    if (done) E.writes[JSON_WRITE_IN_PLACE]++;
    return done;
}

/* ---- Spine ARRAY ---- */

static uint8_t *arrayNodeResize(mutation *m, mutSite *s, uint32_t cap) {
    uint8_t *node = nodeRealloc(s->node, SPINE_HDR + (uint64_t)cap * sizeof(uint64_t), m->acct);
    storeU32(node + SPINE_CAP, cap);
    siteSetNode(s, node);
    return node;
}

/* Replaces elements [at, at + ndel) of an ARRAY node by nins values. */
static void arrayNodeEdit(mutation *m, mutSite *s, uint32_t at, uint32_t ndel, const jsonVal *const *vals, uint32_t nins) {
    uint8_t *node = s->node;
    const uint32_t len = nodeLen(node), new_len = len - ndel + nins;
    uint32_t cap = loadU32(node + SPINE_CAP);
    for (uint32_t i = 0; i < ndel; i++) wordFree(nodeWords(node)[at + i], 1, m->acct);
    if (new_len > cap) {
        cap = growCap(cap, new_len);
        node = arrayNodeResize(m, s, cap);
    }
    uint64_t *words = nodeWords(node);
    memmove(words + at + nins, words + at + ndel, (size_t)(len - at - ndel) * sizeof(uint64_t));
    for (uint32_t k = 0; k < nins; k++) words[at + k] = emitCopy(vals[k], m->acct);
    storeU32(node + SPINE_LEN, new_len);
    if (cap > SPINE_SHRINK_MIN && (uint64_t)new_len * 4 < cap) arrayNodeResize(m, s, new_len * 2);
    E.writes[JSON_WRITE_SLOT]++;
}

/* ---- Spine TARRAY ---- */

static uint8_t *tarrayResize(mutation *m, mutSite *s, uint32_t cap) {
    const size_t width = EK_WIDTH[s->node[TARRAY_EKIND]];
    uint8_t *node = nodeRealloc(s->node, TARRAY_HDR + (uint64_t)cap * width, m->acct);
    storeU32(node + SPINE_CAP, cap);
    siteSetNode(s, node);
    return node;
}

/* Widens an integer or decimal typed array to a larger kind of its family. */
static void tarrayWiden(mutation *m, mutSite *s, uint32_t ekind) {
    uint8_t *old = s->node;
    const uint32_t len = nodeLen(old), old_kind = old[TARRAY_EKIND];
    uint8_t *node = nodeAlloc(TARRAY_HDR + (uint64_t)len * EK_WIDTH[ekind], m->acct);
    memcpy(node, old, TARRAY_HDR);
    node[TARRAY_EKIND] = (uint8_t)ekind;
    storeU32(node + SPINE_CAP, len);
    for (uint32_t i = 0; i < len; i++) {
        const int64_t v = ekInt(tarrayData(old) + (size_t)i * EK_WIDTH[old_kind], old_kind);
        ekStoreInt(node + TARRAY_HDR + (size_t)i * EK_WIDTH[ekind], ekind, v);
    }
    nodeFree(old, m->acct);
    siteSetNode(s, node);
}

/* One element of a typed array as a spine word. */
static uint64_t elementWord(const uint8_t *elem, uint32_t ekind, uint8_t scale, nodeAcct *acct) {
    if (ekind == EK_F64) return emitDouble(loadU64(elem), acct);
    if (ekind == EK_STR) return emitText(elem + 1, elem[0] & EK_STR_LEN_MASK, (elem[0] & EK_STR_NOESC) != 0, 0, acct);
    if (ekIsDec(ekind)) return emitDecimal(ekInt(elem, ekind), scale, acct);
    if (ekind == EK_LIT) return *elem == PK_NULL ? W_NULL : *elem == PK_FALSE ? W_FALSE
                                                                              : W_TRUE;
    return emitInt(ekInt(elem, ekind), acct);
}

/* Turns a typed array into a plain ARRAY with room for `extra` more. */
static void tarrayToArray(mutation *m, mutSite *s, uint32_t extra) {
    uint8_t *old = s->node;
    const uint32_t len = nodeLen(old), ekind = old[TARRAY_EKIND], cap = len + extra;
    uint8_t *node = nodeAlloc(SPINE_HDR + (uint64_t)cap * sizeof(uint64_t), m->acct);
    memset(node, 0, SPINE_HDR);
    node[0] = NK_ARRAY;
    storeU32(node + SPINE_LEN, len);
    storeU32(node + SPINE_CAP, cap);
    for (uint32_t i = 0; i < len; i++) {
        nodeWords(node)[i] = elementWord(tarrayData(old) + (size_t)i * EK_WIDTH[ekind], ekind, old[TARRAY_SCALE], m->acct);
    }
    nodeFree(old, m->acct);
    siteSetNode(s, node);
}

/* Replaces elements [at, at + ndel) of a TARRAY node by nins values. A value
 * too large for the element kind widens the array within its family. A
 * value of another kind or scale makes it a plain array. */
static void tarrayNodeEdit(mutation *m, mutSite *s, uint32_t at, uint32_t ndel, const jsonVal *const *vals, uint32_t nins) {
    uint32_t ekind = s->node[TARRAY_EKIND], need = ekind;
    for (uint32_t k = 0; k < nins && need != EK_NONE; k++) need = ekindToHold(need, s->node[TARRAY_SCALE], vals[k]);
    if (need == EK_NONE) {
        tarrayToArray(m, s, nins);
        arrayNodeEdit(m, s, at, ndel, vals, nins);
        return;
    }
    if (need != ekind) {
        tarrayWiden(m, s, need);
        ekind = need;
    }
    uint8_t *node = s->node;
    const size_t width = EK_WIDTH[ekind];
    const uint32_t len = nodeLen(node), new_len = len - ndel + nins;
    uint32_t cap = loadU32(node + SPINE_CAP);
    if (new_len > cap) {
        cap = growCap(cap, new_len);
        node = tarrayResize(m, s, cap);
    }
    uint8_t *data = node + TARRAY_HDR;
    memmove(data + (at + nins) * width, data + (at + ndel) * width, (size_t)(len - at - ndel) * width);
    uint8_t *o = data + at * width;
    for (uint32_t k = 0; k < nins; k++) o = ekStoreVal(o, ekind, vals[k]);
    storeU32(node + SPINE_LEN, new_len);
    if (cap > SPINE_SHRINK_MIN && (uint64_t)new_len * 4 < cap) tarrayResize(m, s, new_len * 2);
    E.writes[JSON_WRITE_SLOT]++;
}

/* ---- Spine OBJECT ---- */

static void objectIndexRemove(uint8_t *node, uint32_t keyid, uint32_t entry) {
    uint8_t *index = node + (objectIndex(node) - node);
    const uint64_t *entries = nodeWords(node);
    const uint32_t width = node[OBJECT_IDXW], lg = node[OBJECT_LG], mask = ((uint32_t)1 << lg) - 1;
    uint32_t i = objectHome(keyid, lg);
    while (objectIndexGet(index, width, i) != entry) i = (i + 1) & mask;
    /* Backward-shift deletion: later cells of the probe run move up. */
    for (uint32_t j = i;;) {
        j = (j + 1) & mask;
        const uint32_t other = objectIndexGet(index, width, j);
        if (other == OBJECT_NO_ENTRY) break;
        const uint32_t home = objectHome(wordKeyId(entries[(size_t)other * 2]), lg);
        if (((j - home) & mask) >= ((j - i) & mask)) {
            objectIndexSet(index, width, i, other);
            i = j;
        }
    }
    objectIndexSet(index, width, i, OBJECT_NO_ENTRY);
}

/* Rebuilds an OBJECT node with a new capacity, dropping removed entries. */
static uint8_t *objectNodeRebuild(mutation *m, mutSite *s, uint32_t cap) {
    uint8_t *old = s->node;
    const uint32_t used = nodeLen(old), lg = objectIndexLg(cap), width = objectIndexWidth(cap);
    uint8_t *node = nodeAlloc(objectNodeSize(cap), m->acct);
    memcpy(node, old, SPINE_HDR); /* The live count, and the totals of a root. */
    node[OBJECT_IDXW] = (uint8_t)width;
    node[OBJECT_LG] = (uint8_t)lg;
    storeU32(node + SPINE_CAP, cap);
    memset(node + SPINE_HDR + (size_t)cap * OBJECT_ENTRY, 0xFF, (size_t)((uint64_t)1 << lg) * width);
    const uint64_t *from = nodeWords(old);
    uint64_t *to = nodeWords(node);
    uint32_t n = 0;
    for (uint32_t i = 0; i < used; i++) {
        if (!from[(size_t)i * 2]) continue;
        to[(size_t)n * 2] = from[(size_t)i * 2];
        to[(size_t)n * 2 + 1] = from[(size_t)i * 2 + 1];
        objectIndexInsert(node, wordKeyId(from[(size_t)i * 2]), n);
        n++;
    }
    storeU32(node + SPINE_LEN, n);
    nodeFree(old, m->acct);
    siteSetNode(s, node);
    return node;
}

static void objectNodeReplace(mutation *m, mutSite *s, uint32_t keyid, const jsonVal *v) {
    uint64_t *slot = &nodeWords(s->node)[(size_t)objectFind(s->node, keyid) * 2 + 1];
    const uint64_t old = *slot;
    *slot = emitCopy(v, m->acct);
    wordFree(old, 1, m->acct);
    E.writes[JSON_WRITE_SLOT]++;
}

/* Adds a member at the end. The caller's reference on the key moves into
 * the node. */
static void objectNodeAdd(mutation *m, mutSite *s, uint32_t keyid, const jsonVal *v) {
    uint8_t *node = s->node;
    const uint32_t live = loadU32(node + OBJECT_LIVE);
    if (nodeLen(node) == loadU32(node + SPINE_CAP)) node = objectNodeRebuild(m, s, growCap(live, live + 1));
    const uint32_t entry = nodeLen(node);
    nodeWords(node)[(size_t)entry * 2] = wordKey(keyid);
    nodeWords(node)[(size_t)entry * 2 + 1] = emitCopy(v, m->acct);
    objectIndexInsert(node, keyid, entry);
    storeU32(node + SPINE_LEN, entry + 1);
    storeU32(node + OBJECT_LIVE, live + 1);
    E.writes[JSON_WRITE_SLOT]++;
}

static void objectNodeDelete(mutation *m, mutSite *s, uint32_t keyid) {
    uint8_t *node = s->node;
    uint64_t *entries = nodeWords(node);
    const uint32_t entry = objectFind(node, keyid), live = loadU32(node + OBJECT_LIVE) - 1;
    uint32_t used = nodeLen(node);
    objectIndexRemove(node, keyid, entry);
    wordFree(entries[(size_t)entry * 2 + 1], 1, m->acct);
    ktDecr(keyid, 1);
    entries[(size_t)entry * 2] = entries[(size_t)entry * 2 + 1] = 0;
    /* Removed entries at the end are reused at once. Others stay until the
     * node is next rebuilt. */
    while (used && entries[(size_t)(used - 1) * 2] == 0) used--;
    storeU32(node + SPINE_LEN, used);
    storeU32(node + OBJECT_LIVE, live);
    const uint32_t cap = loadU32(node + SPINE_CAP);
    if (cap > SPINE_SHRINK_MIN && (uint64_t)live * 4 < cap) objectNodeRebuild(m, s, live * 2);
    E.writes[JSON_WRITE_SLOT]++;
}

/* ---- Changes at a site ---- */

static uint32_t siteCount(const mutSite *s) {
    packedCont pc;
    switch (nodeKind(s->node)) {
    case NK_ARRAY:
    case NK_TARRAY: return nodeLen(s->node);
    case NK_OBJECT: return loadU32(s->node + OBJECT_LIVE);
    default:
        packedContOpen(s->node + s->cont, &pc);
        return pc.count;
    }
}

/* Replaces elements [at, at + ndel) of the array at a site by nins values. */
static void siteArrayEdit(mutation *m, mutSite *s, uint32_t at, uint32_t ndel, const jsonVal *const *vals, uint32_t nins) {
    packedCont pc;
    const uint8_t *start, *end;
    switch (nodeKind(s->node)) {
    case NK_ARRAY: arrayNodeEdit(m, s, at, ndel, vals, nins); break;
    case NK_TARRAY: tarrayNodeEdit(m, s, at, ndel, vals, nins); break;
    default: {
        packedContOpen(s->node + s->cont, &pc);
        packedElements(&pc, at, ndel, &start, &end);
        const packedChange ch = {at, ndel, (uint32_t)(start - s->node), (uint32_t)(end - s->node), 0, vals, nins, 0};
        packedEdit(m, s, &pc, &ch);
    }
    }
}

/* Replaces the child a step names by a copy of v. */
static void siteReplaceChild(mutation *m, mutSite *s, const jsonStep *step, const jsonVal *v) {
    if (!step->keyid) {
        siteArrayEdit(m, s, step->pos, 1, &v, 1);
    } else if (nodeKind(s->node) == NK_OBJECT) {
        objectNodeReplace(m, s, step->keyid, v);
    } else {
        packedCont pc;
        const uint8_t *key = NULL;
        uint32_t at = 0;
        packedContOpen(s->node + s->cont, &pc);
        const uint8_t *val = packedMember(&pc, step->keyid, &at, &key);
        const packedChange ch = {at, 1, (uint32_t)(val - s->node), (uint32_t)(pkSkip(val) - s->node), 0, &v, 1, 0};
        packedEdit(m, s, &pc, &ch);
    }
}

/* Replaces the whole document. */
static void rootReplace(mutation *m, const jsonVal *v) {
    const jsonDoc old = *m->d;
    *m->d = emitRoot((jsonVal *)(uintptr_t)v);
    tapeRetainRefs(v);
    wordFree(old, 1, NULL);
    m->totals_done = 1;
    E.writes[JSON_WRITE_SLOT]++;
}

/* Sets a slot of a spine container, or the document word, to a scalar. */
static void slotSetScalar(mutation *m, uint64_t *slot, const scalarVal *sv) {
    const uint64_t old = *slot;
    *slot = scalarWord(sv, m->acct);
    wordFree(old, 1, m->acct);
    E.writes[JSON_WRITE_SLOT]++;
}

/* Sets the value at a locator to a scalar: in its place if it can be, else
 * as any other value. */
static void mutSetScalar(jsonDoc *d, const jsonStep *loc, int n, const scalarVal *sv) {
    mutation m;
    mutSite s;
    mutBegin(&m, d);
    if (n == 0) {
        slotSetScalar(&m, d, sv);
        mutEnd(&m);
        return;
    }
    const jsonStep *step = &loc[n - 1];
    int done = 1;
    siteResolve(&m, loc, n - 1, &s);
    switch (nodeKind(s.node)) {
    case NK_ARRAY: slotSetScalar(&m, &nodeWords(s.node)[step->pos], sv); break;
    case NK_OBJECT: slotSetScalar(&m, &nodeWords(s.node)[(size_t)objectFind(s.node, step->keyid) * 2 + 1], sv); break;
    case NK_TARRAY: {
        const uint32_t ekind = s.node[TARRAY_EKIND];
        done = elemSetScalar(s.node + TARRAY_HDR + (size_t)step->pos * EK_WIDTH[ekind], ekind, s.node[TARRAY_SCALE], sv);
        if (done) E.writes[JSON_WRITE_SLOT]++;
        break;
    }
    default: done = packedSetScalar(&s, step, sv);
    }
    if (!done) {
        jsonVal *tmp = scalarToVal(sv);
        siteReplaceChild(&m, &s, step, tmp);
        jsonValFree(tmp);
    }
    siteFree(&s);
    mutEnd(&m);
}

void jsonSet(jsonDoc *d, const jsonStep *loc, int n, const jsonVal *v) {
    scalarVal sv;
    mutation m;
    mutSite s;
    if (tapeRootScalar(v, &sv)) {
        mutSetScalar(d, loc, n, &sv);
        return;
    }
    mutBegin(&m, d);
    if (n == 0) {
        rootReplace(&m, v);
    } else {
        siteResolve(&m, loc, n - 1, &s);
        siteReplaceChild(&m, &s, &loc[n - 1], v);
        siteFree(&s);
    }
    mutEnd(&m);
}

void jsonSetInt(jsonDoc *d, const jsonStep *loc, int n, int64_t v) {
    const scalarVal sv = {T_INT, v, 0, 0};
    mutSetScalar(d, loc, n, &sv);
}

void jsonSetDouble(jsonDoc *d, const jsonStep *loc, int n, double v) {
    scalarVal sv = {T_DBL, 0, doubleBits(v), 0};
    if (classifyDouble(v, &sv.i, &sv.scale) == NUM_DEC) sv.type = T_DEC;
    mutSetScalar(d, loc, n, &sv);
}

void jsonSetNumText(jsonDoc *d, const jsonStep *loc, int n, const char *s, size_t len) {
    jsonVal *v = jsonValNumText(s, len);
    jsonSet(d, loc, n, v);
    jsonValFree(v);
}

void jsonSetBool(jsonDoc *d, const jsonStep *loc, int n, int v) {
    const scalarVal sv = {v ? T_TRUE : T_FALSE, 0, 0, 0};
    mutSetScalar(d, loc, n, &sv);
}

void jsonDelete(jsonDoc *d, const jsonStep *loc, int n) {
    const jsonStep *step = &loc[n - 1];
    mutation m;
    mutSite s;
    mutBegin(&m, d);
    siteResolve(&m, loc, n - 1, &s);
    if (!step->keyid) {
        siteArrayEdit(&m, &s, step->pos, 1, NULL, 0);
    } else if (nodeKind(s.node) == NK_OBJECT) {
        objectNodeDelete(&m, &s, step->keyid);
    } else {
        packedCont pc;
        const uint8_t *key = NULL;
        uint32_t at = 0;
        packedContOpen(s.node + s.cont, &pc);
        const uint8_t *val = packedMember(&pc, step->keyid, &at, &key);
        const packedChange ch = {at, 1, (uint32_t)(key - s.node), (uint32_t)(pkSkip(val) - s.node), 1, NULL, 0, 0};
        packedEdit(&m, &s, &pc, &ch);
    }
    siteFree(&s);
    mutEnd(&m);
}

void jsonObjectAdd(jsonDoc *d, const jsonStep *loc, int n, const char *name, size_t len, const jsonVal *v) {
    mutation m;
    mutSite s;
    mutBegin(&m, d);
    siteResolve(&m, loc, n, &s);
    const uint32_t keyid = ktIntern((const uint8_t *)name, (uint32_t)len, 0);
    if (nodeKind(s.node) == NK_OBJECT) {
        objectNodeAdd(&m, &s, keyid, v);
    } else {
        packedCont pc;
        packedContOpen(s.node + s.cont, &pc);
        const uint32_t end = (uint32_t)(pc.end - s.node);
        const packedChange ch = {pc.count, 0, end, end, 0, &v, 1, keyid};
        packedEdit(&m, &s, &pc, &ch);
    }
    siteFree(&s);
    mutEnd(&m);
}

void jsonArrayInsert(jsonDoc *d, const jsonStep *loc, int n, uint32_t index, const jsonVal *const *vals, uint32_t count) {
    mutation m;
    mutSite s;
    if (!count) return;
    mutBegin(&m, d);
    siteResolve(&m, loc, n, &s);
    siteArrayEdit(&m, &s, index, 0, vals, count);
    siteFree(&s);
    mutEnd(&m);
}

void jsonArrayTrim(jsonDoc *d, const jsonStep *loc, int n, uint32_t start, uint32_t count) {
    mutation m;
    mutSite s;
    mutBegin(&m, d);
    siteResolve(&m, loc, n, &s);
    const uint32_t len = siteCount(&s);
    if (start > len) start = len;
    if (count > len - start) count = len - start;
    /* The tail first, so that the head's positions do not move under it.
     * Removing never re-emits, so the site stays good for the second cut. */
    if (start + count < len) siteArrayEdit(&m, &s, start + count, len - start - count, NULL, 0);
    if (start > 0) siteArrayEdit(&m, &s, 0, start, NULL, 0);
    siteFree(&s);
    mutEnd(&m);
}

/* Appends to the string in a slot of a spine container or in the document
 * word. A string node is grown where it is when its header allows. */
static void slotStringAppend(mutation *m, uint64_t *slot, const uint8_t *s, size_t len) {
    const uint64_t w = *slot;
    const int suffix_noesc = bytesNoEscape(s, len);
    E.writes[JSON_WRITE_SLOT]++;
    if (wordTag(w) == W_STR) {
        uint8_t joined[W_STR_MAX];
        const uint32_t old_len = (uint32_t)(w >> W_STR_LEN_SHIFT) & W_STR_MAX;
        const int noesc = (w & W_STR_NOESC) && suffix_noesc;
        if (old_len + len <= W_STR_MAX) {
            memcpy(joined, wordStrBytes(&w), old_len);
            memcpy(joined + old_len, s, len);
            *slot = emitText(joined, old_len + (uint32_t)len, noesc, 0, m->acct);
            return;
        }
        const uint32_t new_len = old_len + (uint32_t)len;
        uint8_t *node = nodeAlloc(strNodeSize(new_len), m->acct);
        uint8_t *bytes = node + (new_len <= STR_SHORT_MAX ? STR_SHORT_HDR : STR_LONG_HDR);
        node[0] = (uint8_t)(NK_STR | (noesc ? STR_NOESC : 0) | (new_len <= STR_SHORT_MAX ? 0 : STR_LONG));
        if (new_len <= STR_SHORT_MAX) {
            node[1] = (uint8_t)new_len;
        } else {
            storeU32(node + 1, new_len);
        }
        memcpy(bytes, wordStrBytes(&w), old_len);
        memcpy(bytes + old_len, s, len);
        *slot = nodeWord(node);
        return;
    }
    uint8_t *node = wordNode(w);
    uint32_t old_len;
    const uint8_t *old_bytes = strNodeBytes(node, &old_len);
    const uint32_t new_len = old_len + (uint32_t)len;
    if (!suffix_noesc) node[0] &= (uint8_t)~STR_NOESC;
    if ((node[0] & STR_LONG) || new_len <= STR_SHORT_MAX) {
        /* The header keeps its form: grow the node where it is. */
        const size_t hdr = (size_t)(old_bytes - node);
        node = nodeRealloc(node, (uint64_t)hdr + new_len, m->acct);
        memcpy(node + hdr + old_len, s, len);
        if (node[0] & STR_LONG) {
            storeU32(node + 1, new_len);
        } else {
            node[1] = (uint8_t)new_len;
        }
        *slot = nodeWord(node);
        return;
    }
    uint8_t *grown = nodeAlloc(STR_LONG_HDR + (uint64_t)new_len, m->acct);
    grown[0] = node[0] | STR_LONG;
    storeU32(grown + 1, new_len);
    memcpy(grown + STR_LONG_HDR, old_bytes, old_len);
    memcpy(grown + STR_LONG_HDR + old_len, s, len);
    nodeFree(node, m->acct);
    *slot = nodeWord(grown);
}

/* A string and what is appended to it, as one value. */
static jsonVal *joinedString(const uint8_t *old, uint32_t old_len, int old_noesc, const uint8_t *suffix, size_t len) {
    jsonVal *joined = tapeAcquire();
    arenaReserve(joined, (size_t)old_len + len);
    memcpy(joined->arena, old, old_len);
    memcpy(joined->arena + old_len, suffix, len);
    tapeString(joined, joined->arena, old_len + (uint32_t)len, old_noesc && bytesNoEscape(suffix, len));
    return joined;
}

void jsonStringAppend(jsonDoc *d, const jsonStep *loc, int n, const char *str, size_t len) {
    const uint8_t *suffix = (const uint8_t *)str;
    jsonVal *joined = NULL;
    mutation m;
    mutSite s;
    if (!len) return;
    mutBegin(&m, d);
    if (n == 0) {
        slotStringAppend(&m, d, suffix, len);
        mutEnd(&m);
        return;
    }
    const jsonStep *step = &loc[n - 1];
    siteResolve(&m, loc, n - 1, &s);
    switch (nodeKind(s.node)) {
    case NK_ARRAY: slotStringAppend(&m, &nodeWords(s.node)[step->pos], suffix, len); break;
    case NK_OBJECT:
        slotStringAppend(&m, &nodeWords(s.node)[(size_t)objectFind(s.node, step->keyid) * 2 + 1], suffix, len);
        break;
    case NK_TARRAY: {
        /* An element of a string array: in its 16 bytes if the result fits. */
        uint8_t *elem = s.node + TARRAY_HDR + (size_t)step->pos * EK_STR_SIZE;
        const uint32_t old_len = elem[0] & EK_STR_LEN_MASK;
        const int old_noesc = (elem[0] & EK_STR_NOESC) != 0;
        if (old_len + len > EK_STR_MAX) {
            joined = joinedString(elem + 1, old_len, old_noesc, suffix, len);
            break;
        }
        memcpy(elem + 1 + old_len, suffix, len);
        elem[0] = (uint8_t)((old_len + len) | (old_noesc && bytesNoEscape(suffix, len) ? EK_STR_NOESC : 0));
        E.writes[JSON_WRITE_SLOT]++;
        break;
    }
    default: {
        /* A string inside a packed node: write the joined string over it. */
        packedCont pc;
        const uint8_t *child = NULL, *end = NULL;
        uint32_t at = 0, old_len;
        packedContOpen(s.node + s.cont, &pc);
        if (step->keyid) {
            child = packedMember(&pc, step->keyid, &at, &end);
        } else {
            packedElements(&pc, step->pos, 0, &child, &end);
        }
        const int old_noesc = (*child >= PK_SSTR_NE && *child < PK_DEC) || *child == PK_STR16_NE;
        const uint8_t *old = pkText(child, &old_len);
        joined = joinedString(old, old_len, old_noesc, suffix, len);
    }
    }
    if (joined) {
        siteReplaceChild(&m, &s, step, joined);
        jsonValFree(joined);
    }
    siteFree(&s);
    mutEnd(&m);
}

/* ============================================================================
 * 12. Active defrag (design 10)
 *
 * Every allocation of a document is offered to `move` once. A document
 * whose root is not a spine container is a single allocation. A spine
 * document is done in steps that the cursor counts: the index of a child of
 * the root (plus one, so that a cursor in use is never 0), and an index
 * inside that child when the child is itself a spine container. Anything
 * below that second level is moved as one step.
 *
 * The cursor is positional and the document may change between calls, so
 * every step checks the root and its bounds again. After a change some nodes
 * may be offered twice and some not at all, which defragmentation tolerates.
 * Key bytes live in the key table and are not moved.
 *
 * The cursor is the server's unsigned long. With 64 bits each half has 32,
 * which holds any index. With 32 bits there are two forms: bit 31 clear, 15
 * bits of root index plus one and 16 bits of inner index; bit 31 set, 31 bits
 * of root index plus one and an inner index of 0. A position that neither
 * form holds is not one to stop at: the work goes on to the next that is,
 * at the latest the start of the next child of the root. So on a 32-bit
 * target the entries of a spine child past the 65535th, and from root index
 * 32767 on the whole of each spine child, are done without a stop.
 * ========================================================================= */

#define DEFRAG_WIDE (sizeof(unsigned long) >= 8)
#define DEFRAG_ROOT_ONLY 0x80000000u /* 32-bit cursor: the second form. */

/* A 32-bit cursor for a position. Returns 0 if neither form holds it. A
 * 64-bit one is written in place, in jsonDocDefrag. */
static inline int defragCursorPut32(unsigned long *cursor, uint64_t high, uint64_t low) {
    if (high + 1 <= 0x7FFF && low <= 0xFFFF) {
        *cursor = (unsigned long)(((high + 1) << 16) | low);
    } else if (low == 0 && high + 1 < DEFRAG_ROOT_ONLY) {
        *cursor = (unsigned long)(DEFRAG_ROOT_ONLY | (high + 1));
    } else {
        return 0;
    }
    return 1;
}

/* The position of a 32-bit cursor that is not 0. */
static inline void defragCursorGet32(unsigned long cursor, uint64_t *high, uint64_t *low) {
    const uint64_t c = cursor;
    if (c & DEFRAG_ROOT_ONLY) {
        *high = (c & ~(uint64_t)DEFRAG_ROOT_ONLY) - 1;
        *low = 0;
    } else {
        *high = (c >> 16) - 1;
        *low = c & 0xFFFF;
    }
}

typedef void *(*defragMoveFn)(void *ptr, void *ctx);

/* Offers the node a slot points at. */
static inline void defragNode(uint64_t *slot, defragMoveFn move, void *ctx) {
    void *moved = move(wordNode(*slot), ctx);
    if (moved) *slot = nodeWord(moved);
}

/* The slot of child i of a spine node, or NULL if there is no node in it. */
static inline uint64_t *spineChildSlot(uint8_t *node, uint32_t i) {
    uint64_t *slot = nodeKind(node) == NK_ARRAY ? &nodeWords(node)[i] : &nodeWords(node)[(size_t)i * 2 + 1];
    return (*slot && wordTag(*slot) == W_PTR) ? slot : NULL;
}

/* Offers every node of the subtree a slot points at. */
static void defragSubtree(uint64_t *slot, defragMoveFn move, void *ctx) {
    spineFrame inline_frames[WALK_INLINE_FRAMES];
    walkStack stack;
    defragNode(slot, move, ctx);
    if (!nodeIsSpine(wordNode(*slot))) return;
    walkStackInit(&stack, inline_frames, WALK_INLINE_FRAMES, sizeof(spineFrame));
    spinePush(&stack, wordNode(*slot));
    while (stack.n) {
        spineFrame *f = walkStackTop(&stack);
        if (f->i == nodeLen(f->node)) {
            stack.n--;
            continue;
        }
        uint64_t *child = spineChildSlot(f->node, f->i++);
        if (!child) continue;
        defragNode(child, move, ctx); /* The parent is already where it stays. */
        if (nodeIsSpine(wordNode(*child))) spinePush(&stack, wordNode(*child));
    }
    walkStackFree(&stack);
}

int jsonDocDefrag(jsonDoc *d, void *(*move)(void *ptr, void *ctx), int (*stop)(void *ctx), void *ctx, unsigned long *cursor) {
    uint64_t high = 0, low = 0;
    if (!*d || wordTag(*d) != W_PTR || !nodeIsSpine(wordNode(*d))) {
        /* No allocation, or a single one. */
        if (*d && wordTag(*d) == W_PTR) defragNode(d, move, ctx);
        *cursor = 0;
        return 0;
    }
    if (*cursor == 0) {
        defragNode(d, move, ctx);
    } else if (DEFRAG_WIDE) {
        high = ((uint64_t)*cursor >> 32) - 1;
        low = (uint64_t)*cursor & 0xFFFFFFFFu;
    } else {
        defragCursorGet32(*cursor, &high, &low);
    }
    uint8_t *root = wordNode(*d);
    while (high < nodeLen(root)) {
        uint64_t *slot = spineChildSlot(root, (uint32_t)high);
        if (!slot) {
            high++;
            low = 0;
            continue; /* Nothing to move: not a step. */
        }
        if (!nodeIsSpine(wordNode(*slot))) {
            defragNode(slot, move, ctx);
            high++;
            low = 0;
        } else {
            /* A spine child: the node itself with its first child, then one
             * of its children per step. */
            if (low == 0) defragNode(slot, move, ctx);
            uint8_t *child = wordNode(*slot);
            uint64_t *inner = low < nodeLen(child) ? spineChildSlot(child, (uint32_t)low) : NULL;
            if (inner) defragSubtree(inner, move, ctx);
            if (++low >= nodeLen(child)) {
                high++;
                low = 0;
            } else if (!inner) {
                continue;
            }
        }
        if (stop && stop(ctx) && high < nodeLen(root)) {
            if (DEFRAG_WIDE) {
                *cursor = (unsigned long)(((high + 1) << 32) | low);
                return 1;
            }
            if (defragCursorPut32(cursor, high, low)) return 1;
        }
    }
    *cursor = 0;
    return 0;
}
