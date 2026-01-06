#include <stdio.h>
#include <string.h>
#include "../server.h"
#include "../rdb.h"
#include "test_help.h"

int test_compression_config_validation(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    int result = 0;
    const char *err = NULL;
    
    /* Test 1: LZF with valid chunk size */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZF;
    server.rdb_chunk_size = 65536;
    
    result = validateCompressionConfig(&err);
    TEST_ASSERT_MESSAGE("LZF should work with valid chunk size", result == 1);
    
    /* Test 2: LZ4-stream with valid chunk size */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZ4_STREAM;
    server.rdb_chunk_size = 65536;
    
    result = validateCompressionConfig(&err);
    TEST_ASSERT_MESSAGE("LZ4-stream should work with valid chunk size", result == 1);
    
    /* Test 3: chunk size within INT_MAX */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZF;
    server.rdb_chunk_size = 65536;
    
    result = validateCompressionConfig(&err);
    TEST_ASSERT_MESSAGE("Normal chunk size should be valid", result == 1);
    
    return 0;
}
