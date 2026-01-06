#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "test_help.h"

#include "../server.h"
#include "../rdb.h"
#include "../rio.h"
#include "../zmalloc.h"
#include "../sds.h"

/* The server global is already declared in server.h, we just need to initialize it */

/* External functions from rdb.c that we need to test */
extern struct rdbChunkBuffer *rdbChunkBufferCreate(rio *rdb, size_t chunk_size, 
                                                    rdbCompressionAlgorithm algorithm, size_t dict_size);
extern struct rdbChunkBuffer *rdbChunkBufferCreateForRead(rio *rdb, rdbCompressionAlgorithm algorithm);
extern void rdbChunkBufferFree(struct rdbChunkBuffer *buf);
extern ssize_t rdbChunkBufferWrite(struct rdbChunkBuffer *buf, void *data, size_t len);
extern int rdbChunkBufferFlush(struct rdbChunkBuffer *buf);

/* Test chunk buffer creation and initialization */
int test_chunkBufferCreate(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Disable logging for unit tests */
    server.verbosity = LL_NOTHING;
    
    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Test valid chunk size (64KB) */
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, 65536, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation with valid size should succeed", chunk_buf != NULL);
    if (chunk_buf) rdbChunkBufferFree(chunk_buf);

    /* Test minimum chunk size (4KB) */
    chunk_buf = rdbChunkBufferCreate(&r, 4096, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation with minimum size (4KB) should succeed", chunk_buf != NULL);
    if (chunk_buf) rdbChunkBufferFree(chunk_buf);

    /* Test maximum chunk size (1MB) */
    chunk_buf = rdbChunkBufferCreate(&r, 1024 * 1024, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation with maximum size (1MB) should succeed", chunk_buf != NULL);
    if (chunk_buf) rdbChunkBufferFree(chunk_buf);

    /* Test chunk size too small (< 4KB) */
    chunk_buf = rdbChunkBufferCreate(&r, 4095, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation with size < 4KB should fail", chunk_buf == NULL);

    /* Test chunk size too large (> 1MB) */
    chunk_buf = rdbChunkBufferCreate(&r, 1024 * 1024 + 1, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation with size > 1MB should fail", chunk_buf == NULL);

    /* Test NULL rio pointer */
    chunk_buf = rdbChunkBufferCreate(NULL, 65536, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation with NULL rio should fail", chunk_buf == NULL);

    sdsfree(buf);
    return 0;
}

/* Test writing data smaller than chunk size */
int test_chunkBufferWriteSmall(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer with 8KB size */
    size_t chunk_size = 8192;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write small data (1KB) */
    char data[1024];
    memset(data, 'A', sizeof(data));
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, sizeof(data));
    TEST_ASSERT_MESSAGE("Writing 1KB should succeed", written == sizeof(data));

    /* Write another small chunk (2KB) */
    char data2[2048];
    memset(data2, 'B', sizeof(data2));
    written = rdbChunkBufferWrite(chunk_buf, data2, sizeof(data2));
    TEST_ASSERT_MESSAGE("Writing 2KB should succeed", written == sizeof(data2));

    /* Flush the buffer */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing buffer should succeed", result == 0);

    /* Clean up */
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr); /* Free the potentially reallocated buffer */
    return 0;
}

/* Test writing data larger than chunk size (multiple chunks) */
int test_chunkBufferWriteLarge(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer with small size (4KB) for easier testing */
    size_t chunk_size = 4096;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write data larger than chunk size (12KB = 3 chunks) */
    size_t data_size = 12288;
    char *data = zmalloc(data_size);
    memset(data, 'X', data_size);
    
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, data_size);
    TEST_ASSERT_MESSAGE("Writing 12KB should succeed", written == (ssize_t)data_size);

    /* Flush remaining data */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing buffer should succeed", result == 0);

    /* Clean up */
    zfree(data);
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr); /* Free the potentially reallocated buffer */
    return 0;
}

/* Test flushing partial chunks */
int test_chunkBufferFlushPartial(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer */
    size_t chunk_size = 8192;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write partial chunk (3KB) */
    char data[3072];
    memset(data, 'P', sizeof(data));
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, sizeof(data));
    TEST_ASSERT_MESSAGE("Writing 3KB should succeed", written == sizeof(data));

    /* Flush the partial chunk */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing partial chunk should succeed", result == 0);

    /* Flush again (should be no-op with empty buffer) */
    result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing empty buffer should succeed", result == 0);

    /* Clean up */
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr); /* Free the potentially reallocated buffer */
    return 0;
}

/* Test buffer overflow handling */
int test_chunkBufferOverflow(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer with minimum size */
    size_t chunk_size = 4096;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write data exactly at chunk boundary */
    char *data = zmalloc(chunk_size);
    memset(data, 'Z', chunk_size);
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, chunk_size);
    TEST_ASSERT_MESSAGE("Writing exactly chunk_size should succeed", written == (ssize_t)chunk_size);

    /* Write one more byte (should trigger new chunk) */
    char byte = 'Y';
    written = rdbChunkBufferWrite(chunk_buf, &byte, 1);
    TEST_ASSERT_MESSAGE("Writing one more byte should succeed", written == 1);

    /* Flush */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing should succeed", result == 0);

    /* Clean up */
    zfree(data);
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr); /* Free the potentially reallocated buffer */
    return 0;
}

/* Test writing zero bytes */
int test_chunkBufferWriteZero(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer */
    size_t chunk_size = 8192;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Write zero bytes */
    char data[100];
    ssize_t written = rdbChunkBufferWrite(chunk_buf, data, 0);
    TEST_ASSERT_MESSAGE("Writing 0 bytes should return 0", written == 0);

    /* Flush */
    int result = rdbChunkBufferFlush(chunk_buf);
    TEST_ASSERT_MESSAGE("Flushing should succeed", result == 0);

    /* Clean up */
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr); /* Free the potentially reallocated buffer */
    return 0;
}

/* Test NULL pointer handling */
int test_chunkBufferNullHandling(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Test writing to NULL buffer */
    char data[100];
    ssize_t written = rdbChunkBufferWrite(NULL, data, sizeof(data));
    TEST_ASSERT_MESSAGE("Writing to NULL buffer should fail", written == -1);

    /* Test flushing NULL buffer */
    int result = rdbChunkBufferFlush(NULL);
    TEST_ASSERT_MESSAGE("Flushing NULL buffer should fail", result == -1);

    /* Test freeing NULL buffer (should not crash) */
    rdbChunkBufferFree(NULL);
    TEST_ASSERT_MESSAGE("Freeing NULL buffer should not crash", 1);

    return 0;
}

/* Test multiple write and flush cycles */
int test_chunkBufferMultipleCycles(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize compression algorithms */
    rdbInitCompressionAlgorithms();

    /* Create a buffer rio for testing */
    rio r;
    sds buf = sdsempty();
    rioInitWithBuffer(&r, buf);

    /* Create chunk buffer */
    size_t chunk_size = 4096;
    struct rdbChunkBuffer *chunk_buf = rdbChunkBufferCreate(&r, chunk_size, RDB_COMPRESSION_LZF, 0);
    TEST_ASSERT_MESSAGE("Chunk buffer creation should succeed", chunk_buf != NULL);

    /* Perform multiple write-flush cycles */
    for (int i = 0; i < 5; i++) {
        char data[1024];
        memset(data, 'A' + i, sizeof(data));
        
        ssize_t written = rdbChunkBufferWrite(chunk_buf, data, sizeof(data));
        TEST_ASSERT_MESSAGE("Writing should succeed in cycle", written == sizeof(data));
        
        int result = rdbChunkBufferFlush(chunk_buf);
        TEST_ASSERT_MESSAGE("Flushing should succeed in cycle", result == 0);
    }

    /* Clean up */
    rdbChunkBufferFree(chunk_buf);
    sdsfree(r.io.buffer.ptr); /* Free the potentially reallocated buffer */
    return 0;
}
