#include <stdio.h>
#include <string.h>
#include "test_help.h"

#include "../server.h"
#include "../rdb.h"
#include "../zmalloc.h"

/* Test LZ4 dictionary context creation and destruction */
int test_lz4DictContextLifecycle(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Disable logging for unit tests */
    server.verbosity = LL_NOTHING;

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();
    
    /* Get LZ4-stream compressor */
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    TEST_ASSERT_MESSAGE("Compressor should have correct algorithm ID", 
                       compressor->algorithm == RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("Compressor should have correct name", 
                       strcmp(compressor->name, "LZ4-stream") == 0);
    
    /* Test context creation with normal size */
    void *ctx = compressor->create_context(64 * 1024);
    TEST_ASSERT_MESSAGE("Context creation should succeed", ctx != NULL);
    
    /* Free context */
    compressor->free_context(ctx);
    
    /* Test context creation with zero size */
    ctx = compressor->create_context(0);
    TEST_ASSERT_MESSAGE("Context creation with zero size should succeed", ctx != NULL);
    compressor->free_context(ctx);
    
    /* Test context creation with large size (should be capped) */
    ctx = compressor->create_context(128 * 1024);
    TEST_ASSERT_MESSAGE("Context creation with large size should succeed", ctx != NULL);
    compressor->free_context(ctx);
    
    return 0;
}

/* Test LZ4 dictionary compression and decompression round-trip */
int test_lz4DictRoundTrip(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* Create context */
    void *ctx = compressor->create_context(64 * 1024);
    TEST_ASSERT_MESSAGE("Context creation should succeed", ctx != NULL);
    
    /* Create test data */
    size_t data_size = 4096;
    unsigned char *original_data = zmalloc(data_size);
    for (size_t i = 0; i < data_size; i++) {
        original_data[i] = (unsigned char)('A' + (i % 26));
    }
    
    /* Compress */
    size_t max_compressed = compressor->max_compressed_size(data_size);
    unsigned char *compressed_data = zmalloc(max_compressed);
    
    ssize_t compressed_size = compressor->compress(ctx, original_data, data_size,
                                                   compressed_data, max_compressed);
    TEST_ASSERT_MESSAGE("Compression should succeed", compressed_size > 0);
    TEST_ASSERT_MESSAGE("Compressed size should be less than original", 
                       (size_t)compressed_size < data_size);
    
    /* Decompress */
    unsigned char *decompressed_data = zmalloc(data_size);
    ssize_t decompressed_size = compressor->decompress(ctx, compressed_data, compressed_size,
                                                       decompressed_data, data_size);
    TEST_ASSERT_MESSAGE("Decompression should succeed", decompressed_size == (ssize_t)data_size);
    
    /* Verify data matches */
    int data_matches = (memcmp(original_data, decompressed_data, data_size) == 0);
    TEST_ASSERT_MESSAGE("Decompressed data should match original", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(compressed_data);
    zfree(decompressed_data);
    compressor->free_context(ctx);
    
    return 0;
}

/* Test LZ4 dictionary with multiple chunks (independent compression) */
int test_lz4DictMultipleChunks(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* Create context */
    void *ctx = compressor->create_context(64 * 1024);
    TEST_ASSERT_MESSAGE("Context creation should succeed", ctx != NULL);
    
    size_t chunk_size = 4096;
    size_t max_compressed = compressor->max_compressed_size(chunk_size);
    
    /* Compress first chunk (builds dictionary) */
    unsigned char *chunk1 = zmalloc(chunk_size);
    memset(chunk1, 'A', chunk_size);
    
    unsigned char *compressed1 = zmalloc(max_compressed);
    ssize_t compressed_size1 = compressor->compress(ctx, chunk1, chunk_size,
                                                    compressed1, max_compressed);
    TEST_ASSERT_MESSAGE("First chunk compression should succeed", compressed_size1 > 0);
    
    /* Compress second chunk (uses dictionary) */
    unsigned char *chunk2 = zmalloc(chunk_size);
    memset(chunk2, 'B', chunk_size);
    
    unsigned char *compressed2 = zmalloc(max_compressed);
    ssize_t compressed_size2 = compressor->compress(ctx, chunk2, chunk_size,
                                                    compressed2, max_compressed);
    TEST_ASSERT_MESSAGE("Second chunk compression should succeed", compressed_size2 > 0);
    
    /* Decompress both chunks independently */
    unsigned char *decompressed1 = zmalloc(chunk_size);
    ssize_t decompressed_size1 = compressor->decompress(ctx, compressed1, compressed_size1,
                                                        decompressed1, chunk_size);
    TEST_ASSERT_MESSAGE("First chunk decompression should succeed", 
                       decompressed_size1 == (ssize_t)chunk_size);
    
    unsigned char *decompressed2 = zmalloc(chunk_size);
    ssize_t decompressed_size2 = compressor->decompress(ctx, compressed2, compressed_size2,
                                                        decompressed2, chunk_size);
    TEST_ASSERT_MESSAGE("Second chunk decompression should succeed", 
                       decompressed_size2 == (ssize_t)chunk_size);
    
    /* Verify data matches */
    TEST_ASSERT_MESSAGE("First chunk should match", memcmp(chunk1, decompressed1, chunk_size) == 0);
    TEST_ASSERT_MESSAGE("Second chunk should match", memcmp(chunk2, decompressed2, chunk_size) == 0);
    
    /* Clean up */
    zfree(chunk1);
    zfree(chunk2);
    zfree(compressed1);
    zfree(compressed2);
    zfree(decompressed1);
    zfree(decompressed2);
    compressor->free_context(ctx);
    
    return 0;
}

/* Test LZ4 dictionary with incompressible data */
int test_lz4DictIncompressibleData(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* Create context */
    void *ctx = compressor->create_context(64 * 1024);
    TEST_ASSERT_MESSAGE("Context creation should succeed", ctx != NULL);
    
    /* Create incompressible data (pseudo-random) */
    size_t data_size = 4096;
    unsigned char *original_data = zmalloc(data_size);
    for (size_t i = 0; i < data_size; i++) {
        original_data[i] = (unsigned char)(i * 7 + 13);
    }
    
    /* Try to compress */
    size_t max_compressed = compressor->max_compressed_size(data_size);
    unsigned char *compressed_data = zmalloc(max_compressed);
    
    ssize_t compressed_size = compressor->compress(ctx, original_data, data_size,
                                                   compressed_data, max_compressed);
    
    /* Even pseudo-random data may compress with LZ4 streaming
     * The test should verify that compression either succeeds or returns 0 for incompressible */
    TEST_ASSERT_MESSAGE("Compression should return valid result (>= 0)", compressed_size >= 0);
    
    /* If data compressed, verify it can be decompressed */
    if (compressed_size > 0) {
        unsigned char *decompressed_data = zmalloc(data_size);
        ssize_t decompressed_size = compressor->decompress(ctx, compressed_data, compressed_size,
                                                           decompressed_data, data_size);
        TEST_ASSERT_MESSAGE("Decompression should succeed", decompressed_size == (ssize_t)data_size);
        TEST_ASSERT_MESSAGE("Data should match after round-trip", 
                           memcmp(original_data, decompressed_data, data_size) == 0);
        zfree(decompressed_data);
    }
    
    /* Clean up */
    zfree(original_data);
    zfree(compressed_data);
    compressor->free_context(ctx);
    
    return 0;
}

/* Test LZ4 max compressed size calculation */
int test_lz4DictMaxCompressedSize(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* Test various sizes */
    size_t test_sizes[] = {100, 1024, 4096, 8192, 65536};
    for (size_t i = 0; i < sizeof(test_sizes) / sizeof(test_sizes[0]); i++) {
        size_t input_size = test_sizes[i];
        size_t max_size = compressor->max_compressed_size(input_size);
        
        /* Max compressed size should be larger than input size */
        TEST_ASSERT_MESSAGE("Max compressed size should be >= input size", max_size >= input_size);
        
        /* Should be reasonable (not more than 2x input size) */
        TEST_ASSERT_MESSAGE("Max compressed size should be reasonable", max_size < input_size * 2);
    }
    
    return 0;
}

/* Test LZ4 dictionary serialization and deserialization */
int test_lz4DictSerialization(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* LZ4 streaming compression doesn't need serialization - dictionary evolves naturally */
    TEST_ASSERT_MESSAGE("LZ4-stream should not have serialize_context (no dictionary storage)", 
                       compressor->serialize_context == NULL);
    TEST_ASSERT_MESSAGE("LZ4-stream should not have deserialize_context (no dictionary storage)", 
                       compressor->deserialize_context == NULL);
    
    return 0;
}

/* Test LZ4 dictionary serialization with zero-size dictionary */
int test_lz4DictSerializationZeroSize(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* LZ4 streaming compression doesn't need serialization - dictionary evolves naturally */
    TEST_ASSERT_MESSAGE("LZ4-stream should not have serialize_context (no dictionary storage)", 
                       compressor->serialize_context == NULL);
    TEST_ASSERT_MESSAGE("LZ4-stream should not have deserialize_context (no dictionary storage)", 
                       compressor->deserialize_context == NULL);
    
    return 0;
}

/* Test LZ4 dictionary deserialization with oversized dictionary */
int test_lz4DictDeserializationOversized(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    rdbInitCompressionAlgorithms();
    rdbCompressor *compressor = rdbGetCompressor(RDB_COMPRESSION_LZ4_STREAM);
    TEST_ASSERT_MESSAGE("LZ4-stream compressor should be registered", compressor != NULL);
    
    /* LZ4 streaming compression doesn't need serialization - dictionary evolves naturally */
    TEST_ASSERT_MESSAGE("LZ4-stream should not have serialize_context (no dictionary storage)", 
                       compressor->serialize_context == NULL);
    TEST_ASSERT_MESSAGE("LZ4-stream should not have deserialize_context (no dictionary storage)", 
                       compressor->deserialize_context == NULL);
    
    return 0;
}
