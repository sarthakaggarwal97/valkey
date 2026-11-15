/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* apimod.c -- a test module that calls the C functions the JSON module exports
 * to other modules (json_api.h) and replies with what they return.
 *
 * It declares the five functions itself and leaves them undefined, so the
 * loader resolves them when the module is loaded: from the server binary when
 * JSON is built in, from the external JSON module's library otherwise. That is
 * what a real consumer does.
 *
 *   APIMOD.ISKEY key                  -> [is_json_key, is_json_key2]
 *   APIMOD.TYPE key path              -> [result, type or null, length]
 *   APIMOD.VALUE key path             -> [result, value or null, length]
 *   APIMOD.MULTI key path [path ...]  -> [result, values, lengths, types, type lengths]
 *   APIMOD.VALUES key path [path ...] -> [result, values, lengths]  (types not asked for)
 *
 * Everything the functions hand back is freed here with ValkeyModule_Free. */

#include "valkeymodule.h"

#include <string.h>

extern int is_json_key(ValkeyModuleCtx *ctx, ValkeyModuleKey *key);
extern int is_json_key2(ValkeyModuleCtx *ctx, ValkeyModuleString *keystr);
extern int get_json_value_type(ValkeyModuleCtx *ctx,
                               const char *keyname,
                               const size_t key_len,
                               const char *path,
                               char **type,
                               size_t *len);
extern int get_json_value(ValkeyModuleCtx *ctx,
                          const char *keyname,
                          const size_t key_len,
                          const char *path,
                          char **value,
                          size_t *len);
extern int get_json_values_and_types(ValkeyModuleCtx *ctx,
                                     const char *keyname,
                                     const size_t key_len,
                                     const char **paths,
                                     const int num_paths,
                                     char ***values,
                                     size_t **lengths,
                                     char ***types,
                                     size_t **type_lengths);

static void replyString(ValkeyModuleCtx *ctx, const char *s, size_t len) {
    if (s == NULL) {
        ValkeyModule_ReplyWithNull(ctx);
    } else if (strlen(s) != len) {
        /* The string must end with a NUL right after `len` bytes. */
        ValkeyModule_ReplyWithError(ctx, "ERR string is not NUL-terminated at its length");
    } else {
        ValkeyModule_ReplyWithStringBuffer(ctx, s, len);
    }
}

static int isKeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModuleKey *key;
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithLongLong(ctx, is_json_key(ctx, key));
    ValkeyModule_ReplyWithLongLong(ctx, is_json_key2(ctx, argv[1]));
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

static int oneCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int want_type) {
    size_t key_len, len = 12345; /* The functions must set the outputs even when they fail. */
    char *out = (char *)ctx;
    const char *key, *path;
    int rc;

    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    key = ValkeyModule_StringPtrLen(argv[1], &key_len);
    path = ValkeyModule_StringPtrLen(argv[2], NULL);
    rc = want_type ? get_json_value_type(ctx, key, key_len, path, &out, &len)
                   : get_json_value(ctx, key, key_len, path, &out, &len);
    ValkeyModule_ReplyWithArray(ctx, 3);
    ValkeyModule_ReplyWithLongLong(ctx, rc);
    replyString(ctx, out, len);
    ValkeyModule_ReplyWithLongLong(ctx, (long long)len);
    if (out != NULL) ValkeyModule_Free(out);
    return VALKEYMODULE_OK;
}

static int typeCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return oneCommand(ctx, argv, argc, 1);
}

static int valueCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return oneCommand(ctx, argv, argc, 0);
}

static void replyStrings(ValkeyModuleCtx *ctx, char **strings, size_t *lengths, int n) {
    if (strings == NULL || lengths == NULL) {
        ValkeyModule_ReplyWithNull(ctx);
        ValkeyModule_ReplyWithNull(ctx);
        return;
    }
    ValkeyModule_ReplyWithArray(ctx, n);
    for (int i = 0; i < n; i++) replyString(ctx, strings[i], lengths[i]);
    ValkeyModule_ReplyWithArray(ctx, n);
    for (int i = 0; i < n; i++) ValkeyModule_ReplyWithLongLong(ctx, (long long)lengths[i]);
}

static void freeStrings(char **strings, size_t *lengths, int n) {
    if (strings != NULL) {
        for (int i = 0; i < n; i++) {
            if (strings[i] != NULL) ValkeyModule_Free(strings[i]);
        }
        ValkeyModule_Free(strings);
    }
    if (lengths != NULL) ValkeyModule_Free(lengths);
}

static int manyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int want_types) {
    int n = argc - 2, rc;
    size_t key_len;
    const char *key, **paths;
    char **values = (char **)ctx, **types = (char **)ctx;
    size_t *lengths = (size_t *)ctx, *type_lengths = (size_t *)ctx;

    if (argc < 3) return ValkeyModule_WrongArity(ctx);
    key = ValkeyModule_StringPtrLen(argv[1], &key_len);
    paths = ValkeyModule_Alloc((size_t)n * sizeof(char *));
    for (int i = 0; i < n; i++) paths[i] = ValkeyModule_StringPtrLen(argv[i + 2], NULL);
    rc = get_json_values_and_types(ctx, key, key_len, paths, n, &values, &lengths, want_types ? &types : NULL,
                                   want_types ? &type_lengths : NULL);
    ValkeyModule_ReplyWithArray(ctx, want_types ? 5 : 3);
    ValkeyModule_ReplyWithLongLong(ctx, rc);
    replyStrings(ctx, values, lengths, n);
    freeStrings(values, lengths, n);
    if (want_types) {
        replyStrings(ctx, types, type_lengths, n);
        freeStrings(types, type_lengths, n);
    }
    ValkeyModule_Free(paths);
    return VALKEYMODULE_OK;
}

static int multiCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return manyCommand(ctx, argv, argc, 1);
}

static int valuesCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return manyCommand(ctx, argv, argc, 0);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "apimod", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "apimod.iskey", isKeyCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "apimod.type", typeCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "apimod.value", valueCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "apimod.multi", multiCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "apimod.values", valuesCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}
