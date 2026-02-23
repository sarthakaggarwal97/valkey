/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_STREAM_H
#define COMPRESSION_STREAM_H

#include "compression.h"

/* Generic caller-agnostic streaming writer config.
 * Defaults:
 * - content_checksum: 0 (off)
 * - raw_frame: 0 (emit VKCS envelope before compressed frame) */
typedef struct {
    compression_algo_t algo;
    int level;
    uint8_t stream_kind;  /* STREAM_KIND_RDB or STREAM_KIND_REPL */
    int content_checksum; /* LZ4 frame content checksum toggle */
    int raw_frame;        /* 1 => emit raw codec frame (no VKCS envelope) */
} stream_writer_config_t;

/* Opaque writer context owned by the streaming writer API. */
typedef struct stream_writer stream_writer_t;

/* Generic streaming writer API. */
stream_writer_t *stream_writer_create(const stream_writer_config_t *cfg,
                                      vkcsEmitFn emit_cb,
                                      void *emit_ctx);
int stream_writer_write(stream_writer_t *t, const void *buf, size_t len);
int stream_writer_flush(stream_writer_t *t);
int stream_writer_finish(stream_writer_t *t);
void stream_writer_destroy(stream_writer_t *t);
int stream_writer_is_errored(const stream_writer_t *t);
int stream_writer_is_finished(const stream_writer_t *t);
void stream_writer_set_error(stream_writer_t *t);

#endif /* COMPRESSION_STREAM_H */
