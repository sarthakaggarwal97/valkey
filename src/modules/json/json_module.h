/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_module.h -- what the command file needs from the module glue.
 *
 * json_module.c owns the entry point, the data type and its callbacks, the
 * configuration, the statistics and the shared API. json_cmd.c owns the
 * commands. */

#ifndef JSON_MODULE_H
#define JSON_MODULE_H

#include "../../valkeymodule.h"
#include "json_engine.h"

#define JSON_MODULE_NAME "json"
#define JSON_TYPE_NAME "ReJSON-RL"
#define JSON_TYPE_ENCVER 3

extern ValkeyModuleType *jsonModuleType;

/* A key's value is the document word itself, stored in the value pointer, so
 * a document costs no allocation beyond its own nodes. */
static inline jsonDoc jsonDocFromValue(void *value) {
    return (jsonDoc)(uintptr_t)value;
}
static inline void *jsonDocToValue(jsonDoc d) {
    return (void *)(uintptr_t)d;
}

/* Configuration, kept current by the config callbacks. */
typedef struct jsonModuleConfig {
    long long max_document_size;  /* json.max-document-size. 0 means no limit. */
    long long max_path_limit;     /* json.max-path-limit: document nesting. */
    long long max_packed_bytes;   /* json.max-packed-bytes. */
    long long max_packed_entries; /* json.max-packed-entries. */
} jsonModuleConfig;
extern jsonModuleConfig jsonConfig;

/* The deepest nesting any document may have on this server. An ordinary client
 * is held to json.max-path-limit. Loading (RDB, RESTORE) and commands from a
 * primary or an AOF file are held to this larger bound, because the limit may
 * have been lowered after a document was written. */
uint32_t jsonLoadDepthLimit(void);

/* Statistics behind INFO and JSON.DEBUG. The arguments are the external
 * module's (src/json/stats.h): sizes in bytes of the document before and
 * after, and of the value read, written or removed. */
void jsonStatsOnRead(size_t fetched_val_size);
void jsonStatsOnInsert(int is_new_key, size_t orig_size, size_t new_size, size_t inserted_val_size);
void jsonStatsOnUpdate(size_t orig_size, size_t new_size, size_t input_json_size);
void jsonStatsOnDelete(int is_delete_key, size_t orig_size, size_t new_size, size_t deleted_val_size);
void jsonStatsUpdateMaxDepth(size_t depth);
unsigned long long jsonStatsMaxDepthEverSeen(void);
unsigned long long jsonStatsMaxSizeEverSeen(void);

/* Registers the 24 commands. Implemented in json_cmd.c, called once from the
 * entry point after the data type exists. Returns VALKEYMODULE_OK or
 * VALKEYMODULE_ERR. */
int jsonRegisterCommands(ValkeyModuleCtx *ctx);

/* Reads one path of one key for other modules (the JSON_GetValue shared API).
 * Implemented in json_cmd.c with the semantics of the external module's
 * src/json/shared_api.cc. On success returns 0 and appends the reply text to
 * out. */
int jsonSharedGetValue(ValkeyModuleKey *key, const char *path, size_t path_len, jsonBuf *out);

#endif /* JSON_MODULE_H */
