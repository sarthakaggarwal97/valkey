/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_REPL_H
#define COMPRESSION_REPL_H

/* Push-mode replication adapter for the shared streaming codec. The incoming
 * stream may be compressed or plaintext and is classified from its leading
 * bytes. */

#include "compression.h"
#include "compression_stream.h"
#include "sds.h"

/* Max raw replication-backlog bytes compressed per write dispatch cycle;
 * bounds per-batch latency and the staging buffer. */
#define REPL_COMPRESSION_BATCH_LIMIT (1024 * 1024)

/* ===== Primary-side per-replica compressor ===== */

typedef struct replCompressor {
    streamCompressor stream;
    sds out_buf;        /* Compressed bytes staged for the socket. */
    size_t out_buf_pos; /* Next unsent byte offset within out_buf. */
    size_t raw_bytes;   /* Raw backlog bytes represented by out_buf. */
} replCompressor;

/* Write and Flush return C_OK/C_ERR and append directly to out_buf. */
replCompressor *replCompressorCreate(compressionAlgo algo);
void replCompressorFree(replCompressor *rc);
int replCompressorWrite(replCompressor *rc, const void *buf, size_t len);
int replCompressorFlush(replCompressor *rc);
/* Clears the staging buffer for a new batch; reclaims only oversized payloads. */
void replCompressorResetBatch(replCompressor *rc);
/* Approximate heap usage for client-output-buffer accounting. */
size_t replCompressorMemUsage(const replCompressor *rc);

/* ===== Replica-side decompressor ===== */

typedef enum {
    REPL_DECODE_OK = 0,
    REPL_DECODE_ERR = -1,        /* IO/feed/decoder error: disconnect. */
    REPL_DECODE_FRAME_DONE = -2, /* Frame ended on a live link: protocol corruption. */
    REPL_DECODE_OVERFLOW = -3,   /* Decoded output exceeded the bomb-guard cap. */
} replDecodeResult;

typedef enum {
    REPL_DECODE_MODE_PROBE = 0,  /* Still classifying the stream. */
    REPL_DECODE_MODE_COMPRESSED, /* VCS envelope seen; decoder initialized. */
    REPL_DECODE_MODE_PASSTHROUGH /* Non-VCS stream; bytes forwarded as-is. */
} replDecodeMode;

/* Decoder for the single primary link. Accumulates the leading bytes (they may
 * span several reads), classifies the stream, then either feeds the codec or
 * forwards plaintext untouched. */
typedef struct replDecompressor {
    streamDecompressor stream; /* Valid once mode == COMPRESSED. */
    replDecodeMode mode;
    uint8_t envelope[VCS_ENVELOPE_SIZE]; /* Leading bytes gathered during PROBE. */
    size_t envelope_len;
    sds decode_buf; /* Most recent decoded bytes. */
} replDecompressor;

replDecompressor *replDecompressorCreate(void);
void replDecompressorFree(replDecompressor *rd);
/* Returns decoded bytes in decode_buf, or a negative replDecodeResult. */
ssize_t replDecompress(replDecompressor *rd, const void *src, size_t len, size_t output_max);

#endif /* COMPRESSION_REPL_H */
