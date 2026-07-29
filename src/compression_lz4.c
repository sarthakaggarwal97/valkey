/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_lz4.h"
#include "serverassert.h"
#include "zmalloc.h"
#include <limits.h>

#define LZ4F_STATIC_LINKING_ONLY
#include <lz4frame.h>

#define LZ4_STREAM_BLOCK_SIZE (64 * 1024)

static void *lz4Zmalloc(void *opaque, size_t size) {
    (void)opaque;
    return zmalloc(size);
}

static void *lz4Zcalloc(void *opaque, size_t size) {
    (void)opaque;
    return zcalloc(size);
}

static void lz4Zfree(void *opaque, void *address) {
    (void)opaque;
    zfree(address);
}

static const LZ4F_CustomMem lz4f_mem = {
    .customAlloc = lz4Zmalloc,
    .customCalloc = lz4Zcalloc,
    .customFree = lz4Zfree,
    .opaqueState = NULL,
};

/* Compression level does not affect bounds. Keep one immutable preference set
 * per checksum policy so bound calculations match the active frame. */
static const LZ4F_preferences_t lz4f_prefs[] = {
    [false] = {
        .frameInfo = {
            .blockChecksumFlag = LZ4F_noBlockChecksum,
            .contentChecksumFlag = LZ4F_noContentChecksum,
            .blockSizeID = LZ4F_max64KB,
            .blockMode = LZ4F_blockLinked,
        },
        .compressionLevel = 0,
        .autoFlush = 1,
    },
    [true] = {
        .frameInfo = {
            .blockChecksumFlag = LZ4F_blockChecksumEnabled,
            .contentChecksumFlag = LZ4F_contentChecksumEnabled,
            .blockSizeID = LZ4F_max64KB,
            .blockMode = LZ4F_blockLinked,
        },
        .compressionLevel = 0,
        .autoFlush = 1,
    },
};

static LZ4F_preferences_t compressionLz4Preferences(const streamCompressor *compressor) {
    LZ4F_preferences_t prefs = lz4f_prefs[compressor->codec_checksum];
    prefs.compressionLevel = compressor->level;
    return prefs;
}

static int compressionLz4CompressorInit(streamCompressor *compressor) {
    compressor->ctx = LZ4F_createCompressionContext_advanced(lz4f_mem, LZ4F_VERSION);
    return compressor->ctx ? 0 : -1;
}

static void compressionLz4CompressorFree(streamCompressor *compressor) {
    if (compressor->ctx) {
        LZ4F_freeCompressionContext((LZ4F_cctx *)compressor->ctx);
        compressor->ctx = NULL;
    }
}

static int compressionLz4DecompressorInit(streamDecompressor *decompressor) {
    decompressor->ctx = LZ4F_createDecompressionContext_advanced(lz4f_mem, LZ4F_VERSION);
    if (!decompressor->ctx) return -1;
    decompressor->input_hint = LZ4F_HEADER_SIZE_MIN;
    return 0;
}

static void compressionLz4DecompressorFree(streamDecompressor *decompressor) {
    if (decompressor->ctx) {
        LZ4F_freeDecompressionContext((LZ4F_dctx *)decompressor->ctx);
        decompressor->ctx = NULL;
    }
}

static size_t compressionLz4OutputBound(const streamCompressor *compressor, size_t input_len) {
    size_t full_blocks = input_len / LZ4_STREAM_BLOCK_SIZE;
    if (full_blocks > UINT_MAX ||
        (full_blocks == UINT_MAX && input_len % LZ4_STREAM_BLOCK_SIZE != 0)) {
        return 0;
    }

    /* streamWriter supplies complete blocks and explicitly drains partial
     * blocks. With autoFlush, one bound covers update plus flush/end overhead. */
    LZ4F_preferences_t prefs = compressionLz4Preferences(compressor);
    size_t bound = LZ4F_compressBound(input_len, &prefs);
    if (LZ4F_isError(bound)) return 0;

    if (!compressor->stream_started) {
        if (bound > SIZE_MAX - LZ4F_HEADER_SIZE_MAX) return 0;
        bound += LZ4F_HEADER_SIZE_MAX;
    }
    if (bound > (size_t)SSIZE_MAX) return 0;
    return bound;
}

static ssize_t compressionLz4CompressFeed(streamCompressor *compressor,
                                          uint8_t *output,
                                          size_t output_capacity,
                                          const uint8_t *input,
                                          size_t input_len,
                                          bool input_stable,
                                          compressFlushMode flush_mode) {
    assert(compressor->ctx != NULL);

    LZ4F_cctx *cctx = (LZ4F_cctx *)compressor->ctx;
    size_t offset = 0;

    /* A failed compressBegin leaves a fresh context retriable. Once a frame is
     * open, any failure is permanent because input or frame bytes may already
     * have been consumed. */

    if (!compressor->stream_started) {
        LZ4F_preferences_t prefs = compressionLz4Preferences(compressor);
        size_t r = LZ4F_compressBegin(cctx, output, output_capacity, &prefs);
        if (LZ4F_isError(r)) return -1;
        offset = r;
        compressor->stream_started = true;
    }

    if (input_len > 0) {
        LZ4F_compressOptions_t options = {
            .stableSrc = input_stable,
        };
        if (offset >= output_capacity) {
            compressor->errored = true;
            return -1;
        }
        size_t r = LZ4F_compressUpdate(cctx, output + offset, output_capacity - offset,
                                       input, input_len, &options);
        if (LZ4F_isError(r)) {
            compressor->errored = true;
            return -1;
        }
        offset += r;
    }

    switch (flush_mode) {
    case FLUSH_CONTINUE:
        break;
    case FLUSH_SYNC: {
        if (offset >= output_capacity) {
            compressor->errored = true;
            return -1;
        }
        size_t r = LZ4F_flush(cctx, output + offset, output_capacity - offset, NULL);
        if (LZ4F_isError(r)) {
            compressor->errored = true;
            return -1;
        }
        offset += r;
        break;
    }
    case FLUSH_END: {
        if (offset >= output_capacity) {
            compressor->errored = true;
            return -1;
        }
        size_t r = LZ4F_compressEnd(cctx, output + offset, output_capacity - offset, NULL);
        if (LZ4F_isError(r)) {
            compressor->errored = true;
            return -1;
        }
        offset += r;
        compressor->stream_started = false;
        break;
    }
    default:
        assert(0 && "invalid compressFlushMode");
        compressor->errored = true;
        return -1;
    }

    if (offset > (size_t)SSIZE_MAX) {
        compressor->errored = true;
        return -1;
    }
    return (ssize_t)offset;
}

static ssize_t compressionLz4DecompressFeed(streamDecompressor *decompressor,
                                            uint8_t *output,
                                            size_t output_capacity,
                                            const uint8_t *input,
                                            size_t input_len,
                                            size_t *input_consumed) {
    assert(decompressor->ctx != NULL);

    LZ4F_dctx *dctx = (LZ4F_dctx *)decompressor->ctx;
    size_t dst_size = output_capacity;
    size_t src_size = input_len;
    size_t ret = LZ4F_decompress(dctx, output, &dst_size, input, &src_size, NULL);
    if (LZ4F_isError(ret)) {
        decompressor->errored = true;
        return -1;
    }
    *input_consumed = src_size;
    decompressor->input_hint = ret;
    if (ret == 0) decompressor->frame_done = true;
    return (ssize_t)dst_size;
}

const compressionCodec compressionLz4Codec = {
    .algo = ALGO_LZ4,
    .vcs_id = VCS_CODEC_LZ4,
    .name = "lz4",
    .chunk_size = LZ4_STREAM_BLOCK_SIZE,
    .compressor_init = compressionLz4CompressorInit,
    .compressor_output_bound = compressionLz4OutputBound,
    .compressor_feed = compressionLz4CompressFeed,
    .compressor_free = compressionLz4CompressorFree,
    .decompressor_init = compressionLz4DecompressorInit,
    .decompressor_feed = compressionLz4DecompressFeed,
    .decompressor_free = compressionLz4DecompressorFree,
};
