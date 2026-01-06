# Quick chunk size optimization test - tests key chunk sizes only

# Helper to populate typical workload
proc populate_typical_workload {r prefix count} {
    for {set i 0} {$i < $count} {incr i} {
        # Mix of data types
        if {$i % 10 < 4} {
            if {$i % 3 == 0} {
                $r set "${prefix}:str:$i" [string repeat "user_data_pattern_" 20]
            } else {
                $r set "${prefix}:str:$i" "value_${i}_[expr {int(rand() * 1000000)}]"
            }
        }
        if {$i % 10 >= 4 && $i % 10 < 6} {
            for {set j 0} {$j < 15} {incr j} {
                $r lpush "${prefix}:list:$i" "item_${i}_${j}_[string repeat "data" 5]"
            }
        }
        if {$i % 10 >= 6 && $i % 10 < 8} {
            for {set j 0} {$j < 10} {incr j} {
                $r hset "${prefix}:hash:$i" "field_$j" "value_${i}_${j}_[string repeat "x" 10]"
            }
        }
    }
}

start_server {tags {"rdb chunk-size quick"}} {
    test {Quick Chunk Size Test: 64KB vs 128KB vs 256KB} {
        puts ""
        puts "========================================="
        puts "QUICK CHUNK SIZE COMPARISON"
        puts "Dataset: 3000 keys, mixed data types"
        puts "========================================="
        puts ""
        
        # Test 64KB (baseline)
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 65536
        populate_typical_workload r "test64" 3000
        r save
        set info_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_64 _ compressed_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_64 _ uncompressed_64
        regexp {rdb_last_save_chunks:(\d+)} $info_64 _ chunks_64
        set ratio_64 [expr {double($uncompressed_64) / double($compressed_64)}]
        
        puts "--- 64KB (Baseline) ---"
        puts "Compression ratio: [format %.2f $ratio_64]"
        puts "Compressed: [format %.2f [expr {$compressed_64 / 1024.0}]] KB"
        puts "Uncompressed: [format %.2f [expr {$uncompressed_64 / 1024.0}]] KB"
        puts "Chunks: $chunks_64"
        puts ""
        
        # Test 128KB
        r flushall
        r config set rdb-chunk-size 131072
        populate_typical_workload r "test128" 3000
        r save
        set info_128 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_128 _ compressed_128
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_128 _ uncompressed_128
        regexp {rdb_last_save_chunks:(\d+)} $info_128 _ chunks_128
        set ratio_128 [expr {double($uncompressed_128) / double($compressed_128)}]
        set improvement_128 [expr {($ratio_128 / $ratio_64 - 1.0) * 100}]
        
        puts "--- 128KB ---"
        puts "Compression ratio: [format %.2f $ratio_128] ([format %+.1f $improvement_128]% vs baseline)"
        puts "Compressed: [format %.2f [expr {$compressed_128 / 1024.0}]] KB"
        puts "Uncompressed: [format %.2f [expr {$uncompressed_128 / 1024.0}]] KB"
        puts "Chunks: $chunks_128"
        puts ""
        
        # Test 256KB
        r flushall
        r config set rdb-chunk-size 262144
        populate_typical_workload r "test256" 3000
        r save
        set info_256 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_256 _ compressed_256
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_256 _ uncompressed_256
        regexp {rdb_last_save_chunks:(\d+)} $info_256 _ chunks_256
        set ratio_256 [expr {double($uncompressed_256) / double($compressed_256)}]
        set improvement_256 [expr {($ratio_256 / $ratio_64 - 1.0) * 100}]
        
        puts "--- 256KB ---"
        puts "Compression ratio: [format %.2f $ratio_256] ([format %+.1f $improvement_256]% vs baseline)"
        puts "Compressed: [format %.2f [expr {$compressed_256 / 1024.0}]] KB"
        puts "Uncompressed: [format %.2f [expr {$uncompressed_256 / 1024.0}]] KB"
        puts "Chunks: $chunks_256"
        puts ""
        
        # Test 512KB
        r flushall
        r config set rdb-chunk-size 524288
        populate_typical_workload r "test512" 3000
        r save
        set info_512 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_512 _ compressed_512
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_512 _ uncompressed_512
        regexp {rdb_last_save_chunks:(\d+)} $info_512 _ chunks_512
        set ratio_512 [expr {double($uncompressed_512) / double($compressed_512)}]
        set improvement_512 [expr {($ratio_512 / $ratio_64 - 1.0) * 100}]
        
        puts "--- 512KB ---"
        puts "Compression ratio: [format %.2f $ratio_512] ([format %+.1f $improvement_512]% vs baseline)"
        puts "Compressed: [format %.2f [expr {$compressed_512 / 1024.0}]] KB"
        puts "Uncompressed: [format %.2f [expr {$uncompressed_512 / 1024.0}]] KB"
        puts "Chunks: $chunks_512"
        puts ""
        
        # Test 1MB
        r flushall
        r config set rdb-chunk-size 1048576
        populate_typical_workload r "test1mb" 3000
        r save
        set info_1mb [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_1mb _ compressed_1mb
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_1mb _ uncompressed_1mb
        regexp {rdb_last_save_chunks:(\d+)} $info_1mb _ chunks_1mb
        set ratio_1mb [expr {double($uncompressed_1mb) / double($compressed_1mb)}]
        set improvement_1mb [expr {($ratio_1mb / $ratio_64 - 1.0) * 100}]
        
        puts "--- 1MB ---"
        puts "Compression ratio: [format %.2f $ratio_1mb] ([format %+.1f $improvement_1mb]% vs baseline)"
        puts "Compressed: [format %.2f [expr {$compressed_1mb / 1024.0}]] KB"
        puts "Uncompressed: [format %.2f [expr {$uncompressed_1mb / 1024.0}]] KB"
        puts "Chunks: $chunks_1mb"
        puts ""
        
        puts "========================================="
        puts "SUMMARY"
        puts "========================================="
        puts "Chunk Size | Ratio | Improvement | Chunks"
        puts "64KB       | [format %.2f $ratio_64] | baseline    | $chunks_64"
        puts "128KB      | [format %.2f $ratio_128] | [format %+.1f $improvement_128]%      | $chunks_128"
        puts "256KB      | [format %.2f $ratio_256] | [format %+.1f $improvement_256]%      | $chunks_256"
        puts "512KB      | [format %.2f $ratio_512] | [format %+.1f $improvement_512]%      | $chunks_512"
        puts "1MB        | [format %.2f $ratio_1mb] | [format %+.1f $improvement_1mb]%      | $chunks_1mb"
        puts "========================================="
        puts ""
        
        # Verify all tests completed
        assert {$ratio_64 > 1.0}
        assert {$ratio_128 > 1.0}
        assert {$ratio_256 > 1.0}
        assert {$ratio_512 > 1.0}
        assert {$ratio_1mb > 1.0}
    }
}
