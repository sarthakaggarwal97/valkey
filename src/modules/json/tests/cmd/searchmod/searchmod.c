/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* searchmod.c -- a test module that uses the JSON module the way valkey-search
 * does (see the table at the top of ../test_search.py for what that is and
 * where it was read).
 *
 *   - It finds the JSON module by calling MODULE LIST and looking for the
 *     name "json".
 *   - It takes the shared API "JSON_GetValue" as
 *     int (*)(ValkeyModuleKey *key, const char *path, ValkeyModuleString **result)
 *     and falls back to calling JSON.GET when the API is missing.
 *   - It opens keys with VALKEYMODULE_OPEN_KEY_NOEFFECTS | VALKEYMODULE_READ,
 *     treats a key as JSON when its type is VALKEYMODULE_KEYTYPE_MODULE, and
 *     frees a returned string with ValkeyModule_FreeString(NULL, ...).
 *   - It subscribes to MODULE, GENERIC, EXPIRED and EVICTED keyspace events,
 *     and on each event re-reads the key, as an index would.
 *
 * Loaded with no argument it refuses to load when JSON is not there. With the
 * argument "lenient" it loads anyway and reports JSON as unsupported, which
 * is what valkey-search itself does.
 *
 *   SEARCHMOD.STATUS            -> [json listed, shared API found, json version]
 *   SEARCHMOD.GET key path      -> [raw text, outcome, attribute]  (shared API)
 *   SEARCHMOD.CALLGET key path  -> the same through JSON.GET
 *   SEARCHMOD.TYPE key          -> -1 no key, 1 module type, 0 another type
 *   SEARCHMOD.EVENTS            -> the events recorded since the last call */

#include "valkeymodule.h"

#include <string.h>

typedef int (*JsonSharedAPIGetValueFn)(ValkeyModuleKey *key, const char *path, ValkeyModuleString **result);

static JsonSharedAPIGetValueFn json_get;
static int json_loaded; /* Cached only once it is true, as valkey-search does. */
static long long json_version = -1;

#define KEY_OPEN_MODE (VALKEYMODULE_OPEN_KEY_NOEFFECTS | VALKEYMODULE_READ)

/* MODULE LIST: an array of modules, each an array of name and value pairs. */
static int moduleListed(ValkeyModuleCtx *ctx, const char *name, long long *version) {
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "MODULE", "c", "LIST");
    int found = 0;
    if (reply == NULL) return 0;
    if (ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_ARRAY) {
        size_t modules = ValkeyModule_CallReplyLength(reply);
        for (size_t i = 0; i < modules && !found; i++) {
            ValkeyModuleCallReply *info = ValkeyModule_CallReplyArrayElement(reply, i);
            long long ver = -1;
            int is_it = 0;
            size_t len;
            if (info == NULL || ValkeyModule_CallReplyType(info) != VALKEYMODULE_REPLY_ARRAY) continue;
            len = ValkeyModule_CallReplyLength(info);
            for (size_t j = 0; j + 1 < len; j += 2) {
                ValkeyModuleCallReply *k = ValkeyModule_CallReplyArrayElement(info, j);
                ValkeyModuleCallReply *v = ValkeyModule_CallReplyArrayElement(info, j + 1);
                size_t klen = 0, vlen = 0;
                const char *ks = ValkeyModule_CallReplyStringPtr(k, &klen);
                const char *vs = ValkeyModule_CallReplyStringPtr(v, &vlen);
                if (ks == NULL) continue;
                if (klen == 4 && memcmp(ks, "name", 4) == 0 && vs != NULL && vlen == strlen(name) &&
                    memcmp(vs, name, vlen) == 0) {
                    is_it = 1;
                }
                if (klen == 3 && memcmp(ks, "ver", 3) == 0) ver = ValkeyModule_CallReplyInteger(v);
            }
            if (is_it) {
                found = 1;
                *version = ver;
            }
        }
    }
    ValkeyModule_FreeCallReply(reply);
    return found;
}

/* valkey-search's IsJsonModuleSupported(). */
static int jsonSupported(ValkeyModuleCtx *ctx) {
    if (json_loaded) return 1;
    json_loaded = moduleListed(ctx, "json", &json_version);
    if (!json_loaded) return 0;
    json_get = (JsonSharedAPIGetValueFn)(unsigned long)ValkeyModule_GetSharedAPI(ctx, "JSON_GetValue");
    return 1;
}

/* valkey-search's NormalizeJsonAttribute(), without the JSON unquoting: the
 * text must be a `$` path result, "[...]". The brackets and one pair of quotes
 * are taken off. */
static void replyAttribute(ValkeyModuleCtx *ctx, ValkeyModuleString *raw) {
    size_t len;
    const char *s = ValkeyModule_StringPtrLen(raw, &len);
    int was_string = 0;

    ValkeyModule_ReplyWithArray(ctx, 3);
    ValkeyModule_ReplyWithStringBuffer(ctx, s, len);
    if (len > 0 && s[0] != '[') {
        ValkeyModule_ReplyWithSimpleString(ctx, "invalid-attribute");
        ValkeyModule_ReplyWithNull(ctx);
        return;
    }
    if (len > 0 && s[0] == '[') {
        s++, len--;
        if (len > 0 && s[len - 1] == ']') len--;
        if (len > 0 && s[0] == '"') {
            s++, len--;
            if (len > 0 && s[len - 1] == '"') len--;
            was_string = 1;
        }
    }
    if (!was_string && len == 0) {
        ValkeyModule_ReplyWithSimpleString(ctx, "empty-attribute");
        ValkeyModule_ReplyWithNull(ctx);
        return;
    }
    ValkeyModule_ReplyWithSimpleString(ctx, was_string ? "string" : "value");
    ValkeyModule_ReplyWithStringBuffer(ctx, s, len);
}

/* valkey-search's GetJsonAttribute(). `use_call` forces the JSON.GET path it
 * takes when the shared API is missing. */
static int getCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int use_call) {
    const char *path;
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    path = ValkeyModule_StringPtrLen(argv[2], NULL);
    if (!jsonSupported(ctx)) return ValkeyModule_ReplyWithError(ctx, "ERR The JSON module is not supported");

    if (json_get != NULL && !use_call) {
        ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], KEY_OPEN_MODE);
        ValkeyModuleString *value = NULL;
        if (key == NULL) return ValkeyModule_ReplyWithError(ctx, "ERR Key not found");
        if (json_get(key, path, &value) == VALKEYMODULE_ERR) {
            ValkeyModule_ReplyWithError(ctx, "ERR No such attribute with identifier");
        } else {
            replyAttribute(ctx, value);
            ValkeyModule_FreeString(NULL, value);
        }
        ValkeyModule_CloseKey(key);
        return VALKEYMODULE_OK;
    }

    {
        const char *keyname = ValkeyModule_StringPtrLen(argv[1], NULL);
        ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "JSON.GET", "cc", keyname, path);
        if (reply == NULL) return ValkeyModule_ReplyWithError(ctx, "ERR No such attribute with identifier");
        if (ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_STRING) {
            ValkeyModule_ReplyWithError(ctx, "ERR JSON.GET returned a non string value");
        } else {
            ValkeyModuleString *value = ValkeyModule_CreateStringFromCallReply(reply);
            replyAttribute(ctx, value);
            ValkeyModule_FreeString(NULL, value);
        }
        ValkeyModule_FreeCallReply(reply);
    }
    return VALKEYMODULE_OK;
}

static int getApiCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return getCommand(ctx, argv, argc, 0);
}

static int getCallCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return getCommand(ctx, argv, argc, 1);
}

/* valkey-search's JsonAttributeDataType::IsProperType(). */
static int keyKind(ValkeyModuleCtx *ctx, ValkeyModuleString *name) {
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, name, KEY_OPEN_MODE);
    int kind;
    if (key == NULL) return -1;
    kind = ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_MODULE;
    ValkeyModule_CloseKey(key);
    return kind;
}

static int typeCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    return ValkeyModule_ReplyWithLongLong(ctx, keyKind(ctx, argv[1]));
}

static int statusCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_ReplyWithArray(ctx, 3);
    ValkeyModule_ReplyWithLongLong(ctx, jsonSupported(ctx));
    ValkeyModule_ReplyWithLongLong(ctx, json_get != NULL);
    ValkeyModule_ReplyWithLongLong(ctx, json_version);
    return VALKEYMODULE_OK;
}

/* ---- keyspace events ---------------------------------------------------- */

typedef struct eventRecord {
    int type;
    int kind;                  /* keyKind() of the key when the event arrived. */
    ValkeyModuleString *event; /* The event name. */
    ValkeyModuleString *key;
    ValkeyModuleString *document; /* JSON_GetValue(key, "$") at that moment, or NULL. */
} eventRecord;

#define MAX_EVENTS 4096
static eventRecord events[MAX_EVENTS];
static int num_events;

/* On every event the key is read again, as valkey-search's
 * IndexSchema::ProcessKeyspaceNotification() does. */
static int onKeyspaceEvent(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    eventRecord *rec;
    ValkeyModuleKey *open_key;

    if (num_events == MAX_EVENTS) return VALKEYMODULE_OK;
    rec = &events[num_events++];
    rec->type = type;
    rec->event = ValkeyModule_CreateString(NULL, event, strlen(event));
    rec->key = ValkeyModule_CreateStringFromString(NULL, key);
    rec->document = NULL;
    rec->kind = -1;
    open_key = ValkeyModule_OpenKey(ctx, key, KEY_OPEN_MODE);
    if (open_key != NULL) {
        rec->kind = ValkeyModule_KeyType(open_key) == VALKEYMODULE_KEYTYPE_MODULE;
        if (rec->kind == 1 && json_get != NULL && json_get(open_key, "$", &rec->document) == VALKEYMODULE_ERR) {
            rec->document = NULL;
        }
        ValkeyModule_CloseKey(open_key);
    }
    return VALKEYMODULE_OK;
}

static int eventsCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_ReplyWithArray(ctx, num_events);
    for (int i = 0; i < num_events; i++) {
        eventRecord *rec = &events[i];
        ValkeyModule_ReplyWithArray(ctx, 5);
        ValkeyModule_ReplyWithLongLong(ctx, rec->type);
        ValkeyModule_ReplyWithString(ctx, rec->event);
        ValkeyModule_ReplyWithString(ctx, rec->key);
        ValkeyModule_ReplyWithLongLong(ctx, rec->kind);
        if (rec->document != NULL) {
            ValkeyModule_ReplyWithString(ctx, rec->document);
            ValkeyModule_FreeString(NULL, rec->document);
        } else {
            ValkeyModule_ReplyWithNull(ctx);
        }
        ValkeyModule_FreeString(NULL, rec->event);
        ValkeyModule_FreeString(NULL, rec->key);
    }
    num_events = 0;
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    int lenient;

    if (ValkeyModule_Init(ctx, "searchmod", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    lenient = argc > 0 && strcmp(ValkeyModule_StringPtrLen(argv[0], NULL), "lenient") == 0;

    /* The JSON module must already be there when this runs: an external one
     * because it is loaded first, the built-in one because it is loaded before
     * every module of the configuration. */
    if (!jsonSupported(ctx) || json_get == NULL) {
        ValkeyModule_Log(ctx, "warning", "searchmod: Json not supported (module listed: %d, JSON_GetValue: %s)",
                         json_loaded, json_get != NULL ? "found" : "missing");
        if (!lenient) return VALKEYMODULE_ERR;
    } else {
        ValkeyModule_Log(ctx, "notice", "searchmod: Json supported");
    }

    if (ValkeyModule_SubscribeToKeyspaceEvents(ctx,
                                               VALKEYMODULE_NOTIFY_MODULE | VALKEYMODULE_NOTIFY_GENERIC |
                                                   VALKEYMODULE_NOTIFY_EXPIRED | VALKEYMODULE_NOTIFY_EVICTED,
                                               onKeyspaceEvent) != VALKEYMODULE_OK) {
        return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_CreateCommand(ctx, "searchmod.status", statusCommand, "readonly", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "searchmod.get", getApiCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "searchmod.callget", getCallCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "searchmod.type", typeCommand, "readonly", 1, 1, 1) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "searchmod.events", eventsCommand, "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}
