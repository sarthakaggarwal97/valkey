/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression.h"
#include "compression_lz4.h"
#include "serverassert.h"
#include <string.h>

typedef struct {
    int (*compressor_init)(streamCompressor *sc);
    void (*compressor_free)(streamCompressor *sc);
    int (*decompressor_init)(streamDecompressor *sd);
    void (*decompressor_free)(streamDecompressor *sd);
    size_t (*compress_output_bound)(size_t input_len);
    ssize_t (*compress_feed)(streamCompressor *sc,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             compressFlushMode flush_mode);
    ssize_t (*decompress_feed)(streamDecompressor *sd,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);
} compressionCodec;

static const compressionCodec compressionLz4Codec = {
    .compressor_init = compressionLz4CompressorInit,
    .compressor_free = compressionLz4CompressorFree,
    .decompressor_init = compressionLz4DecompressorInit,
    .decompressor_free = compressionLz4DecompressorFree,
    .compress_output_bound = compressionLz4OutputBound,
    .compress_feed = compressionLz4CompressFeed,
    .decompress_feed = compressionLz4DecompressFeed,
};

static const compressionCodec *compressionCodecForAlgo(compressionAlgo algo) {
    switch (algo) {
    case ALGO_LZ4:
        return &compressionLz4Codec;
    default:
        return NULL;
    }
}

bool compressionAlgoSupportsStreaming(compressionAlgo algo) {
    return compressionCodecForAlgo(algo) != NULL;
}

const char *compressionAlgoName(compressionAlgo algo) {
    switch (algo) {
    case ALGO_NONE:
        return "none";
    case ALGO_LZF:
        return "lzf";
    case ALGO_LZ4:
        return "lz4";
    default:
        return "unknown";
    }
}

int streamCompressorInit(streamCompressor *sc, compressionAlgo algo, int level) {
    memset(sc, 0, sizeof(*sc));

    const compressionCodec *impl = compressionCodecForAlgo(algo);
    if (!impl) return -1;

    sc->algo = algo;
    sc->level = level;

    if (impl->compressor_init(sc) != 0) {
        impl->compressor_free(sc);
        return -1;
    }
    return 0;
}

void streamCompressorFree(streamCompressor *sc) {
    const compressionCodec *impl = compressionCodecForAlgo(sc->algo);
    assert(impl != NULL);
    impl->compressor_free(sc);
}

int streamDecompressorInit(streamDecompressor *sd, compressionAlgo algo) {
    memset(sd, 0, sizeof(*sd));

    const compressionCodec *impl = compressionCodecForAlgo(algo);
    if (!impl) return -1;

    sd->algo = algo;

    if (impl->decompressor_init(sd) != 0) {
        impl->decompressor_free(sd);
        return -1;
    }
    return 0;
}

void streamDecompressorFree(streamDecompressor *sd) {
    const compressionCodec *impl = compressionCodecForAlgo(sd->algo);
    assert(impl != NULL);
    impl->decompressor_free(sd);
}

size_t streamCompressOutputBound(streamCompressor *sc, size_t input_len) {
    const compressionCodec *impl = compressionCodecForAlgo(sc->algo);
    assert(impl != NULL);
    return impl->compress_output_bound(input_len);
}

ssize_t streamCompressFeed(streamCompressor *sc,
                           uint8_t *output,
                           size_t output_capacity,
                           const uint8_t *input,
                           size_t input_len,
                           compressFlushMode flush_mode) {
    if (sc->errored) return -1;

    const compressionCodec *impl = compressionCodecForAlgo(sc->algo);
    assert(impl != NULL);
    return impl->compress_feed(sc, output, output_capacity, input, input_len, flush_mode);
}

ssize_t streamDecompressFeed(streamDecompressor *sd,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             size_t *input_consumed) {
    *input_consumed = 0;
    if (sd->errored) return -1;
    if (sd->frame_done) return 0;

    const compressionCodec *impl = compressionCodecForAlgo(sd->algo);
    assert(impl != NULL);
    return impl->decompress_feed(sd, output, output_capacity, input, input_len, input_consumed);
}
