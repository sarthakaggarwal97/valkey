/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Streaming compression/decompression using algorithm-native framing.
 * LZ4 via LZ4F frame API, ZSTD via streaming API.
 * See .kiro/specs/compression-module/design-compact.md for full design. */

#include "compression.h"

#include <string.h>

/* --- Envelope --- */

/* Write 8-byte VKCS envelope via callback.
 * Shared utility used by BOTH sync (rio decorator) and async (replication)
 * paths.  Prevents "sync emits envelope but async forgets it" class of bugs.
 *
 * Layout (little-endian where applicable):
 *   [0..3] magic  "VKCS" (0x56 0x4B 0x43 0x53)
 *   [4]    version (VKCS_VERSION, currently 1)
 *   [5]    algo_id (compression_algo_t value)
 *   [6]    flags   (bit 0 = stream_kind: 0=RDB, 1=REPL)
 *   [7]    reserved (must be 0)
 *
 * Returns 0 on success, -1 on error (invalid algo). */
int write_vkcs_envelope(void (*emit_cb)(void *ctx, const uint8_t *data, size_t len),
                        void *ctx,
                        compression_algo_t algo,
                        uint8_t stream_kind) {
    /* Only streaming algorithms are valid in the envelope. */
    if (algo != ALGO_LZ4 && algo != ALGO_ZSTD) return -1;
    if (stream_kind != STREAM_KIND_RDB && stream_kind != STREAM_KIND_REPL) return -1;

    uint8_t envelope[VKCS_ENVELOPE_SIZE];
    envelope[0] = VKCS_MAGIC_0;
    envelope[1] = VKCS_MAGIC_1;
    envelope[2] = VKCS_MAGIC_2;
    envelope[3] = VKCS_MAGIC_3;
    envelope[4] = VKCS_VERSION;
    envelope[5] = (uint8_t)algo;
    envelope[6] = stream_kind & 0x01; /* bit 0 = stream_kind */
    envelope[7] = 0;                  /* reserved */

    emit_cb(ctx, envelope, VKCS_ENVELOPE_SIZE);
    return 0;
}

/* Parse 8-byte VKCS envelope from buffer.
 * Validates magic bytes, version, and algorithm.
 * On success populates *algo and *stream_kind and returns 0.
 * Returns -1 on error (bad magic, unsupported version, unknown algo). */
int envelope_read(const uint8_t *buf, size_t len, compression_algo_t *algo, uint8_t *stream_kind) {
    if (len < VKCS_ENVELOPE_SIZE) return -1;

    /* Validate magic */
    if (buf[0] != VKCS_MAGIC_0 || buf[1] != VKCS_MAGIC_1 ||
        buf[2] != VKCS_MAGIC_2 || buf[3] != VKCS_MAGIC_3) {
        return -1;
    }

    /* Validate version — only version 1 is supported */
    if (buf[4] != VKCS_VERSION) return -1;

    /* Extract and validate algorithm */
    uint8_t algo_id = buf[5];
    if (algo_id != ALGO_LZ4 && algo_id != ALGO_ZSTD) return -1;

    /* Extract flags */
    uint8_t flags = buf[6];
    uint8_t kind = flags & 0x01;

    if (algo) *algo = (compression_algo_t)algo_id;
    if (stream_kind) *stream_kind = kind;

    return 0;
}

/* --- Streaming compressor --- */

int stream_compressor_init(stream_compressor_t *sc, compression_algo_t algo, int level) {
    /* TODO: Task 2.1 (LZ4), Task 22.1 (ZSTD) */
    (void)sc;
    (void)algo;
    (void)level;
    return -1;
}

void stream_compressor_destroy(stream_compressor_t *sc) {
    /* TODO: Task 2.1 (LZ4), Task 22.1 (ZSTD) */
    (void)sc;
}

int stream_decompressor_init(stream_decompressor_t *sd, compression_algo_t algo) {
    /* TODO: Task 2.2 (LZ4), Task 22.2 (ZSTD) */
    (void)sd;
    (void)algo;
    return -1;
}

void stream_decompressor_destroy(stream_decompressor_t *sd) {
    /* TODO: Task 2.2 (LZ4), Task 22.2 (ZSTD) */
    (void)sd;
}

size_t streamCompressOutputBound(compression_algo_t algo, size_t input_len, int frame_started, int flush_mode) {
    /* TODO: Task 2.3 */
    (void)algo;
    (void)input_len;
    (void)frame_started;
    (void)flush_mode;
    return 0;
}

ssize_t streamCompressFeed(stream_compressor_t *sc,
                           uint8_t **output_ptr,
                           size_t output_capacity,
                           const uint8_t *input,
                           size_t input_len,
                           int flush_mode) {
    /* TODO: Task 2.3 */
    (void)sc;
    (void)output_ptr;
    (void)output_capacity;
    (void)input;
    (void)input_len;
    (void)flush_mode;
    return -1;
}

ssize_t streamDecompressFeed(stream_decompressor_t *sd,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             size_t *input_consumed) {
    /* TODO: Task 2.2 */
    (void)sd;
    (void)output;
    (void)output_capacity;
    (void)input;
    (void)input_len;
    (void)input_consumed;
    return -1;
}
