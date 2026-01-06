# Test LZ4-stream compression with uncompressed chunks
# This test verifies that the streaming dictionary stays in sync when
# incompressible data causes chunks to be written uncompressed.
#
# Bug: When LZ4-stream compression returns 0 (incompressible), the encoder
# has already advanced its dictionary state, but the chunk is written
# uncompressed. During decompression, if we skip updating the decoder
# dictionary for uncompressed chunks, subsequent compressed chunks will
# fail because encoder and decoder dictionaries are out of sync.

start_server {tags {"rdb"}} {
    test "LZ4-stream handles uncompressed chunks correctly" {
        r flushall
        r config set rdb-compression-algorithm lz4-stream
        
        # Create a mix of compressible and incompressible data
        # This should trigger some uncompressed chunks
        
        # 1. Highly compressible data (should compress well)
        set compressible_value [string repeat "A" 10000]
        r set key:compressible:1 $compressible_value
        
        # 2. Random incompressible data (should write uncompressed)
        set random_value ""
        for {set i 0} {$i < 10000} {incr i} {
            append random_value [format "%c" [expr {int(rand() * 256)}]]
        }
        r set key:random:1 $random_value
        
        # 3. More compressible data (should compress well)
        # This is the critical test - if decoder dictionary wasn't updated
        # for the uncompressed chunk, this will fail to decompress
        set compressible_value2 [string repeat "B" 10000]
        r set key:compressible:2 $compressible_value2
        
        # 4. Another random chunk
        set random_value2 ""
        for {set i 0} {$i < 10000} {incr i} {
            append random_value2 [format "%c" [expr {int(rand() * 256)}]]
        }
        r set key:random:2 $random_value2
        
        # 5. Final compressible data
        set compressible_value3 [string repeat "C" 10000]
        r set key:compressible:3 $compressible_value3
        
        # Save RDB
        r save
        
        # Get original values
        set orig_comp1 [r get key:compressible:1]
        set orig_rand1 [r get key:random:1]
        set orig_comp2 [r get key:compressible:2]
        set orig_rand2 [r get key:random:2]
        set orig_comp3 [r get key:compressible:3]
        
        # Restart server to load RDB
        restart_server 0 true false
        
        # Verify all data loaded correctly
        # This is the critical test - if decoder dictionary wasn't updated
        # for uncompressed chunks, decompression would fail here
        assert_equal $orig_comp1 [r get key:compressible:1]
        assert_equal $orig_rand1 [r get key:random:1]
        assert_equal $orig_comp2 [r get key:compressible:2]
        assert_equal $orig_rand2 [r get key:random:2]
        assert_equal $orig_comp3 [r get key:compressible:3]
    }
    
    test "LZ4-stream handles large dataset with mixed compressibility" {
        r flushall
        r config set rdb-compression-algorithm lz4-stream
        
        # Create a larger dataset with alternating compressible/incompressible data
        set num_keys 100
        
        for {set i 0} {$i < $num_keys} {incr i} {
            if {$i % 2 == 0} {
                # Compressible data
                r set key:$i [string repeat [format "%c" [expr {65 + ($i % 26)}]] 1000]
            } else {
                # Random incompressible data
                set random_val ""
                for {set j 0} {$j < 1000} {incr j} {
                    append random_val [format "%c" [expr {int(rand() * 256)}]]
                }
                r set key:$i $random_val
            }
        }
        
        # Save and get checksums
        r save
        set checksums {}
        for {set i 0} {$i < $num_keys} {incr i} {
            lappend checksums [r get key:$i]
        }
        
        # Restart and verify
        restart_server 0 true false
        
        for {set i 0} {$i < $num_keys} {incr i} {
            set expected [lindex $checksums $i]
            set actual [r get key:$i]
            assert_equal $expected $actual
        }
    }
}
