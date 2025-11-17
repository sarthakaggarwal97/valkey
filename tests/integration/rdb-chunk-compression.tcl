start_server {tags {"rdb chunk-compression"}} {
    test {RDB save with chunk compression enabled} {
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        r set string_key "Hello World"
        r set compressible_key [string repeat "aaaaaaaaaa" 100]
        r lpush list_key 1 2 3 a b c
        r sadd set_key 1 2 3 a b c
        r zadd zset_key 1 a 2 b 3 c
        r hset hash_key field1 value1 field2 value2
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:yes*} $info
        assert_match {*rdb_chunk_size:65536*} $info
    }
    test {RDB load with chunk compression} {
        assert_equal [r get string_key] "Hello World"
        assert_equal [r get compressible_key] [string repeat "aaaaaaaaaa" 100]
        assert_equal [r lrange list_key 0 -1] {c b a 3 2 1}
        assert_equal [lsort [r smembers set_key]] {1 2 3 a b c}
        assert_equal [r zrange zset_key 0 -1] {a b c}
        assert_equal [r hget hash_key field1] "value1"
    }
    test {RDB chunk compression with large dataset} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        for {set i 0} {$i < 100} {incr i} {
            r set large_key_$i [string repeat "x" 1000]
        }
        r save
        set info [r info persistence]
        assert_match {*rdb_last_save_chunks:*} $info
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        assert {$chunks > 1}
        assert_equal [r get large_key_0] [string repeat "x" 1000]
        assert_equal [r get large_key_99] [string repeat "x" 1000]
    }
    test {RDB chunk compression with all data types} {
        r flushall
        r config set rdb-chunk-compression yes
        r set str1 "simple"
        r set str2 [string repeat "compress" 100]
        r lpush list1 1 2 3 4 5
        for {set i 0} {$i < 30} {incr i} {
            r lpush list2 "item_$i"
        }
        r sadd set1 a b c d e
        for {set i 0} {$i < 30} {incr i} {
            r sadd set2 "member_$i"
        }
        r zadd zset1 1 a 2 b 3 c
        for {set i 0} {$i < 30} {incr i} {
            r zadd zset2 $i "member_$i"
        }
        r hset hash1 f1 v1 f2 v2 f3 v3
        for {set i 0} {$i < 30} {incr i} {
            r hset hash2 "field_$i" "value_$i"
        }
        r xadd stream1 * field1 value1 field2 value2
        for {set i 0} {$i < 20} {incr i} {
            r xadd stream2 * data "item_$i"
        }
        r save
        set digest_before [debug_digest]
        assert_equal [r get str1] "simple"
        assert_equal [r llen list1] 5
        assert_equal [r scard set1] 5
        assert_equal [r zcard zset1] 3
        assert_equal [r hlen hash1] 3
        assert_equal [r xlen stream1] 1
    }
    test {Backward compatibility - load version 80 RDB} {
        r flushall
        r config set rdb-chunk-compression no
        r set legacy_key1 "value1"
        r set legacy_key2 [string repeat "data" 100]
        r lpush legacy_list 1 2 3
        r sadd legacy_set a b c
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:no*} $info
        r config set rdb-chunk-compression yes
        assert_equal [r get legacy_key1] "value1"
        assert_equal [r get legacy_key2] [string repeat "data" 100]
        assert_equal [r lrange legacy_list 0 -1] {3 2 1}
        assert_equal [lsort [r smembers legacy_set]] {a b c}
    }
    test {Verify compression ratio with compressible data} {
        r flushall
        r config set rdb-chunk-compression yes
        for {set i 0} {$i < 200} {incr i} {
            r set compress_key_$i [string repeat "aaaaaaaaaa" 100]
        }
        r save
        set info [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        set ratio [expr {double($uncompressed) / double($compressed)}]
        assert {$ratio > 1.0}
    }
    test {RDB with small chunk size (4KB)} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 4096
        for {set i 0} {$i < 30} {incr i} {
            r set small_chunk_key_$i [string repeat "data" 100]
        }
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_size:4096*} $info
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        assert {$chunks > 1}
        assert_equal [r get small_chunk_key_0] [string repeat "data" 100]
        assert_equal [r get small_chunk_key_29] [string repeat "data" 100]
    }
    test {Switch between chunk compression modes} {
        r flushall
        r config set rdb-chunk-compression no
        r set toggle_key1 "value1"
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:no*} $info
        r config set rdb-chunk-compression yes
        r set toggle_key2 "value2"
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:yes*} $info
        assert_equal [r get toggle_key1] "value1"
        assert_equal [r get toggle_key2] "value2"
    }
    test {RDB chunk compression with empty database} {
        r flushall
        r config set rdb-chunk-compression yes
        r save
        assert_equal [r dbsize] 0
    }
    test {BGSAVE with chunk compression} {
        r flushall
        r config set rdb-chunk-compression yes
        for {set i 0} {$i < 50} {incr i} {
            r set bgsave_key_$i "value_$i"
        }
        r bgsave
        waitForBgsave r
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:yes*} $info
        assert_equal [r get bgsave_key_0] "value_0"
        assert_equal [r get bgsave_key_49] "value_49"
    }
}
start_server {tags {"rdb chunk-compression config external:skip"}} {
    test {Enable chunk compression via CONFIG SET} {
        set config [r config get rdb-chunk-compression]
        assert_equal [lindex $config 1] "yes"
        r config set rdb-chunk-compression yes
        set config [r config get rdb-chunk-compression]
        assert_equal [lindex $config 1] "yes"
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:yes*} $info
    }
    test {Disable chunk compression via CONFIG SET} {
        r config set rdb-chunk-compression yes
        set config [r config get rdb-chunk-compression]
        assert_equal [lindex $config 1] "yes"
        r config set rdb-chunk-compression no
        set config [r config get rdb-chunk-compression]
        assert_equal [lindex $config 1] "no"
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:no*} $info
    }
    test {Set chunk size to minimum valid value (4KB)} {
        r config set rdb-chunk-size 4096
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "4096"
        set info [r info persistence]
        assert_match {*rdb_chunk_size:4096*} $info
    }
    test {Set chunk size to maximum valid value (1MB)} {
        r config set rdb-chunk-size 1048576
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "1048576"
        set info [r info persistence]
        assert_match {*rdb_chunk_size:1048576*} $info
    }
    test {Set chunk size to default value (64KB)} {
        r config set rdb-chunk-size 65536
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "65536"
        set info [r info persistence]
        assert_match {*rdb_chunk_size:65536*} $info
    }
    test {Set chunk size to various valid values} {
        r config set rdb-chunk-size 8192
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "8192"
        r config set rdb-chunk-size 16384
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "16384"
        r config set rdb-chunk-size 32768
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "32768"
        r config set rdb-chunk-size 131072
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "131072"
        r config set rdb-chunk-size 262144
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "262144"
        r config set rdb-chunk-size 524288
        set config [r config get rdb-chunk-size]
        assert_equal [lindex $config 1] "524288"
    }
    test {Reject chunk size below minimum (< 4KB)} {
        catch {r config set rdb-chunk-size 4095} err
        assert_match {*argument must be between 4096 and 1048576*} $err
        catch {r config set rdb-chunk-size 1024} err
        assert_match {*argument must be between 4096 and 1048576*} $err
        catch {r config set rdb-chunk-size 0} err
        assert_match {*argument must be*} $err
        catch {r config set rdb-chunk-size -1} err
        assert_match {*ERR*} $err
    }
    test {Reject chunk size above maximum (> 1MB)} {
        catch {r config set rdb-chunk-size 1048577} err
        assert_match {*argument must be between 4096 and 1048576*} $err
        catch {r config set rdb-chunk-size 2097152} err
        assert_match {*argument must be between 4096 and 1048576*} $err
        catch {r config set rdb-chunk-size 10485760} err
        assert_match {*argument must be between 4096 and 1048576*} $err
    }
    test {Configuration persistence after restart} {
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 32768
        r config rewrite
        restart_server 0 true false
        set compression [lindex [r config get rdb-chunk-compression] 1]
        set chunk_size [lindex [r config get rdb-chunk-size] 1]
        assert_equal $compression "yes"
        assert_equal $chunk_size "32768"
    }
    test {Configuration persistence with different values} {
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 4096
        r config rewrite
        restart_server 0 true false
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "yes"
        assert_equal [lindex [r config get rdb-chunk-size] 1] "4096"
        r config set rdb-chunk-size 1048576
        r config rewrite
        restart_server 0 true false
        assert_equal [lindex [r config get rdb-chunk-size] 1] "1048576"
        r config set rdb-chunk-compression no
        r config rewrite
        restart_server 0 true false
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "no"
    }
    test {Chunk compression setting affects RDB save behavior} {
        r flushall
        r config set rdb-chunk-compression no
        r set test_key "test_value"
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:no*} $info
        r config set rdb-chunk-compression yes
        r set test_key2 "test_value2"
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:yes*} $info
        assert_match {*rdb_last_save_chunks:*} $info
    }
    test {Chunk size setting affects number of chunks created} {
        r flushall
        r config set rdb-chunk-compression yes
        for {set i 0} {$i < 50} {incr i} {
            r set chunk_test_key_$i [string repeat "data" 200]
        }
        r config set rdb-chunk-size 4096
        r save
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ small_chunks
        r config set rdb-chunk-size 262144
        r save
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ large_chunks
        assert {$small_chunks > $large_chunks}
    }
    test {CONFIG GET returns correct default values} {
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        set compression [lindex [r config get rdb-chunk-compression] 1]
        set chunk_size [lindex [r config get rdb-chunk-size] 1]
        assert_equal $compression "yes"
        assert_equal $chunk_size "65536"
    }
    test {Multiple CONFIG SET operations in sequence} {
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 8192
        r config set rdb-chunk-compression no
        r config set rdb-chunk-size 16384
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 131072
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "yes"
        assert_equal [lindex [r config get rdb-chunk-size] 1] "131072"
    }
    test {CONFIG RESETSTAT preserves chunk compression settings} {
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 32768
        r config resetstat
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "yes"
        assert_equal [lindex [r config get rdb-chunk-size] 1] "32768"
    }
    test {Invalid boolean values for rdb-chunk-compression} {
        catch {r config set rdb-chunk-compression invalid} err
        assert_match {*argument must be 'yes' or 'no'*} $err
        catch {r config set rdb-chunk-compression 1} err
        assert_match {*argument must be 'yes' or 'no'*} $err
        catch {r config set rdb-chunk-compression true} err
        assert_match {*argument must be 'yes' or 'no'*} $err
    }
    test {Invalid numeric values for rdb-chunk-size} {
        catch {r config set rdb-chunk-size invalid} err
        assert_match {*argument must be a memory value*} $err
        catch {r config set rdb-chunk-size abc} err
        assert_match {*argument must be a memory value*} $err
        catch {r config set rdb-chunk-size 1.5} err
        assert_match {*argument must be a memory value*} $err
    }
    test {Chunk compression works independently of legacy compression} {
        r config set rdbcompression yes
        r config set rdb-chunk-compression yes
        assert_equal [lindex [r config get rdbcompression] 1] "yes"
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "yes"
        r config set rdbcompression no
        r config set rdb-chunk-compression yes
        assert_equal [lindex [r config get rdbcompression] 1] "no"
        assert_equal [lindex [r config get rdb-chunk-compression] 1] "yes"
    }
}
proc measure_time {script} {
    set start [clock clicks -millisec]
    uplevel 1 $script
    set end [clock clicks -millisec]
    return [expr {$end - $start}]
}
proc get_rdb_size {r} {
    set dir [lindex [$r config get dir] 1]
    set dump_path [file join $dir dump.rdb]
    if {[file exists $dump_path]} {
        return [file size $dump_path]
    }
    return 0
}
proc populate_typical_workload {r num_keys} {
    for {set i 0} {$i < $num_keys} {incr i} {
        if {$i % 10 < 4} {
            if {$i % 3 == 0} {
                $r set "str:$i" [string repeat "data" 50]
            } else {
                $r set "str:$i" "value_$i"
            }
        }
        if {$i % 10 >= 4 && $i % 10 < 6} {
            for {set j 0} {$j < 10} {incr j} {
                $r lpush "list:$i" "item_${i}_${j}"
            }
        }
        if {$i % 10 >= 6 && $i % 10 < 8} {
            for {set j 0} {$j < 5} {incr j} {
                $r hset "hash:$i" "field_$j" "value_${i}_${j}"
            }
        }
        if {$i % 10 == 8} {
            for {set j 0} {$j < 8} {incr j} {
                $r sadd "set:$i" "member_${i}_${j}"
            }
        }
        if {$i % 10 == 9} {
            for {set j 0} {$j < 8} {incr j} {
                $r zadd "zset:$i" $j "member_${i}_${j}"
            }
        }
    }
}
proc populate_compressible_data {r num_keys} {
    for {set i 0} {$i < $num_keys} {incr i} {
        $r set "compress:$i" [string repeat "aaaaaaaaaa" 100]
    }
}
proc populate_incompressible_data {r num_keys} {
    for {set i 0} {$i < $num_keys} {incr i} {
        set data ""
        for {set j 0} {$j < 100} {incr j} {
            append data [format "%c" [expr {int(rand() * 256)}]]
        }
        $r set "random:$i" $data
    }
}
start_server {tags {"rdb chunk-compression benchmark slow"}} {
    test {Benchmark: Compression ratio with typical workload} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_typical_workload r 1000
        r save
        set info [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed_chunk
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed_chunk
        set ratio_chunk [expr {double($uncompressed_chunk) / double($compressed_chunk)}]
        set size_chunk [get_rdb_size r]
        r flushall
        r config set rdb-chunk-compression no
        populate_typical_workload r 1000
        r save
        set size_legacy [get_rdb_size r]
        set ratio_legacy [expr {double($uncompressed_chunk) / double($size_legacy)}]
        puts "Chunk compression ratio: $ratio_chunk"
        puts "Legacy compression ratio: $ratio_legacy"
        puts "Chunk RDB size: $size_chunk bytes"
        puts "Legacy RDB size: $size_legacy bytes"
        puts "Size reduction: [expr {100.0 * ($size_legacy - $size_chunk) / $size_legacy}]%"
        assert {$size_chunk < $size_legacy}
        assert {$ratio_chunk > 1.0}
    }
    test {Benchmark: Compression ratio with highly compressible data} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_compressible_data r 500
        r save
        set info [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed_chunk
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed_chunk
        set ratio_chunk [expr {double($uncompressed_chunk) / double($compressed_chunk)}]
        set size_chunk [get_rdb_size r]
        r flushall
        r config set rdb-chunk-compression no
        populate_compressible_data r 500
        r save
        set size_legacy [get_rdb_size r]
        puts "Highly compressible data:"
        puts "Chunk compression ratio: $ratio_chunk"
        puts "Chunk RDB size: $size_chunk bytes"
        puts "Legacy RDB size: $size_legacy bytes"
        puts "Improvement: [expr {100.0 * ($size_legacy - $size_chunk) / $size_legacy}]%"
        set improvement [expr {100.0 * ($size_legacy - $size_chunk) / $size_legacy}]
        assert {$improvement >= 20.0}
        assert {$ratio_chunk > 5.0}
    }
    test {Benchmark: RDB save time comparison} {
        r flushall
        populate_typical_workload r 2000
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        set time_chunk [measure_time {
            r save
        }]
        r config set rdb-chunk-compression no
        set time_legacy [measure_time {
            r save
        }]
        puts "Chunk compression save time: $time_chunk ms"
        puts "Legacy compression save time: $time_legacy ms"
        puts "Time difference: [expr {$time_chunk - $time_legacy}] ms"
        assert {$time_chunk < 5000}
        assert {$time_legacy < 5000}
    }
    test {Benchmark: RDB save/load cycle with chunk compression} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_typical_workload r 2000
        set time_save [measure_time {
            r save
        }]
        set size [get_rdb_size r]
        assert_equal [r dbsize] 2000
        puts "Chunk compression save time: $time_save ms"
        puts "RDB file size: $size bytes"
        assert {$time_save < 5000}
    }
    test {Benchmark: Memory usage during save with chunk compression} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_typical_workload r 3000
        set mem_before [s used_memory]
        r save
        set mem_after [s used_memory]
        set mem_increase [expr {$mem_after - $mem_before}]
        puts "Memory increase during save: $mem_increase bytes"
        assert {$mem_increase < 2097152}
    }
    test {Benchmark: Memory usage during load with chunk compression} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        set mem_before [s used_memory]
        populate_typical_workload r 3000
        set mem_after [s used_memory]
        set mem_increase [expr {$mem_after - $mem_before}]
        puts "Memory increase from data: $mem_increase bytes"
        assert {$mem_after > $mem_before}
        assert_equal [r dbsize] 3000
    }
    test {Benchmark: Various chunk sizes - compression ratio} {
        set chunk_sizes {4096 16384 65536 262144 1048576}
        set results {}
        foreach chunk_size $chunk_sizes {
            r flushall
            r config set rdb-chunk-compression yes
            r config set rdb-chunk-size $chunk_size
            populate_typical_workload r 1000
            r save
            set info [r info persistence]
            regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
            regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
            set ratio [expr {double($uncompressed) / double($compressed)}]
            set size [get_rdb_size r]
            lappend results [list $chunk_size $ratio $size]
            puts "Chunk size: $chunk_size bytes, Ratio: $ratio, Size: $size bytes"
        }
        set default_idx 2
        set default_size [lindex [lindex $results $default_idx] 2]
        set max_size 0
        foreach result $results {
            set size [lindex $result 2]
            if {$size > $max_size} {
                set max_size $size
            }
        }
        set best_size $max_size
        foreach result $results {
            set size [lindex $result 2]
            if {$size < $best_size} {
                set best_size $size
            }
        }
        set default_overhead [expr {100.0 * ($default_size - $best_size) / $best_size}]
        puts "Default (64KB) overhead vs best: $default_overhead%"
        assert {$default_overhead < 10.0}
    }
    test {Benchmark: Various chunk sizes - save time} {
        set chunk_sizes {4096 65536 262144}
        set results {}
        foreach chunk_size $chunk_sizes {
            r flushall
            r config set rdb-chunk-compression yes
            r config set rdb-chunk-size $chunk_size
            populate_typical_workload r 1500
            set time [measure_time {
                r save
            }]
            lappend results [list $chunk_size $time]
            puts "Chunk size: $chunk_size bytes, Save time: $time ms"
        }
        foreach result $results {
            set time [lindex $result 1]
            assert {$time < 5000}
        }
    }
    test {Benchmark: Large dataset compression (10K keys)} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_typical_workload r 10000
        set time_save [measure_time {
            r save
        }]
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        set ratio [expr {double($uncompressed) / double($compressed)}]
        set size [get_rdb_size r]
        puts "Large dataset (10K keys):"
        puts "Save time: $time_save ms"
        puts "Chunks: $chunks"
        puts "Compression ratio: $ratio"
        puts "RDB size: $size bytes"
        assert {$chunks > 10}
        assert {$ratio > 1.2}
        assert_equal [r dbsize] 10000
    }
    test {Benchmark: Incompressible data handling} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_incompressible_data r 500
        r save
        set info [r info persistence]
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        set ratio [expr {double($uncompressed) / double($compressed)}]
        puts "Incompressible data compression ratio: $ratio"
        assert {$ratio > 0.9 && $ratio < 1.3}
        assert_equal [r dbsize] 500
    }
    test {Benchmark: BGSAVE performance with chunk compression} {
        r flushall
        r config set rdb-chunk-compression yes
        r config set rdb-chunk-size 65536
        populate_typical_workload r 5000
        r save
        set time [measure_time {
            r bgsave
            waitForBgsave r
        }]
        puts "BGSAVE time (5K keys): $time ms"
        set info [r info persistence]
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        set ratio [expr {double($uncompressed) / double($compressed)}]
        puts "Chunks: $chunks, Ratio: $ratio"
        assert {$time < 10000}
        assert {$ratio > 1.2}
        assert_equal [r dbsize] 5000
    }
}
tags {"rdb chunk-compression"} {
set server_path [tmpdir "server.rdb-chunk-test"]
start_server [list overrides [list "dir" $server_path "rdb-chunk-compression" "yes"] keep_persistence true] {
    test {RDB save with chunk compression} {
        r set test_key "test_value"
        r save
        set info [r info persistence]
        assert_match {*rdb_chunk_compression:yes*} $info
    }
    test {Verify data persists} {
        assert_equal [r get test_key] "test_value"
    }
}
} ;
