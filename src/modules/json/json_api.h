/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* json_api.h -- the C functions the JSON module exports to other modules.
 *
 * These are the five functions of the external valkey-json module's
 * src/json/json_api.h, with the same names, signatures, results and memory
 * rules. Another module declares them and resolves them at load time from the
 * server binary, as it resolved them from the JSON module's shared library.
 *
 * Memory. Every string and every array handed back through an output
 * parameter is allocated with ValkeyModule_Alloc. The caller frees each one
 * with ValkeyModule_Free. Strings are NUL-terminated, and the reported length
 * does not count the terminator.
 *
 * Results. 0 is success. An error is -1, which is what the external module
 * returns (its header says 1). On an error every output is NULL or 0. */

#ifndef VALKEY_JSON_API_H
#define VALKEY_JSON_API_H

#include <stdlib.h>

typedef struct ValkeyModuleCtx ValkeyModuleCtx;
typedef struct ValkeyModuleKey ValkeyModuleKey;
typedef struct ValkeyModuleString ValkeyModuleString;

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the open key holds a JSON document, else 0. A NULL key is 0. */
int is_json_key(ValkeyModuleCtx *ctx, ValkeyModuleKey *key);

/* The same for a key name. The key is opened for reading and closed again. */
int is_json_key2(ValkeyModuleCtx *ctx, ValkeyModuleString *keystr);

/* The type of the value at `path`, as JSON.TYPE names it ("object", "array",
 * "string", "integer", "number", "boolean", "null"). If the path selects
 * several values, the type of the first one. `path` is a NUL-terminated
 * string.
 *
 * Fails if the key is not a JSON document, the path is invalid, or it selects
 * nothing. */
int get_json_value_type(ValkeyModuleCtx *ctx,
                        const char *keyname,
                        const size_t key_len,
                        const char *path,
                        char **type,
                        size_t *len);

/* The compact JSON text JSON.GET replies for one path. A legacy path gives
 * its first value and fails if it selects nothing. A JSONPath ("$") path
 * gives a JSON array of every value it selects, "[]" for none.
 *
 * Fails if the key is not a JSON document or the path is invalid. */
int get_json_value(ValkeyModuleCtx *ctx,
                   const char *keyname,
                   const size_t key_len,
                   const char *path,
                   char **value,
                   size_t *len);

/* get_json_value and get_json_value_type for `num_paths` paths at once.
 *
 * *values and *lengths receive arrays of num_paths entries. An entry whose
 * path failed is NULL with length 0. `types` may be NULL to skip the types.
 * Otherwise `type_lengths` must not be NULL, and both receive arrays in the
 * same way.
 *
 * Fails, with every output NULL, only if the key is not a JSON document. The
 * caller frees the four arrays and every string in them. */
int get_json_values_and_types(ValkeyModuleCtx *ctx,
                              const char *keyname,
                              const size_t key_len,
                              const char **paths,
                              const int num_paths,
                              char ***values,
                              size_t **lengths,
                              char ***types,
                              size_t **type_lengths);

#ifdef __cplusplus
}
#endif

#endif /* VALKEY_JSON_API_H */
