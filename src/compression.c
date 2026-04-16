/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Streaming compression/decompression dispatch.
 * Codec-specific implementations live in implementation files. */

#include "compression.h"
#include "compression_lz4.h"
#include <string.h>

typedef struct {
    bool supports_level;
    int (*compressor_init)(streamCompressor *sc);
    void (*compressor_destroy)(streamCompressor *sc);
    int (*decompressor_init)(streamDecompressor *sd);
    void (*decompressor_destroy)(streamDecompressor *sd);
    size_t (*compress_output_bound)(size_t inputLen);
    ssize_t (*compress_feed)(streamCompressor *sc,
                             uint8_t *output,
                             size_t outputCapacity,
                             const uint8_t *input,
                             size_t inputLen,
                             compressFlushMode flushMode);
    ssize_t (*decompress_feed)(streamDecompressor *sd,
                               uint8_t *output,
                               size_t outputCapacity,
                               const uint8_t *input,
                               size_t inputLen,
                               size_t *inputConsumed);
} compressionCodecImpl;

static const compressionCodecImpl compressionLz4CodecImpl = {
    .supports_level = true,
    .compressor_init = compressionLz4CompressorInit,
    .compressor_destroy = compressionLz4CompressorDestroy,
    .decompressor_init = compressionLz4DecompressorInit,
    .decompressor_destroy = compressionLz4DecompressorDestroy,
    .compress_output_bound = compressionLz4OutputBound,
    .compress_feed = compressionLz4CompressFeed,
    .decompress_feed = compressionLz4DecompressFeed,
};

static const char *const compressionAlgoNameByAlgo[] = {
    [ALGO_NONE] = "none",
    [ALGO_LZF] = "lzf",
    [ALGO_LZ4] = "lz4",
};

static const compressionCodecImpl *const compressionCodecImplByAlgo[] = {
    [ALGO_LZ4] = &compressionLz4CodecImpl,
};

static const char *compressionAlgoNameForAlgo(compressionAlgo algo) {
    unsigned int algoIndex = (unsigned int)algo;

    if (algoIndex >= sizeof(compressionAlgoNameByAlgo) / sizeof(compressionAlgoNameByAlgo[0])) {
        return NULL;
    }
    return compressionAlgoNameByAlgo[algoIndex];
}

static const compressionCodecImpl *compressionCodecImplForAlgo(compressionAlgo algo) {
    unsigned int algoIndex = (unsigned int)algo;

    if (algoIndex >= sizeof(compressionCodecImplByAlgo) / sizeof(compressionCodecImplByAlgo[0])) {
        return NULL;
    }
    return compressionCodecImplByAlgo[algoIndex];
}

bool compressionAlgoSupportsStreaming(compressionAlgo algo) {
    return compressionCodecImplForAlgo(algo) != NULL;
}

bool compressionAlgoSupportsLevel(compressionAlgo algo) {
    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(algo);
    return codecImpl && codecImpl->supports_level;
}

const char *compressionAlgoName(compressionAlgo algo) {
    const char *algoName = compressionAlgoNameForAlgo(algo);

    return algoName ? algoName : "unknown";
}

int streamCompressorInit(streamCompressor *sc, compressionAlgo algo, int level) {
    if (!sc) return -1;
    memset(sc, 0, sizeof(*sc));

    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(algo);
    if (!codecImpl || !codecImpl->compressor_init) return -1;

    sc->algo = algo;
    sc->level = level;

    if (codecImpl->compressor_init(sc) != 0) {
        streamCompressorDestroy(sc);
        return -1;
    }
    return 0;
}

void streamCompressorDestroy(streamCompressor *sc) {
    if (!sc) return;

    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(sc->algo);
    if (codecImpl && codecImpl->compressor_destroy) {
        codecImpl->compressor_destroy(sc);
    }
    memset(sc, 0, sizeof(*sc));
}

int streamDecompressorInit(streamDecompressor *sd, compressionAlgo algo) {
    if (!sd) return -1;
    memset(sd, 0, sizeof(*sd));

    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(algo);
    if (!codecImpl || !codecImpl->decompressor_init) return -1;

    sd->algo = algo;

    if (codecImpl->decompressor_init(sd) != 0) {
        streamDecompressorDestroy(sd);
        return -1;
    }
    return 0;
}

void streamDecompressorDestroy(streamDecompressor *sd) {
    if (!sd) return;

    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(sd->algo);
    if (codecImpl && codecImpl->decompressor_destroy) {
        codecImpl->decompressor_destroy(sd);
    }
    memset(sd, 0, sizeof(*sd));
}

size_t streamCompressOutputBound(const streamCompressor *sc, size_t inputLen) {
    if (!sc) return 0;
    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(sc->algo);
    if (!codecImpl || !codecImpl->compress_output_bound) return 0;
    return codecImpl->compress_output_bound(inputLen);
}

ssize_t streamCompressFeed(streamCompressor *sc,
                           uint8_t *output,
                           size_t outputCapacity,
                           const uint8_t *input,
                           size_t inputLen,
                           compressFlushMode flushMode) {
    if (!sc || !output) return -1;
    if (sc->errored) return -1;

    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(sc->algo);
    if (!codecImpl || !codecImpl->compress_feed) return -1;

    return codecImpl->compress_feed(sc, output, outputCapacity,
                                    input, inputLen, flushMode);
}

ssize_t streamDecompressFeed(streamDecompressor *sd,
                             uint8_t *output,
                             size_t outputCapacity,
                             const uint8_t *input,
                             size_t inputLen,
                             size_t *inputConsumed) {
    if (!sd || !inputConsumed) return -1;
    if (sd->errored) return -1;
    *inputConsumed = 0;
    if (sd->frame_done) return 0;
    /* Zero output capacity is a caller bug — returning 0 with no progress
     * would cause streaming loops to spin forever. */
    if (!output || outputCapacity == 0) {
        sd->errored = true;
        return -1;
    }

    const compressionCodecImpl *codecImpl = compressionCodecImplForAlgo(sd->algo);
    if (!codecImpl || !codecImpl->decompress_feed) {
        sd->errored = true;
        return -1;
    }

    return codecImpl->decompress_feed(sd, output, outputCapacity,
                                      input, inputLen, inputConsumed);
}
