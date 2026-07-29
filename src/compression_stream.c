/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_stream.h"
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

static int writeVcsEnvelope(streamWriterWriteFn write_cb,
                            void *ctx,
                            const compressionCodec *codec) {
    uint8_t envelope[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        [VCS_OFFSET_VERSION] = VCS_VERSION,
        [VCS_OFFSET_CODEC] = codec->vcs_id,
        [VCS_OFFSET_RESERVED] = 0,
        [VCS_OFFSET_STREAM_KIND] = VCS_STREAM_RDB,
    };
    return write_cb(ctx, envelope, VCS_ENVELOPE_SIZE) == 0 ? 0 : -1;
}

/* Reject a nonzero reserved byte so a future envelope extension fails loudly
 * rather than being silently misinterpreted. */
static int readVcsEnvelope(const uint8_t *buf, compressionAlgo *algo) {
    if (buf[VCS_OFFSET_VERSION] != VCS_VERSION) return -1;

    const compressionCodec *codec = compressionCodecByVcsId(buf[VCS_OFFSET_CODEC]);
    if (!codec) return -1;
    *algo = codec->algo;
    if (buf[VCS_OFFSET_RESERVED] != 0) return -1;
    if (buf[VCS_OFFSET_STREAM_KIND] != VCS_STREAM_RDB) return -1;
    return 0;
}

/* ===== Streaming Writer ===== */

int streamWriterInit(streamWriter *writer, const streamWriterConfig *cfg, streamWriterWriteFn write_cb, void *write_ctx) {
    memset(writer, 0, sizeof(*writer));
    writer->write_cb = write_cb;
    writer->write_ctx = write_ctx;

    if (streamCompressorInit(&writer->compressor, cfg->algo, cfg->level,
                             cfg->codec_checksum_enabled) != 0) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return -1;
    }
    return 0;
}

static int streamWriterEnsureScratch(streamWriter *writer) {
    if (writer->scratch) return 0;

    size_t input_size = streamCompressorChunkSize(&writer->compressor);
    size_t output_size = streamCompressorOutputBound(&writer->compressor, input_size);
    if (input_size == 0 || output_size == 0 || input_size > SIZE_MAX - output_size) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return -1;
    }

    writer->scratch = zmalloc(input_size + output_size);
    writer->in_buf = writer->scratch;
    writer->in_buf_size = input_size;
    writer->out_buf = writer->scratch + input_size;
    writer->out_buf_size = output_size;
    return 0;
}

/* Envelope is emitted lazily so a writer that's created but never written
 * doesn't leave a stub envelope on the sink. */
static int streamWriterEnsureEnvelope(streamWriter *writer) {
    if (writer->state == STREAM_WRITER_STATE_ACTIVE) return 0;
    if (writer->state != STREAM_WRITER_STATE_INITIAL) return -1;
    if (writeVcsEnvelope(writer->write_cb, writer->write_ctx, writer->compressor.codec) != 0) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return -1;
    }
    writer->state = STREAM_WRITER_STATE_ACTIVE;
    return 0;
}

static int streamWriterFeedAndWrite(streamWriter *writer,
                                    const uint8_t *input,
                                    size_t input_len,
                                    bool input_stable,
                                    compressFlushMode flush_mode) {
    if (streamWriterEnsureScratch(writer) != 0) return -1;
    if (input_len > writer->in_buf_size) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return -1;
    }

    const ssize_t compressed = streamCompressorFeed(&writer->compressor, writer->out_buf,
                                                    writer->out_buf_size,
                                                    input, input_len, input_stable,
                                                    flush_mode);
    if (compressed < 0 || (size_t)compressed > writer->out_buf_size) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return -1;
    }
    if (compressed > 0 && writer->write_cb(writer->write_ctx, writer->out_buf, (size_t)compressed) != 0) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return -1;
    }
    return 0;
}

/* Starting the codec separately lets the writer buffer the first small input
 * without delaying header errors or changing lazy empty-stream behavior. */
static int streamWriterStart(streamWriter *writer) {
    if (streamWriterEnsureEnvelope(writer) != 0) return -1;
    if (writer->compressor.stream_started) return 0;
    return streamWriterFeedAndWrite(writer, NULL, 0, false, COMPRESS_FLUSH_CONTINUE);
}

static int streamWriterDrainInput(streamWriter *writer, bool input_stable, compressFlushMode flush_mode) {
    const uint8_t *input = writer->in_buf_len ? writer->in_buf : NULL;
    size_t input_len = writer->in_buf_len;

    if (streamWriterFeedAndWrite(writer, input, input_len, input_stable, flush_mode) != 0) return -1;
    writer->in_buf_len = 0;
    return 0;
}

int streamWriterWrite(streamWriter *writer, const void *buf, size_t len) {
    /* Writes after finish are a caller bug; silently dropping them would
     * corrupt the consumer's view of the stream. */
    if (writer->state == STREAM_WRITER_STATE_FINISHED || writer->state == STREAM_WRITER_STATE_ERROR) return -1;
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
        /* Stable input lets a linked-block codec keep referencing the fed
         * buffer as its dictionary window instead of copying it aside. The
         * promise only requires the buffer to survive until the next feed, so
         * it may be given exactly when a full-block feed follows within this
         * call: in_buf is not rewritten before the loop below runs, and the
         * loop's final iteration always passes false so no codec reference to
         * caller memory outlives this call. */
        if (writer->in_buf_len == writer->in_buf_size &&
            streamWriterDrainInput(writer, remaining >= writer->in_buf_size,
                                   COMPRESS_FLUSH_CONTINUE) != 0)
            return -1;
    }

    while (remaining >= writer->in_buf_size) {
        bool input_stable = remaining - writer->in_buf_size >= writer->in_buf_size;
        if (streamWriterFeedAndWrite(writer, src, writer->in_buf_size,
                                     input_stable, COMPRESS_FLUSH_CONTINUE) != 0)
            return -1;
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
    if (writer->state == STREAM_WRITER_STATE_ERROR) return -1;
    /* Flush after finish is a no-op: frame is already closed. */
    if (writer->state == STREAM_WRITER_STATE_FINISHED) return 0;

    if (writer->state == STREAM_WRITER_STATE_INITIAL) return 0;
    return streamWriterDrainInput(writer, false, COMPRESS_FLUSH_SYNC);
}

int streamWriterFinish(streamWriter *writer) {
    if (writer->state == STREAM_WRITER_STATE_ERROR) return -1;
    if (writer->state == STREAM_WRITER_STATE_FINISHED) return 0;

    /* Even an empty stream produces a valid envelope + empty frame so the
     * loader sees a well-formed file. */
    if (streamWriterEnsureEnvelope(writer) != 0) return -1;
    if (streamWriterDrainInput(writer, false, COMPRESS_FLUSH_END) != 0) return -1;
    writer->state = STREAM_WRITER_STATE_FINISHED;
    return 0;
}

void streamWriterFree(streamWriter *writer) {
    streamCompressorFree(&writer->compressor);
    zfree(writer->scratch);
    writer->scratch = NULL;
    writer->in_buf = NULL;
    writer->out_buf = NULL;
}

/* ===== Streaming Reader ===== */

static void streamReaderSetError(streamReader *reader, streamReaderErrorKind error_kind) {
    if (reader->error_kind == STREAM_READER_ERROR_NONE) reader->error_kind = error_kind;
}

int streamReaderInit(streamReader *reader, const streamReaderConfig *cfg, streamReaderReadFn read_cb, void *read_ctx, compressionAlgo *detected_algo) {
    memset(reader, 0, sizeof(*reader));
    reader->read_cb = read_cb;
    reader->read_ctx = read_ctx;
    reader->buffer_size = cfg->buffer_size < STREAM_READER_BUFFER_SIZE_MIN
                              ? STREAM_READER_BUFFER_SIZE_MIN
                              : cfg->buffer_size;
    compressionAlgo algo = ALGO_NONE;
    while (true) {
        size_t need = reader->probe.header_len < VCS_MAGIC_SIZE
                          ? VCS_MAGIC_SIZE - reader->probe.header_len
                          : VCS_ENVELOPE_SIZE - reader->probe.header_len;
        ssize_t got = reader->read_cb(reader->read_ctx,
                                      reader->probe.header + reader->probe.header_len,
                                      need);

        if (got < 0 || (size_t)got > need) {
            streamReaderSetError(reader, STREAM_READER_ERROR_IO);
            return -1;
        }
        reader->probe.header_len += (size_t)got;

        if (reader->probe.header_len >= VCS_MAGIC_SIZE &&
            !vcsHasMagicPrefix(reader->probe.header, VCS_MAGIC_SIZE)) {
            if (!cfg->allow_passthrough) {
                streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
                return -1;
            }
            reader->state = STREAM_READER_STATE_PASSTHROUGH;
            break;
        }

        if (reader->probe.header_len == VCS_ENVELOPE_SIZE) {
            if (readVcsEnvelope(reader->probe.header, &algo) != 0) {
                streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
                return -1;
            }
            if (streamDecompressorInit(&reader->decompressor, algo,
                                       cfg->skip_codec_checksum_validation) != 0) {
                streamReaderSetError(reader, STREAM_READER_ERROR_IO);
                return -1;
            }
            size_t input_slack = reader->decompressor.codec->decode_input_slack;
            if (reader->buffer_size > SIZE_MAX - input_slack ||
                reader->buffer_size + input_slack > SIZE_MAX - reader->buffer_size) {
                streamReaderSetError(reader, STREAM_READER_ERROR_IO);
                return -1;
            }
            reader->compressed_buf_size = reader->buffer_size + input_slack;
            reader->scratch = zmalloc(reader->compressed_buf_size + reader->buffer_size);
            reader->compressed_buf = reader->scratch;
            reader->decompressed_buf = reader->scratch + reader->compressed_buf_size;
            reader->state = STREAM_READER_STATE_COMPRESSED;
            break;
        }

        if (got > 0) continue;

        /* EOF mid-magic looks like a truncated VCS, not passthrough. */
        if ((reader->probe.header_len > 0 &&
             vcsHasMagicPrefix(reader->probe.header, reader->probe.header_len)) ||
            !cfg->allow_passthrough) {
            streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
            return -1;
        }
        reader->state = STREAM_READER_STATE_PASSTHROUGH;
        break;
    }

    if (detected_algo) *detected_algo = algo;
    return 0;
}

/* Replay any probe-buffered bytes before reading from the wrapped source. */
static ssize_t streamReaderReadPassthrough(streamReader *reader, uint8_t *dst, size_t len) {
    size_t total = 0;
    size_t prefix_avail = reader->probe.header_len - reader->probe_replay_pos;
    if (prefix_avail > 0) {
        size_t from_prefix = prefix_avail < len ? prefix_avail : len;
        memcpy(dst, reader->probe.header + reader->probe_replay_pos, from_prefix);
        reader->probe_replay_pos += from_prefix;
        dst += from_prefix;
        len -= from_prefix;
        total += from_prefix;
    }
    if (len == 0) return (ssize_t)total;

    ssize_t got = reader->read_cb(reader->read_ctx, dst, len);
    if (got < 0 || (size_t)got > len) {
        streamReaderSetError(reader, STREAM_READER_ERROR_IO);
        return total > 0 ? (ssize_t)total : -1;
    }
    return (ssize_t)(total + (size_t)got);
}

/* Decode directly into dst. Input is refilled only after the previous window
 * is consumed, so no input compaction is needed. Respecting the codec hint
 * also prevents reading beyond the frame boundary. */
static ssize_t streamReaderDecode(streamReader *reader, uint8_t *dst, size_t capacity) {
    size_t written = 0;

    while (written < capacity && !reader->decompressor.frame_done) {
        if (reader->compressed_buf_len == 0) {
            size_t read_size = reader->compressed_buf_size;
            size_t input_hint = reader->decompressor.input_hint;
            if (input_hint > 0 && read_size > input_hint) read_size = input_hint;
            if (read_size > (size_t)SSIZE_MAX) read_size = (size_t)SSIZE_MAX;

            ssize_t got = reader->read_cb(reader->read_ctx, reader->compressed_buf, read_size);
            if (got < 0 || (size_t)got > read_size) {
                streamReaderSetError(reader, STREAM_READER_ERROR_IO);
                return written > 0 ? (ssize_t)written : -1;
            }
            if (got == 0) {
                streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
                return written > 0 ? (ssize_t)written : -1;
            }
            reader->compressed_buf_pos = 0;
            reader->compressed_buf_len = (size_t)got;
        }

        size_t consumed = 0;
        size_t feed_len = reader->compressed_buf_len;
        size_t input_hint = reader->decompressor.input_hint;
        if (input_hint > 0 && feed_len > input_hint) feed_len = input_hint;

        ssize_t produced = streamDecompressorFeed(
            &reader->decompressor,
            dst + written, capacity - written,
            reader->compressed_buf + reader->compressed_buf_pos,
            feed_len, &consumed);
        if (produced < 0 || consumed > feed_len ||
            (size_t)produced > capacity - written) {
            streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
            return written > 0 ? (ssize_t)written : -1;
        }

        reader->compressed_buf_pos += consumed;
        reader->compressed_buf_len -= consumed;
        written += (size_t)produced;
        if (reader->compressed_buf_len == 0) reader->compressed_buf_pos = 0;

        if (!reader->decompressor.frame_done && consumed == 0 && produced == 0) {
            streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
            return written > 0 ? (ssize_t)written : -1;
        }
    }
    return (ssize_t)written;
}

/* Preserve a decoded window for the parser's small reads. Any error found
 * after partial output remains sticky but the produced bytes stay readable. */
static int streamReaderFillDecompressedBuf(streamReader *reader) {
    reader->decompressed_buf_pos = 0;
    reader->decompressed_buf_len = 0;

    ssize_t produced = streamReaderDecode(reader, reader->decompressed_buf,
                                          reader->buffer_size);
    if (produced > 0) reader->decompressed_buf_len = (size_t)produced;
    return reader->error_kind == STREAM_READER_ERROR_NONE ? 0 : -1;
}

ssize_t streamReaderRead(streamReader *reader, void *buf, size_t len) {
    /* Filling decodes eagerly, so an error may be latched while decoded bytes
     * the parser has not requested yet sit in the buffer. Those bytes stay
     * readable: a parser can consume a complete logical payload from a frame
     * whose trailer is truncated, with the sticky error reported by the first
     * read past the buffered output, or by finish. */
    if (reader->error_kind != STREAM_READER_ERROR_NONE &&
        reader->decompressed_buf_pos >= reader->decompressed_buf_len)
        return -1;
    if (reader->state == STREAM_READER_STATE_FINISHED) return 0;
    if (len == 0) return 0;
    if (len > (size_t)SSIZE_MAX) return -1;

    if (reader->state == STREAM_READER_STATE_PASSTHROUGH) {
        return streamReaderReadPassthrough(reader, (uint8_t *)buf, len);
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    size_t total = 0;
    while (remaining > 0) {
        size_t available = reader->decompressed_buf_len - reader->decompressed_buf_pos;
        if (available > 0) {
            size_t to_copy = available < remaining ? available : remaining;
            memcpy(dst, reader->decompressed_buf + reader->decompressed_buf_pos, to_copy);
            reader->decompressed_buf_pos += to_copy;
            dst += to_copy;
            remaining -= to_copy;
            total += to_copy;
            continue;
        }

        if (reader->error_kind != STREAM_READER_ERROR_NONE)
            return total > 0 ? (ssize_t)total : -1;
        if (reader->decompressor.frame_done) break;

        if (remaining >= reader->buffer_size) {
            ssize_t produced = streamReaderDecode(reader, dst, remaining);
            if (produced < 0) return total > 0 ? (ssize_t)total : -1;
            if (produced == 0) break;
            dst += produced;
            remaining -= (size_t)produced;
            total += (size_t)produced;
            if (reader->error_kind != STREAM_READER_ERROR_NONE) break;
        } else {
            int fill_result = streamReaderFillDecompressedBuf(reader);
            if (reader->decompressed_buf_len == 0) {
                if (fill_result != 0) return total > 0 ? (ssize_t)total : -1;
                break;
            }
        }
    }
    return (ssize_t)total;
}

int streamReaderFinish(streamReader *reader) {
    uint8_t buf[4096];

    if (reader->error_kind != STREAM_READER_ERROR_NONE) return -1;
    if (reader->state == STREAM_READER_STATE_FINISHED) return 0;
    if (reader->state == STREAM_READER_STATE_PASSTHROUGH) {
        reader->state = STREAM_READER_STATE_FINISHED;
        return 0;
    }
    if (reader->decompressed_buf_len > reader->decompressed_buf_pos) {
        streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
        return -1;
    }

    while (!reader->decompressor.frame_done) {
        ssize_t nread = streamReaderDecode(reader, buf, sizeof(buf));
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

    reader->state = STREAM_READER_STATE_FINISHED;
    return 0;
}

void streamReaderFree(streamReader *reader) {
    streamDecompressorFree(&reader->decompressor);
    zfree(reader->scratch);
    reader->scratch = NULL;
    reader->compressed_buf = NULL;
    reader->decompressed_buf = NULL;
}
