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

bool compressionAlgoSupportsStreaming(compressionAlgo algo) {
    return compressionCodecByAlgo(algo) != NULL;
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

int streamCompressorInit(streamCompressor *compressor,
                         compressionAlgo algo,
                         int level,
                         bool codec_checksum) {
    memset(compressor, 0, sizeof(*compressor));
    compressor->codec = compressionCodecByAlgo(algo);
    compressor->level = level;
    compressor->codec_checksum = codec_checksum;

    if (!compressor->codec) return -1;
    if (compressor->codec->compressor_init(compressor) != 0) {
        compressor->codec->compressor_free(compressor);
        compressor->codec = NULL;
        return -1;
    }
    return 0;
}

void streamCompressorFree(streamCompressor *compressor) {
    if (compressor->codec) compressor->codec->compressor_free(compressor);
}

compressionAlgo streamCompressorAlgo(const streamCompressor *compressor) {
    return compressor && compressor->codec ? compressor->codec->algo : ALGO_NONE;
}

size_t streamCompressorChunkSize(const streamCompressor *compressor) {
    assert(compressor->codec != NULL);
    return compressor->codec->chunk_size;
}

int streamDecompressorInit(streamDecompressor *decompressor, compressionAlgo algo) {
    memset(decompressor, 0, sizeof(*decompressor));
    decompressor->codec = compressionCodecByAlgo(algo);

    if (!decompressor->codec) return -1;
    if (decompressor->codec->decompressor_init(decompressor) != 0) {
        decompressor->codec->decompressor_free(decompressor);
        decompressor->codec = NULL;
        return -1;
    }
    return 0;
}

void streamDecompressorFree(streamDecompressor *decompressor) {
    if (decompressor->codec) decompressor->codec->decompressor_free(decompressor);
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
                             bool input_stable,
                             compressFlushMode flush_mode) {
    if (compressor->errored) return -1;

    assert(compressor->codec != NULL);
    assert(!input_stable || flush_mode == FLUSH_CONTINUE);
    return compressor->codec->compressor_feed(compressor, output, output_capacity,
                                              input, input_len, input_stable, flush_mode);
}

ssize_t streamDecompressorFeed(streamDecompressor *decompressor,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed) {
    *input_consumed = 0;
    if (decompressor->errored) return -1;
    if (decompressor->frame_done) return 0;

    assert(decompressor->codec != NULL);
    return decompressor->codec->decompressor_feed(decompressor, output, output_capacity,
                                                  input, input_len, input_consumed);
}
