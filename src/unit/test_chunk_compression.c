#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "test_help.h"

#include "../server.h"
#include "../rdb.h"
#include "../rio.h"
#include "../zmalloc.h"
#include "../sds.h"
#include "../lzf.h"

/* External functions from rdb.c that we need to test */
extern struct rdbChunkBuffer *rdbChunkBufferCreate(rio *rdb, size_t chunk_size);
extern struct rdbChunkBuffer *rdbChunkBufferCreateForRead(rio *rdb);
extern void rdbChunkBufferFree(struct rdbChunkBuffer *buf);
extern void rdbChunkBufferFreeForRead(struct rdbChunkBuffer *buf);
extern ssize_t rdbChunkBufferWrite(struct rdbChunkBuffer *buf, void *data, size_t len);
extern ssize_t rdbChunkBufferRead(struct rdbChunkBuffer *buf, void *data, size_t len);
extern int rdbChunkBufferFlush(struct rdbChunkBuffer *buf);

/* Test compression of highly compressible data (repeated pattern) */
int test_compressHighlyCompressibleData(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer with 8KB size */
    size_t chunk_size = 8192;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write highly compressible data (repeated 'A's) */
    char *data = zmalloc(chunk_size);
    memset(data, 'A', chunk_size);
    
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, chunk_size);
    TEST_ASSERT_MESSAGE("Writing compressible data should succeed", written == (ssize_t)chunk_size);

    /* Flush to trigger compression */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing should succeed", result == 0);

    /* Check that compression occurred (compressed size should be much smaller) */
    size_t output_size = sdslen(r.io.buffer.ptr);
    TEST_ASSERT_MESSAGE("Compressed data should be smaller than original", output_size < chunk_size);

    /* Clean up */
    zfree(data);
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr);
    return 0;
}

/* Test compression of incompressible data (random bytes) */
int test_compressIncompressibleData(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer with 4KB size */
    size_t chunk_size = 4096;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write incompressible data (pseudo-random pattern) */
    char *data = zmalloc(chunk_size);
    for (size_t i = 0; i < chunk_size; i++) {
        data[i] = (char)(i * 7 + 13); /* Simple pseudo-random pattern */
    }
    
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, chunk_size);
    TEST_ASSERT_MESSAGE("Writing incompressible data should succeed", written == (ssize_t)chunk_size);

    /* Flush to trigger compression */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing should succeed", result == 0);

    /* Incompressible data should be stored uncompressed (compressed_size = 0) */
    /* We can't easily verify this without inspecting the buffer, but the operation should succeed */

    /* Clean up */
    zfree(data);
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr);
    return 0;
}

/* Test compression ratio improvements with various data patterns */
int test_compressionRatioVariousPatterns(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 8192;
    
    /* Test pattern 1: All zeros (best compression) */
    {
        rio r;
        sds buf = sdsempty();
        rioInitWithBuffer(&r, buf);
        
        struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size);
        TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);
        
        char *data = zmalloc(chunk_size);
        memset(data, 0, chunk_size);
        
        rdbChunkBufferWrite(chunk_buf, data, chunk_size);
        rdbChunkBufferFlush(chunk_buf);
        
        size_t output_size = sdslen(r.io.buffer.ptr);
        TEST_ASSERT_MESSAGE("All-zeros should compress very well", output_size < chunk_size / 10);
        
        zfree(data);
        rdbChunkBufferFree(chunk_buf);
        sdsfree(r.io.buffer.ptr);
    }
    
    /* Test pattern 2: Repeated string pattern */
    {
        rio r;
        sds buf = sdsempty();
        rioInitWithBuffer(&r, buf);
        
        struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size);
        TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);
        
        char *data = zmalloc(chunk_size);
        const char *pattern = "VALKEY";
        size_t pattern_len = strlen(pattern);
        for (size_t i = 0; i < chunk_size; i++) {
            data[i] = pattern[i % pattern_len];
        }
        
        rdbChunkBufferWrite(chunk_buf, data, chunk_size);
        rdbChunkBufferFlush(chunk_buf);
        
        size_t output_size = sdslen(r.io.buffer.ptr);
        TEST_ASSERT_MESSAGE("Repeated pattern should compress well", output_size < chunk_size / 5);
        
        zfree(data);
        rdbChunkBufferFree(chunk_buf);
        sdsfree(r.io.buffer.ptr);
    }

    return 0;
}

/* Test decompression of valid compressed chunks */
int test_decompressValidChunks(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    
    /* First, create compressed data */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    /* Write test data */
    char *original_data = zmalloc(chunk_size);
    for (size_t i = 0; i < chunk_size; i++) {
        original_data[i] = (char)('A' + (i % 26));
    }
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, chunk_size);
    rdbChunkBufferFlush(write_chunk_buf);
    rdbChunkBufferFree(write_chunk_buf);
    
    /* Now read and decompress */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    /* Read decompressed data */
    char *decompressed_data = zmalloc(chunk_size);
    ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, chunk_size);
    TEST_ASSERT_MESSAGE("Reading decompressed data should succeed", read_bytes == (ssize_t)chunk_size);
    
    /* Verify data integrity */
    int data_matches = (memcmp(original_data, decompressed_data, chunk_size) == 0);
    TEST_ASSERT_MESSAGE("Decompressed data should match original", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}

/* Test decompression of multiple chunks */
int test_decompressMultipleChunks(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    size_t total_data_size = chunk_size * 3; /* 3 chunks */
    
    /* Create compressed data with multiple chunks */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    /* Write test data that spans multiple chunks */
    char *original_data = zmalloc(total_data_size);
    for (size_t i = 0; i < total_data_size; i++) {
        original_data[i] = (char)('0' + (i % 10));
    }
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, total_data_size);
    rdbChunkBufferFlush(write_chunk_buf);
    rdbChunkBufferFree(write_chunk_buf);
    
    /* Read and decompress all chunks */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    /* Read all decompressed data */
    char *decompressed_data = zmalloc(total_data_size);
    ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, total_data_size);
    TEST_ASSERT_MESSAGE("Reading all decompressed data should succeed", read_bytes == (ssize_t)total_data_size);
    
    /* Verify data integrity */
    int data_matches = (memcmp(original_data, decompressed_data, total_data_size) == 0);
    TEST_ASSERT_MESSAGE("Decompressed data should match original across multiple chunks", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}

/* Test handling of uncompressed chunks (compressed_size = 0) */
int test_uncompressedChunkHandling(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    
    /* Create data that will be stored uncompressed */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    /* Write incompressible data */
    char *original_data = zmalloc(chunk_size);
    for (size_t i = 0; i < chunk_size; i++) {
        original_data[i] = (char)(i * 7 + 13);
    }
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, chunk_size);
    rdbChunkBufferFlush(write_chunk_buf);
    rdbChunkBufferFree(write_chunk_buf);
    
    /* Read back the uncompressed chunk */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    char *decompressed_data = zmalloc(chunk_size);
    ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, chunk_size);
    TEST_ASSERT_MESSAGE("Reading uncompressed chunk should succeed", read_bytes == (ssize_t)chunk_size);
    
    /* Verify data integrity */
    int data_matches = (memcmp(original_data, decompressed_data, chunk_size) == 0);
    TEST_ASSERT_MESSAGE("Uncompressed chunk data should match original", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}

/* Test compression with mixed compressible and incompressible data */
int test_compressionMixedData(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    size_t total_size = chunk_size * 2;
    
    /* Create mixed data: first half compressible, second half incompressible */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    char *original_data = zmalloc(total_size);
    /* First half: highly compressible */
    memset(original_data, 'X', chunk_size);
    /* Second half: incompressible */
    for (size_t i = chunk_size; i < total_size; i++) {
        original_data[i] = (char)(i * 7 + 13);
    }
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, total_size);
    rdbChunkBufferFlush(write_chunk_buf);
    rdbChunkBufferFree(write_chunk_buf);
    
    /* Read back and verify */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    char *decompressed_data = zmalloc(total_size);
    ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, total_size);
    TEST_ASSERT_MESSAGE("Reading mixed data should succeed", read_bytes == (ssize_t)total_size);
    
    /* Verify data integrity */
    int data_matches = (memcmp(original_data, decompressed_data, total_size) == 0);
    TEST_ASSERT_MESSAGE("Mixed data should match original", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}

/* Test round-trip compression and decompression with various sizes */
int test_roundTripCompressionVariousSizes(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t test_sizes[] = {100, 1024, 4096, 8192, 16384};
    size_t num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
    
    for (size_t t = 0; t < num_tests; t++) {
        size_t data_size = test_sizes[t];
        size_t chunk_size = 8192;
        
        /* Create original data */
        char *original_data = zmalloc(data_size);
        for (size_t i = 0; i < data_size; i++) {
            original_data[i] = (char)(i % 256);
        }
        
        /* Compress */
        rio write_rio;
        sds write_buf = sdsempty();
        rioInitWithBuffer(&write_rio, write_buf);
        
        struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
        TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
        
        rdbChunkBufferWrite(write_chunk_buf, original_data, data_size);
        rdbChunkBufferFlush(write_chunk_buf);
        rdbChunkBufferFree(write_chunk_buf);
        
        /* Decompress */
        rio read_rio;
        rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
        
        struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
        TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
        
        char *decompressed_data = zmalloc(data_size);
        ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, data_size);
        TEST_ASSERT_MESSAGE("Reading decompressed data should succeed", read_bytes == (ssize_t)data_size);
        
        /* Verify */
        int data_matches = (memcmp(original_data, decompressed_data, data_size) == 0);
        TEST_ASSERT_MESSAGE("Round-trip data should match", data_matches);
        
        /* Clean up */
        zfree(original_data);
        zfree(decompressed_data);
        rdbChunkBufferFreeForRead(read_chunk_buf);
        sdsfree(write_rio.io.buffer.ptr);
    }
    
    return 0;
}

/* Test handling of empty chunks */
int test_compressionEmptyData(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer */
    size_t chunk_size = 4096;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Flush without writing any data */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing empty buffer should succeed", result == 0);

    /* Output should be empty (no chunk written) */
    size_t output_size = sdslen(r.io.buffer.ptr);
    TEST_ASSERT_MESSAGE("Empty buffer should produce no output", output_size == 0);

    /* Clean up */
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr);
    return 0;
}

/* Test that compression actually reduces size for compressible data */
int test_verifyCompressionActuallyOccurs(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 8192;
    
    /* Create highly compressible data */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    char *original_data = zmalloc(chunk_size);
    memset(original_data, 'Z', chunk_size);
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, chunk_size);
    rdbChunkBufferFlush(write_chunk_buf);
    
    size_t compressed_output_size = sdslen(write_rio.io.buffer.ptr);
    
    /* Verify compression actually happened - should be much smaller */
    /* With LZF, 8KB of repeated 'Z' should compress to < 100 bytes */
    TEST_ASSERT_MESSAGE("Compression should significantly reduce size", compressed_output_size < 200);
    
    /* Now verify we can decompress it back correctly */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    char *decompressed_data = zmalloc(chunk_size);
    ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, chunk_size);
    TEST_ASSERT_MESSAGE("Reading should succeed", read_bytes == (ssize_t)chunk_size);
    
    /* Verify every byte matches */
    int data_matches = (memcmp(original_data, decompressed_data, chunk_size) == 0);
    TEST_ASSERT_MESSAGE("Decompressed data must match original exactly", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFree(write_chunk_buf);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}

/* Test reading data in smaller chunks than written */
int test_partialReads(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    size_t total_data_size = chunk_size * 2;
    
    /* Create and compress data */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    char *original_data = zmalloc(total_data_size);
    for (size_t i = 0; i < total_data_size; i++) {
        original_data[i] = (char)(i & 0xFF);
    }
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, total_data_size);
    rdbChunkBufferFlush(write_chunk_buf);
    rdbChunkBufferFree(write_chunk_buf);
    
    /* Read back in small chunks */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    char *decompressed_data = zmalloc(total_data_size);
    size_t total_read = 0;
    size_t read_chunk_size = 512; /* Read in 512-byte chunks */
    
    while (total_read < total_data_size) {
        size_t to_read = (total_data_size - total_read < read_chunk_size) ? 
                         (total_data_size - total_read) : read_chunk_size;
        ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, 
                                                 decompressed_data + total_read, 
                                                 to_read);
        TEST_ASSERT_MESSAGE("Partial read should succeed", read_bytes == (ssize_t)to_read);
        total_read += read_bytes;
    }
    
    TEST_ASSERT_MESSAGE("Should read all data", total_read == total_data_size);
    
    /* Verify data integrity */
    int data_matches = (memcmp(original_data, decompressed_data, total_data_size) == 0);
    TEST_ASSERT_MESSAGE("Data from partial reads should match original", data_matches);
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}

/* Test that different data produces different compressed output */
int test_differentDataProducesDifferentOutput(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    
    /* Compress data pattern 1 */
    rio write_rio1;
    sds write_buf1 = sdsempty();
    rioInitWithBuffer(&write_rio1, write_buf1);
    
    struct rdbChunkBuffer *write_chunk_buf1 = rdbChunkBufferCreate(&write_rio1, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer 1 creation should succeed", write_chunk_buf1 != NULL);
    
    char *data1 = zmalloc(chunk_size);
    memset(data1, 'A', chunk_size);
    
    rdbChunkBufferWrite(write_chunk_buf1, data1, chunk_size);
    rdbChunkBufferFlush(write_chunk_buf1);
    rdbChunkBufferFree(write_chunk_buf1);
    
    /* Compress data pattern 2 (different) */
    rio write_rio2;
    sds write_buf2 = sdsempty();
    rioInitWithBuffer(&write_rio2, write_buf2);
    
    struct rdbChunkBuffer *write_chunk_buf2 = rdbChunkBufferCreate(&write_rio2, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer 2 creation should succeed", write_chunk_buf2 != NULL);
    
    char *data2 = zmalloc(chunk_size);
    memset(data2, 'B', chunk_size);
    
    rdbChunkBufferWrite(write_chunk_buf2, data2, chunk_size);
    rdbChunkBufferFlush(write_chunk_buf2);
    rdbChunkBufferFree(write_chunk_buf2);
    
    /* Compressed outputs should be different */
    size_t size1 = sdslen(write_rio1.io.buffer.ptr);
    size_t size2 = sdslen(write_rio2.io.buffer.ptr);
    
    /* Sizes might be the same for highly compressible data, but content should differ */
    int outputs_differ = (size1 != size2) || 
                         (memcmp(write_rio1.io.buffer.ptr, write_rio2.io.buffer.ptr, 
                                size1 < size2 ? size1 : size2) != 0);
    TEST_ASSERT_MESSAGE("Different input data should produce different compressed output", outputs_differ);
    
    /* Clean up */
    zfree(data1);
    zfree(data2);
    sdsfree(write_rio1.io.buffer.ptr);
    sdsfree(write_rio2.io.buffer.ptr);
    return 0;
}

/* Test compression preserves exact byte values */
int test_compressionPreservesExactBytes(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t chunk_size = 4096;
    
    /* Create data with all possible byte values */
    rio write_rio;
    sds write_buf = sdsempty();
    rioInitWithBuffer(&write_rio, write_buf);
    
    struct rdbChunkBuffer *write_chunk_buf = rdbChunkBufferCreate(&write_rio, chunk_size);
    TEST_ASSERT_MESSAGE("Write chunk buffer creation should succeed", write_chunk_buf != NULL);
    
    char *original_data = zmalloc(chunk_size);
    for (size_t i = 0; i < chunk_size; i++) {
        original_data[i] = (char)(i % 256); /* All byte values 0-255 */
    }
    
    rdbChunkBufferWrite(write_chunk_buf, original_data, chunk_size);
    rdbChunkBufferFlush(write_chunk_buf);
    rdbChunkBufferFree(write_chunk_buf);
    
    /* Decompress */
    rio read_rio;
    rioInitWithBuffer(&read_rio, write_rio.io.buffer.ptr);
    
    struct rdbChunkBuffer *read_chunk_buf = rdbChunkBufferCreateForRead(&read_rio);
    TEST_ASSERT_MESSAGE("Read chunk buffer creation should succeed", read_chunk_buf != NULL);
    
    char *decompressed_data = zmalloc(chunk_size);
    ssize_t read_bytes = rdbChunkBufferRead(read_chunk_buf, decompressed_data, chunk_size);
    TEST_ASSERT_MESSAGE("Reading should succeed", read_bytes == (ssize_t)chunk_size);
    
    /* Verify every single byte matches exactly */
    for (size_t i = 0; i < chunk_size; i++) {
        if (original_data[i] != decompressed_data[i]) {
            TEST_PRINT_INFO("Byte mismatch at position %zu: expected 0x%02X, got 0x%02X", 
                           i, (unsigned char)original_data[i], (unsigned char)decompressed_data[i]);
            TEST_ASSERT_MESSAGE("All bytes must match exactly", 0);
        }
    }
    
    /* Clean up */
    zfree(original_data);
    zfree(decompressed_data);
    rdbChunkBufferFreeForRead(read_chunk_buf);
    sdsfree(write_rio.io.buffer.ptr);
    return 0;
}
