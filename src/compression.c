/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression.h"
#include "compression_lz4.h"
#include "serverassert.h"
#include <string.h>

static const compressionCodec *const stream_codecs[] = {
    &compressionLz4Codec,
};

const compressionCodec *compressionCodecByAlgo(compressionAlgo algo) {
    for (size_t i = 0; i < sizeof(stream_codecs) / sizeof(stream_codecs[0]); i++) {
        if (stream_codecs[i]->algo == algo) return stream_codecs[i];
    }
    return NULL;
}

const compressionCodec *compressionCodecByVcsId(uint8_t vcs_id) {
    for (size_t i = 0; i < sizeof(stream_codecs) / sizeof(stream_codecs[0]); i++) {
        if (stream_codecs[i]->vcs_id == vcs_id) return stream_codecs[i];
    }
    return NULL;
}

const char *compressionAlgoName(compressionAlgo algo) {
    switch (algo) {
    case ALGO_NONE:
        return "none";
    case ALGO_LZF:
        return "lzf";
    default:
        break;
    }

    const compressionCodec *codec = compressionCodecByAlgo(algo);
    return codec ? codec->name : "unknown";
}

/* ===== Compressor ===== */

int streamCompressorInit(streamCompressor *compressor,
                         compressionAlgo algo,
                         int level,
                         bool codec_checksum) {
    memset(compressor, 0, sizeof(*compressor));
    compressor->codec = compressionCodecByAlgo(algo);
    compressor->level = level;
    compressor->codec_checksum = codec_checksum;

    if (!compressor->codec) return -1;
    return compressor->codec->compressor_init(compressor);
}

size_t streamCompressorChunkSize(const streamCompressor *compressor) {
    assert(compressor->codec != NULL);
    return compressor->codec->chunk_size;
}

size_t streamCompressorOutputBound(const streamCompressor *compressor, size_t input_len) {
    assert(compressor->codec != NULL);
    return compressor->codec->compressor_output_bound(compressor, input_len);
}

ssize_t streamCompressorFeed(streamCompressor *compressor,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             compressFlushMode flush_mode) {
    assert(compressor->codec != NULL);
    return compressor->codec->compressor_feed(compressor, output, output_capacity,
                                              input, input_len, flush_mode);
}

void streamCompressorFree(streamCompressor *compressor) {
    if (compressor->codec) compressor->codec->compressor_free(compressor);
}

/* ===== Decompressor ===== */

int streamDecompressorInit(streamDecompressor *decompressor,
                           compressionAlgo algo,
                           bool skip_codec_checksum_validation) {
    memset(decompressor, 0, sizeof(*decompressor));
    decompressor->codec = compressionCodecByAlgo(algo);
    decompressor->skip_codec_checksum_validation = skip_codec_checksum_validation;

    if (!decompressor->codec) return -1;
    return decompressor->codec->decompressor_init(decompressor);
}

ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed) {
    *input_consumed = 0;
    if (decompressor->frame_done) return 0;

    assert(decompressor->codec != NULL);
    return decompressor->codec->decompressor_feed(decompressor, output, output_capacity,
                                                  input, input_len, input_consumed);
}

void streamDecompressorFree(streamDecompressor *decompressor) {
    if (decompressor->codec) decompressor->codec->decompressor_free(decompressor);
}
