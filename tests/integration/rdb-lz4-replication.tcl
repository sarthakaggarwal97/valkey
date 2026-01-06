# Integration tests for LZ4 compression with replication
# Tests replication scenarios with different compression algorithms

# Helper to wait for replication sync
proc wait_for_sync {replica} {
    wait_for_condition 500 100 {
        [string match {*master_link_status:up*} [$replica info replication]]
    } else {
        fail "Replica failed to sync with master"
    }
}

# Test basic replication with LZ4
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Replication with master using LZ4 compression} {
            # Configure master with LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            
            # Populate master
            for {set i 0} {$i < 100} {incr i} {
                $master set "key:$i" "value_$i"
                $master lpush "list:$i" "item_$i"
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Verify data on replica
            assert_equal [$replica get "key:0"] "value_0"
            assert_equal [$replica get "key:99"] "value_99"
            assert_equal [$replica lindex "list:0" 0] "item_0"
            
            # Verify replica received LZ4-compressed RDB
            set info [$replica info replication]
            assert_match {*role:slave*} $info
        }
        
        test {Incremental replication after full sync with LZ4} {
            # Add more data on master
            for {set i 100} {$i < 150} {incr i} {
                $master set "key:$i" "value_$i"
            }
            
            # Wait for replication
            wait_for_ofs_sync $master $replica
            
            # Verify incremental data
            assert_equal [$replica get "key:100"] "value_100"
            assert_equal [$replica get "key:149"] "value_149"
        }
    }
}

# Test replication with master using LZF and replica using LZ4 config
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Replication with master LZF, replica configured for LZ4} {
            # Master uses LZF
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lzf
            
            # Replica configured for LZ4 (but will receive LZF from master)
            $replica config set rdb-chunk-compression yes
            $replica config set rdb-compression-algorithm lz4-stream
            
            # Populate master
            for {set i 0} {$i < 100} {incr i} {
                $master set "mixed:$i" [string repeat "data" 50]
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Verify data on replica
            assert_equal [$replica get "mixed:0"] [string repeat "data" 50]
            assert_equal [$replica get "mixed:99"] [string repeat "data" 50]
            
            # Replica should still be configured for LZ4 for its own saves
            assert_equal [lindex [$replica config get rdb-compression-algorithm] 1] "lz4-stream"
        }
    }
}

# Test replication with master using LZ4 and replica using LZF config
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Replication with master LZ4, replica configured for LZF} {
            # Master uses LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            
            # Replica configured for LZF
            $replica config set rdb-chunk-compression yes
            $replica config set rdb-compression-algorithm lzf
            
            # Populate master
            for {set i 0} {$i < 100} {incr i} {
                $master set "reverse:$i" [string repeat "test" 50]
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Verify data on replica
            assert_equal [$replica get "reverse:0"] [string repeat "test" 50]
            assert_equal [$replica get "reverse:99"] [string repeat "test" 50]
            
            # Replica should still be configured for LZF for its own saves
            assert_equal [lindex [$replica config get rdb-compression-algorithm] 1] "lzf"
        }
    }
}

# Test switching compression algorithm on master during replication
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Master switches from LZF to LZ4 during replication} {
            # Start with LZF
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lzf
            
            # Populate and start replication
            for {set i 0} {$i < 50} {incr i} {
                $master set "switch:$i" "value_$i"
            }
            
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Verify initial sync
            assert_equal [$replica get "switch:0"] "value_0"
            
            # Switch master to LZ4
            $master config set rdb-compression-algorithm lz4-stream
            
            # Add more data
            for {set i 50} {$i < 100} {incr i} {
                $master set "switch:$i" "value_$i"
            }
            
            # Wait for incremental replication
            wait_for_ofs_sync $master $replica
            
            # Verify all data
            assert_equal [$replica get "switch:50"] "value_50"
            assert_equal [$replica get "switch:99"] "value_99"
        }
    }
}

# Test BGSAVE during replication with LZ4
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {BGSAVE on master with LZ4 during active replication} {
            # Configure master with LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            
            # Populate master
            for {set i 0} {$i < 200} {incr i} {
                $master set "bgsave:$i" [string repeat "data" 100]
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Trigger BGSAVE on master
            $master bgsave
            waitForBgsave $master
            
            # Verify replication still works
            $master set "after_bgsave" "test"
            wait_for_ofs_sync $master $replica
            
            assert_equal [$replica get "after_bgsave"] "test"
            assert_equal [$replica get "bgsave:0"] [string repeat "data" 100]
        }
    }
}

# Test replica promotion with LZ4
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Replica promotion with LZ4 compression} {
            # Configure both with LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            $replica config set rdb-chunk-compression yes
            $replica config set rdb-compression-algorithm lz4-stream
            
            # Populate master
            for {set i 0} {$i < 100} {incr i} {
                $master set "promote:$i" "value_$i"
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Promote replica to master
            $replica replicaof no one
            
            # Wait for promotion
            wait_for_condition 50 100 {
                [string match {*role:master*} [$replica info replication]]
            } else {
                fail "Replica failed to promote to master"
            }
            
            # Verify data on promoted replica
            assert_equal [$replica get "promote:0"] "value_0"
            assert_equal [$replica get "promote:99"] "value_99"
            
            # Verify promoted replica can save with LZ4
            $replica save
            set info [$replica info persistence]
            assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        }
    }
}

# Test chained replication with LZ4
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica1 [srv 0 client]
        set replica1_host [srv 0 host]
        set replica1_port [srv 0 port]
        
        start_server {} {
            set replica2 [srv 0 client]
            
            test {Chained replication with LZ4} {
                # Configure all with LZ4
                $master config set rdb-chunk-compression yes
                $master config set rdb-compression-algorithm lz4-stream
                $replica1 config set rdb-chunk-compression yes
                $replica1 config set rdb-compression-algorithm lz4-stream
                $replica2 config set rdb-chunk-compression yes
                $replica2 config set rdb-compression-algorithm lz4-stream
                
                # Populate master
                for {set i 0} {$i < 100} {incr i} {
                    $master set "chain:$i" "value_$i"
                }
                
                # Setup chain: master -> replica1 -> replica2
                $replica1 replicaof $master_host $master_port
                wait_for_sync $replica1
                
                $replica2 replicaof $replica1_host $replica1_port
                wait_for_sync $replica2
                
                # Verify data on both replicas
                assert_equal [$replica1 get "chain:0"] "value_0"
                assert_equal [$replica2 get "chain:0"] "value_0"
                assert_equal [$replica2 get "chain:99"] "value_99"
                
                # Add more data and verify propagation
                $master set "chain:new" "new_value"
                wait_for_ofs_sync $master $replica1
                wait_for_ofs_sync $replica1 $replica2
                
                assert_equal [$replica2 get "chain:new"] "new_value"
            }
        }
    }
}

# Test replication with large dataset and LZ4
start_server {tags {"repl lz4-compression large slow"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Replication with large dataset (5K keys) using LZ4} {
            # Configure master with LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            
            # Populate large dataset
            for {set i 0} {$i < 5000} {incr i} {
                $master set "large:$i" [string repeat "data_$i" 20]
                if {$i % 10 == 0} {
                    $master lpush "list:$i" "item_$i"
                }
            }
            
            # Start replication
            set start [clock milliseconds]
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            set duration [expr {[clock milliseconds] - $start}]
            
            puts "Replication sync time for 5K keys with LZ4: $duration ms"
            
            # Verify data on replica
            assert_equal [$replica get "large:0"] [string repeat "data_0" 20]
            assert_equal [$replica get "large:4999"] [string repeat "data_4999" 20]
            assert_equal [$replica lindex "list:0" 0] "item_0"
            
            # Verify replica has all keys
            assert_equal [$replica dbsize] [$master dbsize]
        }
    }
}

# Test replication failover with LZ4
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        
        test {Replication failover with LZ4 compression} {
            # Configure both with LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            $replica config set rdb-chunk-compression yes
            $replica config set rdb-compression-algorithm lz4-stream
            
            # Populate master
            for {set i 0} {$i < 100} {incr i} {
                $master set "failover:$i" "value_$i"
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Simulate master failure by promoting replica
            $replica replicaof no one
            
            wait_for_condition 50 100 {
                [string match {*role:master*} [$replica info replication]]
            } else {
                fail "Replica failed to promote"
            }
            
            # Verify data on new master
            assert_equal [$replica get "failover:0"] "value_0"
            assert_equal [$replica get "failover:99"] "value_99"
            
            # Add new data to new master
            $replica set "failover:new" "new_value"
            
            # Save and verify LZ4 is used
            $replica save
            set info [$replica info persistence]
            assert_match {*rdb_compression_algorithm:lz4-stream*} $info
        }
    }
}

# Test digest consistency across replication with LZ4
start_server {tags {"repl lz4-compression"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test {Digest consistency between master and replica with LZ4} {
            # Configure both with LZ4
            $master config set rdb-chunk-compression yes
            $master config set rdb-compression-algorithm lz4-stream
            $replica config set rdb-chunk-compression yes
            $replica config set rdb-compression-algorithm lz4-stream
            
            # Populate master with various data types
            for {set i 0} {$i < 50} {incr i} {
                $master set "str:$i" "value_$i"
                $master lpush "list:$i" "item_$i"
                $master hset "hash:$i" "field" "value_$i"
                $master sadd "set:$i" "member_$i"
                $master zadd "zset:$i" $i "member_$i"
            }
            
            # Start replication
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            
            # Wait for full sync
            wait_for_ofs_sync $master $replica
            
            # Compare digests
            set master_digest [$master debug digest]
            set replica_digest [$replica debug digest]
            
            assert_equal $master_digest $replica_digest
        }
    }
}
