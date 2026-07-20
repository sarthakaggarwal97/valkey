/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_LZ4_H
#define COMPRESSION_LZ4_H

#include "compression.h"

typedef enum {
    COMPRESS_FLUSH_CONTINUE = 0, /* Buffer internally. */
    COMPRESS_FLUSH_SYNC = 1,     /* Drain buffered bytes, keep frame open. */
    COMPRESS_FLUSH_END = 2,      /* Finalize frame. */
} compressionFlushMode;

typedef struct {
    int level; /* 0 selects the codec default. */
    void *ctx;
    bool stream_started;
    bool codec_checksum;
} compressionLz4Compressor;

typedef struct {
    bool frame_done;
    bool skip_codec_checksum_validation;
    void *ctx;
    size_t input_hint; /* Preferred compressed bytes for next feed, 0 if unknown. */
} compressionLz4Decompressor;

/* Initializes LZ4 compressor state. */
void compressionLz4CompressorInit(compressionLz4Compressor *compressor, int level, bool codec_checksum);

/* Releases LZ4 compressor resources. */
void compressionLz4CompressorFree(compressionLz4Compressor *compressor);

/* Initializes LZ4 decompressor state. */
void compressionLz4DecompressorInit(compressionLz4Decompressor *decompressor, bool skip_codec_checksum_validation);

/* Releases LZ4 decompressor resources. */
void compressionLz4DecompressorFree(compressionLz4Decompressor *decompressor);

/* Returns a conservative upper bound for any flush mode. */
size_t compressionLz4OutputBound(size_t input_len);

/* Compresses input into output. input may be NULL when input_len is zero. */
ssize_t compressionLz4CompressFeed(compressionLz4Compressor *compressor,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressionFlushMode flush_mode);

/* Decompresses input into output and reports consumed input bytes. */
ssize_t compressionLz4DecompressFeed(compressionLz4Decompressor *decompressor,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     const uint8_t *input,
                                     size_t input_len,
                                     size_t *input_consumed);

#endif /* COMPRESSION_LZ4_H */
