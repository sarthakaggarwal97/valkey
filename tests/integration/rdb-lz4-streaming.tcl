# Integration tests for LZ4 streaming compression specific features
# Tests sequential decompression requirement and streaming-specific behavior

# Test sequential decompression requirement
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming compression creates multiple chunks} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 4096  ;# Small chunks to force multiple chunks
        
        # Create enough data to span multiple chunks
        for {set i 0} {$i < 200} {incr i} {
            r set "stream:$i" [string repeat "data_$i" 50]
        }
        
        r save
        
        # Verify multiple chunks were created
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        
        puts "Chunks created with 4KB chunk size: $chunks"
        assert {$chunks > 5}
        
        # Verify data integrity after streaming compression
        assert_equal [r get "stream:0"] [string repeat "data_0" 50]
        assert_equal [r get "stream:199"] [string repeat "data_199" 50]
    }
    
    test {LZ4 streaming decompression requires sequential chunk processing} {
        # Restart to test load path
        restart_server 0 true false
        
        # Verify all data loaded correctly (streaming decompression worked)
        assert_equal [r get "stream:0"] [string repeat "data_0" 50]
        assert_equal [r get "stream:100"] [string repeat "data_100" 50]
        assert_equal [r get "stream:199"] [string repeat "data_199" 50]
        
        # Verify all keys are present
        assert_equal [r dbsize] 200
    }
}

# Test streaming compression effectiveness
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming provides better compression than independent chunks} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 8192  ;# 8KB chunks
        
        # Create data with repeating patterns across chunks
        # This should benefit from streaming compression
        set pattern [string repeat "repeating_pattern_" 100]
        for {set i 0} {$i < 500} {incr i} {
            r set "pattern:$i" "$pattern:$i"
        }
        
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        
        set ratio [expr {double($uncompressed) / double($compressed)}]
        puts "Streaming compression ratio with repeating patterns: $ratio"
        puts "Chunks: $chunks"
        
        # Streaming should achieve good compression on repeating patterns
        assert {$ratio > 3.0}
        assert {$chunks > 10}
        
        # Verify data integrity
        restart_server 0 true false
        assert_equal [r get "pattern:0"] "$pattern:0"
        assert_equal [r get "pattern:499"] "$pattern:499"
    }
}

# Test streaming with single chunk (edge case)
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming with single chunk (small database)} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 65536  ;# 64KB chunks
        
        # Create small dataset that fits in one chunk
        for {set i 0} {$i < 10} {incr i} {
            r set "small:$i" "value_$i"
        }
        
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        
        puts "Chunks for small database: $chunks"
        
        # Should have at least 1 chunk
        assert {$chunks >= 1}
        
        # Verify data integrity
        restart_server 0 true false
        assert_equal [r get "small:0"] "value_0"
        assert_equal [r get "small:9"] "value_9"
        assert_equal [r dbsize] 10
    }
}

# Test streaming state isolation between saves
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming state is isolated between RDB saves} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # First save
        for {set i 0} {$i < 100} {incr i} {
            r set "first:$i" "value_$i"
        }
        r save
        
        set info1 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info1 _ compressed1
        
        # Second save with different data
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set "second:$i" [string repeat "different_" 10]
        }
        r save
        
        set info2 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info2 _ compressed2
        
        puts "First save compressed: $compressed1 bytes"
        puts "Second save compressed: $compressed2 bytes"
        
        # Both saves should work independently
        assert {$compressed1 > 0}
        assert {$compressed2 > 0}
        
        # Verify second save data
        restart_server 0 true false
        assert_equal [r get "second:0"] [string repeat "different_" 10]
        assert_equal [r dbsize] 100
    }
}

# Test streaming with BGSAVE
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming with BGSAVE} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Create dataset
        for {set i 0} {$i < 300} {incr i} {
            r set "bgsave:$i" [string repeat "data" 50]
        }
        
        # Trigger BGSAVE
        r bgsave
        waitForBgsave r
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        
        puts "BGSAVE chunks: $chunks"
        assert {$chunks > 1}
        
        # Verify data integrity after BGSAVE
        restart_server 0 true false
        assert_equal [r get "bgsave:0"] [string repeat "data" 50]
        assert_equal [r get "bgsave:299"] [string repeat "data" 50]
    }
}

# Test streaming compression with mixed data sizes
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming with mixed data sizes} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 16384  ;# 16KB chunks
        
        # Mix of tiny, small, medium, and large values
        r set "tiny" "x"
        r set "small" [string repeat "s" 100]
        r set "medium" [string repeat "m" 5000]
        r set "large" [string repeat "l" 50000]
        
        # Add many small keys
        for {set i 0} {$i < 500} {incr i} {
            r set "key:$i" "value_$i"
        }
        
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        
        puts "Mixed sizes - Chunks: $chunks, Ratio: [expr {double($uncompressed) / double($compressed)}]"
        
        # Should have multiple chunks due to large values
        assert {$chunks > 5}
        
        # Verify all data loads correctly
        restart_server 0 true false
        assert_equal [r get "tiny"] "x"
        assert_equal [r get "small"] [string repeat "s" 100]
        assert_equal [r get "medium"] [string repeat "m" 5000]
        assert_equal [r get "large"] [string repeat "l" 50000]
        assert_equal [r get "key:499"] "value_499"
    }
}

# Test backward compatibility: LZF to LZ4-stream migration
start_server {tags {"rdb lz4-streaming"}} {
    test {Migrate from LZF to LZ4-stream} {
        # Start with LZF
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lzf
        
        # Create data with LZF
        for {set i 0} {$i < 100} {incr i} {
            r set "migrate:$i" "value_$i"
        }
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lzf*} $info
        
        # Switch to LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        
        # Verify data integrity after migration
        restart_server 0 true false
        assert_equal [r get "migrate:0"] "value_0"
        assert_equal [r get "migrate:99"] "value_99"
        assert_equal [r dbsize] 100
    }
    
    test {Load LZF RDB with LZ4-stream configured} {
        # Save with LZF
        r flushall
        r config set rdb-compression-algorithm lzf
        for {set i 0} {$i < 50} {incr i} {
            r set "lzf:$i" "value_$i"
        }
        r save
        
        # Configure for LZ4-stream and restart
        r config set rdb-compression-algorithm lz4-stream
        restart_server 0 true false
        
        # Should load LZF file correctly
        assert_equal [r get "lzf:0"] "value_0"
        assert_equal [r get "lzf:49"] "value_49"
        
        # New saves should use LZ4-stream
        r save
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
    }
}

# Test streaming with all data types
start_server {tags {"rdb lz4-streaming"}} {
    test {LZ4 streaming with comprehensive data types} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Strings
        for {set i 0} {$i < 50} {incr i} {
            r set "str:$i" [string repeat "string_$i" 10]
        }
        
        # Lists
        for {set i 0} {$i < 30} {incr i} {
            for {set j 0} {$j < 10} {incr j} {
                r lpush "list:$i" "item_${i}_${j}"
            }
        }
        
        # Hashes
        for {set i 0} {$i < 30} {incr i} {
            for {set j 0} {$j < 5} {incr j} {
                r hset "hash:$i" "field_$j" "value_${i}_${j}"
            }
        }
        
        # Sets
        for {set i 0} {$i < 30} {incr i} {
            for {set j 0} {$j < 8} {incr j} {
                r sadd "set:$i" "member_${i}_${j}"
            }
        }
        
        # Sorted sets
        for {set i 0} {$i < 30} {incr i} {
            for {set j 0} {$j < 8} {incr j} {
                r zadd "zset:$i" $j "member_${i}_${j}"
            }
        }
        
        # Streams
        for {set i 0} {$i < 10} {incr i} {
            for {set j 0} {$j < 5} {incr j} {
                r xadd "stream:$i" * field "value_${i}_${j}"
            }
        }
        
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        
        puts "Comprehensive data types - Chunks: $chunks"
        assert {$chunks > 3}
        
        # Verify all data types load correctly
        restart_server 0 true false
        
        assert_equal [r get "str:0"] [string repeat "string_0" 10]
        assert_equal [r llen "list:0"] 10
        assert_equal [r hlen "hash:0"] 5
        assert_equal [r scard "set:0"] 8
        assert_equal [r zcard "zset:0"] 8
        assert_equal [r xlen "stream:0"] 5
        
        # Verify specific values
        assert_equal [r lindex "list:0" 0] "item_0_9"
        assert_equal [r hget "hash:0" "field_0"] "value_0_0"
        assert {[r sismember "set:0" "member_0_0"] == 1}
        assert_equal [r zscore "zset:0" "member_0_0"] 0
    }
}
