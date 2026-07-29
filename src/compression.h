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

typedef enum {
    FLUSH_CONTINUE = 0, /* Keep the frame open. */
    FLUSH_SYNC = 1,     /* Drain buffered bytes, keep frame open. */
    FLUSH_END = 2,      /* Finalize frame. */
} compressFlushMode;

/* Stable VCS wire codec identifiers. */
#define VCS_CODEC_LZ4 0x02

typedef struct streamCompressor streamCompressor;
typedef struct streamDecompressor streamDecompressor;

/* Streaming codecs are selected once at initialization. Keeping their
 * operations together makes adding a codec a descriptor registration instead
 * of another branch in every compression entry point. */
typedef struct compressionCodec {
    compressionAlgo algo;
    uint8_t vcs_id;
    const char *name;
    size_t chunk_size; /* Preferred uncompressed writer block size. */

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

struct streamCompressor {
    const compressionCodec *codec;
    int level; /* 0 selects the codec default. */
    void *ctx;
    bool stream_started;
    bool codec_checksum;
    bool errored;
};

struct streamDecompressor {
    const compressionCodec *codec;
    bool errored;
    bool frame_done;
    void *ctx;
    size_t input_hint; /* Preferred compressed bytes for next feed, 0 if unknown. */
};

/* Compression APIs expect caller-owned streamCompressor/streamDecompressor
 * storage and valid pointer arguments. Instances are not thread-safe. */

/* Returns the descriptor registered for algo, or NULL if unsupported. */
const compressionCodec *compressionCodecByAlgo(compressionAlgo algo);

/* Returns the descriptor registered for a VCS wire codec ID. */
const compressionCodec *compressionCodecByVcsId(uint8_t vcs_id);

/* Returns true when algo has a streaming codec implementation. */
bool compressionAlgoSupportsStreaming(compressionAlgo algo);

/* Returns a static algorithm name for logs and config output. */
const char *compressionAlgoName(compressionAlgo algo);

/* Initializes compressor state and immutable frame options. */
int streamCompressorInit(streamCompressor *compressor,
                         compressionAlgo algo,
                         int level,
                         bool codec_checksum);

/* Releases resources owned by an initialized compressor. */
void streamCompressorFree(streamCompressor *compressor);

/* Returns the active algorithm, or ALGO_NONE for an uninitialized compressor. */
compressionAlgo streamCompressorAlgo(const streamCompressor *compressor);

/* Returns the codec's preferred uncompressed writer block size. */
size_t streamCompressorChunkSize(const streamCompressor *compressor);

/* Initializes decompressor state for algo. Returns 0 on success. */
int streamDecompressorInit(streamDecompressor *decompressor, compressionAlgo algo);

/* Releases resources owned by an initialized decompressor. */
void streamDecompressorFree(streamDecompressor *decompressor);

/* Conservative bound covering any pending frame header, input, and enough
 * overhead to flush or end the frame. */
size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len);

/* Feeds input into the compressor and writes compressed bytes to output.
 * Called repeatedly to build one frame: FLUSH_CONTINUE leaves it open,
 * FLUSH_SYNC drains codec-buffered bytes, and FLUSH_END closes it. output must
 * be at least streamCompressorOutputBound(input_len) bytes. input_stable is
 * valid only with FLUSH_CONTINUE and promises that input remains unchanged
 * until a later nonempty feed returns or the frame closes. Returns bytes
 * written, or -1 on error. */
ssize_t streamCompressorFeed(streamCompressor *compressor,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             bool input_stable,
                             compressFlushMode flush_mode);

/* Decompresses input into output. When output fills before input is drained,
 * only part of input is used: *input_consumed reports how many input bytes
 * were read, and the caller feeds the rest on the next call with more output
 * space. Returns bytes written, or -1 on error. */
ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);

#endif /* COMPRESSION_H */
