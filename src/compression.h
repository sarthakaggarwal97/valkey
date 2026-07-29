/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_H
#define COMPRESSION_H

#include "fmacros.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    ALGO_NONE = 0,
    ALGO_LZF = 1, /* Per-string LZF inside the RDB payload (legacy). */
    ALGO_LZ4 = 2,
} compressionAlgo;

/* Stable VCS wire codec identifiers. */
#define VCS_CODEC_LZ4 0x01

typedef enum {
    COMPRESS_FLUSH_CONTINUE = 0, /* Buffer internally. */
    COMPRESS_FLUSH_SYNC = 1,     /* Drain buffered bytes, keep frame open. */
    COMPRESS_FLUSH_END = 2,      /* Finalize frame. */
} compressFlushMode;

typedef struct streamCompressor streamCompressor;
typedef struct streamDecompressor streamDecompressor;

/* A streaming codec is selected once during initialization. Keeping dispatch
 * here avoids algorithm branches in the stream hot path and makes adding a
 * codec a descriptor registration rather than a set of parallel switches. */
typedef struct compressionCodec {
    compressionAlgo algo;
    uint8_t vcs_id;
    const char *name;
    size_t chunk_size;         /* Preferred uncompressed writer block size. */
    size_t decode_input_slack; /* Encoded framing overhead around one block. */

    int (*compressor_init)(streamCompressor *compressor);
    size_t (*compressor_output_bound)(const streamCompressor *compressor, size_t input_len);
    ssize_t (*compressor_feed)(streamCompressor *compressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               bool input_stable,
                               compressFlushMode flush_mode);
    void (*compressor_free)(streamCompressor *compressor);

    int (*decompressor_init)(streamDecompressor *decompressor);
    ssize_t (*decompressor_feed)(streamDecompressor *decompressor,
                                 uint8_t *output,
                                 size_t output_capacity,
                                 const uint8_t *input,
                                 size_t input_len,
                                 size_t *input_consumed);
    void (*decompressor_free)(streamDecompressor *decompressor);
} compressionCodec;

const compressionCodec *compressionCodecByAlgo(compressionAlgo algo);
const compressionCodec *compressionCodecByVcsId(uint8_t vcs_id);

/* Returns a static algorithm name for logs and config output. */
const char *compressionAlgoName(compressionAlgo algo);

/* ===== Compressor ===== */

struct streamCompressor {
    const compressionCodec *codec;
    int level; /* 0 selects the codec default. */
    void *ctx;
    bool stream_started;
    bool codec_checksum;
};

/* Compressor lifecycle. Codec dispatch used by streamWriter; the writer owns
 * sticky error state while these functions manage only codec state. */
int streamCompressorInit(streamCompressor *compressor, compressionAlgo algo, int level, bool codec_checksum);
size_t streamCompressorChunkSize(const streamCompressor *compressor);
size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len);
/* input_stable is valid only for CONTINUE and lets the codec retain references
 * to input. The bytes must remain unchanged until a later nonempty feed with
 * input_stable false returns, or until the frame ends. */
ssize_t streamCompressorFeed(streamCompressor *compressor,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             bool input_stable,
                             compressFlushMode flush_mode);
void streamCompressorFree(streamCompressor *compressor);

/* ===== Decompressor ===== */

struct streamDecompressor {
    const compressionCodec *codec;
    bool frame_done;
    bool skip_codec_checksum_validation;
    void *ctx;
    size_t input_hint; /* Preferred compressed bytes for next feed, 0 if unknown. */
};

/* Decompressor lifecycle. Codec dispatch used by streamReader; the reader owns
 * buffering and sticky error state. */
int streamDecompressorInit(streamDecompressor *decompressor,
                           compressionAlgo algo,
                           bool skip_codec_checksum_validation);
ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);
void streamDecompressorFree(streamDecompressor *decompressor);

#endif /* COMPRESSION_H */
