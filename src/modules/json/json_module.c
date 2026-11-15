/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_module.c -- entry point and data type of the built-in JSON module.
 *
 * The module is compiled into the server and loaded at startup under the name
 * "json". It registers the same data type name and encoding version as the
 * external valkey-json module, so RDB files, DUMP payloads and other modules
 * that look for "json" see no difference.
 *
 * This file is the only place that connects the engine to the server: the
 * allocator, the data type callbacks (RDB, AOF, copy, digest, defrag, free),
 * the configuration, the INFO section and the shared API. The commands are in
 * json_cmd.c. */

#include "json_module.h"
#include "json_num.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define JSON_MODULE_VERSION 20000 /* 2.0.0: the C engine. The external module is 1.x. */

#define JSON_DEFAULT_MAX_DOCUMENT_SIZE 0 /* No limit. */
#define JSON_DEFAULT_MAX_PATH_LIMIT 128
#define JSON_DEFAULT_MAX_PACKED_BYTES 4096
#define JSON_DEFAULT_MAX_PACKED_ENTRIES 128
#define JSON_MAX_PACKED_BYTES_LIMIT 65535
/* A stored document may be nested deeper than json.max-path-limit, because
 * the limit can be lowered after the document was written. Loading therefore
 * accepts more than the limit, but not without bound: a payload can also come
 * from RESTORE, and the cost of some commands grows with depth. */
#define JSON_LOAD_MIN_DEPTH_LIMIT 1024

ValkeyModuleType *jsonModuleType;

jsonModuleConfig jsonConfig = {
    .max_document_size = JSON_DEFAULT_MAX_DOCUMENT_SIZE,
    .max_path_limit = JSON_DEFAULT_MAX_PATH_LIMIT,
    .max_packed_bytes = JSON_DEFAULT_MAX_PACKED_BYTES,
    .max_packed_entries = JSON_DEFAULT_MAX_PACKED_ENTRIES,
};

/* ----------------------------------------------------------------------------
 * Statistics
 *
 * A document can be freed on a lazy-free thread, so the counters it touches
 * are atomic. Ordering between them does not matter.
 * ------------------------------------------------------------------------- */

static struct {
    atomic_ullong doc_bytes; /* Sum of jsonDocMemUsage over all documents. */
    atomic_ullong num_docs;
} jsonStats;

static void statsResize(size_t orig_size, size_t new_size) {
    if (new_size >= orig_size) {
        atomic_fetch_add_explicit(&jsonStats.doc_bytes, new_size - orig_size, memory_order_relaxed);
    } else {
        atomic_fetch_sub_explicit(&jsonStats.doc_bytes, orig_size - new_size, memory_order_relaxed);
    }
}

void jsonStatsOnRead(size_t fetched_val_size) {
    VALKEYMODULE_NOT_USED(fetched_val_size);
}

void jsonStatsOnInsert(int is_new_key, size_t orig_size, size_t new_size, size_t inserted_val_size) {
    VALKEYMODULE_NOT_USED(inserted_val_size);
    if (is_new_key) atomic_fetch_add_explicit(&jsonStats.num_docs, 1, memory_order_relaxed);
    statsResize(orig_size, new_size);
}

void jsonStatsOnUpdate(size_t orig_size, size_t new_size, size_t input_json_size) {
    VALKEYMODULE_NOT_USED(input_json_size);
    statsResize(orig_size, new_size);
}

void jsonStatsOnDelete(int is_delete_key, size_t orig_size, size_t new_size, size_t deleted_val_size) {
    VALKEYMODULE_NOT_USED(deleted_val_size);
    statsResize(orig_size, new_size);
    if (is_delete_key) atomic_fetch_sub_explicit(&jsonStats.num_docs, 1, memory_order_relaxed);
}

static void jsonInfo(ValkeyModuleInfoCtx *ctx, int for_crash_report) {
    VALKEYMODULE_NOT_USED(for_crash_report);
    if (ValkeyModule_InfoAddSection(ctx, "core_metrics") == VALKEYMODULE_ERR) return;
    jsonEngineStats es;
    jsonEngineGetStats(&es);
    unsigned long long doc_bytes = atomic_load_explicit(&jsonStats.doc_bytes, memory_order_relaxed);
    /* Key names count while a document refers to them. The table's own reserve
     * is left out, so the figure returns to where it was when documents go. */
    ValkeyModule_InfoAddFieldULongLong(ctx, "total_memory_bytes", doc_bytes + es.keytable_live_bytes);
    ValkeyModule_InfoAddFieldULongLong(ctx, "num_documents",
                                       atomic_load_explicit(&jsonStats.num_docs, memory_order_relaxed));
}

/* ----------------------------------------------------------------------------
 * Allocator
 * ------------------------------------------------------------------------- */

static void *glueAlloc(size_t n) {
    return ValkeyModule_Alloc(n);
}

static void *glueRealloc(void *p, size_t n) {
    return ValkeyModule_Realloc(p, n);
}

static void glueFree(void *p) {
    ValkeyModule_Free(p);
}

static size_t glueUsable(void *p) {
    return ValkeyModule_MallocUsableSize(p);
}

/* ----------------------------------------------------------------------------
 * RDB
 *
 * Encoding version 3 is the document as one JSON text. Version 0 is the
 * external module's older form, one node at a time, and is only read.
 * ------------------------------------------------------------------------- */

/* Node codes of encoding version 0. */
enum {
    JSON_V0_NULL = 0x01,
    JSON_V0_STRING = 0x02,
    JSON_V0_DOUBLE = 0x04,
    JSON_V0_INTEGER = 0x08,
    JSON_V0_BOOLEAN = 0x10,
    JSON_V0_OBJECT = 0x20,
    JSON_V0_ARRAY = 0x40,
    JSON_V0_PAIR = 0x80
};

typedef struct v0Frame {
    uint64_t left; /* Children still to read. */
    int is_object;
} v0Frame;

/* Reads one string and appends it as a JSON string literal. */
static int v0AppendString(ValkeyModuleIO *rdb, jsonBuf *out) {
    size_t len;
    char *s = ValkeyModule_LoadStringBuffer(rdb, &len);
    if (s == NULL || ValkeyModule_IsIOError(rdb)) {
        if (s) ValkeyModule_Free(s);
        return 0;
    }
    jsonBufAppendQuoted(out, s, len);
    ValkeyModule_Free(s);
    return 1;
}

/* The deepest nesting a document may have on this server, whatever its
 * source: json.max-path-limit, or JSON_LOAD_MIN_DEPTH_LIMIT if that is larger.
 * It bounds loading and commands that arrive from a primary or an AOF file,
 * so a server never holds a document it could not load again. */
uint32_t jsonLoadDepthLimit(void) {
    long long limit = jsonConfig.max_path_limit;
    if (limit < JSON_LOAD_MIN_DEPTH_LIMIT) limit = JSON_LOAD_MIN_DEPTH_LIMIT;
    if (limit > UINT32_MAX) limit = UINT32_MAX;
    return (uint32_t)limit;
}

/* Converts a version 0 payload to JSON text, which is then parsed like a
 * version 3 payload. Not recursive: nesting is tracked on a heap stack.
 * Returns 0 on a read error or a malformed payload. */
static int v0LoadAsText(ValkeyModuleIO *rdb, jsonBuf *out) {
    v0Frame *stack = NULL;
    size_t depth = 0, cap = 0;
    int ok = 0;
    char num[64];

    for (;;) {
        if (depth && stack[depth - 1].is_object) {
            if (ValkeyModule_LoadUnsigned(rdb) != JSON_V0_PAIR || ValkeyModule_IsIOError(rdb)) goto done;
            if (!v0AppendString(rdb, out)) goto done;
            jsonBufAppend(out, ":", 1);
        }
        uint64_t code = ValkeyModule_LoadUnsigned(rdb);
        if (ValkeyModule_IsIOError(rdb)) goto done;
        int opened = 0;
        switch (code) {
        case JSON_V0_NULL: jsonBufAppend(out, "null", 4); break;
        case JSON_V0_STRING:
            if (!v0AppendString(rdb, out)) goto done;
            break;
        case JSON_V0_DOUBLE: {
            /* The external module turns a legacy double into "%.17g" text. */
            double d = ValkeyModule_LoadDouble(rdb);
            if (ValkeyModule_IsIOError(rdb)) goto done;
            int n = snprintf(num, sizeof(num), "%.17g", d);
            jsonBufAppend(out, num, (size_t)n);
            break;
        }
        case JSON_V0_INTEGER: {
            int64_t v = ValkeyModule_LoadSigned(rdb);
            if (ValkeyModule_IsIOError(rdb)) goto done;
            jsonBufAppend(out, num, (size_t)jsonInt64Print(v, num));
            break;
        }
        case JSON_V0_BOOLEAN: {
            size_t len;
            char *s = ValkeyModule_LoadStringBuffer(rdb, &len);
            char c = (s && len == 1) ? *s : 0;
            if (s) ValkeyModule_Free(s);
            if (ValkeyModule_IsIOError(rdb) || (c != '0' && c != '1')) goto done;
            if (c == '1') {
                jsonBufAppend(out, "true", 4);
            } else {
                jsonBufAppend(out, "false", 5);
            }
            break;
        }
        case JSON_V0_OBJECT:
        case JSON_V0_ARRAY: {
            uint64_t count = ValkeyModule_LoadUnsigned(rdb);
            if (ValkeyModule_IsIOError(rdb)) goto done;
            int is_object = (code == JSON_V0_OBJECT);
            jsonBufAppend(out, is_object ? "{" : "[", 1);
            if (count == 0) {
                jsonBufAppend(out, is_object ? "}" : "]", 1);
                break;
            }
            if (depth >= jsonLoadDepthLimit()) goto done;
            if (depth == cap) {
                cap = cap ? cap * 2 : 32;
                stack = ValkeyModule_Realloc(stack, cap * sizeof(*stack));
            }
            stack[depth].left = count;
            stack[depth].is_object = is_object;
            depth++;
            opened = 1;
            break;
        }
        default: goto done;
        }
        if (opened) continue;
        /* A value is complete: close every container it finishes. */
        while (depth) {
            v0Frame *top = &stack[depth - 1];
            if (--top->left) {
                jsonBufAppend(out, ",", 1);
                break;
            }
            jsonBufAppend(out, top->is_object ? "}" : "]", 1);
            depth--;
        }
        if (depth == 0) {
            ok = 1;
            goto done;
        }
    }
done:
    if (stack) ValkeyModule_Free(stack);
    return ok;
}

static void *jsonTypeRdbLoad(ValkeyModuleIO *rdb, int encver) {
    jsonVal *val = NULL;
    jsonErr err = JSON_OK;

    if (encver == JSON_TYPE_ENCVER) {
        size_t len;
        char *text = ValkeyModule_LoadStringBuffer(rdb, &len);
        if (text == NULL || ValkeyModule_IsIOError(rdb)) {
            if (text) ValkeyModule_Free(text);
            return NULL;
        }
        val = jsonValParse(text, len, JSON_PARSE_TRUSTED, jsonLoadDepthLimit(), &err, NULL);
        ValkeyModule_Free(text);
    } else if (encver == 0) {
        jsonBuf text;
        jsonBufInit(&text, NULL, 0);
        if (v0LoadAsText(rdb, &text)) {
            val = jsonValParse(text.p, text.len, JSON_PARSE_TRUSTED, jsonLoadDepthLimit(), &err, NULL);
        }
        jsonBufFree(&text);
    } else {
        ValkeyModule_LogIOError(rdb, "warning", "Cannot load JSON data type version %d: this server reads versions 0 and %d.",
                                encver, JSON_TYPE_ENCVER);
        return NULL;
    }
    if (val == NULL) {
        ValkeyModule_LogIOError(rdb, "warning", "Unable to load JSON document: %s",
                                err == JSON_ERR_DEPTH ? "nested deeper than this server loads" : "invalid payload");
        return NULL;
    }
    jsonDoc doc = jsonValToDoc(val);
    size_t size = jsonDocMemUsage(doc);
    jsonStatsOnInsert(1, 0, size, size);
    return jsonValueNew(doc);
}

/* Can run on a save thread (forkless save). It only reads the document. */
static void jsonTypeRdbSave(ValkeyModuleIO *rdb, void *value) {
    jsonRef root;
    jsonBuf text;
    jsonDocRoot(jsonDocFromValue(value), &root);
    jsonBufInit(&text, NULL, 0);
    jsonSerialize(&root, NULL, &text);
    ValkeyModule_SaveStringBuffer(rdb, text.p, text.len);
    jsonBufFree(&text);
    if (ValkeyModule_IsIOError(rdb)) {
        ValkeyModule_LogIOError(rdb, "warning", "Unable to save JSON object, I/O error");
    }
}

static void jsonTypeAofRewrite(ValkeyModuleIO *aof, ValkeyModuleString *key, void *value) {
    jsonRef root;
    jsonBuf text;
    jsonDocRoot(jsonDocFromValue(value), &root);
    jsonBufInit(&text, NULL, 0);
    jsonSerialize(&root, NULL, &text);
    ValkeyModule_EmitAOF(aof, "JSON.SET", "scb", key, ".", text.p, text.len);
    jsonBufFree(&text);
}

/* ----------------------------------------------------------------------------
 * Other data type callbacks
 * ------------------------------------------------------------------------- */

/* Can run on a lazy-free thread. */
static void jsonTypeFree(void *value) {
    jsonDoc doc = jsonDocFromValue(value);
    size_t size = jsonDocMemUsage(doc);
    jsonStatsOnDelete(1, size, 0, size);
    jsonDocFree(doc);
#if JSON_DOC_BOXED
    ValkeyModule_Free(value);
#endif
}

static size_t jsonTypeMemUsage(ValkeyModuleKeyOptCtx *ctx, const void *value, size_t sample_size) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(sample_size);
    size_t size = jsonDocMemUsage(jsonDocFromValue(value));
#if JSON_DOC_BOXED
    size += ValkeyModule_MallocUsableSize((void *)value);
#endif
    return size;
}

static size_t jsonTypeFreeEffort(ValkeyModuleKeyOptCtx *ctx, const void *value) {
    VALKEYMODULE_NOT_USED(ctx);
    return jsonDocFreeEffort(jsonDocFromValue(value));
}

static void *jsonTypeCopy(ValkeyModuleKeyOptCtx *ctx, const void *value) {
    VALKEYMODULE_NOT_USED(ctx);
    jsonDoc copy = jsonDocCopy(jsonDocFromValue(value));
    size_t size = jsonDocMemUsage(copy);
    jsonStatsOnInsert(1, 0, size, size);
    return jsonValueNew(copy);
}

/* Feeds one scalar to the digest the way the external module does. */
static void digestScalar(ValkeyModuleDigest *md, const jsonRef *r) {
    switch (jsonRefType(r)) {
    case JSON_NULL: ValkeyModule_DigestAddLongLong(md, -1); break;
    case JSON_FALSE: ValkeyModule_DigestAddLongLong(md, 0); break;
    case JSON_TRUE: ValkeyModule_DigestAddLongLong(md, 1); break;
    case JSON_INT: {
        int64_t v = 0;
        jsonRefInt(r, &v);
        ValkeyModule_DigestAddLongLong(md, v);
        break;
    }
    case JSON_DOUBLE:
    case JSON_RAWNUM: {
        double d = jsonRefDouble(r);
        int64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        ValkeyModule_DigestAddLongLong(md, bits);
        break;
    }
    case JSON_STRING: {
        uint32_t len;
        const char *s = jsonRefString(r, &len);
        ValkeyModule_DigestAddStringBuffer(md, s, len);
        break;
    }
    default: break;
    }
    ValkeyModule_DigestEndSequence(md);
}

/* The digest covers the document in order: each container contributes its
 * count, each member its name, each scalar its value. Not recursive. */
static void jsonTypeDigest(ValkeyModuleDigest *md, void *value) {
    jsonIter *stack = NULL;
    size_t depth = 0, cap = 0;
    jsonRef cur, child;
    jsonStep step;
    const char *name;
    uint32_t name_len;

    jsonDocRoot(jsonDocFromValue(value), &cur);
    for (;;) {
        jsonType t = jsonRefType(&cur);
        if (t == JSON_ARRAY || t == JSON_OBJECT) {
            ValkeyModule_DigestAddLongLong(md, jsonRefCount(&cur));
            ValkeyModule_DigestEndSequence(md);
            if (depth == cap) {
                cap = cap ? cap * 2 : 32;
                stack = ValkeyModule_Realloc(stack, cap * sizeof(*stack));
            }
            jsonIterInit(&stack[depth++], &cur);
        } else {
            digestScalar(md, &cur);
        }
        /* Next value in document order. */
        for (;;) {
            if (depth == 0) goto done;
            name = NULL;
            if (jsonIterNext(&stack[depth - 1], &child, &step, &name, &name_len)) break;
            depth--;
        }
        if (name) ValkeyModule_DigestAddStringBuffer(md, name, name_len);
        cur = child;
    }
done:
    if (stack) ValkeyModule_Free(stack);
}

static void *defragMove(void *ptr, void *ctx) {
    return ValkeyModule_DefragAlloc(ctx, ptr);
}

static int defragStop(void *ctx) {
    return ValkeyModule_DefragShouldStop(ctx);
}

/* The server offers a cursor only for values whose free effort is above
 * active-defrag-max-scan-fields. Smaller documents are done in one call. */
static int jsonTypeDefrag(ValkeyModuleDefragCtx *ctx, ValkeyModuleString *key, void **value) {
    VALKEYMODULE_NOT_USED(key);
#if JSON_DOC_BOXED
    /* The cell that holds the document word is an allocation too. */
    void *moved = ValkeyModule_DefragAlloc(ctx, *value);
    if (moved) *value = moved;
#endif
    jsonDoc doc = jsonDocFromValue(*value);
    unsigned long cursor = 0;
    int has_cursor = (ValkeyModule_DefragCursorGet(ctx, &cursor) == VALKEYMODULE_OK);
    if (!has_cursor) cursor = 0;
    int more = jsonDocDefrag(&doc, defragMove, defragStop, ctx, &cursor);
#if JSON_DOC_BOXED
    *(jsonDoc *)*value = doc;
#else
    *value = (void *)(uintptr_t)doc;
#endif
    if (!more) return 0;
    /* More to do. Without a cursor the server cannot resume, so stop here:
     * the rest is picked up on the next defrag cycle from the start. */
    if (ValkeyModule_DefragCursorSet(ctx, cursor) != VALKEYMODULE_OK) return 0;
    return 1;
}

/* ----------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

static long long configGet(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(name);
    return *(long long *)privdata;
}

static int configSet(const char *name, long long val, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(err);
    *(long long *)privdata = val;
    return VALKEYMODULE_OK;
}

static int configApplyCaps(ValkeyModuleCtx *ctx, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(err);
    jsonEngineSetCaps((uint32_t)jsonConfig.max_packed_bytes, (uint32_t)jsonConfig.max_packed_entries);
    return VALKEYMODULE_OK;
}

static int registerConfigs(ValkeyModuleCtx *ctx) {
    if (ValkeyModule_RegisterNumericConfig(ctx, "max-document-size", JSON_DEFAULT_MAX_DOCUMENT_SIZE,
                                           VALKEYMODULE_CONFIG_MEMORY, 0, LLONG_MAX, configGet, configSet, NULL,
                                           &jsonConfig.max_document_size) == VALKEYMODULE_ERR ||
        ValkeyModule_RegisterNumericConfig(ctx, "max-path-limit", JSON_DEFAULT_MAX_PATH_LIMIT,
                                           VALKEYMODULE_CONFIG_DEFAULT, 0, INT_MAX, configGet, configSet, NULL,
                                           &jsonConfig.max_path_limit) == VALKEYMODULE_ERR ||
        ValkeyModule_RegisterNumericConfig(ctx, "max-packed-bytes", JSON_DEFAULT_MAX_PACKED_BYTES,
                                           VALKEYMODULE_CONFIG_DEFAULT, 0, JSON_MAX_PACKED_BYTES_LIMIT, configGet,
                                           configSet, configApplyCaps, &jsonConfig.max_packed_bytes) == VALKEYMODULE_ERR ||
        ValkeyModule_RegisterNumericConfig(ctx, "max-packed-entries", JSON_DEFAULT_MAX_PACKED_ENTRIES,
                                           VALKEYMODULE_CONFIG_DEFAULT, 0, JSON_MAX_PACKED_BYTES_LIMIT, configGet,
                                           configSet, configApplyCaps, &jsonConfig.max_packed_entries) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return ValkeyModule_LoadConfigs(ctx);
}

/* ----------------------------------------------------------------------------
 * Shared API and server events
 * ------------------------------------------------------------------------- */

/* JSON_GetValue, as other modules (valkey-search) call it. */
static int jsonSharedApiGetValue(ValkeyModuleKey *key, const char *path, ValkeyModuleString **result) {
    char stack_buf[1024];
    jsonBuf out;
    jsonBufInit(&out, stack_buf, sizeof(stack_buf));
    int rc = VALKEYMODULE_ERR;
    if (jsonSharedGetValue(key, path, strlen(path), &out) == 0) {
        *result = ValkeyModule_CreateString(NULL, out.p, out.len);
        rc = VALKEYMODULE_OK;
    }
    jsonBufFree(&out);
    return rc;
}

static void jsonCronHandler(ValkeyModuleCtx *ctx, ValkeyModuleEvent eid, uint64_t subevent, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(eid);
    VALKEYMODULE_NOT_USED(subevent);
    VALKEYMODULE_NOT_USED(data);
    jsonKeyTableSweep();
}

/* ----------------------------------------------------------------------------
 * Entry points. The server resolves these two symbols by name
 * (moduleLoadStatic).
 * ------------------------------------------------------------------------- */

int ValkeyModule_OnLoad_json(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, JSON_MODULE_NAME, JSON_MODULE_VERSION, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    static const jsonAllocator allocator = {glueAlloc, glueRealloc, glueFree, glueUsable};
    jsonEngineInit(&allocator);
    /* Member names come from clients, so the key table hash is seeded. */
    uint8_t seed[16];
    ValkeyModule_GetRandomBytes(seed, sizeof(seed));
    jsonEngineSetHashSeed(seed);

    ValkeyModuleTypeMethods tm;
    memset(&tm, 0, sizeof(tm));
    tm.version = VALKEYMODULE_TYPE_METHOD_VERSION;
    tm.rdb_load = jsonTypeRdbLoad;
    tm.rdb_save = jsonTypeRdbSave;
    tm.aof_rewrite = jsonTypeAofRewrite;
    tm.free = jsonTypeFree;
    tm.digest = jsonTypeDigest;
    tm.defrag = jsonTypeDefrag;
    tm.mem_usage2 = jsonTypeMemUsage;
    tm.free_effort2 = jsonTypeFreeEffort;
    tm.copy2 = jsonTypeCopy;
    jsonModuleType = ValkeyModule_CreateDataType(ctx, JSON_TYPE_NAME, JSON_TYPE_ENCVER, &tm);
    if (jsonModuleType == NULL) {
        ValkeyModule_Log(ctx, "warning", "Failed to create data type %s", JSON_TYPE_NAME);
        return VALKEYMODULE_ERR;
    }

    /* The save callback is thread-safe and every command declares its keys,
     * so forkless save and atomic slot migration stay available to a server
     * that has this module, which is every server. */
    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_IO_ERRORS | VALKEYMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD |
                                           VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION |
                                           VALKEYMODULE_OPTIONS_HANDLE_FORKLESS);

    if (registerConfigs(ctx) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to register module configs");
        return VALKEYMODULE_ERR;
    }
    jsonEngineSetCaps((uint32_t)jsonConfig.max_packed_bytes, (uint32_t)jsonConfig.max_packed_entries);

    if (ValkeyModule_RegisterInfoFunc(ctx, jsonInfo) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (ValkeyModule_AddACLCategory(ctx, "json") == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (jsonRegisterCommands(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (ValkeyModule_ExportSharedAPI(ctx, "JSON_GetValue", (void *)(uintptr_t)jsonSharedApiGetValue) != VALKEYMODULE_OK) {
        return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_CronLoop, jsonCronHandler) != VALKEYMODULE_OK) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}

/* A module that exports a data type is never unloaded. The symbol has to
 * exist because the server looks it up at shutdown. */
int ValkeyModule_OnUnload_json(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);
    return VALKEYMODULE_OK;
}
