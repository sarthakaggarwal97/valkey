/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COMPRESSION_REPL_H
#define COMPRESSION_REPL_H

/* Replication compression adapter.
 *
 * This is the replication-stream counterpart to compression_rio.c: both are
 * thin adapters on top of compression_stream. compression_rio wraps a blocking
 * rio (used for RDB save/load); this file wraps the non-blocking replication
 * transport.
 *
 * Why a separate adapter: the RDB path uses the pull-mode streamReader because
 * a file rio blocks until the requested bytes are available. The replica reads
 * its primary link from a non-blocking socket inside the event loop, where a
 * read returns whatever bytes happen to be ready and a blocking pull callback
 * cannot work. The replica advertises the compression capability, but the
 * primary only compresses if its own config is enabled, so the incoming stream
 * may be compressed (VCS envelope) or plaintext. This adapter classifies the
 * stream from its leading bytes: on a VCS envelope it parses the header and
 * feeds the codec (streamDecompressorFeed) directly (the decoder retains
 * partial-frame state internally); otherwise it forwards the bytes untouched. */

#include "compression.h"
#include "compression_stream.h"
#include "sds.h"

#ifndef __cplusplus
#include <stdatomic.h>
#endif

/* Max raw replication-backlog bytes compressed per write dispatch cycle, to
 * bound worst-case per-batch latency and keep the staging buffer predictable. */
#define REPL_COMPRESSION_BATCH_LIMIT (1024 * 1024) /* 1 MB per dispatch cycle */

/* ===== Primary-side per-replica compressor ===== */

/* Owns the streamWriter (VCS/LZ4 frame encoder) and the staging buffer of
 * compressed bytes pending socket write for one replica connection. The
 * networking layer still drives which replication-backlog bytes get compressed
 * and performs the socket writes; this adapter owns the codec state and the
 * staging buffer so that ownership is not scattered through the client struct. */
typedef struct replCompressor {
    streamWriter writer;       /* VCS/LZ4 frame encoder (STREAM_KIND_REPL). */
    sds out_buf;               /* Compressed bytes staged for the socket. */
    size_t out_buf_pos;        /* Next unsent byte offset within out_buf. */
    size_t raw_bytes;          /* Raw backlog bytes represented by out_buf. */
    _Atomic(size_t) mem_usage; /* Cached for lock-free main-thread accounting. */
} replCompressor;

/* Create a per-replica compressor. Returns NULL on error. */
replCompressor *replCompressorCreate(compressionAlgo algo, int level);

/* Destroy a compressor created by replCompressorCreate. NULL-safe. */
void replCompressorDestroy(replCompressor *rc);

/* Feed raw replication bytes into the encoder, appending compressed output to
 * rc->out_buf. Returns 0 on success or -1 on error. */
int replCompressorWrite(replCompressor *rc, const void *buf, size_t len);

/* Flush the encoder so any buffered input is materialized into out_buf.
 * Returns 0 on success, -1 on error. */
int replCompressorFlush(replCompressor *rc);

/* Clear the staging buffer for a new batch and release oversized scratch so a
 * long-lived replica does not permanently retain peak allocation. */
void replCompressorResetBatch(replCompressor *rc);

/* Approximate adapter heap usage (object + writer scratch + staging buffer)
 * for client-output-buffer accounting. Codec-owned allocations are omitted. */
size_t replCompressorMemUsage(const replCompressor *rc);

/* The compression algorithm this compressor was initialized with (for INFO and
 * error logging). Returns ALGO_NONE if rc is NULL. */
compressionAlgo replCompressorAlgo(const replCompressor *rc);

/* ===== Replica-side decompressor ===== */

/* Outcome of replDecompressorDecode. */
typedef enum {
    REPL_DECODE_OK = 0,          /* Decoded (possibly 0) bytes; need more input next tick. */
    REPL_DECODE_PASSTHROUGH = 1, /* Stream is plaintext; input is already the output. */
    REPL_DECODE_ERR = -1,        /* IO/feed/decoder error: disconnect. */
    REPL_DECODE_FRAME_DONE = -2, /* Frame ended on a live link: protocol corruption. */
    REPL_DECODE_OVERFLOW = -3,   /* Decoded output exceeded the bomb-guard cap. */
} replDecodeResult;

/* Owns the VCS/LZ4 decoder for the single primary link. The replica negotiated
 * the compression capability, but the primary only actually compresses if its
 * own config is enabled, so the stream may be compressed or plaintext. This
 * adapter accumulates the leading bytes (they may span several non-blocking
 * reads), classifies the stream, and either parses the VCS envelope and feeds
 * the codec or forwards plaintext untouched. */
typedef struct replDecompressor {
    streamProbe probe;
    streamDecompressor decompressor;
    sds decode_buf; /* Scratch buffer holding the most recent decoded bytes. */
    bool decompressor_initialized;
} replDecompressor;

/* Create a replica-side decompressor. Returns NULL on error. */
replDecompressor *replDecompressorCreate(void);

/* Destroy a decompressor created by replDecompressorCreate. NULL-safe. */
void replDecompressorDestroy(replDecompressor *rd);

/* Feed `len` raw transport bytes and drain all currently-available decoded
 * output into rd->decode_buf (which is cleared first). On REPL_DECODE_OK,
 * *out_len is set to the decoded byte count (0 means "buffered a partial frame,
 * resume next tick"). REPL_DECODE_PASSTHROUGH means src itself is the output,
 * with *out_len set to len. output_max bounds either output path as a
 * decompression bomb guard. */
replDecodeResult replDecompressorDecode(replDecompressor *rd,
                                        const void *src,
                                        size_t len,
                                        size_t output_max,
                                        size_t *out_len);

/* Access the decode scratch buffer (valid bytes = sdslen) after a decode. */
sds replDecompressorBuf(replDecompressor *rd);

/* Transfers the decoded buffer to the caller and installs replacement as the
 * cleared scratch buffer, avoiding a decoded-byte copy. */
sds replDecompressorTakeBuf(replDecompressor *rd, sds replacement);

#endif /* COMPRESSION_REPL_H */
