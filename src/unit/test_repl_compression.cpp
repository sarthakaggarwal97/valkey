/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Unit tests for replication compression configuration and capability constants. */

#include "generated_wrappers.hpp"

extern "C" {
#include "compression.h"
#include "compression_repl.h"
#include "server.h"
}

/* zmalloc.h defines helper macros that collide with libstdc++ internals. */
#ifdef __xstr
#undef __xstr
#endif
#ifdef __str
#undef __str
#endif

TEST(replCompression, capaCompressionBitNoConflict) {
    EXPECT_EQ(REPLICA_CAPA_COMPRESSION, (1 << 4));
    EXPECT_EQ(REPLICA_CAPA_COMPRESSION &
                  (REPLICA_CAPA_EOF | REPLICA_CAPA_PSYNC2 |
                   REPLICA_CAPA_DUAL_CHANNEL | REPLICA_CAPA_SKIP_RDB_CHECKSUM),
              0);
}

TEST(replCompression, resetBatchRetainsAllocationForIncompressibleBatch) {
    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    /* ~1 MiB of incompressible (xorshift) bytes: compressed payload ~= input,
     * exercising the greedy-SDS over-allocation path. Stays at/under the 1 MiB
     * batch cap so the payload is within the retention bound. */
    const size_t n = 1000000;
    unsigned char *buf = (unsigned char *)zmalloc(n);
    uint32_t x = 0x9e3779b9u;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (unsigned char)(x >> 24);
    }
    ASSERT_EQ(replCompressorWrite(rc, buf, n), C_OK);
    ASSERT_EQ(replCompressorFlush(rc), C_OK);
    size_t alloc_before = sdsalloc(rc->out_buf);
    size_t len_before = sdslen(rc->out_buf);
    EXPECT_GT(len_before, (size_t)(900 * 1024));    /* ratio ~1: payload is large */
    EXPECT_GT(alloc_before, (size_t)(1024 * 1024)); /* greedy SDS over-allocates past 1 MiB */
    replCompressorResetBatch(rc);
    EXPECT_EQ(sdslen(rc->out_buf), (size_t)0);      /* cleared */
    EXPECT_EQ(sdsalloc(rc->out_buf), alloc_before); /* retained, not freed to empty */
    zfree(buf);
    replCompressorFree(rc);
}

/* ===== Decoder corruption and edge cases (crafted bytes) ===== */

/* Fill buf with incompressible xorshift bytes. */
static void fillIncompressible(unsigned char *buf, size_t n, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (unsigned char)(x >> 24);
    }
}

/* LZ4 frame FLG byte (follows the 4-byte frame magic): bit 2 = content
 * checksum present, bit 4 = block checksums present. Frozen wire format. */
#define LZ4F_FLG_CONTENT_CHECKSUM 0x04
#define LZ4F_FLG_BLOCK_CHECKSUM 0x10

static int finishReplFrame(replCompressor *rc) {
    size_t bound = streamCompressorOutputBound(&rc->stream, 0);
    rc->out_buf = sdsMakeRoomFor(rc->out_buf, bound);
    ssize_t written = streamCompressorFeed(&rc->stream,
                                           (uint8_t *)rc->out_buf + sdslen(rc->out_buf),
                                           sdsavail(rc->out_buf), NULL, 0, COMPRESS_FLUSH_END);
    if (written < 0) return C_ERR;
    sdsIncrLen(rc->out_buf, (size_t)written);
    return C_OK;
}

TEST(replCompression, replFrameOmitsContentChecksum) {
    /* A repl frame never ends, so its content checksum would be computed on
     * every byte but never emitted or validated. It must be off in the frame
     * header while block checksums stay on. */
    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    const char payload[] = "content-checksum-off-for-repl";
    ASSERT_EQ(replCompressorWrite(rc, payload, sizeof(payload)), C_OK);
    ASSERT_EQ(replCompressorFlush(rc), C_OK);
    const unsigned char *stream = (const unsigned char *)rc->out_buf;
    ASSERT_GE(sdslen(rc->out_buf), (size_t)(VCS_ENVELOPE_SIZE + 5));
    /* LZ4 frame magic 0x184D2204 (little-endian) right after the envelope. */
    EXPECT_EQ(stream[VCS_ENVELOPE_SIZE + 0], 0x04);
    EXPECT_EQ(stream[VCS_ENVELOPE_SIZE + 1], 0x22);
    EXPECT_EQ(stream[VCS_ENVELOPE_SIZE + 2], 0x4D);
    EXPECT_EQ(stream[VCS_ENVELOPE_SIZE + 3], 0x18);
    unsigned char flg = stream[VCS_ENVELOPE_SIZE + 4];
    EXPECT_EQ(flg & LZ4F_FLG_CONTENT_CHECKSUM, 0x00);
    EXPECT_EQ(flg & LZ4F_FLG_BLOCK_CHECKSUM, LZ4F_FLG_BLOCK_CHECKSUM);

    /* Round-trip: the decoder learns checksum presence from the frame header,
     * so it needs no matching configuration. */
    replDecompressor *rd = replDecompressorCreate();
    ssize_t out_len = replDecompress(rd, rc->out_buf, sdslen(rc->out_buf), 1024 * 1024);
    ASSERT_EQ(out_len, (ssize_t)sizeof(payload));
    EXPECT_EQ(memcmp(rd->decode_buf, payload, sizeof(payload)), 0);

    replDecompressorFree(rd);
    replCompressorFree(rc);
}

TEST(replCompression, rdbFrameKeepsContentChecksum) {
    /* RDB uses the codec default, where both checksums stay on. */
    streamCompressor stream;
    ASSERT_EQ(streamCompressorInit(&stream, ALGO_LZ4, 0, true), C_OK);
    size_t bound = streamCompressorOutputBound(&stream, 9);
    unsigned char *output = (unsigned char *)zmalloc(bound);
    ssize_t written = streamCompressorFeed(&stream, output, bound,
                                           (const uint8_t *)"rdb-bytes", 9,
                                           COMPRESS_FLUSH_CONTINUE);
    ASSERT_GE(written, 5);
    unsigned char flg = output[4];
    EXPECT_EQ(flg & LZ4F_FLG_CONTENT_CHECKSUM, LZ4F_FLG_CONTENT_CHECKSUM);
    EXPECT_EQ(flg & LZ4F_FLG_BLOCK_CHECKSUM, LZ4F_FLG_BLOCK_CHECKSUM);
    streamCompressorFree(&stream);
    zfree(output);
}

TEST(replCompression, decodeFrameDoneOnLiveLink) {
    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    const char payload[] = "frame-done-on-live-link";
    ASSERT_EQ(replCompressorWrite(rc, payload, sizeof(payload)), C_OK);
    /* Finish ends the frame; a live replication link must never see that. */
    ASSERT_EQ(finishReplFrame(rc), C_OK);

    replDecompressor *rd = replDecompressorCreate();
    EXPECT_EQ(replDecompress(rd, rc->out_buf, sdslen(rc->out_buf), 1024 * 1024),
              REPL_DECODE_FRAME_DONE);

    replDecompressorFree(rd);
    replCompressorFree(rc);
}

TEST(replCompression, decodeOverflowGuard) {
    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    /* Incompressible payload so decoded output far exceeds the cap. */
    const size_t n = 64 * 1024;
    unsigned char *buf = (unsigned char *)zmalloc(n);
    fillIncompressible(buf, n, 0x12345678u);
    ASSERT_EQ(replCompressorWrite(rc, buf, n), C_OK);
    ASSERT_EQ(replCompressorFlush(rc), C_OK); /* frame stays open */

    replDecompressor *rd = replDecompressorCreate();
    EXPECT_EQ(replDecompress(rd, rc->out_buf, sdslen(rc->out_buf), 1024),
              REPL_DECODE_OVERFLOW);

    replDecompressorFree(rd);
    zfree(buf);
    replCompressorFree(rc);
}

TEST(replCompression, decodeEnvelopeSplitAcrossFeeds) {
    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    const size_t n = 10 * 1024;
    unsigned char *payload = (unsigned char *)zmalloc(n);
    memset(payload, 'A', n);
    ASSERT_EQ(replCompressorWrite(rc, payload, n), C_OK);
    ASSERT_EQ(replCompressorFlush(rc), C_OK);
    const unsigned char *stream = (const unsigned char *)rc->out_buf;
    const size_t stream_len = sdslen(rc->out_buf);
    ASSERT_GT(stream_len, (size_t)VCS_ENVELOPE_SIZE);

    replDecompressor *rd = replDecompressorCreate();
    unsigned char *decoded = (unsigned char *)zmalloc(n);
    size_t decoded_len = 0;
    ssize_t out_len;

    /* Byte 0 alone: probe cannot classify yet, nothing decodes. */
    out_len = replDecompress(rd, stream, 1, 1024 * 1024);
    ASSERT_EQ(out_len, 0);

    /* Bytes 1-2: magic complete, envelope still short. */
    out_len = replDecompress(rd, stream + 1, 2, 1024 * 1024);
    ASSERT_EQ(out_len, 0);

    /* Remainder: envelope parses and the payload decodes. */
    out_len = replDecompress(rd, stream + 3, stream_len - 3, 1024 * 1024);
    ASSERT_GE(out_len, 0);
    ASSERT_LE(decoded_len + (size_t)out_len, n);
    memcpy(decoded + decoded_len, rd->decode_buf, (size_t)out_len);
    decoded_len += (size_t)out_len;

    ASSERT_EQ(decoded_len, n);
    EXPECT_EQ(memcmp(decoded, payload, n), 0);

    zfree(decoded);
    zfree(payload);
    replDecompressorFree(rd);
    replCompressorFree(rc);
}

TEST(replCompression, decodePassthroughReplaysPrefix) {
    replDecompressor *rd = replDecompressorCreate();
    /* "V" alone could still open the VCS magic: buffered, nothing emitted. */
    ASSERT_EQ(replDecompress(rd, "V", 1, 1024), 0);
    EXPECT_NE(rd->mode, REPL_DECODE_MODE_PASSTHROUGH);
    /* "X" rules out the magic: the buffered "V" replays ahead of the new bytes. */
    ASSERT_EQ(replDecompress(rd, "XYZ", 3, 1024), 4);
    EXPECT_EQ(rd->mode, REPL_DECODE_MODE_PASSTHROUGH);
    ASSERT_EQ(sdslen(rd->decode_buf), (size_t)4);
    EXPECT_EQ(memcmp(rd->decode_buf, "VXYZ", 4), 0);
    replDecompressorFree(rd);
}

TEST(replCompression, decodeRejectsBadCodec) {
    replDecompressor *rd = replDecompressorCreate();
    /* Valid magic/version/kind, unknown codec id 0xFF. */
    unsigned char stream[VCS_ENVELOPE_SIZE + 4] = {'V', 'C', 'S', VCS_VERSION, 0xFF, 0x00,
                                                   VCS_STREAM_REPL, 0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(replDecompress(rd, stream, sizeof(stream), 1024), REPL_DECODE_ERR);
    replDecompressorFree(rd);
}

TEST(replCompression, decodeRejectsNonzeroReserved) {
    replDecompressor *rd = replDecompressorCreate();
    /* Valid magic/version/codec/kind, nonzero reserved byte. */
    unsigned char stream[VCS_ENVELOPE_SIZE + 4] = {'V', 'C', 'S', VCS_VERSION, VCS_CODEC_LZ4, 0x01,
                                                   VCS_STREAM_REPL, 0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(replDecompress(rd, stream, sizeof(stream), 1024), REPL_DECODE_ERR);
    replDecompressorFree(rd);
}

TEST(replCompression, decodeErrOnCorruptPayload) {
    /* A real compressor emits the envelope on first write; reuse those bytes. */
    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    ASSERT_EQ(replCompressorWrite(rc, "seed", 4), C_OK);
    ASSERT_GE(sdslen(rc->out_buf), (size_t)VCS_ENVELOPE_SIZE);

    /* Valid envelope followed by garbage that LZ4F rejects. */
    unsigned char stream[VCS_ENVELOPE_SIZE + 64];
    memcpy(stream, rc->out_buf, VCS_ENVELOPE_SIZE);
    memset(stream + VCS_ENVELOPE_SIZE, 0xFF, 64);

    replDecompressor *rd = replDecompressorCreate();
    EXPECT_EQ(replDecompress(rd, stream, sizeof(stream), 1024 * 1024),
              REPL_DECODE_ERR);

    replDecompressorFree(rd);
    replCompressorFree(rc);
}

TEST(replCompression, decodeDrainsBufferedOutputWithoutMoreInput) {
    /* The writer emits 64KB LZ4 blocks while the decoder offers 16KB of room
     * per iteration, so LZ4F decodes a compressed block into its internal
     * buffer and can report the block's input consumed with output still
     * undelivered. Once input runs out the decoder must keep draining with
     * empty input; otherwise the tail is stranded inside the codec until
     * later transport bytes arrive (worst case a 10s replication PING). The
     * payload ends with a compressible run: a stored (incompressible) block
     * streams straight to the caller's buffer and would not strand. */
    const size_t incompressible = 36 * 1024;
    const size_t compressible = 64 * 1024;
    const size_t n = incompressible + compressible; /* ~100KB: multiple blocks */
    unsigned char *payload = (unsigned char *)zmalloc(n);
    fillIncompressible(payload, incompressible, 0xC0FFEE42u);
    memset(payload + incompressible, 'A', compressible);

    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    ASSERT_EQ(replCompressorWrite(rc, payload, n), C_OK);
    ASSERT_EQ(replCompressorFlush(rc), C_OK); /* frame stays open */

    /* All compressed bytes in ONE call: no later input can push out whatever
     * the codec buffered, so the decode itself must drain it. */
    replDecompressor *rd = replDecompressorCreate();
    ssize_t out_len = replDecompress(rd, rc->out_buf, sdslen(rc->out_buf), 4 * 1024 * 1024);
    ASSERT_EQ(out_len, (ssize_t)n);
    ASSERT_EQ(sdslen(rd->decode_buf), n);
    EXPECT_EQ(memcmp(rd->decode_buf, payload, n), 0);

    replDecompressorFree(rd);
    replCompressorFree(rc);
    zfree(payload);
}

TEST(replCompression, decodeCallOutputStaysUnderCapAtMaxRatio) {
    /* Feed paths hand the decoder at most PROTO_IOBUF_LEN (16KB) per call and
     * LZ4 expansion is bounded, so one call's output stays far under the
     * 16MB overflow cap even for maximally compressible input. */
    const size_t n = 4 * 1024 * 1024; /* 4MB of one byte: near-max ratio */
    unsigned char *payload = (unsigned char *)zmalloc(n);
    memset(payload, 'Z', n);

    replCompressor *rc = replCompressorCreate(ALGO_LZ4);
    ASSERT_TRUE(rc != NULL);
    ASSERT_EQ(replCompressorWrite(rc, payload, n), C_OK);
    ASSERT_EQ(replCompressorFlush(rc), C_OK);

    /* One decode call fed a single clamped read (16KB of wire bytes): output
     * must stay far under the 16MB cap; a 255x bound on 16KB is ~4MB. */
    size_t chunk = sdslen(rc->out_buf);
    if (chunk > (size_t)16 * 1024) chunk = (size_t)16 * 1024;
    replDecompressor *rd = replDecompressorCreate();
    ssize_t out_len = replDecompress(rd, rc->out_buf, chunk, 16 * 1024 * 1024);
    ASSERT_GT(out_len, 0);
    EXPECT_GT((size_t)out_len, (size_t)1024 * 1024); /* high ratio actually exercised */
    EXPECT_LT((size_t)out_len, (size_t)16 * 1024 * 1024);

    replDecompressorFree(rd);
    replCompressorFree(rc);
    zfree(payload);
}
