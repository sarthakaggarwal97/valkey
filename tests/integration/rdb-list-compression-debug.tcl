# Debug script to understand why lists compress worse with LZ4-stream

start_server {tags {"rdb list debug"}} {
    test {Debug: Analyze list serialization patterns} {
        r flushall
        r config set rdb-chunk-compression yes
        
        puts ""
        puts "========================================="
        puts "LIST COMPRESSION DEBUG ANALYSIS"
        puts "========================================="
        puts ""
        
        # Create a simple list with repetitive data
        puts "--- Test 1: Simple repetitive list ---"
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r lpush "list:simple" "item_[string repeat "data" 10]"
        }
        
        # Test with LZF (64KB)
        r config set rdb-compression-algorithm lzf
        r config set rdb-chunk-size 65536
        r save
        set info_lzf_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf_64 _ comp_lzf_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf_64 _ uncomp_lzf_64
        set ratio_lzf_64 [expr {double($uncomp_lzf_64) / double($comp_lzf_64)}]
        
        # Test with LZ4-stream (64KB)
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 65536
        r save
        set info_lz4_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4_64 _ comp_lz4_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4_64 _ uncomp_lz4_64
        set ratio_lz4_64 [expr {double($uncomp_lz4_64) / double($comp_lz4_64)}]
        
        # Test with LZ4-stream (512KB)
        r config set rdb-chunk-size 524288
        r save
        set info_lz4_512 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4_512 _ comp_lz4_512
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4_512 _ uncomp_lz4_512
        set ratio_lz4_512 [expr {double($uncomp_lz4_512) / double($comp_lz4_512)}]
        
        puts "Simple repetitive list (100 items):"
        puts "  LZF (64KB):         Ratio: [format %.2f $ratio_lzf_64], Compressed: $comp_lzf_64 bytes"
        puts "  LZ4-stream (64KB):  Ratio: [format %.2f $ratio_lz4_64], Compressed: $comp_lz4_64 bytes ([format %+.1f [expr {($ratio_lz4_64/$ratio_lzf_64-1)*100}]]%)"
        puts "  LZ4-stream (512KB): Ratio: [format %.2f $ratio_lz4_512], Compressed: $comp_lz4_512 bytes ([format %+.1f [expr {($ratio_lz4_512/$ratio_lzf_64-1)*100}]]%)"
        puts ""
        
        # Test 2: Multiple small lists
        puts "--- Test 2: Multiple small lists (10 items each) ---"
        r flushall
        for {set i 0} {$i < 50} {incr i} {
            for {set j 0} {$j < 10} {incr j} {
                r lpush "list:$i" "item_${i}_${j}_[string repeat "data" 5]"
            }
        }
        
        # Test with LZF (64KB)
        r config set rdb-compression-algorithm lzf
        r config set rdb-chunk-size 65536
        r save
        set info_lzf_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf_64 _ comp_lzf_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf_64 _ uncomp_lzf_64
        set ratio_lzf_64 [expr {double($uncomp_lzf_64) / double($comp_lzf_64)}]
        
        # Test with LZ4-stream (64KB)
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 65536
        r save
        set info_lz4_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4_64 _ comp_lz4_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4_64 _ uncomp_lz4_64
        set ratio_lz4_64 [expr {double($uncomp_lz4_64) / double($comp_lz4_64)}]
        
        # Test with LZ4-stream (512KB)
        r config set rdb-chunk-size 524288
        r save
        set info_lz4_512 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4_512 _ comp_lz4_512
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4_512 _ uncomp_lz4_512
        set ratio_lz4_512 [expr {double($uncomp_lz4_512) / double($comp_lz4_512)}]
        
        puts "Multiple small lists (50 lists, 10 items each):"
        puts "  LZF (64KB):         Ratio: [format %.2f $ratio_lzf_64], Compressed: $comp_lzf_64 bytes"
        puts "  LZ4-stream (64KB):  Ratio: [format %.2f $ratio_lz4_64], Compressed: $comp_lz4_64 bytes ([format %+.1f [expr {($ratio_lz4_64/$ratio_lzf_64-1)*100}]]%)"
        puts "  LZ4-stream (512KB): Ratio: [format %.2f $ratio_lz4_512], Compressed: $comp_lz4_512 bytes ([format %+.1f [expr {($ratio_lz4_512/$ratio_lzf_64-1)*100}]]%)"
        puts ""
        
        # Test 3: Large list with varied data
        puts "--- Test 3: Large list with varied data ---"
        r flushall
        for {set i 0} {$i < 500} {incr i} {
            r lpush "list:large" "item_${i}_value_[expr {int(rand() * 1000)}]_[string repeat "x" 20]"
        }
        
        # Test with LZF (64KB)
        r config set rdb-compression-algorithm lzf
        r config set rdb-chunk-size 65536
        r save
        set info_lzf_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf_64 _ comp_lzf_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf_64 _ uncomp_lzf_64
        regexp {rdb_last_save_chunks:(\d+)} $info_lzf_64 _ chunks_lzf_64
        set ratio_lzf_64 [expr {double($uncomp_lzf_64) / double($comp_lzf_64)}]
        
        # Test with LZ4-stream (64KB)
        r config set rdb-compression-algorithm lz4-stream
        r config set rdb-chunk-size 65536
        r save
        set info_lz4_64 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4_64 _ comp_lz4_64
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4_64 _ uncomp_lz4_64
        regexp {rdb_last_save_chunks:(\d+)} $info_lz4_64 _ chunks_lz4_64
        set ratio_lz4_64 [expr {double($uncomp_lz4_64) / double($comp_lz4_64)}]
        
        # Test with LZ4-stream (512KB)
        r config set rdb-chunk-size 524288
        r save
        set info_lz4_512 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4_512 _ comp_lz4_512
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4_512 _ uncomp_lz4_512
        regexp {rdb_last_save_chunks:(\d+)} $info_lz4_512 _ chunks_lz4_512
        set ratio_lz4_512 [expr {double($uncomp_lz4_512) / double($comp_lz4_512)}]
        
        puts "Large list with varied data (500 items):"
        puts "  LZF (64KB):         Ratio: [format %.2f $ratio_lzf_64], Compressed: $comp_lzf_64 bytes, Chunks: $chunks_lzf_64"
        puts "  LZ4-stream (64KB):  Ratio: [format %.2f $ratio_lz4_64], Compressed: $comp_lz4_64 bytes, Chunks: $chunks_lz4_64 ([format %+.1f [expr {($ratio_lz4_64/$ratio_lzf_64-1)*100}]]%)"
        puts "  LZ4-stream (512KB): Ratio: [format %.2f $ratio_lz4_512], Compressed: $comp_lz4_512 bytes, Chunks: $chunks_lz4_512 ([format %+.1f [expr {($ratio_lz4_512/$ratio_lzf_64-1)*100}]]%)"
        puts ""
        
        # Test 4: Compare with equivalent string data
        puts "--- Test 4: Same data as strings vs list ---"
        r flushall
        
        # Create as list
        for {set i 0} {$i < 100} {incr i} {
            r lpush "data:list" "item_${i}_[string repeat "pattern" 10]"
        }
        
        # Create same data as individual strings
        for {set i 0} {$i < 100} {incr i} {
            r set "data:str:$i" "item_${i}_[string repeat "pattern" 10]"
        }
        
        # Test list with LZF
        r config set rdb-compression-algorithm lzf
        r config set rdb-chunk-size 65536
        r save
        set info_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lzf _ comp_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lzf _ uncomp_lzf
        set ratio_lzf [expr {double($uncomp_lzf) / double($comp_lzf)}]
        
        # Test list with LZ4-stream
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_lz4 _ comp_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_lz4 _ uncomp_lz4
        set ratio_lz4 [expr {double($uncomp_lz4) / double($comp_lz4)}]
        
        puts "Same data (100 items) - list + strings:"
        puts "  LZF:        Ratio: [format %.2f $ratio_lzf], Compressed: $comp_lzf bytes"
        puts "  LZ4-stream: Ratio: [format %.2f $ratio_lz4], Compressed: $comp_lz4 bytes ([format %+.1f [expr {($ratio_lz4/$ratio_lzf-1)*100}]]%)"
        puts ""
        
        # Test 5: Analyze RDB structure overhead
        puts "--- Test 5: RDB structure overhead analysis ---"
        r flushall
        
        # Single large string
        r set "single:string" [string repeat "data_pattern_" 1000]
        r config set rdb-compression-algorithm lzf
        r save
        set info_str_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_str_lzf _ comp_str_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_str_lzf _ uncomp_str_lzf
        
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_str_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_str_lz4 _ comp_str_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_str_lz4 _ uncomp_str_lz4
        
        # Same data as list
        r flushall
        for {set i 0} {$i < 1000} {incr i} {
            r lpush "single:list" "data_pattern_"
        }
        r config set rdb-compression-algorithm lzf
        r save
        set info_list_lzf [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_list_lzf _ comp_list_lzf
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_list_lzf _ uncomp_list_lzf
        
        r config set rdb-compression-algorithm lz4-stream
        r save
        set info_list_lz4 [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info_list_lz4 _ comp_list_lz4
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info_list_lz4 _ uncomp_list_lz4
        
        puts "Structure overhead comparison:"
        puts "  Single string (1000x 'data_pattern_'):"
        puts "    LZF:        Uncompressed: $uncomp_str_lzf, Compressed: $comp_str_lzf"
        puts "    LZ4-stream: Uncompressed: $uncomp_str_lz4, Compressed: $comp_str_lz4"
        puts "  List (1000 items of 'data_pattern_'):"
        puts "    LZF:        Uncompressed: $uncomp_list_lzf, Compressed: $comp_list_lzf"
        puts "    LZ4-stream: Uncompressed: $uncomp_list_lz4, Compressed: $comp_list_lz4"
        puts "  Overhead (list vs string):"
        puts "    Uncompressed: [expr {$uncomp_list_lzf - $uncomp_str_lzf}] bytes ([format %.1f [expr {double($uncomp_list_lzf - $uncomp_str_lzf) / $uncomp_str_lzf * 100}]]%)"
        puts "    LZF compressed: [expr {$comp_list_lzf - $comp_str_lzf}] bytes ([format %.1f [expr {double($comp_list_lzf - $comp_str_lzf) / $comp_str_lzf * 100}]]%)"
        puts "    LZ4 compressed: [expr {$comp_list_lz4 - $comp_str_lz4}] bytes ([format %.1f [expr {double($comp_list_lz4 - $comp_str_lz4) / $comp_str_lz4 * 100}]]%)"
        puts ""
        
        puts "========================================="
        puts "ANALYSIS COMPLETE"
        puts "========================================="
        puts ""
    }
}
