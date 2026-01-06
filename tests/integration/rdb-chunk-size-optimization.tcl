# Chunk size optimization benchmarks for LZ4-stream compression
# Tests various chunk sizes to find optimal compression ratio vs memory trade-off

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

# Test a specific chunk size
proc test_chunk_size {r chunk_size workload_name workload_size populate_proc} {
    r flushall
    r config set rdb-chunk-compression yes
    r config set rdb-compression-algorithm lz4-stream
    r config set rdb-chunk-size $chunk_size
    
    # Populate data
    $populate_proc r "test" $workload_size
    
    # Measure memory before save
    set mem_before [s used_memory]
    
    # Measure save time
    set start [clock milliseconds]
    r save
    set save_time [expr {[clock milliseconds] - $start}]
    
    # Measure memory after save
    set mem_after [s used_memory]
    set mem_increase [expr {$mem_after - $mem_before}]
    
    # Get compression stats
    set info [r info persistence]
    regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
    regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
    regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
    
    if {$compressed == 0} {
        set compressed 1
    }
    set ratio [expr {double($uncompressed) / double($compressed)}]
    
    # Measure load time
    set start [clock milliseconds]
    restart_server 0 true false
    set load_time [expr {[clock milliseconds] - $start}]
    
    return [list \
        chunk_size $chunk_size \
        compressed $compressed \
        uncompressed $uncompressed \
        ratio $ratio \
        chunks $chunks \
        save_time $save_time \
        load_time $load_time \
        mem_increase $mem_increase]
}

# Chunk size optimization tests
start_server {tags {"rdb chunk-size optimization slow"}} {
    test {Chunk Size Optimization: Typical Workload (5000 keys)} {
        puts ""
        puts "========================================="
        puts "CHUNK SIZE OPTIMIZATION: TYPICAL WORKLOAD"
        puts "Dataset: 5000 keys, mixed data types"
        puts "========================================="
        puts ""
        
        # Test different chunk sizes
        set chunk_sizes {65536 131072 262144 524288 1048576}
        set chunk_names {"64KB" "128KB" "256KB" "512KB" "1MB"}
        set results {}
        
        # Baseline with 64KB
        set baseline_result [test_chunk_size r 65536 "typical" 5000 populate_typical_workload]
        lappend results $baseline_result
        set baseline_ratio [dict get $baseline_result ratio]
        
        puts "--- 64KB (Baseline) ---"
        puts "Compression ratio: [format %.2f [dict get $baseline_result ratio]]"
        puts "Compressed size: [format %.2f [expr {[dict get $baseline_result compressed] / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {[dict get $baseline_result uncompressed] / 1024.0}]] KB"
        puts "Chunks: [dict get $baseline_result chunks]"
        puts "Save time: [dict get $baseline_result save_time] ms"
        puts "Load time: [dict get $baseline_result load_time] ms"
        puts "Memory increase: [format %.2f [expr {[dict get $baseline_result mem_increase] / 1024.0}]] KB"
        puts ""
        
        # Test other chunk sizes
        foreach chunk_size {131072 262144 524288 1048576} chunk_name {"128KB" "256KB" "512KB" "1MB"} {
            set result [test_chunk_size r $chunk_size "typical" 5000 populate_typical_workload]
            lappend results $result
            
            set ratio [dict get $result ratio]
            set improvement [expr {($ratio / $baseline_ratio - 1.0) * 100}]
            
            puts "--- $chunk_name ---"
            puts "Compression ratio: [format %.2f $ratio] ([format %+.1f $improvement]% vs baseline)"
            puts "Compressed size: [format %.2f [expr {[dict get $result compressed] / 1024.0}]] KB"
            puts "Uncompressed size: [format %.2f [expr {[dict get $result uncompressed] / 1024.0}]] KB"
            puts "Chunks: [dict get $result chunks]"
            puts "Save time: [dict get $result save_time] ms"
            puts "Load time: [dict get $result load_time] ms"
            puts "Memory increase: [format %.2f [expr {[dict get $result mem_increase] / 1024.0}]] KB"
            puts ""
        }
        
        puts "========================================="
        puts ""
        
        # Verify all tests completed
        assert {[llength $results] == 5}
    }
    
    test {Chunk Size Optimization: Highly Compressible Data (1000 keys)} {
        puts ""
        puts "========================================="
        puts "CHUNK SIZE OPTIMIZATION: HIGHLY COMPRESSIBLE"
        puts "Dataset: 1000 keys, repetitive patterns"
        puts "========================================="
        puts ""
        
        # Baseline with 64KB
        set baseline_result [test_chunk_size r 65536 "compressible" 1000 populate_compressible_data]
        set baseline_ratio [dict get $baseline_result ratio]
        
        puts "--- 64KB (Baseline) ---"
        puts "Compression ratio: [format %.2f [dict get $baseline_result ratio]]"
        puts "Compressed size: [format %.2f [expr {[dict get $baseline_result compressed] / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {[dict get $baseline_result uncompressed] / 1024.0}]] KB"
        puts "Chunks: [dict get $baseline_result chunks]"
        puts "Save time: [dict get $baseline_result save_time] ms"
        puts "Load time: [dict get $baseline_result load_time] ms"
        puts "Memory increase: [format %.2f [expr {[dict get $baseline_result mem_increase] / 1024.0}]] KB"
        puts ""
        
        # Test other chunk sizes
        foreach chunk_size {131072 262144 524288 1048576} chunk_name {"128KB" "256KB" "512KB" "1MB"} {
            set result [test_chunk_size r $chunk_size "compressible" 1000 populate_compressible_data]
            
            set ratio [dict get $result ratio]
            set improvement [expr {($ratio / $baseline_ratio - 1.0) * 100}]
            
            puts "--- $chunk_name ---"
            puts "Compression ratio: [format %.2f $ratio] ([format %+.1f $improvement]% vs baseline)"
            puts "Compressed size: [format %.2f [expr {[dict get $result compressed] / 1024.0}]] KB"
            puts "Uncompressed size: [format %.2f [expr {[dict get $result uncompressed] / 1024.0}]] KB"
            puts "Chunks: [dict get $result chunks]"
            puts "Save time: [dict get $result save_time] ms"
            puts "Load time: [dict get $result load_time] ms"
            puts "Memory increase: [format %.2f [expr {[dict get $result mem_increase] / 1024.0}]] KB"
            puts ""
        }
        
        puts "========================================="
        puts ""
    }
    
    test {Chunk Size Optimization: Lists Workload (500 keys)} {
        puts ""
        puts "========================================="
        puts "CHUNK SIZE OPTIMIZATION: LISTS WORKLOAD"
        puts "Dataset: 500 keys, 30 items per list"
        puts "========================================="
        puts ""
        
        # Populate list-heavy workload
        proc populate_lists {r prefix count} {
            for {set i 0} {$i < $count} {incr i} {
                for {set j 0} {$j < 30} {incr j} {
                    $r lpush "${prefix}:list:$i" "item_${i}_${j}_[string repeat "data" 10]"
                }
            }
        }
        
        # Baseline with 64KB
        set baseline_result [test_chunk_size r 65536 "lists" 500 populate_lists]
        set baseline_ratio [dict get $baseline_result ratio]
        
        puts "--- 64KB (Baseline) ---"
        puts "Compression ratio: [format %.2f [dict get $baseline_result ratio]]"
        puts "Compressed size: [format %.2f [expr {[dict get $baseline_result compressed] / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {[dict get $baseline_result uncompressed] / 1024.0}]] KB"
        puts "Chunks: [dict get $baseline_result chunks]"
        puts "Save time: [dict get $baseline_result save_time] ms"
        puts "Load time: [dict get $baseline_result load_time] ms"
        puts "Memory increase: [format %.2f [expr {[dict get $baseline_result mem_increase] / 1024.0}]] KB"
        puts ""
        
        # Test other chunk sizes
        foreach chunk_size {131072 262144 524288 1048576} chunk_name {"128KB" "256KB" "512KB" "1MB"} {
            set result [test_chunk_size r $chunk_size "lists" 500 populate_lists]
            
            set ratio [dict get $result ratio]
            set improvement [expr {($ratio / $baseline_ratio - 1.0) * 100}]
            
            puts "--- $chunk_name ---"
            puts "Compression ratio: [format %.2f $ratio] ([format %+.1f $improvement]% vs baseline)"
            puts "Compressed size: [format %.2f [expr {[dict get $result compressed] / 1024.0}]] KB"
            puts "Uncompressed size: [format %.2f [expr {[dict get $result uncompressed] / 1024.0}]] KB"
            puts "Chunks: [dict get $result chunks]"
            puts "Save time: [dict get $result save_time] ms"
            puts "Load time: [dict get $result load_time] ms"
            puts "Memory increase: [format %.2f [expr {[dict get $result mem_increase] / 1024.0}]] KB"
            puts ""
        }
        
        puts "========================================="
        puts ""
    }
    
    test {Chunk Size Optimization: Hashes Workload (500 keys)} {
        puts ""
        puts "========================================="
        puts "CHUNK SIZE OPTIMIZATION: HASHES WORKLOAD"
        puts "Dataset: 500 keys, 20 fields per hash"
        puts "========================================="
        puts ""
        
        # Populate hash-heavy workload
        proc populate_hashes {r prefix count} {
            for {set i 0} {$i < $count} {incr i} {
                for {set j 0} {$j < 20} {incr j} {
                    $r hset "${prefix}:hash:$i" "field_$j" "value_${i}_${j}_[string repeat "x" 20]"
                }
            }
        }
        
        # Baseline with 64KB
        set baseline_result [test_chunk_size r 65536 "hashes" 500 populate_hashes]
        set baseline_ratio [dict get $baseline_result ratio]
        
        puts "--- 64KB (Baseline) ---"
        puts "Compression ratio: [format %.2f [dict get $baseline_result ratio]]"
        puts "Compressed size: [format %.2f [expr {[dict get $baseline_result compressed] / 1024.0}]] KB"
        puts "Uncompressed size: [format %.2f [expr {[dict get $baseline_result uncompressed] / 1024.0}]] KB"
        puts "Chunks: [dict get $baseline_result chunks]"
        puts "Save time: [dict get $baseline_result save_time] ms"
        puts "Load time: [dict get $baseline_result load_time] ms"
        puts "Memory increase: [format %.2f [expr {[dict get $baseline_result mem_increase] / 1024.0}]] KB"
        puts ""
        
        # Test other chunk sizes
        foreach chunk_size {131072 262144 524288 1048576} chunk_name {"128KB" "256KB" "512KB" "1MB"} {
            set result [test_chunk_size r $chunk_size "hashes" 500 populate_hashes]
            
            set ratio [dict get $result ratio]
            set improvement [expr {($ratio / $baseline_ratio - 1.0) * 100}]
            
            puts "--- $chunk_name ---"
            puts "Compression ratio: [format %.2f $ratio] ([format %+.1f $improvement]% vs baseline)"
            puts "Compressed size: [format %.2f [expr {[dict get $result compressed] / 1024.0}]] KB"
            puts "Uncompressed size: [format %.2f [expr {[dict get $result uncompressed] / 1024.0}]] KB"
            puts "Chunks: [dict get $result chunks]"
            puts "Save time: [dict get $result save_time] ms"
            puts "Load time: [dict get $result load_time] ms"
            puts "Memory increase: [format %.2f [expr {[dict get $result mem_increase] / 1024.0}]] KB"
            puts ""
        }
        
        puts "========================================="
        puts ""
    }
}
