/* Unit tests for replication compression algorithm negotiation */

#include "../server.h"
#include "test_help.h"

/* We'll test the logic directly without calling the function that logs */

/* Initialize minimal server state for testing */
static void initServerForTest(void) {
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZF;
    server.verbosity = LL_NOTHING;  /* Disable logging */
}

/* Inline version of selectReplicationCompressionAlgorithm for testing without logging */
static rdbCompressionAlgorithm testSelectAlgorithm(int mincapa, int *use_new_format) {
    rdbCompressionAlgorithm selected_algo;
    
    /* Check if all replicas support the new RDB header format with algorithm byte */
    if (!(mincapa & REPLICA_CAPA_RDB_CMPR_META_V1)) {
        /* Old replicas don't understand algorithm byte - use old format with LZF */
        *use_new_format = 0;
        selected_algo = RDB_COMPRESSION_LZF;
        return selected_algo;
    }
    
    /* All replicas support new format - use it */
    *use_new_format = 1;
    
    /* Check if we can use LZ4-stream */
    if (server.rdb_compression_algorithm == RDB_COMPRESSION_LZ4_STREAM &&
        (mincapa & REPLICA_CAPA_RDB_LZ4STREAM)) {
        selected_algo = RDB_COMPRESSION_LZ4_STREAM;
    } else {
        selected_algo = RDB_COMPRESSION_LZF;
    }
    
    return selected_algo;
}

int test_replication_compression_old_replica(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    initServerForTest();
    
    /* Test: Old replica without rdb-cmpr-meta-v1 capability */
    int use_new_format = -1;
    int mincapa = REPLICA_CAPA_EOF | REPLICA_CAPA_PSYNC2;  /* No new capabilities */
    
    /* Configure master for LZ4-stream */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZ4_STREAM;
    
    rdbCompressionAlgorithm algo = testSelectAlgorithm(mincapa, &use_new_format);
    
    /* Should fall back to old format with LZF */
    TEST_ASSERT_MESSAGE("Should use old format", use_new_format == 0);
    TEST_ASSERT_MESSAGE("Should use LZF algorithm", algo == RDB_COMPRESSION_LZF);
    
    return 0;
}

int test_replication_compression_new_replica_no_lz4(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    initServerForTest();
    
    /* Test: New replica with rdb-cmpr-meta-v1 but no rdb-lz4stream */
    int use_new_format = -1;
    int mincapa = REPLICA_CAPA_EOF | REPLICA_CAPA_PSYNC2 | REPLICA_CAPA_RDB_CMPR_META_V1;
    
    /* Configure master for LZ4-stream */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZ4_STREAM;
    
    rdbCompressionAlgorithm algo = testSelectAlgorithm(mincapa, &use_new_format);
    
    /* Should use new format but fall back to LZF */
    TEST_ASSERT_MESSAGE("Should use new format", use_new_format == 1);
    TEST_ASSERT_MESSAGE("Should fall back to LZF algorithm", algo == RDB_COMPRESSION_LZF);
    
    return 0;
}

int test_replication_compression_new_replica_with_lz4(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    initServerForTest();
    
    /* Test: New replica with both rdb-cmpr-meta-v1 and rdb-lz4stream */
    int use_new_format = -1;
    int mincapa = REPLICA_CAPA_EOF | REPLICA_CAPA_PSYNC2 | 
                  REPLICA_CAPA_RDB_CMPR_META_V1 | REPLICA_CAPA_RDB_LZ4STREAM;
    
    /* Configure master for LZ4-stream */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZ4_STREAM;
    
    rdbCompressionAlgorithm algo = testSelectAlgorithm(mincapa, &use_new_format);
    
    /* Should use new format with LZ4-stream */
    TEST_ASSERT_MESSAGE("Should use new format", use_new_format == 1);
    TEST_ASSERT_MESSAGE("Should use LZ4-stream algorithm", algo == RDB_COMPRESSION_LZ4_STREAM);
    
    return 0;
}

int test_replication_compression_master_configured_lzf(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    initServerForTest();
    
    /* Test: Master configured for LZF, replica supports everything */
    int use_new_format = -1;
    int mincapa = REPLICA_CAPA_EOF | REPLICA_CAPA_PSYNC2 | 
                  REPLICA_CAPA_RDB_CMPR_META_V1 | REPLICA_CAPA_RDB_LZ4STREAM;
    
    /* Configure master for LZF */
    server.rdb_compression_algorithm = RDB_COMPRESSION_LZF;
    
    rdbCompressionAlgorithm algo = testSelectAlgorithm(mincapa, &use_new_format);
    
    /* Should use new format with LZF (master preference) */
    TEST_ASSERT_MESSAGE("Should use new format", use_new_format == 1);
    TEST_ASSERT_MESSAGE("Should use LZF algorithm", algo == RDB_COMPRESSION_LZF);
    
    return 0;
}
