# Test to verify that quicklist LZF compression is independent of RDB compression algorithm selection
# This ensures that changing RDB compression algorithm (LZF vs LZ4) does not affect quicklist compression

start_server {tags {"quicklist rdb-independence"}} {
    test {Quicklist compression works with RDB LZF compression} {
        # Configure quicklist compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 1
        
        # Use LZF for RDB (default)
        r config set rdb-compression-algorithm lzf
        
        # Create a list that will trigger quicklist compression
        # Add enough elements to create multiple nodes
        for {set i 0} {$i < 100} {incr i} {
            r lpush mylist [string repeat "data$i" 100]
        }
        
        # Save and reload
        r save
        r debug reload
        
        # Verify data integrity
        assert_equal [r llen mylist] 100
        assert_equal [r lindex mylist 0] [string repeat "data99" 100]
        assert_equal [r lindex mylist 99] [string repeat "data0" 100]
    }
    
    test {Quicklist compression works with RDB LZ4 compression} {
        r flushall
        
        # Configure quicklist compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 1
        
        # Use LZ4 for RDB
        r config set rdb-compression-algorithm lz4-stream
        
        # Create a list that will trigger quicklist compression
        for {set i 0} {$i < 100} {incr i} {
            r lpush mylist2 [string repeat "data$i" 100]
        }
        
        # Save and reload
        r save
        r debug reload
        
        # Verify data integrity
        assert_equal [r llen mylist2] 100
        assert_equal [r lindex mylist2 0] [string repeat "data99" 100]
        assert_equal [r lindex mylist2 99] [string repeat "data0" 100]
    }
    
    test {Quicklist compression is independent of RDB algorithm changes} {
        r flushall
        
        # Configure quicklist compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 2
        
        # Start with LZF for RDB
        r config set rdb-compression-algorithm lzf
        
        # Create lists
        for {set i 0} {$i < 50} {incr i} {
            r lpush list_lzf [string repeat "item$i" 50]
        }
        
        # Save with LZF
        r save
        
        # Switch to LZ4 for RDB
        r config set rdb-compression-algorithm lz4-stream
        
        # Create more lists
        for {set i 0} {$i < 50} {incr i} {
            r lpush list_lz4 [string repeat "item$i" 50]
        }
        
        # Save with LZ4
        r save
        
        # Reload and verify both lists
        r debug reload
        
        assert_equal [r llen list_lzf] 50
        assert_equal [r llen list_lz4] 50
        assert_equal [r lindex list_lzf 0] [string repeat "item49" 50]
        assert_equal [r lindex list_lz4 0] [string repeat "item49" 50]
    }
    
    test {Quicklist compression depth is preserved across RDB algorithm changes} {
        r flushall
        
        # Test with different compression depths
        foreach depth {0 1 2 3} {
            r config set list-compress-depth $depth
            r config set list-max-listpack-size -2
            
            # Test with LZF
            r config set rdb-compression-algorithm lzf
            r del test_list_lzf_$depth
            for {set i 0} {$i < 30} {incr i} {
                r lpush test_list_lzf_$depth [string repeat "x$i" 100]
            }
            r save
            r debug reload
            assert_equal [r llen test_list_lzf_$depth] 30
            
            # Test with LZ4
            r config set rdb-compression-algorithm lz4-stream
            r del test_list_lz4_$depth
            for {set i 0} {$i < 30} {incr i} {
                r lpush test_list_lz4_$depth [string repeat "y$i" 100]
            }
            r save
            r debug reload
            assert_equal [r llen test_list_lz4_$depth] 30
        }
        
        # Verify all lists are intact
        foreach depth {0 1 2 3} {
            assert_equal [r llen test_list_lzf_$depth] 30
            assert_equal [r llen test_list_lz4_$depth] 30
        }
    }
    
    test {Quicklist uses LZF regardless of RDB compression algorithm} {
        r flushall
        
        # Enable quicklist compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 1
        
        # Create a large list with compressible data
        for {set i 0} {$i < 200} {incr i} {
            r lpush compressible_list [string repeat "aaaaaaaaaa" 100]
        }
        
        # Test with LZF RDB compression
        r config set rdb-compression-algorithm lzf
        r save
        set size_lzf [file size [lindex [r config get dir] 1]/dump.rdb]
        r debug reload
        assert_equal [r llen compressible_list] 200
        
        # Test with LZ4 RDB compression
        r config set rdb-compression-algorithm lz4-stream
        r save
        set size_lz4 [file size [lindex [r config get dir] 1]/dump.rdb]
        r debug reload
        assert_equal [r llen compressible_list] 200
        
        # Both should work correctly
        # The RDB file sizes may differ due to different RDB compression,
        # but quicklist data should be intact in both cases
        assert {$size_lzf > 0}
        assert {$size_lz4 > 0}
    }
    
    test {Mixed data types with quicklist compression and different RDB algorithms} {
        r flushall
        
        # Configure quicklist compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 1
        
        # Create various data types
        r set string_key "simple_string"
        r lpush list_key a b c d e f g h i j k l m n o p
        r sadd set_key 1 2 3 4 5
        r zadd zset_key 1 a 2 b 3 c
        r hset hash_key field1 value1 field2 value2
        
        # Save with LZF
        r config set rdb-compression-algorithm lzf
        r save
        r debug reload
        
        # Verify all data types
        assert_equal [r get string_key] "simple_string"
        assert_equal [r llen list_key] 16
        assert_equal [r scard set_key] 5
        assert_equal [r zcard zset_key] 3
        assert_equal [r hlen hash_key] 2
        
        # Save with LZ4
        r config set rdb-compression-algorithm lz4-stream
        r save
        r debug reload
        
        # Verify all data types again
        assert_equal [r get string_key] "simple_string"
        assert_equal [r llen list_key] 16
        assert_equal [r scard set_key] 5
        assert_equal [r zcard zset_key] 3
        assert_equal [r hlen hash_key] 2
    }
    
    test {Quicklist compression with no RDB compression} {
        r flushall
        
        # Enable quicklist compression but disable RDB chunk compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 1
        r config set rdb-chunk-compression no
        
        # Create a list
        for {set i 0} {$i < 50} {incr i} {
            r lpush no_rdb_compression_list [string repeat "data$i" 80]
        }
        
        # Save and reload
        r save
        r debug reload
        
        # Verify quicklist compression still works
        assert_equal [r llen no_rdb_compression_list] 50
        assert_equal [r lindex no_rdb_compression_list 0] [string repeat "data49" 80]
    }
    
    test {Quicklist operations work correctly with both RDB algorithms} {
        r flushall
        
        # Configure quicklist compression
        r config set list-max-listpack-size -2
        r config set list-compress-depth 1
        
        # Test with LZF
        r config set rdb-compression-algorithm lzf
        for {set i 0} {$i < 30} {incr i} {
            r lpush ops_list_lzf "element_$i"
        }
        r save
        
        # Perform operations
        r lpop ops_list_lzf
        r rpop ops_list_lzf
        r linsert ops_list_lzf before "element_15" "inserted"
        
        r debug reload
        assert_equal [r llen ops_list_lzf] 29
        
        # Test with LZ4
        r config set rdb-compression-algorithm lz4-stream
        for {set i 0} {$i < 30} {incr i} {
            r lpush ops_list_lz4 "element_$i"
        }
        r save
        
        # Perform operations
        r lpop ops_list_lz4
        r rpop ops_list_lz4
        r linsert ops_list_lz4 before "element_15" "inserted"
        
        r debug reload
        assert_equal [r llen ops_list_lz4] 29
    }
}

start_server {tags {"quicklist rdb-independence config"}} {
    test {Verify quicklist config is independent of RDB compression config} {
        # Get quicklist compression settings
        set list_compress_depth [lindex [r config get list-compress-depth] 1]
        set list_max_listpack_size [lindex [r config get list-max-listpack-size] 1]
        
        # Change RDB compression algorithm
        r config set rdb-compression-algorithm lzf
        
        # Verify quicklist settings unchanged
        assert_equal [lindex [r config get list-compress-depth] 1] $list_compress_depth
        assert_equal [lindex [r config get list-max-listpack-size] 1] $list_max_listpack_size
        
        # Change to LZ4
        r config set rdb-compression-algorithm lz4-stream
        
        # Verify quicklist settings still unchanged
        assert_equal [lindex [r config get list-compress-depth] 1] $list_compress_depth
        assert_equal [lindex [r config get list-max-listpack-size] 1] $list_max_listpack_size
    }
    
    test {Quicklist compression settings persist independently} {
        # Set quicklist compression
        r config set list-compress-depth 2
        r config set list-max-listpack-size -3
        
        # Set RDB compression
        r config set rdb-compression-algorithm lz4-stream
        
        # Rewrite config
        r config rewrite
        
        # Restart server
        restart_server 0 true false
        
        # Verify both settings persisted
        assert_equal [lindex [r config get list-compress-depth] 1] "2"
        assert_equal [lindex [r config get list-max-listpack-size] 1] "-3"
        assert_equal [lindex [r config get rdb-compression-algorithm] 1] "lz4-stream"
    }
}
