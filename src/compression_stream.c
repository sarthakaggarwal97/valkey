/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_stream.h"
#include "zmalloc.h"
#include <limits.h>
#include <string.h>

/* --- VKCS envelope --- */

typedef enum {
    VKCS_PROBE_NEED_INPUT = 0,
    VKCS_PROBE_PASSTHROUGH = 1,
    VKCS_PROBE_COMPRESSED = 2,
    VKCS_PROBE_ERROR = 3,
} vkcsProbeResult;

/* Probe options. */
typedef struct {
    bool allowPassthrough;
    uint8_t expectedStreamKind;
} vkcsProbeConfig;

/* Probe state. */
typedef struct {
    uint8_t header[VKCS_ENVELOPE_SIZE];
    size_t headerLen;
    bool ready;
    bool compressed;
    bool codecChecksumEnabled;
    compressionAlgo algo;
    uint8_t streamKind;
} vkcsProbe;

static bool vkcsCodecIsSupported(vkcsCodec codec) {
    return codec == VKCS_CODEC_LZ4;
}

static bool vkcsProbeHasMagicPrefix(const vkcsProbe *probe) {
    if (probe->headerLen == 0) return false;
    size_t magicPrefixLen = probe->headerLen < 4 ? probe->headerLen : 4;
    return memcmp(probe->header, "VKCS", magicPrefixLen) == 0;
}

static void vkcsProbeSetPassthrough(vkcsProbe *probe) {
    probe->ready = true;
    probe->compressed = false;
    probe->codecChecksumEnabled = false;
    probe->algo = ALGO_NONE;
    probe->streamKind = 0;
}

static void vkcsProbeSetCompressed(vkcsProbe *probe,
                                   compressionAlgo algo,
                                   uint8_t streamKind,
                                   bool codecChecksumEnabled) {
    probe->ready = true;
    probe->compressed = true;
    probe->codecChecksumEnabled = codecChecksumEnabled;
    probe->algo = algo;
    probe->streamKind = streamKind;
}

static int compressionAlgoToVkcsCodec(compressionAlgo algo, vkcsCodec *codec) {
    switch (algo) {
    case ALGO_LZ4:
        *codec = VKCS_CODEC_LZ4;
        return 0;
    default:
        return -1;
    }
}

static int vkcsCodecToCompressionAlgo(vkcsCodec codec, compressionAlgo *algo) {
    switch (codec) {
    case VKCS_CODEC_LZ4:
        *algo = ALGO_LZ4;
        return 0;
    default:
        return -1;
    }
}

/* Write 8-byte VKCS envelope via callback.
 * Layout: [0..3] magic "VKCS", [4] version, [5] codec_id, [6] flags, [7] streamKind.
 * Returns 0 on success, -1 on error (invalid codec or emitCb failure). */
static int writeVkcsEnvelope(vkcsEmitFn emitCb,
                               void *ctx,
                               vkcsCodec codec,
                               uint8_t streamKind,
                               bool codecChecksumEnabled) {
    if (!emitCb) return -1;
    if (!vkcsCodecIsSupported(codec)) return -1;

    uint8_t envelope[VKCS_ENVELOPE_SIZE];
    envelope[0] = VKCS_MAGIC_0;
    envelope[1] = VKCS_MAGIC_1;
    envelope[2] = VKCS_MAGIC_2;
    envelope[3] = VKCS_MAGIC_3;
    envelope[4] = VKCS_VERSION;
    envelope[5] = (uint8_t)codec;
    envelope[6] = codecChecksumEnabled ? VKCS_FLAG_CODEC_CHECKSUM : 0;
    envelope[7] = streamKind;

    return emitCb(ctx, envelope, VKCS_ENVELOPE_SIZE) == 0 ? 0 : -1;
}

/* Parse 8-byte VKCS envelope from buffer.
 * Validates magic bytes, version, codec, and reserved fields.
 * Rejects envelopes with unknown flag bits so future versions are detected
 * early rather than causing silent data corruption.
 * On success populates *codec and *streamKind and returns 0.
 * Returns -1 on error (bad magic, unsupported version, unknown codec,
 * reserved bits set). */
static int readVkcsEnvelope(const uint8_t *buf,
                              size_t len,
                              vkcsCodec *codec,
                              uint8_t *streamKind,
                              bool *codecChecksumEnabled) {
    if (!buf || len < VKCS_ENVELOPE_SIZE) return -1;

    if (buf[0] != VKCS_MAGIC_0 || buf[1] != VKCS_MAGIC_1 ||
        buf[2] != VKCS_MAGIC_2 || buf[3] != VKCS_MAGIC_3) {
        return -1;
    }
    if (buf[4] != VKCS_VERSION) return -1;

    vkcsCodec parsedCodec = (vkcsCodec)buf[5];
    if (!vkcsCodecIsSupported(parsedCodec)) return -1;

    uint8_t flags = buf[6];
    if (flags & ~VKCS_FLAG_CODEC_CHECKSUM) return -1;

    if (codec) *codec = parsedCodec;
    if (streamKind) *streamKind = buf[7];
    if (codecChecksumEnabled) *codecChecksumEnabled = (flags & VKCS_FLAG_CODEC_CHECKSUM) != 0;
    return 0;
}

static int readVkcsEnvelopeInfo(const uint8_t *buf,
                                uint8_t expectedStreamKind,
                                streamReaderInfo *info) {
    vkcsCodec codec;
    uint8_t streamKind = 0;
    bool codecChecksumEnabled = false;
    compressionAlgo algo = ALGO_NONE;

    if (readVkcsEnvelope(buf, VKCS_ENVELOPE_SIZE,
                           &codec, &streamKind, &codecChecksumEnabled) != 0 ||
        streamKind != expectedStreamKind ||
        vkcsCodecToCompressionAlgo(codec, &algo) != 0) {
        return -1;
    }

    info->compressed = true;
    info->codec_checksum_enabled = codecChecksumEnabled;
    info->algo = algo;
    info->stream_kind = streamKind;
    return 0;
}

static void vkcsProbeInit(vkcsProbe *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->algo = ALGO_NONE;
    probe->codecChecksumEnabled = false;
}

/* Probe incrementally because wrapped rios may legally return fewer than
 * VKCS_ENVELOPE_SIZE bytes per read. The probe also retains any consumed
 * prefix so passthrough streams can replay those bytes exactly. */
static vkcsProbeResult vkcsProbeFeed(vkcsProbe *probe,
                                         const vkcsProbeConfig *cfg,
                                         const uint8_t *src,
                                         size_t srcLen,
                                         bool inputEof,
                                         size_t *srcConsumed) {
    size_t consumed = 0;

    *srcConsumed = 0;
    if (probe->ready) {
        return probe->compressed ? VKCS_PROBE_COMPRESSED : VKCS_PROBE_PASSTHROUGH;
    }

    while (consumed < srcLen) {
        size_t target = probe->headerLen < 4 ? 4 : VKCS_ENVELOPE_SIZE;
        size_t need = target - probe->headerLen;
        size_t take = srcLen - consumed < need ? srcLen - consumed : need;

        memcpy(probe->header + probe->headerLen, src + consumed, take);
        probe->headerLen += take;
        consumed += take;

        if (probe->headerLen >= 4 &&
            memcmp(probe->header, "VKCS", 4) != 0) {
            *srcConsumed = consumed;
            if (!cfg->allowPassthrough) return VKCS_PROBE_ERROR;
            vkcsProbeSetPassthrough(probe);
            return VKCS_PROBE_PASSTHROUGH;
        }

        if (probe->headerLen == VKCS_ENVELOPE_SIZE) {
            streamReaderInfo info = {0};

            if (readVkcsEnvelopeInfo(probe->header, cfg->expectedStreamKind, &info) != 0) {
                *srcConsumed = consumed;
                return VKCS_PROBE_ERROR;
            }

            vkcsProbeSetCompressed(probe, info.algo, info.stream_kind,
                                   info.codec_checksum_enabled);
            *srcConsumed = consumed;
            return VKCS_PROBE_COMPRESSED;
        }
    }

    if (inputEof) {
        *srcConsumed = consumed;
        /* If EOF lands in the middle of a potential VKCS header, treat it as
         * a malformed compressed stream rather than silently downgrading it to
         * passthrough mode. */
        if (vkcsProbeHasMagicPrefix(probe)) return VKCS_PROBE_ERROR;
        if (!cfg->allowPassthrough) return VKCS_PROBE_ERROR;
        vkcsProbeSetPassthrough(probe);
        return VKCS_PROBE_PASSTHROUGH;
    }

    *srcConsumed = consumed;
    return VKCS_PROBE_NEED_INPUT;
}

/* Generic streaming writer implementation. */

#define STREAM_WRITER_INPUT_CHUNK_SIZE (1024 * 1024)

/* Streaming writer context. */
struct stream_writer {
    streamCompressor compressor;
    uint8_t *outBuf;     /* Reusable output buffer, sized via streamCompressOutputBound */
    size_t outBufSize;  /* Current allocation size of outBuf */
    vkcsEmitFn emitCb; /* Returns 0 on success, -1 on error */
    void *emitCtx;
    uint8_t streamKind; /* Concrete on-wire stream kind */
    bool envelopeWritten;
    bool finished;          /* Set by streamWriterFinish — blocks further writes.
                             * Prevents accidental multi-frame output under one envelope. */
    bool errored;           /* Sticky error flag — once set, all writes fail */
    uint64_t bytesEmitted; /* Running total of bytes successfully emitted */
};

static int streamWriterInitContext(streamWriter *t,
                                   const streamWriterConfig *cfg,
                                   vkcsEmitFn emitCb,
                                   void *emitCtx) {
    memset(t, 0, sizeof(*t));
    t->emitCb = emitCb;
    t->emitCtx = emitCtx;
    t->streamKind = cfg->stream_kind;

    if (streamCompressorInit(&t->compressor, cfg->algo, cfg->level) != 0) {
        return -1;
    }
    t->compressor.codec_checksum = cfg->codec_checksum_enabled;
    return 0;
}

/* Emit envelope lazily on first write/flush/finish.
 * Returns 0 on success, -1 on error (and sets t->errored). */
static int streamWriterEnsureEnvelope(streamWriter *t) {
    if (t->envelopeWritten) return 0;
    vkcsCodec codec;
    if (compressionAlgoToVkcsCodec(t->compressor.algo, &codec) != 0 ||
        writeVkcsEnvelope(t->emitCb, t->emitCtx, codec,
                            t->streamKind, t->compressor.codec_checksum) != 0) {
        t->errored = true;
        return -1;
    }
    t->bytesEmitted += VKCS_ENVELOPE_SIZE;
    t->envelopeWritten = true;
    return 0;
}

/* Emit compressed bytes to the output sink.
 * Returns 0 on success, -1 on error (and sets t->errored). */
static int streamWriterEmit(streamWriter *t, const uint8_t *buf, size_t len) {
    if (len == 0) return 0;
    if (t->emitCb(t->emitCtx, buf, len) != 0) {
        t->errored = true;
        return -1;
    }
    t->bytesEmitted += len;
    return 0;
}

/* Ensure the output buffer is large enough for the given input.
 * Reuses the existing buffer when possible to avoid per-write allocation.
 * zmalloc aborts on OOM, so this cannot fail. */
static void streamWriterEnsureOutBuf(streamWriter *t, size_t inputLen) {
    size_t needed = streamCompressOutputBound(&t->compressor, inputLen);
    if (needed == 0) {
        /* Ensure a minimal valid buffer so streamCompressFeed never gets NULL. */
        if (t->outBuf == NULL) {
            t->outBuf = zmalloc(64);
            t->outBufSize = 64;
        }
        return;
    }
    if (needed > t->outBufSize) {
        t->outBuf = zrealloc(t->outBuf, needed);
        t->outBufSize = needed;
    }
}

/* Compress one chunk with the requested flush mode and emit produced bytes.
 * Returns 0 on success, -1 on error (and sets t->errored). */
static int streamWriterFeedAndEmit(streamWriter *t,
                                   const uint8_t *input,
                                   size_t inputLen,
                                   compressFlushMode flush_mode) {
    streamWriterEnsureOutBuf(t, inputLen);

    ssize_t compressed = streamCompressFeed(&t->compressor, t->outBuf,
                                            t->outBufSize,
                                            input, inputLen, flush_mode);
    if (compressed < 0) {
        t->errored = true;
        return -1;
    }
    return streamWriterEmit(t, t->outBuf, (size_t)compressed);
}

/* Release stream compressor state owned by streamWriter.
 * Does not free the context object itself. */
static void streamWriterReleaseContext(streamWriter *t) {
    streamCompressorDestroy(&t->compressor);
    if (t->outBuf) {
        zfree(t->outBuf);
        t->outBuf = NULL;
    }
    t->outBufSize = 0;
}

streamWriter *streamWriterCreate(const streamWriterConfig *cfg,
                                      vkcsEmitFn emitCb,
                                      void *emitCtx) {
    if (!cfg || !emitCb || !compressionAlgoSupportsStreaming(cfg->algo)) {
        return NULL;
    }

    streamWriter *t = zmalloc(sizeof(*t));
    if (streamWriterInitContext(t, cfg, emitCb, emitCtx) != 0) {
        zfree(t);
        return NULL;
    }
    return t;
}

ssize_t streamWriterWrite(streamWriter *t, const void *buf, size_t len) {
    if (!t) return -1;
    if (t->errored) return -1;
    /* Writes after finish are always a caller bug. Returning an error
     * prevents silent data drops in shared API users (rio/replication). */
    if (t->finished) return -1;
    if (len == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    uint64_t emittedBefore = t->bytesEmitted;
    if (streamWriterEnsureEnvelope(t) != 0) return -1;
    while (remaining > 0) {
        size_t chunkLen = remaining < STREAM_WRITER_INPUT_CHUNK_SIZE
                               ? remaining
                               : STREAM_WRITER_INPUT_CHUNK_SIZE;
        if (streamWriterFeedAndEmit(t, src, chunkLen, FLUSH_CONTINUE) != 0) return -1;
        src += chunkLen;
        remaining -= chunkLen;
    }
    uint64_t emittedDelta = t->bytesEmitted - emittedBefore;
    if (emittedDelta > (uint64_t)SSIZE_MAX) {
        t->errored = true;
        return -1;
    }
    return (ssize_t)emittedDelta;
}

int streamWriterFlush(streamWriter *t) {
    if (!t) return -1;
    if (t->errored) return -1;
    /* Flush-after-finish is a harmless no-op: frame is already closed. */
    if (t->finished) return 0;

    if (!t->envelopeWritten || !t->compressor.frame_started) return 0;
    if (streamWriterFeedAndEmit(t, NULL, 0, FLUSH_SYNC) != 0) return -1;
    return 0;
}

int streamWriterFinish(streamWriter *t) {
    if (!t) return -1;
    if (t->errored) return -1;
    if (t->finished) return 0;
    t->finished = true;

    /* If nothing was ever written, emit envelope + empty frame end. */
    if (streamWriterEnsureEnvelope(t) != 0) return -1;
    if (streamWriterFeedAndEmit(t, NULL, 0, FLUSH_END) != 0) return -1;
    return 0;
}

void streamWriterDestroy(streamWriter *t) {
    if (!t) return;
    streamWriterReleaseContext(t);
    zfree(t);
}

int streamWriterIsErrored(const streamWriter *t) {
    return t && t->errored;
}

void streamWriterSetError(streamWriter *t) {
    if (!t) return;
    t->errored = true;
}

/* Streaming reader context. */
struct stream_reader {
    streamReaderReadFn readCb; /* Returns >0 bytes, 0 EOF, -1 error */
    void *readCtx;

    vkcsProbeConfig probeCfg;
    vkcsProbe probe;
    size_t probeReplayPos; /* Unread passthrough bytes buffered by vkcsProbeFeed */
    size_t bufferSize;
    bool errored;
    streamReaderError errorKind;

    streamDecompressor decompressor;
    bool decompressorInitialized;

    uint8_t *compressedBuf; /* Buffered compressed input */
    size_t compressedBufPos;
    size_t compressedBufLen;

    uint8_t *decompressedBuf; /* Buffered decompressed output for small caller reads */
    size_t decompressedBufPos;
    size_t decompressedBufLen;
};

static void streamReaderSetError(streamReader *t, streamReaderError errorKind) {
    t->errored = true;
    if (t->errorKind == STREAM_READER_ERROR_NONE) {
        t->errorKind = errorKind;
    }
}

/* Preserve partial output on read errors while latching sticky error state. */
static ssize_t streamReaderFail(streamReader *t, size_t partialBytes) {
    streamReaderSetError(t, STREAM_READER_ERROR_IO);
    return partialBytes > 0 ? (ssize_t)partialBytes : -1;
}

static ssize_t streamReaderFailWithError(streamReader *t,
                                         size_t partialBytes,
                                         streamReaderError errorKind) {
    streamReaderSetError(t, errorKind);
    return partialBytes > 0 ? (ssize_t)partialBytes : -1;
}

static int streamReaderInitCompressedState(streamReader *t, size_t bufferSize) {
    if (streamDecompressorInit(&t->decompressor, t->probe.algo) != 0) {
        return -1;
    }
    t->decompressorInitialized = true;

    t->compressedBuf = zmalloc(bufferSize);
    t->compressedBufPos = 0;
    t->compressedBufLen = 0;
    t->decompressedBuf = zmalloc(bufferSize);
    t->decompressedBufPos = 0;
    t->decompressedBufLen = 0;
    return 0;
}

static void streamReaderResetCompressedState(streamReader *t) {
    if (t->decompressorInitialized) {
        streamDecompressorDestroy(&t->decompressor);
        t->decompressorInitialized = false;
    }
    if (t->compressedBuf) {
        zfree(t->compressedBuf);
        t->compressedBuf = NULL;
    }
    t->compressedBufPos = 0;
    t->compressedBufLen = 0;
    if (t->decompressedBuf) {
        zfree(t->decompressedBuf);
        t->decompressedBuf = NULL;
    }
    t->decompressedBufPos = 0;
    t->decompressedBufLen = 0;
}

streamReader *streamReaderCreate(const streamReaderConfig *cfg,
                                      streamReaderReadFn readCb,
                                      void *readCtx) {
    if (!readCb) return NULL;
    if (!cfg) return NULL;

    streamReader *t = zmalloc(sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->readCb = readCb;
    t->readCtx = readCtx;
    t->probeCfg.allowPassthrough = cfg->allow_passthrough;
    t->probeCfg.expectedStreamKind = cfg->expected_stream_kind;
    vkcsProbeInit(&t->probe);
    t->bufferSize = cfg->buffer_size ? cfg->buffer_size : STREAM_READER_BUFFER_SIZE_DEFAULT;
    if (t->bufferSize < STREAM_READER_BUFFER_SIZE_MIN) {
        t->bufferSize = STREAM_READER_BUFFER_SIZE_MIN;
    }
    return t;
}

static size_t streamReaderProbeBytesNeeded(const streamReader *t) {
    if (t->probe.headerLen < 4) return 4 - t->probe.headerLen;
    return VKCS_ENVELOPE_SIZE - t->probe.headerLen;
}

int streamReaderProbe(streamReader *t) {
    if (!t) return -1;
    if (t->errored) return -1;
    if (t->probe.ready) return 0;

    while (!t->probe.ready) {
        uint8_t buf[VKCS_ENVELOPE_SIZE];
        size_t need = streamReaderProbeBytesNeeded(t);
        ssize_t got = t->readCb(t->readCtx, buf, need);
        size_t consumed = 0;

        if (got < 0 || (size_t)got > need) {
            streamReaderSetError(t, STREAM_READER_ERROR_IO);
            return -1;
        }

        vkcsProbeResult status = vkcsProbeFeed(&t->probe, &t->probeCfg, buf,
                                                   got > 0 ? (size_t)got : 0,
                                                   got == 0, &consumed);
        if (status == VKCS_PROBE_ERROR) {
            streamReaderSetError(t, STREAM_READER_ERROR_INCOMPATIBLE);
            return -1;
        }
        if (consumed != (size_t)(got > 0 ? got : 0)) {
            streamReaderSetError(t, STREAM_READER_ERROR_IO);
            return -1;
        }
        if (status == VKCS_PROBE_NEED_INPUT) continue;
        if (status == VKCS_PROBE_COMPRESSED &&
            !t->decompressorInitialized &&
            streamReaderInitCompressedState(t, t->bufferSize) != 0) {
            streamReaderSetError(t, STREAM_READER_ERROR_IO);
            return -1;
        }
    }

    return 0;
}

static size_t streamReaderProbeAvail(const streamReader *t) {
    if (t->probe.headerLen <= t->probeReplayPos) return 0;
    return t->probe.headerLen - t->probeReplayPos;
}

/* Passthrough reads may need to replay probe bytes before reading directly
 * from the wrapped source. On a transport read error after replaying some
 * bytes, return the partial payload and latch a sticky error for the next call. */
static ssize_t streamReaderReadPassthrough(streamReader *t,
                                           uint8_t *dst,
                                           size_t len) {
    size_t total = 0;

    size_t prefix_avail = streamReaderProbeAvail(t);
    if (prefix_avail > 0) {
        size_t from_prefix = prefix_avail < len ? prefix_avail : len;
        memcpy(dst, t->probe.header + t->probeReplayPos, from_prefix);
        t->probeReplayPos += from_prefix;
        dst += from_prefix;
        len -= from_prefix;
        total += from_prefix;
    }

    if (len == 0) return (ssize_t)total;

    ssize_t got = t->readCb(t->readCtx, dst, len);
    if (got < 0 || (size_t)got > len) return streamReaderFail(t, total);
    return (ssize_t)(total + (size_t)got);
}

static int streamReaderDrainCompressedBuf(streamReader *t,
                                          uint8_t *out,
                                          size_t out_size,
                                          size_t *out_written) {
    *out_written = 0;
    while (t->compressedBufLen > 0 && *out_written < out_size) {
        size_t consumed = 0;
        ssize_t produced = streamDecompressFeed(
            &t->decompressor,
            out + *out_written, out_size - *out_written,
            t->compressedBuf + t->compressedBufPos,
            t->compressedBufLen, &consumed);
        if (produced < 0) {
            streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
            return -1;
        }
        if (consumed > t->compressedBufLen ||
            (size_t)produced > out_size - *out_written) {
            streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
            return -1;
        }
        *out_written += (size_t)produced;
        t->compressedBufPos += consumed;
        t->compressedBufLen -= consumed;
        if (consumed == 0 && produced == 0) break;
    }
    if (t->compressedBufLen == 0) t->compressedBufPos = 0;
    return 0;
}

static size_t streamReaderCompressedBufTailSpace(streamReader *t) {
    size_t tail_space = t->bufferSize - t->compressedBufPos - t->compressedBufLen;
    if (tail_space > 0) return tail_space;

    if (t->compressedBufLen == 0) {
        t->compressedBufPos = 0;
        return t->bufferSize;
    }

    if (t->compressedBufPos > 0) {
        memmove(t->compressedBuf, t->compressedBuf + t->compressedBufPos, t->compressedBufLen);
        t->compressedBufPos = 0;
        return t->bufferSize - t->compressedBufLen;
    }

    /* The generic stream reader intentionally keeps a fixed-size compressed
     * input buffer and a fixed-size decompressed output window. If the codec
     * cannot make progress while the compressed buffer is full, treat the
     * stream as corrupt instead of growing more state. */
    streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
    return 0;
}

static int streamReaderRefillCompressedBuf(streamReader *t) {
    size_t read_size = streamReaderCompressedBufTailSpace(t);
    if (read_size > (size_t)SSIZE_MAX) read_size = (size_t)SSIZE_MAX;
    if (read_size == 0) return -1;

    ssize_t got = t->readCb(
        t->readCtx,
        t->compressedBuf + t->compressedBufPos + t->compressedBufLen,
        read_size);
    if (got < 0) return -1;
    if (got == 0) return 0;
    if ((size_t)got > read_size) return -1;
    t->compressedBufLen += (size_t)got;
    return 1;
}

static ssize_t streamReaderFillDecompressedBuf(streamReader *t) {
    size_t written = 0;

    t->decompressedBufPos = 0;
    t->decompressedBufLen = 0;

    while (written < t->bufferSize) {
        if (t->compressedBufLen > 0) {
            size_t chunk_written = 0;
            if (streamReaderDrainCompressedBuf(t, t->decompressedBuf + written,
                                               t->bufferSize - written, &chunk_written) != 0) {
                written += chunk_written;
                break;
            }
            written += chunk_written;
            if (written >= t->bufferSize) break;
        }

        int read_rc = streamReaderRefillCompressedBuf(t);
        if (read_rc < 0) {
            streamReaderSetError(t, STREAM_READER_ERROR_IO);
            if (written == 0) return -1;
            break;
        }
        if (read_rc == 0) break;
    }

    t->decompressedBufLen = written;
    return (ssize_t)written;
}

static inline size_t streamReaderDecompressedBufAvail(const streamReader *t) {
    if (t->decompressedBufLen <= t->decompressedBufPos) return 0;
    return t->decompressedBufLen - t->decompressedBufPos;
}

static size_t streamReaderCopyFromDecompressedBuf(streamReader *t,
                                                  uint8_t **dst,
                                                  size_t *remaining) {
    size_t avail = streamReaderDecompressedBufAvail(t);
    if (avail == 0 || *remaining == 0) return 0;

    size_t to_copy = avail < *remaining ? avail : *remaining;
    memcpy(*dst, t->decompressedBuf + t->decompressedBufPos, to_copy);
    t->decompressedBufPos += to_copy;
    *dst += to_copy;
    *remaining -= to_copy;
    return to_copy;
}

static ssize_t streamReaderReadCompressed(streamReader *t, uint8_t *dst, size_t len) {
    size_t remaining = len;
    size_t total = 0;

    total += streamReaderCopyFromDecompressedBuf(t, &dst, &remaining);
    while (remaining > 0) {
        if (streamReaderDecompressedBufAvail(t) == 0) {
            ssize_t filled = streamReaderFillDecompressedBuf(t);
            if (filled < 0) {
                return streamReaderFailWithError(
                    t, total,
                    t->errorKind == STREAM_READER_ERROR_NONE ? STREAM_READER_ERROR_IO
                                                              : t->errorKind);
            }
            if (filled == 0 && !t->decompressor.frame_done) {
                return streamReaderFailWithError(t, total, STREAM_READER_ERROR_CORRUPT);
            }
            if (filled == 0) break;
        }

        total += streamReaderCopyFromDecompressedBuf(t, &dst, &remaining);
        if (t->errored) break;
    }

    return (ssize_t)total;
}

ssize_t streamReaderRead(streamReader *t, void *buf, size_t len) {
    if (!t || !buf) return -1;
    if (t->errored) return -1;
    if (len == 0) return 0;
    if (len > (size_t)SSIZE_MAX) return -1;

    if (streamReaderProbe(t) != 0) return -1;

    if (!t->probe.compressed) {
        return streamReaderReadPassthrough(t, (uint8_t *)buf, len);
    }

    return streamReaderReadCompressed(t, (uint8_t *)buf, len);
}

int streamReaderGetInfo(streamReader *t, streamReaderInfo *info) {
    if (!t || !info) return -1;
    if (streamReaderProbe(t) != 0) return -1;

    info->compressed = t->probe.compressed;
    info->codec_checksum_enabled = t->probe.compressed ? t->probe.codecChecksumEnabled : false;
    info->algo = t->probe.compressed ? t->probe.algo : ALGO_NONE;
    info->stream_kind = t->probe.streamKind;
    return 0;
}

streamReaderError streamReaderGetError(const streamReader *t) {
    return t ? t->errorKind : STREAM_READER_ERROR_IO;
}

void streamReaderDestroy(streamReader *t) {
    if (!t) return;
    streamReaderResetCompressedState(t);
    zfree(t);
}
