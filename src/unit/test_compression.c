/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Property-based tests for the compression module envelope.
 *
 * **Property: Envelope Format Compliance**
 * **Validates: Requirements 2.2, 2.3, 2.4, 2.15, 2.19** */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../compression.h"
#include "test_help.h"

/* --- Emit callback that writes into a flat buffer --- */
typedef struct {
    uint8_t buf[64];
    size_t pos;
} emit_buf_t;

static void emit_to_buf(void *ctx, const uint8_t *data, size_t len) {
    emit_buf_t *eb = (emit_buf_t *)ctx;
    if (eb->pos + len <= sizeof(eb->buf)) {
        memcpy(eb->buf + eb->pos, data, len);
        eb->pos += len;
    }
}

/* --- Property: Envelope round-trip ---
 * For every valid (algo, stream_kind) pair, write_vkcs_envelope followed by
 * envelope_read must recover the original algo and stream_kind. */
int test_envelopeRoundTrip(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    compression_algo_t algos[] = {ALGO_LZ4, ALGO_ZSTD};
    uint8_t kinds[] = {STREAM_KIND_RDB, STREAM_KIND_REPL};

    for (int a = 0; a < 2; a++) {
        for (int k = 0; k < 2; k++) {
            emit_buf_t eb = {.pos = 0};
            int wret = write_vkcs_envelope(emit_to_buf, &eb, algos[a], kinds[k]);
            TEST_ASSERT_MESSAGE("write_vkcs_envelope should succeed for valid params", wret == 0);
            TEST_ASSERT_MESSAGE("envelope should be exactly 8 bytes", eb.pos == VKCS_ENVELOPE_SIZE);

            compression_algo_t got_algo = ALGO_NONE;
            uint8_t got_kind = 0xFF;
            int rret = envelope_read(eb.buf, eb.pos, &got_algo, &got_kind);
            TEST_ASSERT_MESSAGE("envelope_read should succeed", rret == 0);
            TEST_ASSERT_MESSAGE("round-trip algo must match", got_algo == algos[a]);
            TEST_ASSERT_MESSAGE("round-trip stream_kind must match", got_kind == kinds[k]);
        }
    }
    return 0;
}

/* --- Property: Envelope magic bytes are "VKCS" (Req 2.3) --- */
int test_envelopeMagicBytes(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    emit_buf_t eb = {.pos = 0};
    int ret = write_vkcs_envelope(emit_to_buf, &eb, ALGO_LZ4, STREAM_KIND_RDB);
    TEST_ASSERT_MESSAGE("write must succeed", ret == 0);

    TEST_ASSERT_MESSAGE("magic[0] == 'V'", eb.buf[0] == 0x56);
    TEST_ASSERT_MESSAGE("magic[1] == 'K'", eb.buf[1] == 0x4B);
    TEST_ASSERT_MESSAGE("magic[2] == 'C'", eb.buf[2] == 0x43);
    TEST_ASSERT_MESSAGE("magic[3] == 'S'", eb.buf[3] == 0x53);
    TEST_ASSERT_MESSAGE("version == VKCS_VERSION", eb.buf[4] == VKCS_VERSION);
    TEST_ASSERT_MESSAGE("reserved == 0", eb.buf[7] == 0);
    return 0;
}

/* --- Property: Envelope flags encode stream_kind in bit 0 (Req 2.4) --- */
int test_envelopeStreamKindFlag(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* RDB: bit 0 = 0 */
    emit_buf_t eb_rdb = {.pos = 0};
    int ret = write_vkcs_envelope(emit_to_buf, &eb_rdb, ALGO_LZ4, STREAM_KIND_RDB);
    TEST_ASSERT_MESSAGE("write RDB must succeed", ret == 0);
    TEST_ASSERT_MESSAGE("RDB stream_kind: flags bit 0 == 0", (eb_rdb.buf[6] & 0x01) == 0);

    /* REPL: bit 0 = 1 */
    emit_buf_t eb_repl = {.pos = 0};
    ret = write_vkcs_envelope(emit_to_buf, &eb_repl, ALGO_LZ4, STREAM_KIND_REPL);
    TEST_ASSERT_MESSAGE("write REPL must succeed", ret == 0);
    TEST_ASSERT_MESSAGE("REPL stream_kind: flags bit 0 == 1", (eb_repl.buf[6] & 0x01) == 1);
    return 0;
}

/* --- Property: Unrecognized algo_id is rejected (Req 2.15) --- */
int test_envelopeRejectsUnknownAlgo(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Build a valid envelope, then corrupt the algo_id byte */
    emit_buf_t eb = {.pos = 0};
    int wret = write_vkcs_envelope(emit_to_buf, &eb, ALGO_LZ4, STREAM_KIND_RDB);
    TEST_ASSERT_MESSAGE("write must succeed", wret == 0);

    /* Try every invalid algo_id value 0..255 except ALGO_LZ4 and ALGO_ZSTD */
    for (int i = 0; i < 256; i++) {
        if (i == ALGO_LZ4 || i == ALGO_ZSTD) continue;
        eb.buf[5] = (uint8_t)i;
        compression_algo_t a;
        uint8_t k;
        int ret = envelope_read(eb.buf, eb.pos, &a, &k);
        TEST_ASSERT_MESSAGE("envelope_read must reject unknown algo_id", ret == -1);
    }
    return 0;
}

/* --- Property: write rejects non-streaming algorithms --- */
int test_envelopeRejectsNonStreamingAlgo(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    emit_buf_t eb = {.pos = 0};
    TEST_ASSERT_MESSAGE("ALGO_NONE rejected", write_vkcs_envelope(emit_to_buf, &eb, ALGO_NONE, STREAM_KIND_RDB) == -1);
    TEST_ASSERT_MESSAGE("ALGO_LZF rejected", write_vkcs_envelope(emit_to_buf, &eb, ALGO_LZF, STREAM_KIND_RDB) == -1);
    return 0;
}

/* --- Property: envelope_read rejects truncated input --- */
int test_envelopeRejectsTruncated(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    emit_buf_t eb = {.pos = 0};
    int wret = write_vkcs_envelope(emit_to_buf, &eb, ALGO_LZ4, STREAM_KIND_RDB);
    TEST_ASSERT_MESSAGE("write must succeed", wret == 0);

    /* Every length < 8 must fail */
    for (size_t l = 0; l < VKCS_ENVELOPE_SIZE; l++) {
        compression_algo_t a;
        uint8_t k;
        int ret = envelope_read(eb.buf, l, &a, &k);
        TEST_ASSERT_MESSAGE("truncated envelope must be rejected", ret == -1);
    }
    return 0;
}

/* --- Property: envelope_read rejects bad magic --- */
int test_envelopeRejectsBadMagic(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    emit_buf_t eb = {.pos = 0};
    int wret = write_vkcs_envelope(emit_to_buf, &eb, ALGO_LZ4, STREAM_KIND_RDB);
    TEST_ASSERT_MESSAGE("write must succeed", wret == 0);

    /* Flip each magic byte and verify rejection */
    for (int i = 0; i < 4; i++) {
        uint8_t orig = eb.buf[i];
        eb.buf[i] = ~orig;
        compression_algo_t a;
        uint8_t k;
        int ret = envelope_read(eb.buf, eb.pos, &a, &k);
        TEST_ASSERT_MESSAGE("bad magic must be rejected", ret == -1);
        eb.buf[i] = orig;
    }
    return 0;
}

/* --- Property: Randomized round-trip (PBT-style) ---
 * Generate random valid inputs and verify round-trip correctness. */
int test_envelopeRandomRoundTrip(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    int iterations = (flags & UNIT_TEST_ACCURATE) ? 10000 : 1000;
    compression_algo_t algos[] = {ALGO_LZ4, ALGO_ZSTD};
    uint8_t kinds[] = {STREAM_KIND_RDB, STREAM_KIND_REPL};

    /* Use random() seeded by the test harness for reproducibility via --seed */
    for (int i = 0; i < iterations; i++) {
        compression_algo_t algo = algos[random() % 2];
        uint8_t kind = kinds[random() % 2];

        emit_buf_t eb = {.pos = 0};
        int wret = write_vkcs_envelope(emit_to_buf, &eb, algo, kind);
        TEST_ASSERT_MESSAGE("write must succeed", wret == 0);
        TEST_ASSERT_MESSAGE("size must be 8", eb.pos == VKCS_ENVELOPE_SIZE);

        compression_algo_t got_algo = ALGO_NONE;
        uint8_t got_kind = 0xFF;
        int rret = envelope_read(eb.buf, eb.pos, &got_algo, &got_kind);
        TEST_ASSERT_MESSAGE("read must succeed", rret == 0);
        TEST_ASSERT_MESSAGE("algo round-trip", got_algo == algo);
        TEST_ASSERT_MESSAGE("kind round-trip", got_kind == kind);
    }
    return 0;
}
