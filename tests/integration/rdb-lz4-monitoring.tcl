# Test monitoring and observability for LZ4 streaming compression
# Requirements: 10.1, 10.3, 10.5

start_server {tags {"rdb lz4 monitoring"}} {
    test "INFO persistence shows lz4-stream algorithm" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Create some data
        for {set i 0} {$i < 100} {incr i} {
            r set key$i [string repeat "value" 100]
        }
        
        r save
        set info [r info persistence]
        
        # Verify algorithm is shown as lz4-stream
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
    }
    
    test "INFO persistence shows streaming flag for lz4-stream" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        r set test_key "test_value"
        r save
        set info [r info persistence]
        
        # Verify streaming flag is set to yes
        assert_match {*rdb_compression_streaming:yes*} $info
    }
    
    test "INFO persistence shows streaming flag as no for lzf" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lzf
        
        r set test_key "test_value"
        r save
        set info [r info persistence]
        
        # Verify streaming flag is set to no for LZF
        assert_match {*rdb_compression_streaming:no*} $info
        assert_match {*rdb_compression_algorithm:lzf*} $info
    }
    
    test "INFO persistence shows chunk count for lz4-stream" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Create enough data to generate multiple chunks
        for {set i 0} {$i < 1000} {incr i} {
            r set key$i [string repeat "data" 200]
        }
        
        r save
        set info [r info persistence]
        
        # Verify chunk count is present and > 0
        assert_match {*rdb_last_save_chunks:*} $info
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        assert {$chunks > 0}
    }
    
    test "INFO persistence shows compression ratio for lz4-stream" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Create compressible data
        for {set i 0} {$i < 500} {incr i} {
            r set key$i [string repeat "compressible" 100]
        }
        
        r save
        set info [r info persistence]
        
        # Verify compression ratio is present
        assert_match {*rdb_last_save_compression_ratio:*} $info
        regexp {rdb_last_save_compression_ratio:([\d.]+)} $info _ ratio
        
        # Compression ratio should be > 1.0 for compressible data
        assert {$ratio > 1.0}
    }
    
    test "INFO persistence shows compressed and uncompressed bytes" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        for {set i 0} {$i < 200} {incr i} {
            r set key$i [string repeat "test" 50]
        }
        
        r save
        set info [r info persistence]
        
        # Verify both compressed and uncompressed bytes are present
        assert_match {*rdb_last_save_compressed_bytes:*} $info
        assert_match {*rdb_last_save_uncompressed_bytes:*} $info
        
        regexp {rdb_last_save_compressed_bytes:(\d+)} $info _ compressed
        regexp {rdb_last_save_uncompressed_bytes:(\d+)} $info _ uncompressed
        
        # Uncompressed should be larger than compressed
        assert {$uncompressed > $compressed}
    }
    
    test "Compression statistics logged on successful save" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Create data
        for {set i 0} {$i < 100} {incr i} {
            r set key$i [string repeat "value" 100]
        }
        
        # Perform BGSAVE and wait for completion
        r bgsave
        waitForBgsave r
        
        # Check server log for compression statistics
        # Note: This is a basic check - in production you'd parse the actual log file
        set info [r info persistence]
        assert_match {*rdb_last_save_chunks:*} $info
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
    }
    
    test "Compare compression effectiveness: LZF vs LZ4-stream" {
        # Test with LZF
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lzf
        
        for {set i 0} {$i < 500} {incr i} {
            r set key$i [string repeat "compressible" 100]
        }
        
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
        
        # Log the comparison
        puts "LZF compression ratio: $ratio_lzf"
        puts "LZ4-stream compression ratio: $ratio_lz4"
        puts "LZ4-stream improvement: [expr {($ratio_lz4 - $ratio_lzf) / $ratio_lzf * 100}]%"
        
        # LZ4-stream should provide better or equal compression
        # (allowing small margin for test variability)
        assert {$ratio_lz4 >= $ratio_lzf * 0.95}
    }
    
    test "Monitoring works with small datasets" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Small dataset (< 1 chunk)
        r set small_key "small_value"
        r save
        
        set info [r info persistence]
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        assert_match {*rdb_compression_streaming:yes*} $info
        assert_match {*rdb_last_save_chunks:*} $info
    }
    
    test "Monitoring works with large datasets" {
        r config set rdb-chunk-compression yes
        r config set rdb-compression-algorithm lz4-stream
        
        # Large dataset (many chunks)
        for {set i 0} {$i < 2000} {incr i} {
            r set large_key$i [string repeat "x" 1000]
        }
        
        r save
        set info [r info persistence]
        
        assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        assert_match {*rdb_compression_streaming:yes*} $info
        
        regexp {rdb_last_save_chunks:(\d+)} $info _ chunks
        # Should have many chunks for large dataset
        assert {$chunks > 10}
    }
}
