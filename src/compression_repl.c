/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_repl.h"
#include "serverassert.h"
#include "zmalloc.h"
#include <string.h>

/* Release an oversized staging/scratch buffer once a later batch uses less
 * than a quarter of it. Roughly 16 x PROTO_REPLY_CHUNK_BYTES, expressed
 * locally to keep this adapter independent of server.h. */
#define REPL_COMPRESSION_RETAIN_LIMIT (256 * 1024)

/* Grow decoded output in one LZ4-block increments. */
#define REPL_DECODE_CHUNK (64 * 1024)

/* ===== Primary-side per-replica compressor ===== */

static void replCompressorUpdateMemUsage(replCompressor *rc) {
    size_t total = sizeof(*rc) + streamWriterMemUsage(&rc->writer);
    if (rc->out_buf) total += sdsalloc(rc->out_buf);
    atomic_store_explicit(&rc->mem_usage, total, memory_order_relaxed);
}

replCompressor *replCompressorCreate(compressionAlgo algo, int level) {
    replCompressor *rc = zcalloc(sizeof(*rc));
    atomic_init(&rc->mem_usage, 0);

    streamWriterConfig cfg = {
        .algo = algo,
        .level = level,
        .stream_kind = STREAM_KIND_REPL,
        .codec_checksum_enabled = 0,
    };
    rc->out_buf = sdsempty();
    if (streamWriterInit(&rc->writer, &cfg, NULL, NULL) != 0) {
        sdsfree(rc->out_buf);
        zfree(rc);
        return NULL;
    }
    streamWriterSetSink(&rc->writer, &rc->out_buf);
    replCompressorUpdateMemUsage(rc);
    return rc;
}

void replCompressorDestroy(replCompressor *rc) {
    if (!rc) return;
    streamWriterFree(&rc->writer);
    sdsfree(rc->out_buf);
    zfree(rc);
}

int replCompressorWrite(replCompressor *rc, const void *buf, size_t len) {
    return streamWriterWrite(&rc->writer, buf, len);
}

int replCompressorFlush(replCompressor *rc) {
    int result = streamWriterFlush(&rc->writer);
    replCompressorUpdateMemUsage(rc);
    return result;
}

void replCompressorResetBatch(replCompressor *rc) {
    rc->out_buf_pos = 0;
    rc->raw_bytes = 0;
    if (sdsalloc(rc->out_buf) > REPL_COMPRESSION_RETAIN_LIMIT &&
        sdslen(rc->out_buf) < sdsalloc(rc->out_buf) / 4) {
        sdsfree(rc->out_buf);
        rc->out_buf = sdsempty();
    } else {
        sdsclear(rc->out_buf);
    }
    replCompressorUpdateMemUsage(rc);
}

size_t replCompressorMemUsage(const replCompressor *rc) {
    if (!rc) return 0;
    return atomic_load_explicit(&rc->mem_usage, memory_order_relaxed);
}

compressionAlgo replCompressorAlgo(const replCompressor *rc) {
    return rc ? rc->writer.compressor.algo : ALGO_NONE;
}

/* ===== Replica-side decompressor ===== */

replDecompressor *replDecompressorCreate(void) {
    replDecompressor *rd = zcalloc(sizeof(*rd));
    streamProbeInit(&rd->probe, STREAM_KIND_REPL, true);
    rd->decode_buf = sdsempty();
    return rd;
}

void replDecompressorDestroy(replDecompressor *rd) {
    if (!rd) return;
    if (rd->decompressor_initialized) streamDecompressorFree(&rd->decompressor);
    sdsfree(rd->decode_buf);
    zfree(rd);
}

/* Append raw bytes to the decode buffer (passthrough), enforcing output_max. */
static replDecodeResult replDecodeEmit(replDecompressor *rd, const uint8_t *in, size_t len, size_t output_max) {
    if (len == 0) return REPL_DECODE_OK;
    size_t used = sdslen(rd->decode_buf);
    if (used > output_max || len > output_max - used) return REPL_DECODE_OVERFLOW;
    rd->decode_buf = sdscatlen(rd->decode_buf, in, len);
    return REPL_DECODE_OK;
}

/* Drain compressed bytes [in, in+len) through the codec into rd->decode_buf,
 * bounded by output_max (decompression-bomb guard). */
static replDecodeResult replDecodeFeed(replDecompressor *rd, const uint8_t *in, size_t len, size_t output_max) {
    size_t off = 0;
    bool drain_output = false;
    while (off < len || drain_output) {
        size_t used = sdslen(rd->decode_buf);
        if (used > output_max) return REPL_DECODE_OVERFLOW;
        size_t output_remaining = output_max - used;
        /* Allow one byte beyond the cap so exceeding it surfaces as OVERFLOW
         * instead of a zero-capacity feed that looks like a stuck decoder. */
        size_t output_limit = output_remaining < SIZE_MAX ? output_remaining + 1 : output_remaining;
        size_t reserve = output_limit;
        if (reserve > REPL_DECODE_CHUNK) reserve = REPL_DECODE_CHUNK;
        rd->decode_buf = sdsMakeRoomFor(rd->decode_buf, reserve);
        size_t output_capacity = sdsavail(rd->decode_buf);
        if (output_capacity > output_limit) output_capacity = output_limit;
        size_t input_remaining = len - off;
        size_t consumed = 0;
        ssize_t produced = streamDecompressorFeed(&rd->decompressor,
                                                  (uint8_t *)rd->decode_buf + used,
                                                  output_capacity,
                                                  in + off, input_remaining, &consumed);
        if (produced < 0 || consumed > input_remaining ||
            (size_t)produced > output_capacity) {
            return REPL_DECODE_ERR;
        }
        if (produced > 0) {
            if ((size_t)produced > output_remaining) return REPL_DECODE_OVERFLOW;
            sdsIncrLen(rd->decode_buf, (size_t)produced);
        }
        off += consumed;
        /* A long-lived replication stream must never reach a compressed frame
         * end. If it does, the stream is corrupt or the primary sent an
         * unexpected terminator: the caller should disconnect. */
        if (rd->decompressor.frame_done) return REPL_DECODE_FRAME_DONE;
        /* LZ4F always makes progress given non-empty input and output room,
         * so a stuck call is a decoder error rather than permission to drop the
         * unconsumed input tail. */
        if (input_remaining > 0 && consumed == 0 && produced == 0) return REPL_DECODE_ERR;

        /* LZ4F may consume a compressed block while retaining decoded bytes in
         * its internal output buffer. A full destination means there may be
         * more output available without additional transport input. */
        drain_output = off == len && (size_t)produced == output_capacity;
    }
    return REPL_DECODE_OK;
}

replDecodeResult replDecompressorDecode(replDecompressor *rd,
                                        const void *src,
                                        size_t len,
                                        size_t output_max,
                                        size_t *out_len) {
    if (out_len) *out_len = 0;
    sdsclear(rd->decode_buf);

    const uint8_t *in = src;
    size_t off = 0;

    /* Once plaintext is established, the caller already owns the output bytes
     * in its query buffer. Avoid copying them through decode_buf. */
    if (rd->probe.ready && !rd->probe.info.compressed) {
        if (len > output_max) return REPL_DECODE_OVERFLOW;
        if (out_len) *out_len = len;
        return REPL_DECODE_PASSTHROUGH;
    }

    /* A compressed stream whose decoder failed to initialize stays errored;
     * feeding it would silently drop input. */
    if (rd->probe.ready && !rd->decompressor_initialized) return REPL_DECODE_ERR;

    if (!rd->probe.ready) {
        size_t buffered_prefix = rd->probe.header_len;
        streamProbeResult probe_result = streamProbeFeed(&rd->probe, in, len, false, &off);
        switch (probe_result) {
        case STREAM_PROBE_NEED_INPUT:
            return REPL_DECODE_OK;
        case STREAM_PROBE_ERROR:
            return REPL_DECODE_ERR;
        case STREAM_PROBE_PASSTHROUGH: {
            if (buffered_prefix == 0) {
                if (len > output_max) return REPL_DECODE_OVERFLOW;
                if (out_len) *out_len = len;
                return REPL_DECODE_PASSTHROUGH;
            }
            /* Replay bytes retained while probing on this transition. Later
             * calls take the zero-copy passthrough path above. */
            replDecodeResult result = replDecodeEmit(rd, rd->probe.header,
                                                     rd->probe.header_len, output_max);
            if (result != REPL_DECODE_OK) return result;
            result = replDecodeEmit(rd, in + off, len - off, output_max);
            if (result != REPL_DECODE_OK) return result;
            break;
        }
        case STREAM_PROBE_COMPRESSED:
            if (streamDecompressorInit(&rd->decompressor, rd->probe.info.algo) != 0)
                return REPL_DECODE_ERR;
            rd->decompressor_initialized = true;
            break;
        default:
            return REPL_DECODE_ERR;
        }
    }

    if (rd->probe.info.compressed && off < len) {
        replDecodeResult r = replDecodeFeed(rd, in + off, len - off, output_max);
        if (r != REPL_DECODE_OK) return r;
    }

    /* Shrink the scratch buffer if it grew large and is now mostly empty. */
    if (sdsalloc(rd->decode_buf) > REPL_COMPRESSION_RETAIN_LIMIT &&
        sdslen(rd->decode_buf) < sdsalloc(rd->decode_buf) / 4) {
        rd->decode_buf = sdsRemoveFreeSpace(rd->decode_buf, 0);
    }

    if (out_len) *out_len = sdslen(rd->decode_buf);
    return REPL_DECODE_OK;
}

sds replDecompressorBuf(replDecompressor *rd) {
    return rd ? rd->decode_buf : NULL;
}

sds replDecompressorTakeBuf(replDecompressor *rd, sds replacement) {
    assert(rd != NULL);
    assert(replacement != NULL);
    assert(replacement != rd->decode_buf);

    sds decoded = rd->decode_buf;
    rd->decode_buf = replacement;
    sdsclear(rd->decode_buf);
    return decoded;
}
