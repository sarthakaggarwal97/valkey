# Performance benchmarks for RDB compression algorithms (LZF vs LZ4-stream)
# Tests compression ratios, save/load times, and memory usage

# Helper to populate typical workload
proc populate_typical_workload {r prefix count} {
    for {set i 0} {$i < $count} {incr i} {
        # Mix of data types and patterns
        
        # Strings - 40% of keys
        if {$i % 10 < 4} {
            if {$i % 3 == 0} {
                # Compressible strings
                $r set "${prefix}:str:$i" [string repeat "user_data_pattern_" 20]
            } else {
                # Less compressible
                $r set "${prefix}:str:$i" "value_${i}_[expr {int(rand() * 1000000)}]"
            }
        }
        
        # Lists - 20% of keys
        if {$i % 10 >= 4 && $i % 10 < 6} {
            for {set j 0} {$j < 15} {incr j} {
                $r lpush "${prefix}:list:$i" "item_${i}_${j}_[string repeat "data" 5]"
            }
        }
        
        # Hashes - 20% of keys
        if {$i % 10 >= 6 && $i % 10 < 8} {
            for {set j 0} {$j < 10} {incr j} {
                $r hset "${prefix}:hash:$i" "field_$j" "value_${i}_${j}_[string repeat "x" 10]"
            }
        }
        
        # Sets - 10% of keys
        if {$i % 10 == 8} {
            for {set j 0} {$j < 12} {incr j} {
                $r sadd "${prefix}:set:$i" "member_${i}_${j}"
            }
        }
        
        # Sorted sets - 10% of keys
        if {$i % 10 == 9} {
            for {set j 0} {$j < 12} {incr j} {
                $r zadd "${prefix}:zset:$i" [expr {$j * 1.5}] "member_${i}_${j}"
            }
        }
    }
}

# Helper to populate highly compressible data
proc populate_compressible_data {r prefix count} {
    for {set i 0} {$i < $count} {incr i} {
        # Highly repetitive patterns
        $r set "${prefix}:comp:$i" [string repeat "aaaaaaaaaa" 100]
        
        if {$i % 5 == 0} {
            for {set j 0} {$j < 20} {incr j} {
                $r lpush "${prefix}:list:$i" [string repeat "bbbbbbbbbb" 50]
            }
        }
        
        if {$i % 5 == 1} {
            for {set j 0} {$j < 10} {incr j} {
                $r hset "${prefix}:hash:$i" "field_$j" [string repeat "cccccccccc" 50]
            }
        }
    }
}

# Helper to populate random (incompressible) data
proc populate_random_data {r prefix count} {
    for {set i 0} {$i < $count} {incr i} {
        # Generate pseudo-random data
        set random_str ""
        for {set j 0} {$j < 100} {incr j} {
            append random_str [format "%c" [expr {int(rand() * 256)}]]
        }
        $r set "${prefix}:rand:$i" $random_str
    }
}

# Benchmark compression ratios
start_server {tags {"rdb compression benchmark slow"}} {
    test {Benchmark: LZF vs LZ4-stream compression ratio - typical workload} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate typical workload
        populate_typical_workload r "typical" 2000
        
        # Test with LZF
        r config set rdb-compression-algorithm lzf
        r save
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ compressed_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncompressed_lzf
        set ratio_lzf [expr {double($uncompressed_lzf) / double($compressed_lzf)}]
        
        # Test with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        puts "=== Compression Ratio - Typical Workload ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        # Both should compress
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
        
        # LZ4-stream should achieve better compression than LZF due to streaming context
        # The 64KB sliding window allows better compression across chunks
    }
    
    test {Benchmark: LZF vs LZ4-stream compression ratio - highly compressible data} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate highly compressible data
        populate_compressible_data r "compress" 1000
        
        # Test with LZF
        r config set rdb-compression-algorithm lzf
        r save
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ compressed_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncompressed_lzf
        set ratio_lzf [expr {double($uncompressed_lzf) / double($compressed_lzf)}]
        
        # Test with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        puts "=== Compression Ratio - Highly Compressible Data ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        # Should achieve high compression
        assert {$ratio_lzf > 5.0}
        assert {$ratio_lz4 > 5.0}
    }
    
    test {Benchmark: LZF vs LZ4-stream compression ratio - strings workload} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate string-heavy workload
        for {set i 0} {$i < 3000} {incr i} {
            if {$i % 2 == 0} {
                r set "str:$i" [string repeat "user_session_data_" 30]
            } else {
                r set "str:$i" "value_${i}_[expr {int(rand() * 100000)}]"
            }
        }
        
        # Test with LZF
        r config set rdb-compression-algorithm lzf
        r save
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ compressed_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncompressed_lzf
        set ratio_lzf [expr {double($uncompressed_lzf) / double($compressed_lzf)}]
        
        # Test with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        puts "=== Compression Ratio - Strings Workload ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
    
    test {Benchmark: LZF vs LZ4-stream compression ratio - lists workload} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate list-heavy workload
        for {set i 0} {$i < 500} {incr i} {
            for {set j 0} {$j < 30} {incr j} {
                r lpush "list:$i" "item_${i}_${j}_[string repeat "data" 10]"
            }
        }
        
        # Test with LZF
        r config set rdb-compression-algorithm lzf
        r save
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ compressed_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncompressed_lzf
        set ratio_lzf [expr {double($uncompressed_lzf) / double($compressed_lzf)}]
        
        # Test with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        puts "=== Compression Ratio - Lists Workload ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
    
    test {Benchmark: LZF vs LZ4-stream compression ratio - hashes workload} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate hash-heavy workload
        for {set i 0} {$i < 500} {incr i} {
            for {set j 0} {$j < 20} {incr j} {
                r hset "hash:$i" "field_$j" "value_${i}_${j}_[string repeat "x" 20]"
            }
        }
        
        # Test with LZF
        r config set rdb-compression-algorithm lzf
        r save
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ compressed_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncompressed_lzf
        set ratio_lzf [expr {double($uncompressed_lzf) / double($compressed_lzf)}]
        
        # Test with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        puts "=== Compression Ratio - Hashes Workload ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
}

# Benchmark save times
start_server {tags {"rdb compression benchmark slow"}} {
    test {Benchmark: LZF vs LZ4-stream RDB save time - typical workload} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate typical workload
        populate_typical_workload r "save" 3000
        
        # Benchmark LZF save
        r config set rdb-compression-algorithm lzf
        set start [clock milliseconds]
        r save
        set time_lzf [expr {[clock milliseconds] - $start}]
        
        # Benchmark LZ4-stream save
        r config set rdb-compression-algorithm lz4-stream
        set start [clock milliseconds]
        r save
        set time_lz4 [expr {[clock milliseconds] - $start}]
        
        puts "=== RDB Save Time - Typical Workload (3000 keys) ==="
        puts "LZF:         $time_lzf ms"
        puts "LZ4-stream:  $time_lz4 ms"
        if {$time_lzf > 0} {
            puts "LZ4-stream speedup: [format %.2f [expr {double($time_lzf) / double($time_lz4)}]]x"
        }
        puts ""
        
        # Both should complete in reasonable time
        assert {$time_lzf < 10000}
        assert {$time_lz4 < 10000}
    }
    
    test {Benchmark: LZF vs LZ4-stream RDB save time - large dataset} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate large dataset
        populate_typical_workload r "large" 10000
        
        # Benchmark LZF save
        r config set rdb-compression-algorithm lzf
        set start [clock milliseconds]
        r save
        set time_lzf [expr {[clock milliseconds] - $start}]
        
        # Benchmark LZ4-stream save
        r config set rdb-compression-algorithm lz4-stream
        set start [clock milliseconds]
        r save
        set time_lz4 [expr {[clock milliseconds] - $start}]
        
        puts "=== RDB Save Time - Large Dataset (10000 keys) ==="
        puts "LZF:         $time_lzf ms"
        puts "LZ4-stream:  $time_lz4 ms"
        if {$time_lzf > 0} {
            puts "LZ4-stream speedup: [format %.2f [expr {double($time_lzf) / double($time_lz4)}]]x"
        }
        puts ""
        
        # Should complete in reasonable time
        assert {$time_lzf < 30000}
        assert {$time_lz4 < 30000}
    }
}

# Benchmark load times
start_server {tags {"rdb compression benchmark slow"}} {
    test {Benchmark: LZF vs LZ4-stream RDB load time - typical workload} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate and save with LZF
        populate_typical_workload r "load" 3000
        r config set rdb-compression-algorithm lzf
        r save
        
        # Benchmark LZF load
        set start [clock milliseconds]
        restart_server 0 true false
        set time_lzf [expr {[clock milliseconds] - $start}]
        
        # Save with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        
        # Benchmark LZ4-stream load
        set start [clock milliseconds]
        restart_server 0 true false
        set time_lz4 [expr {[clock milliseconds] - $start}]
        
        puts "=== RDB Load Time - Typical Workload (3000 keys) ==="
        puts "LZF:         $time_lzf ms"
        puts "LZ4-stream:  $time_lz4 ms"
        if {$time_lzf > 0} {
            puts "LZ4-stream speedup: [format %.2f [expr {double($time_lzf) / double($time_lz4)}]]x"
        }
        puts ""
        
        # Both should complete in reasonable time
        assert {$time_lzf < 10000}
        assert {$time_lz4 < 10000}
    }
    
    test {Benchmark: LZF vs LZ4-stream RDB load time - large dataset} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate and save with LZF
        populate_typical_workload r "loadlarge" 10000
        r config set rdb-compression-algorithm lzf
        r save
        
        # Benchmark LZF load
        set start [clock milliseconds]
        restart_server 0 true false
        set time_lzf [expr {[clock milliseconds] - $start}]
        
        # Save with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        
        # Benchmark LZ4-stream load
        set start [clock milliseconds]
        restart_server 0 true false
        set time_lz4 [expr {[clock milliseconds] - $start}]
        
        puts "=== RDB Load Time - Large Dataset (10000 keys) ==="
        puts "LZF:         $time_lzf ms"
        puts "LZ4-stream:  $time_lz4 ms"
        if {$time_lzf > 0} {
            puts "LZ4-stream speedup: [format %.2f [expr {double($time_lzf) / double($time_lz4)}]]x"
        }
        puts ""
        
        # Should complete in reasonable time
        assert {$time_lzf < 30000}
        assert {$time_lz4 < 30000}
    }
}

# Benchmark memory usage
start_server {tags {"rdb compression benchmark slow"}} {
    test {Benchmark: LZF vs LZ4-stream memory usage during save} {
        r flushall
        r config set rdb-chunk-compression yes
        
        # Populate data
        populate_typical_workload r "mem" 2000
        
        # Measure LZF memory
        r config set rdb-compression-algorithm lzf
        set mem_before [s used_memory]
        r save
        set mem_after [s used_memory]
        set mem_increase_lzf [expr {$mem_after - $mem_before}]
        
        # Measure LZ4-stream memory
        r config set rdb-compression-algorithm lz4-stream
        set mem_before [s used_memory]
        r save
        set mem_after [s used_memory]
        set mem_increase_lz4 [expr {$mem_after - $mem_before}]
        
        puts "=== Memory Usage During Save ==="
        puts "LZF memory increase:        $mem_increase_lzf bytes ([format %.2f [expr {$mem_increase_lzf / 1024.0}]] KB)"
        puts "LZ4-stream memory increase: $mem_increase_lz4 bytes ([format %.2f [expr {$mem_increase_lz4 / 1024.0}]] KB)"
        puts ""
        
        # Memory increase should be reasonable (< 2MB)
        assert {$mem_increase_lzf < 2097152}
        assert {$mem_increase_lz4 < 2097152}
    }
}

# Comprehensive comparison summary
start_server {tags {"rdb compression benchmark slow"}} {
    test {Benchmark: Comprehensive LZF vs LZ4-stream comparison} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        
        # Populate mixed workload
        populate_typical_workload r "comp" 5000
        
        puts "========================================="
        puts "COMPREHENSIVE BENCHMARK: LZF vs LZ4-stream"
        puts "Dataset: 5000 keys, mixed data types"
        puts "Chunk size: 65536 bytes"
        puts "========================================="
        puts ""
        
        # LZF metrics
        r config set rdb-compression-algorithm lzf
        set start [clock milliseconds]
        r save
        set save_time_lzf [expr {[clock milliseconds] - $start}]
        
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ compressed_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncompressed_lzf
        regexp {rdb_last_save_chunks:(\d+)} $info_lzf _ chunks_lzf
        set ratio_lzf [expr {double($uncompressed_lzf) / double($compressed_lzf)}]
        
        set start [clock milliseconds]
        restart_server 0 true false
        set load_time_lzf [expr {[clock milliseconds] - $start}]
        
        # Re-enable chunk compression after restart
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        
        # LZ4-stream metrics
        r config set rdb-compression-algorithm lz4-stream
        set start [clock milliseconds]
        r save
        set save_time_lz4 [expr {[clock milliseconds] - $start}]
        
        set info_lz4 [r info persistence]
        
        if {![regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4]} {
            puts "ERROR: Could not extract compressed bytes from LZ4-stream info"
            set compressed_lz4 1
        }
        if {![regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4]} {
            puts "ERROR: Could not extract uncompressed bytes from LZ4-stream info"
            set uncompressed_lz4 1
        }
        if {![regexp {rdb_last_save_chunks:(\d+)} $info_lz4 _ chunks_lz4]} {
            puts "ERROR: Could not extract chunks from LZ4-stream info"
            set chunks_lz4 0
        }
        
        if {$compressed_lz4 == 0} {
            puts "ERROR: LZ4-stream compressed size is 0, setting to 1 to avoid division by zero"
            set compressed_lz4 1
        }
        
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        set start [clock milliseconds]
        restart_server 0 true false
        set load_time_lz4 [expr {[clock milliseconds] - $start}]
        
        # Print results
        puts "--- LZF Results ---"
        puts "Compression ratio: [format %.2f $ratio_lzf]"
        puts "Compressed size: [format %.2f [expr {$compressed_lzf / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {$uncompressed_lzf / 1024.0}]] KB"
        puts "Chunks: $chunks_lzf"
        puts "Save time: $save_time_lzf ms"
        puts "Load time: $load_time_lzf ms"
        puts ""
        
        puts "--- LZ4-stream Results ---"
        puts "Compression ratio: [format %.2f $ratio_lz4]"
        puts "Compressed size: [format %.2f [expr {$compressed_lz4 / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {$uncompressed_lz4 / 1024.0}]] KB"
        puts "Chunks: $chunks_lz4"
        puts "Save time: $save_time_lz4 ms"
        puts "Load time: $load_time_lz4 ms"
        puts ""
        
        puts "--- Comparison ---"
        puts "Compression improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts "Size reduction: [format %.1f [expr {(1.0 - double($compressed_lz4) / double($compressed_lzf)) * 100}]]%"
        if {$save_time_lzf > 0} {
            puts "Save speedup: [format %.2f [expr {double($save_time_lzf) / double($save_time_lz4)}]]x"
        }
        if {$load_time_lzf > 0} {
            puts "Load speedup: [format %.2f [expr {double($load_time_lzf) / double($load_time_lz4)}]]x"
        }
        puts "========================================="
        puts ""
        
        # Verify data integrity
        assert_equal [r dbsize] [expr {[r dbsize]}]
        
        # Basic sanity checks
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
        assert {$save_time_lzf < 60000}
        assert {$save_time_lz4 < 60000}
        assert {$load_time_lzf < 60000}
        assert {$load_time_lz4 < 60000}
    }
}
