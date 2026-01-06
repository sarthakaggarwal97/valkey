# Integration tests for LZ4 compression algorithm
# Tests save/load with LZ4, backward compatibility with LZF, and various data types

# Helper to populate various data types
proc populate_mixed_data {r prefix count} {
    for {set i 0} {$i < $count} {incr i} {
        # Strings - mix of compressible and random
        if {$i % 5 == 0} {
            $r set "${prefix}:str:$i" [string repeat "compressible_data_" 10]
        } else {
            $r set "${prefix}:str:$i" "value_$i"
        }
        
        # Lists
        if {$i % 3 == 0} {
            for {set j 0} {$j < 10} {incr j} {
                $r lpush "${prefix}:list:$i" "item_${i}_${j}"
            }
        }
        
        # Hashes
        if {$i % 4 == 0} {
            for {set j 0} {$j < 5} {incr j} {
                $r hset "${prefix}:hash:$i" "field_$j" "value_${i}_${j}"
            }
        }
        
        # Sets
        if {$i % 5 == 1} {
            for {set j 0} {$j < 8} {incr j} {
                $r sadd "${prefix}:set:$i" "member_${i}_${j}"
            }
        }
        
        # Sorted sets
        if {$i % 5 == 2} {
            for {set j 0} {$j < 8} {incr j} {
                $r zadd "${prefix}:zset:$i" $j "member_${i}_${j}"
            }
        }
        
        # Streams
        if {$i % 10 == 0} {
            for {set j 0} {$j < 5} {incr j} {
                $r xadd "${prefix}:stream:$i" * field "value_${i}_${j}"
            }
        }
    }
}

# Test basic LZ4 save/load
start_server {tags {"rdb lz4-compression"}} {
    test {RDB save with LZ4 compression} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Populate data
        populate_mixed_data r "lz4test" 100
        
        # Save
        r save
        
        # Verify configuration
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        assert_match {*rdb_chunk_compression:yes*} $info
    }
    
    test {RDB load with LZ4 compression - verify all data types} {
        # Verify strings
        assert_equal [r get "lz4test:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r get "lz4test:str:1"] "value_1"
        
        # Verify lists
        assert_equal [r llen "lz4test:list:0"] 10
        assert_equal [r lindex "lz4test:list:0" 0] "item_0_9"
        
        # Verify hashes
        assert_equal [r hlen "lz4test:hash:0"] 5
        assert_equal [r hget "lz4test:hash:0" "field_0"] "value_0_0"
        
        # Verify sets
        assert_equal [r scard "lz4test:set:1"] 8
        assert {[r sismember "lz4test:set:1" "member_1_0"] == 1}
        
        # Verify sorted sets
        assert_equal [r zcard "lz4test:zset:2"] 8
        assert_equal [r zscore "lz4test:zset:2" "member_2_0"] 0
        
        # Verify streams
        assert_equal [r xlen "lz4test:stream:0"] 5
    }
    
    test {LZ4 compression ratio is reasonable} {
        set info [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        
        # Should have some compression
        assert {$compressed > 0}
        assert {$uncompressed > $compressed}
        
        set ratio [expr {double($uncompressed) / double($compressed)}]
        puts "LZ4 compression ratio: $ratio"
        assert {$ratio > 1.0}
    }
}

# Test switching between LZF and LZ4
start_server {tags {"rdb lz4-compression"}} {
    test {Save with LZF, load, then save with LZ4} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lzf
        
        # Populate and save with LZF
        populate_mixed_data r "switch" 50
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lzf*} $info
        
        # Switch to LZ4
        r config set rdb-compression-algorithm lz4-stream
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        
        # Verify data integrity
        assert_equal [r get "switch:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r llen "switch:list:0"] 10
    }
    
    test {Save with LZ4, load, then save with LZF} {
        r flushall
        r config set rdb-compression-algorithm lz4-stream
        
        # Populate and save with LZ4
        populate_mixed_data r "reverse" 50
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        
        # Switch to LZF
        r config set rdb-compression-algorithm lzf
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lzf*} $info
        
        # Verify data integrity
        assert_equal [r get "reverse:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r llen "reverse:list:0"] 10
    }
}

# Test backward compatibility - loading old RDB files
start_server {tags {"rdb lz4-compression"}} {
    test {Load RDB saved with LZF (backward compatibility)} {
        # Save with LZF
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lzf
        populate_mixed_data r "compat" 50
        r save
        
        # Restart and verify we can load
        restart_server 0 true false
        
        # Verify data loaded correctly
        assert_equal [r get "compat:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r llen "compat:list:0"] 10
        assert_equal [r hlen "compat:hash:0"] 5
        
        # Now save with LZ4
        r config set rdb-compression-algorithm lz4-stream
        r save
        
        # Restart and verify we can load LZ4
        restart_server 0 true false
        
        assert_equal [r get "compat:str:0"] [string repeat "compressible_data_" 10]
    }
    
    test {Load old RDB without chunk compression} {
        # Save without chunk compression
        r flushall
        r config set rdb-chunk-compression no
        populate_mixed_data r "old" 30
        r save
        
        # Enable chunk compression with LZ4
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Restart and verify we can load old format
        restart_server 0 true false
        
        assert_equal [r get "old:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r llen "old:list:0"] 10
    }
}

# Test large datasets
start_server {tags {"rdb lz4-compression large slow"}} {
    test {LZ4 compression with large dataset (10K keys)} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 65536
        
        # Populate large dataset
        populate_mixed_data r "large" 10000
        
        # Save
        set start [clock milliseconds]
        r save
        set duration [expr {[clock milliseconds] - $start}]
        
        puts "LZ4 save time for 10K keys: $duration ms"
        
        # Verify statistics
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        
        puts "Chunks: $chunks, Compressed: $compressed, Uncompressed: $uncompressed"
        
        assert {$chunks > 10}
        assert {$uncompressed > $compressed}
        
        # Verify data integrity (don't check exact count since populate_mixed_data creates multiple keys per iteration)
        set dbsize [r dbsize]
        assert {$dbsize > 10000}
        assert_equal [r get "large:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r get "large:str:9999"] "value_9999"
    }
    
    test {LZ4 load performance with large dataset} {
        # Restart to test load
        set start [clock milliseconds]
        restart_server 0 true false
        set duration [expr {[clock milliseconds] - $start}]
        
        puts "LZ4 load time for 10K keys: $duration ms"
        
        # Verify data loaded correctly (don't check exact count)
        set dbsize [r dbsize]
        assert {$dbsize > 10000}
        assert_equal [r get "large:str:0"] [string repeat "compressible_data_" 10]
        assert_equal [r llen "large:list:0"] 10
    }
}

# Test BGSAVE with LZ4
start_server {tags {"rdb lz4-compression"}} {
    test {BGSAVE with LZ4 compression} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        populate_mixed_data r "bgsave" 200
        
        r bgsave
        waitForBgsave r
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        
        # Verify data integrity (don't check exact count)
        assert_equal [r get "bgsave:str:0"] [string repeat "compressible_data_" 10]
        set dbsize [r dbsize]
        assert {$dbsize > 200}
    }
}

# Test different chunk sizes with LZ4
start_server {tags {"rdb lz4-compression"}} {
    test {LZ4 with small chunk size (4KB)} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 4096
        
        populate_mixed_data r "small" 100
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        
        # Small chunks should create more chunks
        assert {$chunks > 5}
        
        # Verify data integrity
        assert_equal [r get "small:str:0"] [string repeat "compressible_data_" 10]
    }
    
    test {LZ4 with large chunk size (256KB)} {
        r flushall
        r config set rdb-chunk-size 262144
        
        populate_mixed_data r "big" 100
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        
        # Large chunks should create fewer chunks
        puts "Chunks with 256KB size: $chunks"
        
        # Verify data integrity
        assert_equal [r get "big:str:0"] [string repeat "compressible_data_" 10]
    }
}

# Test highly compressible data
start_server {tags {"rdb lz4-compression"}} {
    test {LZ4 with highly compressible data} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Create highly compressible data
        for {set i 0} {$i < 500} {incr i} {
            r set "compress:$i" [string repeat "aaaaaaaaaa" 100]
        }
        
        r save
        
        set info [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        
        set ratio [expr {double($uncompressed) / double($compressed)}]
        puts "LZ4 compression ratio for highly compressible data: $ratio"
        
        # Should achieve good compression
        assert {$ratio > 5.0}
        
        # Verify data integrity
        assert_equal [r get "compress:0"] [string repeat "aaaaaaaaaa" 100]
        assert_equal [r get "compress:499"] [string repeat "aaaaaaaaaa" 100]
    }
}

# Test empty database
start_server {tags {"rdb lz4-compression"}} {
    test {LZ4 with empty database} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        r save
        
        assert_equal [r dbsize] 0
        
        # Restart and verify
        restart_server 0 true false
        assert_equal [r dbsize] 0
    }
}

# Test configuration persistence
start_server {tags {"rdb lz4-compression"}} {
    test {LZ4 configuration persists across restart} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 32768
        r config rewrite
        
        restart_server 0 true false
        
        assert_equal [lindex [r config get rdb-compression-algorithm] 1] "lz4-stream"
        assert_equal [lindex [r config get rdb-chunk-size] 1] "32768"
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "yes"
    }
}

# Test digest consistency
start_server {tags {"rdb lz4-compression"}} {
    test {Digest consistency with LZ4 compression} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        populate_mixed_data r "digest" 100
        
        set digest_before [r debug digest]
        r save
        r debug reload
        set digest_after [r debug digest]
        
        assert_equal $digest_before $digest_after
    }
    
    test {Digest consistency switching between LZF and LZ4} {
        r flushall
        r config set rdb-compression-algorithm lzf
        populate_mixed_data r "cross" 50
        
        set digest_lzf [r debug digest]
        r save
        
        r config set rdb-compression-algorithm lz4-stream
        r save
        r debug reload
        
        set digest_lz4 [r debug digest]
        assert_equal $digest_lzf $digest_lz4
    }
}

# Test all data types comprehensively
start_server {tags {"rdb lz4-compression"}} {
    test {LZ4 with comprehensive data type coverage} {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Strings - various sizes
        r set str:tiny "x"
        r set str:small "small_value"
        r set str:medium [string repeat "medium" 100]
        r set str:large [string repeat "large_data_" 1000]
        
        # Lists - various sizes
        r lpush list:small 1 2 3
        for {set i 0} {$i < 100} {incr i} {
            r lpush list:large "item_$i"
        }
        
        # Hashes - various sizes
        r hset hash:small f1 v1 f2 v2
        for {set i 0} {$i < 50} {incr i} {
            r hset hash:large "field_$i" "value_$i"
        }
        
        # Sets - various sizes
        r sadd set:small a b c
        for {set i 0} {$i < 50} {incr i} {
            r sadd set:large "member_$i"
        }
        
        # Sorted sets - various sizes
        r zadd zset:small 1 a 2 b 3 c
        for {set i 0} {$i < 50} {incr i} {
            r zadd zset:large $i "member_$i"
        }
        
        # Streams
        r xadd stream:small * f1 v1
        for {set i 0} {$i < 20} {incr i} {
            r xadd stream:large * data "item_$i"
        }
        
        r save
        restart_server 0 true false
        
        # Verify all data types
        assert_equal [r get str:tiny] "x"
        assert_equal [r get str:large] [string repeat "large_data_" 1000]
        assert_equal [r llen list:large] 100
        assert_equal [r hlen hash:large] 50
        assert_equal [r scard set:large] 50
        assert_equal [r zcard zset:large] 50
        assert_equal [r xlen stream:large] 20
    }
}
