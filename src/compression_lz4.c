/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_lz4.h"
#include <limits.h>
#include <lz4frame.h>

/* Shared LZ4F preferences template.
 * - Used by streamCompressOutputBound() for bounds.
 * - Copied and selectively overridden in streamCompressFeed() before
 *   LZ4F_compressBegin() (compression level and checksum mode).
 *
 * Bounds are computed with block-independent mode and block checksums enabled
 * so the returned capacity is safe for both checksum settings. */
static const LZ4F_preferences_t lz4fPrefs = {
    .frameInfo = {
        .blockChecksumFlag = LZ4F_blockChecksumEnabled,
        .contentChecksumFlag = LZ4F_noContentChecksum,
        .blockSizeID = LZ4F_max64KB,
        .blockMode = LZ4F_blockIndependent,
    },
    .compressionLevel = 0, /* bound calculation uses 0 (worst-case); actual
                            * compression uses sc->level via a local copy */
};

int compressionLz4CompressorInit(streamCompressor *sc) {
    LZ4F_cctx *cctx = NULL;
    LZ4F_errorCode_t err;

    if (!sc) return -1;

    err = LZ4F_createCompressionContext(&cctx, LZ4F_VERSION);
    if (LZ4F_isError(err)) return -1;

    sc->ctx = cctx;
    return 0;
}

void compressionLz4CompressorDestroy(streamCompressor *sc) {
    if (!sc || !sc->ctx) return;

    LZ4F_freeCompressionContext((LZ4F_cctx *)sc->ctx);
    sc->ctx = NULL;
}

int compressionLz4DecompressorInit(streamDecompressor *sd) {
    LZ4F_dctx *dctx = NULL;
    LZ4F_errorCode_t err;

    if (!sd) return -1;

    err = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(err)) return -1;

    sd->ctx = dctx;
    return 0;
}

void compressionLz4DecompressorDestroy(streamDecompressor *sd) {
    if (!sd || !sd->ctx) return;

    LZ4F_freeDecompressionContext((LZ4F_dctx *)sd->ctx);
    sd->ctx = NULL;
}

size_t compressionLz4OutputBound(size_t inputLen) {
    /* Conservative worst-case: data bound + frame header + flush/end overhead.
     * Always includes all components so the caller can allocate once and reuse
     * for any flush mode and frame state. */
    return LZ4F_compressBound(inputLen, &lz4fPrefs) + LZ4F_HEADER_SIZE_MAX + LZ4F_compressBound(0, &lz4fPrefs);
}

ssize_t compressionLz4CompressFeed(streamCompressor *sc,
                                   uint8_t *output,
                                   size_t outputCapacity,
                                   const uint8_t *input,
                                   size_t inputLen,
                                   compressFlushMode flushMode) {
    LZ4F_cctx *cctx;
    size_t offset = 0;

    if (!sc || !sc->ctx) return -1;

    cctx = (LZ4F_cctx *)sc->ctx;

    /* Begin frame on first call */
    if (!sc->frame_started) {
        /* Local copy of shared prefs so we can set the actual level
         * and checksum mode per-stream. */
        LZ4F_preferences_t prefs = lz4fPrefs;
        size_t r;

        prefs.compressionLevel = sc->level;
        prefs.frameInfo.blockChecksumFlag = sc->codec_checksum
                                                ? LZ4F_blockChecksumEnabled
                                                : LZ4F_noBlockChecksum;
        r = LZ4F_compressBegin(cctx, output, outputCapacity, &prefs);
        if (LZ4F_isError(r)) {
            /* compressBegin failure before any frame bytes are emitted is
             * recoverable — the LZ4F context is still clean. Caller can
             * retry with a larger buffer. Don't set errored. */
            return -1;
        }
        offset = r;
        sc->frame_started = true;
    }

    /* Compress input data */
    if (inputLen > 0) {
        size_t r;

        if (offset >= outputCapacity) goto lz4_error;
        r = LZ4F_compressUpdate(cctx, output + offset, outputCapacity - offset,
                                input, inputLen, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
    }

    /* Handle flush/end modes */
    if (flushMode == FLUSH_SYNC) {
        size_t r;

        if (offset >= outputCapacity) goto lz4_error;
        r = LZ4F_flush(cctx, output + offset, outputCapacity - offset, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
    } else if (flushMode == FLUSH_END) {
        size_t r;

        if (offset >= outputCapacity) goto lz4_error;
        r = LZ4F_compressEnd(cctx, output + offset, outputCapacity - offset, NULL);
        if (LZ4F_isError(r)) goto lz4_error;
        offset += r;
        sc->frame_started = false;
    }

    if (offset > (size_t)SSIZE_MAX) goto lz4_error;
    return (ssize_t)offset;

lz4_error:
    /* LZ4F state is undefined after an error (lz4frame.h line 325).
     * Mark permanently failed — no mid-stream retry is possible because
     * already-emitted frame bytes cannot be unsent. The caller must
     * tear down the stream (disconnect replica / abort RDB save). */
    sc->errored = true;
    return -1;
}

ssize_t compressionLz4DecompressFeed(streamDecompressor *sd,
                                     uint8_t *output,
                                     size_t outputCapacity,
                                     const uint8_t *input,
                                     size_t inputLen,
                                     size_t *inputConsumed) {
    LZ4F_dctx *dctx;
    size_t dstSize;
    size_t srcSize;
    size_t ret;

    if (!sd || !sd->ctx || !inputConsumed) return -1;

    dctx = (LZ4F_dctx *)sd->ctx;
    dstSize = outputCapacity;
    srcSize = inputLen;
    ret = LZ4F_decompress(dctx, output, &dstSize, input, &srcSize, NULL);
    if (LZ4F_isError(ret)) {
        sd->errored = true;
        return -1;
    }
    *inputConsumed = srcSize;
    if (ret == 0) sd->frame_done = true;
    if (dstSize > (size_t)SSIZE_MAX) {
        sd->errored = true;
        return -1;
    }
    return (ssize_t)dstSize;
}
