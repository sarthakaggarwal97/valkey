/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Unit tests for replication compression configuration and capability constants. */

#include "generated_wrappers.hpp"

#include <cstring>
#include <string>

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
    /* Each REPLICA_CAPA_* must occupy a unique bit position. */
    int all_capas[] = {
        REPLICA_CAPA_EOF,
        REPLICA_CAPA_PSYNC2,
        REPLICA_CAPA_DUAL_CHANNEL,
        REPLICA_CAPA_SKIP_RDB_CHECKSUM,
        REPLICA_CAPA_COMPRESSION,
    };
    int count = sizeof(all_capas) / sizeof(all_capas[0]);

    for (int i = 0; i < count; i++) {
        /* Each value must be a power of two (single bit set). */
        EXPECT_NE(all_capas[i], 0) << "capability " << i << " must be non-zero";
        EXPECT_EQ(all_capas[i] & (all_capas[i] - 1), 0)
            << "capability " << i << " must be a power of two";

        for (int j = i + 1; j < count; j++) {
            EXPECT_EQ(all_capas[i] & all_capas[j], 0)
                << "capabilities " << i << " and " << j << " must not share bits";
        }
    }

    /* Verify the specific value. */
    EXPECT_EQ(REPLICA_CAPA_COMPRESSION, (1 << 4));
}

TEST(replCompression, capaCompressionStr) {
    EXPECT_STREQ(REPLICA_CAPA_COMPRESSION_STR, "compression");
}

TEST(replCompression, algoConstants) {
    /* ALGO_LZ4 must be non-zero so it's distinguishable from zero-init. */
    EXPECT_NE(ALGO_LZ4, 0);
}

TEST(replCompression, adapterRoundTripAndBufferHandoff) {
    std::string payload(256 * 1024, '\0');
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = (char)((i * 17 + 11) & 0xff);
    }

    replCompressor *compressor = replCompressorCreate(ALGO_LZ4, 0);
    ASSERT_NE(compressor, nullptr);
    ASSERT_EQ(replCompressorWrite(compressor, payload.data(), payload.size()), 0);
    ASSERT_EQ(replCompressorFlush(compressor), 0);
    ASSERT_GT(sdslen(compressor->out_buf), (size_t)VCS_ENVELOPE_SIZE);

    replDecompressor *decompressor = replDecompressorCreate();
    ASSERT_NE(decompressor, nullptr);

    size_t decoded_len = 0;
    ASSERT_EQ(replDecompressorDecode(decompressor, compressor->out_buf,
                                     sdslen(compressor->out_buf), payload.size(),
                                     &decoded_len),
              REPL_DECODE_OK);
    ASSERT_EQ(decoded_len, payload.size());

    sds decoded_buf = replDecompressorBuf(decompressor);
    ASSERT_EQ(memcmp(decoded_buf, payload.data(), payload.size()), 0);

    sds replacement = sdsnew("reusable compressed input allocation");
    sds taken = replDecompressorTakeBuf(decompressor, replacement);
    EXPECT_EQ(taken, decoded_buf);
    EXPECT_EQ(sdslen(taken), payload.size());
    EXPECT_EQ(sdslen(replDecompressorBuf(decompressor)), 0u);

    sdsfree(taken);
    replDecompressorDestroy(decompressor);
    replCompressorDestroy(compressor);
}

TEST(replCompression, adapterRoundTripWithFragmentedInput) {
    std::string payload(192 * 1024, '\0');
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = (char)((i * 29 + 7) % 251);
    }

    replCompressor *compressor = replCompressorCreate(ALGO_LZ4, 0);
    ASSERT_NE(compressor, nullptr);
    ASSERT_EQ(replCompressorWrite(compressor, payload.data(), payload.size()), 0);
    ASSERT_EQ(replCompressorFlush(compressor), 0);

    replDecompressor *decompressor = replDecompressorCreate();
    ASSERT_NE(decompressor, nullptr);
    std::string decoded;
    size_t offset = 0;
    while (offset < sdslen(compressor->out_buf)) {
        size_t chunk_len = 1 + (offset % 97);
        if (chunk_len > sdslen(compressor->out_buf) - offset) {
            chunk_len = sdslen(compressor->out_buf) - offset;
        }

        size_t decoded_len = 0;
        ASSERT_EQ(replDecompressorDecode(decompressor, compressor->out_buf + offset,
                                         chunk_len, payload.size(), &decoded_len),
                  REPL_DECODE_OK);
        decoded.append(replDecompressorBuf(decompressor), decoded_len);
        offset += chunk_len;
    }

    EXPECT_EQ(decoded, payload);
    replDecompressorDestroy(decompressor);
    replCompressorDestroy(compressor);
}

TEST(replCompression, fragmentedProbeThenZeroCopyPassthrough) {
    replDecompressor *decompressor = replDecompressorCreate();
    ASSERT_NE(decompressor, nullptr);

    size_t decoded_len = 123;
    ASSERT_EQ(replDecompressorDecode(decompressor, "V", 1, 64, &decoded_len),
              REPL_DECODE_OK);
    EXPECT_EQ(decoded_len, 0u);

    ASSERT_EQ(replDecompressorDecode(decompressor, "Xabc", 4, 64, &decoded_len),
              REPL_DECODE_OK);
    ASSERT_EQ(decoded_len, 5u);
    EXPECT_EQ(memcmp(replDecompressorBuf(decompressor), "VXabc", 5), 0);

    ASSERT_EQ(replDecompressorDecode(decompressor, "plaintext", 9, 64, &decoded_len),
              REPL_DECODE_PASSTHROUGH);
    EXPECT_EQ(decoded_len, 9u);
    EXPECT_EQ(sdslen(replDecompressorBuf(decompressor)), 0u);

    EXPECT_EQ(replDecompressorDecode(decompressor, "too large", 9, 8, &decoded_len),
              REPL_DECODE_OVERFLOW);
    EXPECT_EQ(decoded_len, 0u);

    replDecompressorDestroy(decompressor);
}

TEST(replCompression, plaintextProbeIsZeroCopy) {
    replDecompressor *decompressor = replDecompressorCreate();
    ASSERT_NE(decompressor, nullptr);

    const char *payload = "*1\r\n$4\r\nping\r\n";
    size_t payload_len = strlen(payload);
    size_t decoded_len = 0;
    EXPECT_EQ(replDecompressorDecode(decompressor, payload, payload_len,
                                     payload_len, &decoded_len),
              REPL_DECODE_PASSTHROUGH);
    EXPECT_EQ(decoded_len, payload_len);
    EXPECT_EQ(sdslen(replDecompressorBuf(decompressor)), 0u);

    replDecompressorDestroy(decompressor);
}

TEST(replCompression, compressedOutputLimitIsEnforced) {
    std::string payload(64 * 1024, 'A');
    replCompressor *compressor = replCompressorCreate(ALGO_LZ4, 0);
    ASSERT_NE(compressor, nullptr);
    ASSERT_EQ(replCompressorWrite(compressor, payload.data(), payload.size()), 0);
    ASSERT_EQ(replCompressorFlush(compressor), 0);

    replDecompressor *decompressor = replDecompressorCreate();
    ASSERT_NE(decompressor, nullptr);
    size_t decoded_len = 0;
    EXPECT_EQ(replDecompressorDecode(decompressor, compressor->out_buf,
                                     sdslen(compressor->out_buf), 1024,
                                     &decoded_len),
              REPL_DECODE_OVERFLOW);

    replDecompressorDestroy(decompressor);
    replCompressorDestroy(compressor);
}

TEST(replCompression, compressibleBatchUsesBoundedStagingAllocation) {
    std::string payload(REPL_COMPRESSION_BATCH_LIMIT, 'A');
    replCompressor *compressor = replCompressorCreate(ALGO_LZ4, 0);
    ASSERT_NE(compressor, nullptr);

    ASSERT_EQ(replCompressorWrite(compressor, payload.data(), payload.size()), 0);
    ASSERT_EQ(replCompressorFlush(compressor), 0);
    EXPECT_LT(sdsalloc(compressor->out_buf), (size_t)256 * 1024);
    EXPECT_EQ(replCompressorMemUsage(compressor),
              sizeof(*compressor) + sdsalloc(compressor->out_buf));

    replCompressorDestroy(compressor);
}
