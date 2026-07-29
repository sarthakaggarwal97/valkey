/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_stream.h"
#include "serverassert.h"
#include "zmalloc.h"
#include <limits.h>
#include <string.h>

/* ===== VCS envelope ===== */

static const uint8_t VCS_MAGIC[VCS_MAGIC_SIZE] = {
    VCS_MAGIC_0,
    VCS_MAGIC_1,
    VCS_MAGIC_2,
};

/* True when the first len bytes of buf match the VCS magic. When len is below
 * VCS_MAGIC_SIZE this only compares that prefix. */
static bool vcsHasMagicPrefix(const uint8_t *buf, size_t len) {
    size_t n = len < VCS_MAGIC_SIZE ? len : VCS_MAGIC_SIZE;
    return memcmp(buf, VCS_MAGIC, n) == 0;
}

static bool streamProbeHasMagicPrefix(const streamProbe *probe) {
    if (probe->header_len == 0) return false;
    return vcsHasMagicPrefix(probe->header, probe->header_len);
}

static void streamProbeSetPassthrough(streamProbe *probe) {
    probe->ready = true;
    probe->info.compressed = false;
    probe->info.codec_checksum_enabled = false;
    probe->info.algo = ALGO_NONE;
    probe->info.stream_kind = 0;
}

static void streamProbeSetCompressed(streamProbe *probe, const streamReaderInfo *info) {
    probe->ready = true;
    probe->info = *info;
}

/* Fill the 7-byte VCS envelope. Returns -1 if algo has no streaming codec. */
static int buildVcsEnvelope(uint8_t *out,
                            compressionAlgo algo,
                            uint8_t stream_kind,
                            bool codec_checksum_enabled) {
    const compressionCodec *codec = compressionCodecByAlgo(algo);
    if (!codec) return -1;

    uint8_t envelope[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        [VCS_OFFSET_VERSION] = VCS_VERSION,
        [VCS_OFFSET_ALGO] = codec->vcs_id,
        [VCS_OFFSET_FLAGS] = codec_checksum_enabled ? VCS_FLAG_CODEC_CHECKSUM : 0,
        [VCS_OFFSET_STREAM_KIND] = stream_kind,
    };
    memcpy(out, envelope, VCS_ENVELOPE_SIZE);
    return 0;
}

static int writeVcsEnvelope(streamWriterEmitFn emit_fn,
                            void *ctx,
                            compressionAlgo algo,
                            uint8_t stream_kind,
                            bool codec_checksum_enabled) {
    uint8_t envelope[VCS_ENVELOPE_SIZE];
    if (buildVcsEnvelope(envelope, algo, stream_kind, codec_checksum_enabled) != 0) return -1;
    return emit_fn(ctx, envelope, VCS_ENVELOPE_SIZE) == 0 ? 0 : -1;
}

/* Rejects unknown flag bits so a future format extension fails loud rather
 * than silently corrupting load. */
static int readVcsEnvelope(const uint8_t *buf,
                           size_t len,
                           compressionAlgo *algo,
                           uint8_t *stream_kind,
                           bool *codec_checksum_enabled) {
    if (len < VCS_ENVELOPE_SIZE) return -1;

    if (!vcsHasMagicPrefix(buf, VCS_MAGIC_SIZE)) return -1;
    if (buf[VCS_OFFSET_VERSION] != VCS_VERSION) return -1;

    const compressionCodec *codec = compressionCodecByVcsId(buf[VCS_OFFSET_ALGO]);
    if (!codec) return -1;

    uint8_t flags = buf[VCS_OFFSET_FLAGS];
    if (flags & ~VCS_FLAG_CODEC_CHECKSUM) return -1;

    if (algo) *algo = codec->algo;
    if (stream_kind) *stream_kind = buf[VCS_OFFSET_STREAM_KIND];
    if (codec_checksum_enabled) *codec_checksum_enabled = (flags & VCS_FLAG_CODEC_CHECKSUM) != 0;
    return 0;
}

int streamReadEnvelopeInfo(const uint8_t *buf,
                           size_t len,
                           uint8_t expected_stream_kind,
                           streamReaderInfo *info) {
    uint8_t stream_kind = 0;
    bool codec_checksum_enabled = false;
    compressionAlgo algo = ALGO_NONE;

    if (len < VCS_ENVELOPE_SIZE ||
        readVcsEnvelope(buf, len, &algo, &stream_kind, &codec_checksum_enabled) != 0 ||
        stream_kind != expected_stream_kind) {
        return -1;
    }

    info->compressed = true;
    info->codec_checksum_enabled = codec_checksum_enabled;
    info->algo = algo;
    info->stream_kind = stream_kind;
    return 0;
}

void streamProbeInit(streamProbe *probe, uint8_t expected_stream_kind, bool allow_passthrough) {
    memset(probe, 0, sizeof(*probe));
    probe->expected_stream_kind = expected_stream_kind;
    probe->allow_passthrough = allow_passthrough;
}

size_t streamProbeBytesNeeded(const streamProbe *probe) {
    if (probe->header_len < VCS_MAGIC_SIZE) return VCS_MAGIC_SIZE - probe->header_len;
    return VCS_ENVELOPE_SIZE - probe->header_len;
}

/* Incremental: inputs may contain only part of the VCS envelope. Consumed bytes
 * are retained in probe->header so passthrough callers can replay them exactly. */
streamProbeResult streamProbeFeed(streamProbe *probe,
                                  const uint8_t *src,
                                  size_t src_len,
                                  bool input_eof,
                                  size_t *src_consumed) {
    size_t consumed = 0;
    *src_consumed = 0;
    if (probe->ready) {
        return probe->info.compressed ? STREAM_PROBE_COMPRESSED : STREAM_PROBE_PASSTHROUGH;
    }

    while (consumed < src_len) {
        size_t target = probe->header_len < VCS_MAGIC_SIZE ? VCS_MAGIC_SIZE : VCS_ENVELOPE_SIZE;
        size_t need = target - probe->header_len;
        size_t take = src_len - consumed < need ? src_len - consumed : need;

        memcpy(probe->header + probe->header_len, src + consumed, take);
        probe->header_len += take;
        consumed += take;

        if (!vcsHasMagicPrefix(probe->header, probe->header_len)) {
            *src_consumed = consumed;
            if (!probe->allow_passthrough) return STREAM_PROBE_ERROR;
            streamProbeSetPassthrough(probe);
            return STREAM_PROBE_PASSTHROUGH;
        }

        if (probe->header_len == VCS_ENVELOPE_SIZE) {
            streamReaderInfo info = {0};
            if (streamReadEnvelopeInfo(probe->header, VCS_ENVELOPE_SIZE,
                                       probe->expected_stream_kind, &info) != 0) {
                *src_consumed = consumed;
                return STREAM_PROBE_ERROR;
            }
            streamProbeSetCompressed(probe, &info);
            *src_consumed = consumed;
            return STREAM_PROBE_COMPRESSED;
        }
    }

    if (input_eof) {
        *src_consumed = consumed;
        /* EOF mid-magic looks like a truncated VCS, not a valid passthrough. */
        if (streamProbeHasMagicPrefix(probe)) return STREAM_PROBE_ERROR;
        if (!probe->allow_passthrough) return STREAM_PROBE_ERROR;
        streamProbeSetPassthrough(probe);
        return STREAM_PROBE_PASSTHROUGH;
    }

    *src_consumed = consumed;
    return STREAM_PROBE_NEED_INPUT;
}

/* ===== Streaming writer ===== */

int streamWriterInit(streamWriter *writer,
                     const streamWriterConfig *cfg,
                     streamWriterEmitFn emit_fn,
                     void *emit_ctx) {
    memset(writer, 0, sizeof(*writer));
    writer->emit_fn = emit_fn;
    writer->emit_ctx = emit_ctx;
    writer->stream_kind = cfg->stream_kind;

    return streamCompressorInit(&writer->compressor, cfg->algo, cfg->level,
                                cfg->codec_checksum_enabled);
}

void streamWriterSetSink(streamWriter *writer, sds *sink) {
    writer->sink = sink;
}

/* Envelope is emitted lazily so a writer that's created but never written
 * doesn't leave a stub envelope on the sink. */
static int streamWriterEnsureEnvelope(streamWriter *writer) {
    if (writer->envelope_written) return 0;
    if (writer->sink) {
        uint8_t envelope[VCS_ENVELOPE_SIZE];
        if (buildVcsEnvelope(envelope, streamCompressorAlgo(&writer->compressor), writer->stream_kind,
                             writer->compressor.codec_checksum) != 0) {
            writer->errored = true;
            return -1;
        }
        *writer->sink = sdscatlen(*writer->sink, (char *)envelope, VCS_ENVELOPE_SIZE);
    } else if (writeVcsEnvelope(writer->emit_fn, writer->emit_ctx,
                                streamCompressorAlgo(&writer->compressor),
                                writer->stream_kind, writer->compressor.codec_checksum) != 0) {
        writer->errored = true;
        return -1;
    }
    writer->envelope_written = true;
    return 0;
}

static int streamWriterEmit(streamWriter *writer, const uint8_t *buf, size_t len) {
    if (len == 0) return 0;
    if (writer->emit_fn(writer->emit_ctx, buf, len) != 0) {
        writer->errored = true;
        return -1;
    }
    return 0;
}

static int streamWriterEnsureScratch(streamWriter *writer) {
    if (writer->scratch) return 0;

    size_t input_size = streamCompressorChunkSize(&writer->compressor);
    size_t output_size = 0;
    if (!writer->sink) {
        output_size = streamCompressorOutputBound(&writer->compressor, input_size);
    }
    if (input_size == 0 || (!writer->sink && output_size == 0) ||
        input_size > SIZE_MAX - output_size) {
        writer->errored = true;
        return -1;
    }

    writer->scratch = zmalloc(input_size + output_size);
    writer->in_buf = writer->scratch;
    writer->in_buf_size = input_size;
    writer->out_buf = output_size ? writer->scratch + input_size : NULL;
    writer->out_buf_size = output_size;
    return 0;
}

/* Sink path: compress directly into the caller's sds tail, without an output
 * scratch buffer or emit callback. */
static int streamWriterFeedToSink(streamWriter *writer,
                                  const uint8_t *input,
                                  size_t input_len,
                                  bool input_stable,
                                  compressFlushMode flush_mode) {
    size_t bound = streamCompressorOutputBound(&writer->compressor, input_len);
    if (bound == 0) {
        writer->errored = true;
        return -1;
    }
    if (!writer->compressor.stream_started)
        *writer->sink = sdsMakeRoomForNonGreedy(*writer->sink, bound);
    else
        *writer->sink = sdsMakeRoomFor(*writer->sink, bound);
    ssize_t compressed = streamCompressorFeed(&writer->compressor,
                                              (uint8_t *)(*writer->sink) + sdslen(*writer->sink),
                                              sdsavail(*writer->sink), input, input_len,
                                              input_stable, flush_mode);
    if (compressed < 0 || (size_t)compressed > sdsavail(*writer->sink)) {
        writer->errored = true;
        return -1;
    }
    sdsIncrLen(*writer->sink, (size_t)compressed);
    return 0;
}

static int streamWriterFeedAndEmit(streamWriter *writer,
                                   const uint8_t *input,
                                   size_t input_len,
                                   bool input_stable,
                                   compressFlushMode flush_mode) {
    if (streamWriterEnsureScratch(writer) != 0 || input_len > writer->in_buf_size) return -1;
    if (writer->sink) {
        return streamWriterFeedToSink(writer, input, input_len, input_stable, flush_mode);
    }

    size_t bound = streamCompressorOutputBound(&writer->compressor, input_len);
    if (bound == 0 || bound > writer->out_buf_size) {
        writer->errored = true;
        return -1;
    }

    ssize_t compressed = streamCompressorFeed(&writer->compressor, writer->out_buf,
                                              writer->out_buf_size,
                                              input, input_len, input_stable, flush_mode);
    if (compressed < 0 || (size_t)compressed > writer->out_buf_size) {
        writer->errored = true;
        return -1;
    }
    return streamWriterEmit(writer, writer->out_buf, (size_t)compressed);
}

void streamWriterFree(streamWriter *writer) {
    streamCompressorFree(&writer->compressor);
    zfree(writer->scratch);
    writer->scratch = NULL;
    writer->in_buf = NULL;
    writer->in_buf_len = 0;
    writer->in_buf_size = 0;
    writer->out_buf = NULL;
    writer->out_buf_size = 0;
}

static int streamWriterStart(streamWriter *writer) {
    if (streamWriterEnsureEnvelope(writer) != 0) return -1;
    if (writer->compressor.stream_started) return 0;
    return streamWriterFeedAndEmit(writer, NULL, 0, false, FLUSH_CONTINUE);
}

static int streamWriterDrainInput(streamWriter *writer,
                                  bool input_stable,
                                  compressFlushMode flush_mode) {
    const uint8_t *input = writer->in_buf_len ? writer->in_buf : NULL;
    size_t input_len = writer->in_buf_len;

    if (streamWriterFeedAndEmit(writer, input, input_len, input_stable, flush_mode) != 0) return -1;
    writer->in_buf_len = 0;
    return 0;
}

int streamWriterWrite(streamWriter *writer, const void *buf, size_t len) {
    /* Errors latch: the frame may be partially emitted, so feeding more input
     * could push garbage to the sink rather than fail cleanly. */
    if (writer->errored) return -1;
    /* Writes after finish are a caller bug; silently dropping them would
     * corrupt the consumer's view of the stream. */
    if (writer->finished) return -1;
    if (len == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    if (streamWriterStart(writer) != 0) return -1;

    if (writer->in_buf_len > 0) {
        size_t to_copy = writer->in_buf_size - writer->in_buf_len;
        if (to_copy > remaining) to_copy = remaining;
        memcpy(writer->in_buf + writer->in_buf_len, src, to_copy);
        writer->in_buf_len += to_copy;
        src += to_copy;
        remaining -= to_copy;

        /* Keep a linked-block dictionary in caller or writer memory only when
         * another complete block is fed before this call returns. */
        if (writer->in_buf_len == writer->in_buf_size &&
            streamWriterDrainInput(writer, remaining >= writer->in_buf_size,
                                   FLUSH_CONTINUE) != 0) {
            return -1;
        }
    }

    while (remaining >= writer->in_buf_size) {
        bool input_stable = remaining - writer->in_buf_size >= writer->in_buf_size;
        if (streamWriterFeedAndEmit(writer, src, writer->in_buf_size,
                                    input_stable, FLUSH_CONTINUE) != 0) {
            return -1;
        }
        src += writer->in_buf_size;
        remaining -= writer->in_buf_size;
    }

    if (remaining > 0) {
        memcpy(writer->in_buf, src, remaining);
        writer->in_buf_len = remaining;
    }
    return 0;
}

int streamWriterFlush(streamWriter *writer) {
    if (writer->errored) return -1;
    /* Flush after finish is a no-op: frame is already closed. */
    if (writer->finished) return 0;

    if (!writer->envelope_written || !writer->compressor.stream_started) return 0;
    return streamWriterDrainInput(writer, false, FLUSH_SYNC);
}

int streamWriterFinish(streamWriter *writer) {
    if (writer->errored) return -1;
    if (writer->finished) return 0;
    writer->finished = true;

    /* Even an empty stream produces a valid envelope + empty frame so the
     * loader sees a well-formed file. */
    if (streamWriterEnsureEnvelope(writer) != 0) return -1;
    return streamWriterDrainInput(writer, false, FLUSH_END);
}

/* ===== Streaming reader ===== */

static void streamReaderSetError(streamReader *reader, streamReaderError error_kind) {
    reader->errored = true;
    if (reader->error_kind == STREAM_READER_ERROR_NONE) reader->error_kind = error_kind;
}

static ssize_t streamReaderFail(streamReader *reader, size_t partial_bytes) {
    streamReaderSetError(reader, STREAM_READER_ERROR_IO);
    return partial_bytes > 0 ? (ssize_t)partial_bytes : -1;
}

static ssize_t streamReaderFailWithError(streamReader *reader,
                                         size_t partial_bytes,
                                         streamReaderError error_kind) {
    streamReaderSetError(reader, error_kind);
    return partial_bytes > 0 ? (ssize_t)partial_bytes : -1;
}

static int streamReaderInitCompressedState(streamReader *reader, size_t buffer_size) {
    if (streamDecompressorInit(&reader->decompressor, reader->probe.info.algo) != 0) return -1;
    reader->decompressor_initialized = true;
    reader->compressed_buf = zmalloc(buffer_size);
    return 0;
}

static void streamReaderResetCompressedState(streamReader *reader) {
    if (reader->decompressor_initialized) {
        streamDecompressorFree(&reader->decompressor);
        reader->decompressor_initialized = false;
    }
    if (reader->compressed_buf) {
        zfree(reader->compressed_buf);
        reader->compressed_buf = NULL;
    }
    reader->compressed_buf_pos = 0;
    reader->compressed_buf_len = 0;
}

int streamReaderInit(streamReader *reader,
                     const streamReaderConfig *cfg,
                     streamReaderReadFn read_cb,
                     void *read_ctx) {
    assert(cfg->buffer_size != 0);

    memset(reader, 0, sizeof(*reader));
    reader->read_cb = read_cb;
    reader->read_ctx = read_ctx;
    streamProbeInit(&reader->probe, cfg->expected_stream_kind, cfg->allow_passthrough);
    reader->buffer_size = cfg->buffer_size;
    if (reader->buffer_size < STREAM_READER_BUFFER_SIZE_MIN) {
        reader->buffer_size = STREAM_READER_BUFFER_SIZE_MIN;
    }
    return 0;
}

static int streamReaderProbe(streamReader *reader) {
    if (reader->errored) return -1;
    if (reader->probe.ready) return 0;

    while (!reader->probe.ready) {
        uint8_t buf[VCS_ENVELOPE_SIZE];
        size_t need = streamProbeBytesNeeded(&reader->probe);
        ssize_t got = reader->read_cb(reader->read_ctx, buf, need);
        size_t consumed = 0;

        if (got < 0 || (size_t)got > need) {
            streamReaderSetError(reader, STREAM_READER_ERROR_IO);
            return -1;
        }

        streamProbeResult status = streamProbeFeed(&reader->probe, buf,
                                                   got > 0 ? (size_t)got : 0,
                                                   got == 0, &consumed);
        switch (status) {
        case STREAM_PROBE_ERROR:
            streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
            return -1;
        case STREAM_PROBE_NEED_INPUT:
            continue;
        case STREAM_PROBE_COMPRESSED:
            if (!reader->decompressor_initialized &&
                streamReaderInitCompressedState(reader, reader->buffer_size) != 0) {
                streamReaderSetError(reader, STREAM_READER_ERROR_IO);
                return -1;
            }
            break;
        case STREAM_PROBE_PASSTHROUGH:
            break;
        default:
            streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
            return -1;
        }
    }
    return 0;
}

static size_t streamReaderProbeAvail(streamReader *reader) {
    if (reader->probe.header_len <= reader->probe_replay_pos) return 0;
    return reader->probe.header_len - reader->probe_replay_pos;
}

/* Replay any probe-buffered bytes before reading from the wrapped source. */
static ssize_t streamReaderReadPassthrough(streamReader *reader, uint8_t *dst, size_t len) {
    size_t total = 0;
    size_t prefix_avail = streamReaderProbeAvail(reader);
    if (prefix_avail > 0) {
        size_t from_prefix = prefix_avail < len ? prefix_avail : len;
        memcpy(dst, reader->probe.header + reader->probe_replay_pos, from_prefix);
        reader->probe_replay_pos += from_prefix;
        dst += from_prefix;
        len -= from_prefix;
        total += from_prefix;
    }
    while (len > 0) {
        ssize_t got = reader->read_cb(reader->read_ctx, dst, len);
        if (got < 0 || (size_t)got > len) return streamReaderFail(reader, total);
        if (got == 0) break;
        dst += got;
        len -= (size_t)got;
        total += (size_t)got;
    }
    return (ssize_t)total;
}

static int streamReaderDrainCompressedBuf(streamReader *reader,
                                          uint8_t *out,
                                          size_t out_size,
                                          size_t *out_written) {
    *out_written = 0;
    while (reader->compressed_buf_len > 0 && *out_written < out_size) {
        size_t consumed = 0;
        size_t feed_len = reader->compressed_buf_len;
        size_t input_hint = reader->decompressor.input_hint;
        if (input_hint > 0 && feed_len > input_hint) feed_len = input_hint;
        ssize_t produced = streamDecompressorFeed(
            &reader->decompressor,
            out + *out_written, out_size - *out_written,
            reader->compressed_buf + reader->compressed_buf_pos,
            feed_len, &consumed);
        if (produced < 0 ||
            consumed > feed_len ||
            (size_t)produced > out_size - *out_written) {
            streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
            return -1;
        }
        *out_written += (size_t)produced;
        reader->compressed_buf_pos += consumed;
        reader->compressed_buf_len -= consumed;
        if (reader->decompressor.frame_done) break;
        if (consumed == 0 && produced == 0) break;
    }
    if (reader->compressed_buf_len == 0) reader->compressed_buf_pos = 0;
    return 0;
}

static size_t streamReaderCompressedBufTailSpace(streamReader *reader) {
    size_t tail_space = reader->buffer_size - reader->compressed_buf_pos - reader->compressed_buf_len;
    if (tail_space > 0) return tail_space;

    if (reader->compressed_buf_len == 0) {
        reader->compressed_buf_pos = 0;
        return reader->buffer_size;
    }

    if (reader->compressed_buf_pos > 0) {
        memmove(reader->compressed_buf, reader->compressed_buf + reader->compressed_buf_pos, reader->compressed_buf_len);
        reader->compressed_buf_pos = 0;
        return reader->buffer_size - reader->compressed_buf_len;
    }

    /* Buffer full and the codec made no progress, treat as corrupt rather
     * than grow buffers indefinitely. */
    streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
    return 0;
}

static int streamReaderRefillCompressedBuf(streamReader *reader) {
    if (reader->decompressor.frame_done) return 0;

    size_t read_size = streamReaderCompressedBufTailSpace(reader);
    size_t input_hint = reader->decompressor.input_hint;
    if (input_hint > 0 && read_size > input_hint) read_size = input_hint;
    if (read_size > (size_t)SSIZE_MAX) read_size = (size_t)SSIZE_MAX;
    if (read_size == 0) return -1;

    ssize_t got = reader->read_cb(reader->read_ctx,
                                  reader->compressed_buf + reader->compressed_buf_pos + reader->compressed_buf_len,
                                  read_size);
    if (got < 0 || (size_t)got > read_size) return -1;
    if (got == 0) return 0;
    reader->compressed_buf_len += (size_t)got;
    return 1;
}

static ssize_t streamReaderReadCompressed(streamReader *reader, uint8_t *dst, size_t len) {
    size_t total = 0;

    while (total < len && !reader->decompressor.frame_done) {
        if (reader->compressed_buf_len > 0) {
            size_t written = 0;
            if (streamReaderDrainCompressedBuf(reader, dst + total, len - total, &written) != 0) {
                total += written;
                return streamReaderFailWithError(reader, total, reader->error_kind);
            }
            total += written;
            if (total == len || reader->decompressor.frame_done) break;
        }

        /* The decoder needs more transport bytes to progress. Preserve any
         * unconsumed prefix and append another bounded source read. */
        int read_rc = streamReaderRefillCompressedBuf(reader);
        if (read_rc < 0) {
            streamReaderError error_kind = reader->error_kind == STREAM_READER_ERROR_NONE
                                               ? STREAM_READER_ERROR_IO
                                               : reader->error_kind;
            return streamReaderFailWithError(reader, total, error_kind);
        }
        if (read_rc == 0) {
            return streamReaderFailWithError(reader, total, STREAM_READER_ERROR_CORRUPT);
        }
    }

    return (ssize_t)total;
}

ssize_t streamReaderRead(streamReader *reader, void *buf, size_t len) {
    if (reader->errored) return -1;
    if (len == 0) return 0;
    if (len > (size_t)SSIZE_MAX) return -1;

    if (!reader->probe.ready && streamReaderProbe(reader) != 0) return -1;

    if (!reader->probe.info.compressed) {
        return streamReaderReadPassthrough(reader, (uint8_t *)buf, len);
    }
    return streamReaderReadCompressed(reader, (uint8_t *)buf, len);
}

int streamReaderGetInfo(streamReader *reader, streamReaderInfo *info) {
    if (streamReaderProbe(reader) != 0) return -1;

    *info = reader->probe.info;
    return 0;
}

int streamReaderValidateEnd(streamReader *reader) {
    uint8_t buf[4096];

    if (streamReaderProbe(reader) != 0) return -1;
    if (!reader->probe.info.compressed) return 0;

    while (!reader->decompressor.frame_done) {
        ssize_t nread = streamReaderRead(reader, buf, sizeof(buf));
        if (nread < 0) return -1;
        if (nread > 0) {
            streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
            return -1;
        }
    }

    if (reader->compressed_buf_len > 0) {
        streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
        return -1;
    }

    ssize_t got = reader->read_cb(reader->read_ctx, buf, 1);
    if (got < 0) {
        streamReaderSetError(reader, STREAM_READER_ERROR_IO);
        return -1;
    }
    if (got > 0) {
        streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
        return -1;
    }
    return 0;
}

void streamReaderFree(streamReader *reader) {
    streamReaderResetCompressedState(reader);
}

/* Scratch memory held by the writer, for client-output-buffer accounting. */
size_t streamWriterMemUsage(const streamWriter *writer) {
    if (!writer) return 0;
    return writer->in_buf_size + writer->out_buf_size;
}
