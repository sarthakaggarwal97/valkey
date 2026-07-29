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
    },
    [true] = {
        .frameInfo = {
            .blockChecksumFlag = LZ4F_blockChecksumEnabled,
            .contentChecksumFlag = LZ4F_contentChecksumEnabled,
            .blockSizeID = LZ4F_max64KB,
            .blockMode = LZ4F_blockLinked,
        },
        .compressionLevel = 0,
    },
};

static const LZ4F_preferences_t *compressionLz4Preferences(const streamCompressor *compressor) {
    return &lz4f_prefs[compressor->codec_checksum];
}

int compressionLz4CompressorInit(streamCompressor *compressor) {
    compressor->ctx = LZ4F_createCompressionContext_advanced(lz4f_mem, LZ4F_VERSION);
    assert(compressor->ctx != NULL);
    return 0;
}

void compressionLz4CompressorFree(streamCompressor *compressor) {
    if (compressor->ctx) {
        LZ4F_freeCompressionContext((LZ4F_cctx *)compressor->ctx);
        compressor->ctx = NULL;
    }
}

int compressionLz4DecompressorInit(streamDecompressor *decompressor) {
    decompressor->ctx = LZ4F_createDecompressionContext_advanced(lz4f_mem, LZ4F_VERSION);
    assert(decompressor->ctx != NULL);
    decompressor->input_hint = LZ4F_HEADER_SIZE_MIN;
    return 0;
}

void compressionLz4DecompressorFree(streamDecompressor *decompressor) {
    if (decompressor->ctx) {
        LZ4F_freeDecompressionContext((LZ4F_dctx *)decompressor->ctx);
        decompressor->ctx = NULL;
    }
}

size_t compressionLz4OutputBound(const streamCompressor *compressor,
                                 size_t input_len,
                                 compressFlushMode flush_mode) {
    switch (flush_mode) {
    case FLUSH_CONTINUE:
    case FLUSH_SYNC:
    case FLUSH_END:
        break;
    default:
        assert(0 && "invalid compressFlushMode");
        return 0;
    }

    if (input_len > SIZE_MAX - compressor->input_buffered) return 0;

    /* autoFlush makes compressBound cover both the update and any requested
     * flush/end in one calculation. Include the exact bytes already buffered by
     * the active frame; this avoids a second worst-case 64 KB allowance. */
    LZ4F_preferences_t bound_prefs = *compressionLz4Preferences(compressor);
    bound_prefs.autoFlush = 1;
    size_t bound = LZ4F_compressBound(input_len + compressor->input_buffered, &bound_prefs);
    if (LZ4F_isError(bound)) return 0;

    if (!compressor->stream_started) {
        if (bound > SIZE_MAX - LZ4F_HEADER_SIZE_MAX) return 0;
        bound += LZ4F_HEADER_SIZE_MAX;
    }
    return bound;
}

ssize_t compressionLz4CompressFeed(streamCompressor *compressor,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode) {
    assert(compressor->ctx != NULL);

    LZ4F_cctx *cctx = (LZ4F_cctx *)compressor->ctx;
    size_t offset = 0;

    /* A failed compressBegin leaves a fresh context retriable. Once a frame is
     * open, any failure is permanent because input or frame bytes may already
     * have been consumed. */

    if (!compressor->stream_started) {
        LZ4F_preferences_t prefs = *compressionLz4Preferences(compressor);
        prefs.compressionLevel = compressor->level;
        size_t r = LZ4F_compressBegin(cctx, output, output_capacity, &prefs);
        if (LZ4F_isError(r)) return -1;
        offset = r;
        compressor->stream_started = true;
    }

    if (input_len > 0) {
        if (offset >= output_capacity) {
            compressor->errored = true;
            return -1;
        }
        size_t r = LZ4F_compressUpdate(cctx, output + offset, output_capacity - offset, input, input_len, NULL);
        if (LZ4F_isError(r)) {
            compressor->errored = true;
            return -1;
        }
        offset += r;
        compressor->input_buffered =
            (compressor->input_buffered + input_len % LZ4_STREAM_BLOCK_SIZE) %
            LZ4_STREAM_BLOCK_SIZE;
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
        compressor->input_buffered = 0;
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
        compressor->input_buffered = 0;
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

ssize_t compressionLz4DecompressFeed(streamDecompressor *decompressor,
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
