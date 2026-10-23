/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_rio.h"
#include "zmalloc.h"

#include <string.h>

#define COMPRESSION_RIO_INPUT_CHUNK_SIZE (1024 * 1024)

typedef enum {
    RIO_COMPRESSION_STATE_INITIAL = 0,
    RIO_COMPRESSION_STATE_ACTIVE,
    RIO_COMPRESSION_STATE_FINISHED,
    RIO_COMPRESSION_STATE_ERROR,
} rioCompressionState;

typedef struct {
    streamCompressor compressor;
    uint8_t *output;
    size_t output_size;
    rioCompressionState state;
} rioCompressionContext;

typedef struct {
    streamReader reader;
    size_t reported_source_bytes;
} rioDecompressionContext;

static rioCompressionContext *rioCompressionGetContext(rio *r) {
    return r->io.decorator.ctx;
}

static rioDecompressionContext *rioDecompressionGetContext(rio *r) {
    return r->io.decorator.ctx;
}

static const rioDecompressionContext *rioDecompressionGetConstContext(const rio *r) {
    return r->io.decorator.ctx;
}

static ssize_t rioReadUnsupported(rio *r, void *buf, size_t len) {
    (void)r;
    (void)buf;
    (void)len;
    return -1;
}

static size_t rioWriteUnsupported(rio *r, const void *buf, size_t len) {
    (void)r;
    (void)buf;
    (void)len;
    return 0;
}

static off_t rioCompressionTell(rio *r) {
    rio *inner = rioGetInner(r);
    return inner ? rioTell(inner) : -1;
}

static int rioDecompressionFlush(rio *r) {
    (void)r;
    return 1;
}

static void rioCompressionSetWriteError(rio *r) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);
    if (ctx) ctx->state = RIO_COMPRESSION_STATE_ERROR;
    r->flags |= RIO_FLAG_WRITE_ERROR;
}

static int rioCompressionEnsureEnvelope(rio *r) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);

    if (ctx->state == RIO_COMPRESSION_STATE_ACTIVE) return 0;
    if (ctx->state != RIO_COMPRESSION_STATE_INITIAL) return -1;

    uint8_t envelope[VCS_ENVELOPE_SIZE];
    if (vcsBuildEnvelope(envelope, ctx->compressor.algo, VCS_STREAM_RDB) != 0 ||
        !rioWrite(rioGetInner(r), envelope, sizeof(envelope))) {
        rioCompressionSetWriteError(r);
        return -1;
    }
    ctx->state = RIO_COMPRESSION_STATE_ACTIVE;
    return 0;
}

static int rioCompressionFeed(rio *r,
                              const uint8_t *input,
                              size_t input_len,
                              compressFlushMode flush_mode) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);
    size_t needed = streamCompressorOutputBound(&ctx->compressor, input_len);
    if (needed > ctx->output_size) {
        ctx->output = zrealloc(ctx->output, needed);
        ctx->output_size = needed;
    }

    ssize_t compressed = streamCompressorFeed(&ctx->compressor,
                                              ctx->output,
                                              ctx->output_size,
                                              input,
                                              input_len,
                                              flush_mode);
    if (compressed < 0 ||
        (compressed > 0 &&
         !rioWrite(rioGetInner(r), ctx->output, (size_t)compressed))) {
        rioCompressionSetWriteError(r);
        return -1;
    }
    return 0;
}

static size_t rioCompressionWrite(rio *r, const void *buf, size_t len) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);
    if (!ctx ||
        ctx->state == RIO_COMPRESSION_STATE_FINISHED ||
        ctx->state == RIO_COMPRESSION_STATE_ERROR) {
        rioCompressionSetWriteError(r);
        return 0;
    }

    const uint8_t *input = buf;
    size_t remaining = len;
    if (len > 0 && rioCompressionEnsureEnvelope(r) != 0) return 0;
    while (remaining > 0) {
        size_t chunk = remaining < COMPRESSION_RIO_INPUT_CHUNK_SIZE
                           ? remaining
                           : COMPRESSION_RIO_INPUT_CHUNK_SIZE;
        if (rioCompressionFeed(r, input, chunk, COMPRESS_FLUSH_CONTINUE) != 0)
            return 0;
        input += chunk;
        remaining -= chunk;
    }
    return 1;
}

static int rioCompressionFlush(rio *r) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);
    if (!ctx || ctx->state == RIO_COMPRESSION_STATE_ERROR) {
        rioCompressionSetWriteError(r);
        return 0;
    }

    if (ctx->state == RIO_COMPRESSION_STATE_ACTIVE &&
        rioCompressionFeed(r, NULL, 0, COMPRESS_FLUSH_SYNC) != 0) {
        return 0;
    }
    if (rioFlush(rioGetInner(r))) return 1;

    rioCompressionSetWriteError(r);
    return 0;
}

static int rioCompressionFinishTarget(rio *inner) {
    if (inner->flags & RIO_FLAG_DECORATOR) return rioFinish(inner);
    return rioFlush(inner);
}

static int rioCompressionFinish(rio *r) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);
    if (!ctx || ctx->state == RIO_COMPRESSION_STATE_ERROR) {
        rioCompressionSetWriteError(r);
        return 0;
    }
    if (ctx->state == RIO_COMPRESSION_STATE_FINISHED)
        return !rioGetWriteError(r);

    if (rioCompressionEnsureEnvelope(r) != 0 ||
        rioCompressionFeed(r, NULL, 0, COMPRESS_FLUSH_END) != 0 ||
        !rioCompressionFinishTarget(rioGetInner(r))) {
        rioCompressionSetWriteError(r);
        return 0;
    }

    ctx->state = RIO_COMPRESSION_STATE_FINISHED;
    return 1;
}

static ssize_t rioDecompressionRead(rio *r, void *buf, size_t len) {
    rioDecompressionContext *ctx = rioDecompressionGetContext(r);
    if (!ctx) {
        r->flags |= RIO_FLAG_READ_ERROR;
        return -1;
    }

    ssize_t nread = streamReaderRead(&ctx->reader, buf, len);
    if (nread < 0) r->flags |= RIO_FLAG_READ_ERROR;
    return nread;
}

static int rioDecompressionFinish(rio *r) {
    rioDecompressionContext *ctx = rioDecompressionGetContext(r);
    rio *inner = rioGetInner(r);
    if (!ctx ||
        streamReaderFinish(&ctx->reader) != 0 ||
        ((inner->flags & RIO_FLAG_DECORATOR) && !rioFinish(inner))) {
        r->flags |= RIO_FLAG_READ_ERROR;
        return 0;
    }
    return 1;
}

static void rioCompressionCleanup(rio *r) {
    rioCompressionContext *ctx = rioCompressionGetContext(r);
    if (!ctx) return;

    streamCompressorFree(&ctx->compressor);
    zfree(ctx->output);
    zfree(ctx);
    r->io.decorator.ctx = NULL;
}

static void rioDecompressionCleanup(rio *r) {
    rioDecompressionContext *ctx = rioDecompressionGetContext(r);
    if (!ctx) return;

    streamReaderFree(&ctx->reader);
    zfree(ctx);
    r->io.decorator.ctx = NULL;
}

static const rio rioCompressionIO = {
    .read = rioReadUnsupported,
    .write = rioCompressionWrite,
    .tell = rioCompressionTell,
    .flush = rioCompressionFlush,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = RIO_FLAG_DECORATOR | RIO_FLAG_STREAMING_COMPRESSION,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {.decorator = {
               .inner = NULL,
               .ctx = NULL,
               .finish = rioCompressionFinish,
               .cleanup = rioCompressionCleanup,
           }},
};

static const rio rioDecompressionIO = {
    .read = rioDecompressionRead,
    .write = rioWriteUnsupported,
    .tell = rioCompressionTell,
    .flush = rioDecompressionFlush,
    .update_cksum = NULL,
    .cksum = 0,
    .flags = RIO_FLAG_DECORATOR | RIO_FLAG_STREAMING_DECOMPRESSION,
    .processed_bytes = 0,
    .max_processing_chunk = 0,
    .io = {.decorator = {
               .inner = NULL,
               .ctx = NULL,
               .finish = rioDecompressionFinish,
               .cleanup = rioDecompressionCleanup,
           }},
};

int rioInitWithCompression(rio *r,
                           compressionAlgo algo,
                           rio *inner,
                           bool codec_checksum) {
    if (!r || !inner || r == inner) return -1;

    rioCompressionContext *ctx = zcalloc(sizeof(*ctx));
    uint8_t checksum_flags =
        codec_checksum ? STREAM_CHECKSUM_BLOCK | STREAM_CHECKSUM_CONTENT : 0;
    if (streamCompressorInit(&ctx->compressor, algo, 0, checksum_flags) != 0) {
        zfree(ctx);
        memset(r, 0, sizeof(*r));
        return -1;
    }

    *r = rioCompressionIO;
    r->io.decorator.inner = inner;
    r->io.decorator.ctx = ctx;
    return 0;
}

rioDecompressionInitResult rioInitWithDecompression(rio *r,
                                                    rio *inner,
                                                    bool skip_codec_checksum_validation,
                                                    compressionAlgo *algo) {
    if (!r || !inner || r == inner) return RIO_DECOMPRESSION_INIT_ERROR;

    streamReaderConfig config = {
        .allow_passthrough = true,
        .skip_codec_checksum_validation = skip_codec_checksum_validation,
        .buffer_size = STREAM_READER_BUFFER_SIZE_DEFAULT,
        .eof_mid_frame_is_truncation = (rioCheckType(inner) == RIO_TYPE_CONN),
    };
    compressionAlgo detected_algo = ALGO_NONE;
    rioDecompressionContext *ctx = zcalloc(sizeof(*ctx));
    if (algo) *algo = ALGO_NONE;

    if (streamReaderInitWithRio(&ctx->reader,
                                &config,
                                inner,
                                &detected_algo) != 0) {
        streamReaderErrorKind error = ctx->reader.error_kind;
        streamReaderFree(&ctx->reader);
        zfree(ctx);
        memset(r, 0, sizeof(*r));
        return error == STREAM_READER_ERROR_INCOMPATIBLE
                   ? RIO_DECOMPRESSION_INIT_INCOMPATIBLE
                   : RIO_DECOMPRESSION_INIT_ERROR;
    }

    *r = rioDecompressionIO;
    r->io.decorator.inner = inner;
    r->io.decorator.ctx = ctx;
    r->flags |= inner->flags & RIO_FLAG_SKIP_RDB_CHECKSUM;
    if (detected_algo != ALGO_NONE) {
        r->flags |= RIO_FLAG_STREAMING_COMPRESSION | RIO_FLAG_SKIP_RDB_CHECKSUM;
        if (algo) *algo = detected_algo;
    }
    return RIO_DECOMPRESSION_INIT_OK;
}

static streamReaderErrorKind rioDecompressionGetOwnError(const rio *r) {
    if (!(r->flags & RIO_FLAG_DECORATOR) ||
        !(r->flags & RIO_FLAG_STREAMING_DECOMPRESSION)) {
        return STREAM_READER_ERROR_NONE;
    }

    const rioDecompressionContext *ctx = rioDecompressionGetConstContext(r);
    return ctx ? ctx->reader.error_kind : STREAM_READER_ERROR_INTERNAL;
}

streamReaderErrorKind rioDecompressionGetError(const rio *r) {
    streamReaderErrorKind error = rioDecompressionGetOwnError(r);
    const rio *inner = (r->flags & RIO_FLAG_DECORATOR) ? r->io.decorator.inner : NULL;
    while (inner &&
           (inner->flags & RIO_FLAG_DECORATOR) &&
           (inner->flags & RIO_FLAG_STREAMING_DECOMPRESSION)) {
        streamReaderErrorKind inner_error = rioDecompressionGetOwnError(inner);
        if (inner_error != STREAM_READER_ERROR_NONE &&
            (error == STREAM_READER_ERROR_NONE ||
             error == STREAM_READER_ERROR_IO)) {
            error = inner_error;
        }
        inner = inner->io.decorator.inner;
    }
    return error;
}

size_t rioDecompressionTakeUnreportedBytes(rio *r) {
    if (!(r->flags & RIO_FLAG_DECORATOR) ||
        !(r->flags & RIO_FLAG_STREAMING_DECOMPRESSION)) {
        return 0;
    }

    rioDecompressionContext *ctx = rioDecompressionGetContext(r);
    if (!ctx) return 0;

    rio *source = rioGetInner(r);
    while (source && rioGetInner(source)) source = rioGetInner(source);
    if (!source) return 0;

    size_t current = source->processed_bytes;
    size_t bytes = current - ctx->reported_source_bytes;
    ctx->reported_source_bytes = current;
    return bytes;
}
