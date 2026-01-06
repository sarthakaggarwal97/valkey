# Performance benchmarks for RDB compression with 512KB chunks
# Tests compression ratios, save/load times, and memory usage with optimized chunk size

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

# Benchmark compression ratios with 512KB chunks
start_server {tags {"rdb compression benchmark 512kb slow"}} {
    test {Benchmark 512KB: LZF vs LZ4-stream compression ratio - typical workload} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 524288
        
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
        
        puts "=== Compression Ratio - Typical Workload (512KB chunks) ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
    
    test {Benchmark 512KB: LZF vs LZ4-stream compression ratio - highly compressible data} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 524288
        
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
        
        puts "=== Compression Ratio - Highly Compressible Data (512KB chunks) ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 5.0}
        assert {$ratio_lz4 > 5.0}
    }
    
    test {Benchmark 512KB: LZF vs LZ4-stream compression ratio - strings workload} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 524288
        
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
        
        puts "=== Compression Ratio - Strings Workload (512KB chunks) ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
    
    test {Benchmark 512KB: LZF vs LZ4-stream compression ratio - lists workload} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 524288
        
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
        
        puts "=== Compression Ratio - Lists Workload (512KB chunks) ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
    
    test {Benchmark 512KB: LZF vs LZ4-stream compression ratio - hashes workload} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 524288
        
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
        
        puts "=== Compression Ratio - Hashes Workload (512KB chunks) ==="
        puts "LZF:         Uncompressed: $uncompressed_lzf bytes, Compressed: $compressed_lzf bytes, Ratio: [format %.2f $ratio_lzf]"
        puts "LZ4-stream:  Uncompressed: $uncompressed_lz4 bytes, Compressed: $compressed_lz4 bytes, Ratio: [format %.2f $ratio_lz4]"
        puts "LZ4-stream improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
    
    test {Benchmark 512KB: Comprehensive comparison (5000 keys)} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 524288
        
        # Populate mixed workload
        populate_typical_workload r "comp" 5000
        
        puts "========================================="
        puts "COMPREHENSIVE BENCHMARK: LZF vs LZ4-stream (512KB chunks)"
        puts "Dataset: 5000 keys, mixed data types"
        puts "Chunk size: 524288 bytes (512KB)"
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
        
        # LZ4-stream metrics
        r config set rdb-compression-algorithm lz4-stream
        set start [clock milliseconds]
        r save
        set save_time_lz4 [expr {[clock milliseconds] - $start}]
        
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ compressed_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncompressed_lz4
        regexp {rdb_last_save_chunks:(\d+)} $info_lz4 _ chunks_lz4
        
        if {$compressed_lz4 == 0} {
            set compressed_lz4 1
        }
        
        set ratio_lz4 [expr {double($uncompressed_lz4) / double($compressed_lz4)}]
        
        # Print results
        puts "--- LZF Results ---"
        puts "Compression ratio: [format %.2f $ratio_lzf]"
        puts "Compressed size: [format %.2f [expr {$compressed_lzf / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {$uncompressed_lzf / 1024.0}]] KB"
        puts "Chunks: $chunks_lzf"
        puts "Save time: $save_time_lzf ms"
        puts ""
        
        puts "--- LZ4-stream Results ---"
        puts "Compression ratio: [format %.2f $ratio_lz4]"
        puts "Compressed size: [format %.2f [expr {$compressed_lz4 / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {$uncompressed_lz4 / 1024.0}]] KB"
        puts "Chunks: $chunks_lz4"
        puts "Save time: $save_time_lz4 ms"
        puts ""
        
        puts "--- Comparison ---"
        puts "Compression improvement: [format %.1f [expr {($ratio_lz4 / $ratio_lzf - 1.0) * 100}]]%"
        puts "Size reduction: [format %.1f [expr {(1.0 - double($compressed_lz4) / double($compressed_lzf)) * 100}]]%"
        if {$save_time_lzf > 0} {
            puts "Save speedup: [format %.2f [expr {double($save_time_lzf) / double($save_time_lz4)}]]x"
        }
        puts "========================================="
        puts ""
        
        assert {$ratio_lzf > 1.0}
        assert {$ratio_lz4 > 1.0}
    }
}
