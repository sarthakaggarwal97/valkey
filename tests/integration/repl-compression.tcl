tags {"repl-compression external:skip needs:debug"} {

proc repl_compression_log_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

proc write_repl_compression_dataset {client prefix count} {
    $client flushall
    for {set i 0} {$i < $count} {incr i} {
        $client set "${prefix}:str:$i" [string repeat "${prefix}:payload:$i " 64]
    }
    $client lpush "${prefix}:list" a b c d e
    $client hset "${prefix}:hash" f1 v1 f2 [string repeat "${prefix}:hash " 32]
}

proc assert_repl_compression_sync {primary replica msg} {
    wait_for_sync $replica
    wait_done_loading $replica
    wait_for_condition 50 100 {
        [status $replica master_link_status] eq "up" &&
        [$primary debug digest] eq [$replica debug digest]
    } else {
        fail $msg
    }
}

proc read_repl_compression_eof_payload {client} {
    assert_match {FULLRESYNC * *} [$client read]

    set prefix [$client rawread 47]
    assert_equal {$EOF:} [string range $prefix 0 4]
    set eofmark [string range $prefix 5 44]
    assert_equal "\r\n" [string range $prefix 45 46]

    set rest [$client rawread]
    set marker_pos [string last $eofmark $rest]
    if {$marker_pos < 0} {
        fail "Diskless RDB EOF marker was not found"
    }
    string range $rest 0 [expr {$marker_pos - 1}]
}

proc assert_repl_compression_payload_codec_checksum {payload enabled} {
    assert_equal VKCS [string range $payload 0 3]
    binary scan [string index $payload 6] cu flags
    if {$enabled} {
        assert {($flags & 1) != 0}
    } else {
        assert {($flags & 1) == 0}
    }
}

start_server {overrides {save ""}} {
    set replica [srv 0 client]
    set replica_log [srv 0 stdout]
    $replica config set replcompression yes
    $replica config set repl-diskless-load swapdb

    start_server {overrides {save ""}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_log [srv 0 stdout]

        test {Diskless full sync RDB payload is compressed when both endpoints enable replication compression} {
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $primary config set replcompression yes
            write_repl_compression_dataset $primary compressed-full-sync 400

            $replica replicaof $primary_host $primary_port
            assert_repl_compression_sync $primary $replica "Replica digest mismatch after compressed diskless full sync"

            wait_for_condition 50 100 {
                [repl_compression_log_matches $primary_log "*target: replicas sockets*with compression: lz4*"]
            } else {
                fail "Primary did not log compressed diskless full sync"
            }
            wait_for_condition 50 100 {
                [repl_compression_log_matches $replica_log "*Loading compressed RDB (algo=lz4) from primary*"]
            } else {
                fail "Replica did not log compressed RDB load from primary"
            }

            $primary set compressed-full-sync:post "after"
            wait_for_condition 50 100 {
                [$replica get compressed-full-sync:post] eq "after"
            } else {
                fail "Replica did not receive post-sync write"
            }
        }
    }
}

start_server {overrides {save ""}} {
    set replica [srv 0 client]
    set replica_log [srv 0 stdout]
    $replica config set replcompression yes
    $replica config set repl-diskless-load swapdb

    start_server {overrides {save ""}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_log [srv 0 stdout]

        test {In-flight compressed full sync survives replcompression config change on replica} {
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $primary config set replcompression yes
            $primary config set rdb-key-save-delay 1000
            write_repl_compression_dataset $primary compressed-full-sync-config-change 300

            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [repl_compression_log_matches $primary_log "*target: replicas sockets*with compression: lz4*"]
            } else {
                fail "Primary did not start compressed diskless full sync"
            }

            $replica config set replcompression no
            assert_repl_compression_sync $primary $replica \
                "Replica digest mismatch after disabling replcompression during compressed full sync"

            wait_for_condition 50 100 {
                [repl_compression_log_matches $replica_log "*Loading compressed RDB (algo=lz4) from primary*"]
            } else {
                fail "Replica did not keep decoding the negotiated compressed RDB"
            }
            $primary config set rdb-key-save-delay 0
        }
    }
}

start_server {overrides {save ""}} {
    set replica_compressed [srv 0 client]
    $replica_compressed config set replcompression yes
    $replica_compressed config set repl-diskless-load swapdb

    start_server {overrides {save ""}} {
        set replica_plain [srv 0 client]
        $replica_plain config set replcompression no
        $replica_plain config set repl-diskless-load swapdb

        start_server {overrides {save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set primary_log [srv 0 stdout]

            test {Mixed diskless full sync cohort falls back to plain RDB payload} {
                $primary config set repl-diskless-sync yes
                $primary config set repl-diskless-sync-delay 100
                $primary config set repl-diskless-sync-max-replicas 2
                $primary config set replcompression yes
                write_repl_compression_dataset $primary mixed-full-sync 300

                $replica_compressed replicaof $primary_host $primary_port
                $replica_plain replicaof $primary_host $primary_port

                assert_repl_compression_sync $primary $replica_compressed \
                    "Compressed-capable replica digest mismatch after mixed full sync"
                assert_repl_compression_sync $primary $replica_plain \
                    "Plain replica digest mismatch after mixed full sync"
                assert {![repl_compression_log_matches $primary_log "*with compression: lz4*"]}
            }
        }
    }
}

start_server {overrides {save ""}} {
    set replica_a [srv 0 client]
    $replica_a config set replcompression yes
    $replica_a config set repl-diskless-load swapdb

    start_server {overrides {save ""}} {
        set replica_b [srv 0 client]
        $replica_b config set replcompression yes
        $replica_b config set repl-diskless-load swapdb

        start_server {overrides {save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set primary_log [srv 0 stdout]

            test {Multiple compressed replicas share a compressed diskless full sync payload} {
                $primary config set repl-diskless-sync yes
                $primary config set repl-diskless-sync-delay 100
                $primary config set repl-diskless-sync-max-replicas 2
                $primary config set replcompression yes
                write_repl_compression_dataset $primary multi-compressed-full-sync 300

                $replica_a replicaof $primary_host $primary_port
                $replica_b replicaof $primary_host $primary_port

                assert_repl_compression_sync $primary $replica_a \
                    "First compressed replica digest mismatch after multi-replica full sync"
                assert_repl_compression_sync $primary $replica_b \
                    "Second compressed replica digest mismatch after multi-replica full sync"
                wait_for_condition 50 100 {
                    [repl_compression_log_matches $primary_log "*target: replicas sockets*with compression: lz4*"]
                } else {
                    fail "Primary did not log compressed diskless full sync for multi-replica cohort"
                }
            }
        }
    }
}

start_server {overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set primary_log [srv 0 stdout]

    test {Filtered RDB-only full sync uses compressed socket payload when replica advertises compression} {
        $primary config set repl-diskless-sync no
        $primary config set replcompression yes
        write_repl_compression_dataset $primary filtered-compressed-full-sync 300

        set replica [valkey_deferring_client_by_addr $primary_host $primary_port]
        $replica replconf capa eof
        assert_equal OK [$replica read]
        $replica replconf capa compression
        assert_equal OK [$replica read]
        $replica replconf rdb-only 1
        assert_equal OK [$replica read]
        $replica replconf rdb-filter-only ""
        assert_equal OK [$replica read]
        $replica psync ? -1

        set payload [read_repl_compression_eof_payload $replica]
        $replica close

        assert_repl_compression_payload_codec_checksum $payload 1
        wait_for_condition 50 100 {
            [repl_compression_log_matches $primary_log "*target: replicas sockets*with compression: lz4*"]
        } else {
            fail "Primary did not log compressed filtered full sync"
        }
    }
}

start_server {overrides {save ""}} {
    set replica [srv 0 client]
    $replica config set replcompression yes
    $replica config set repl-diskless-load disabled

    start_server {overrides {save ""}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_log [srv 0 stdout]

        test {Diskless full sync compression works when replica writes transfer to disk first} {
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $primary config set replcompression yes
            write_repl_compression_dataset $primary compressed-full-sync-disk-load 300

            $replica replicaof $primary_host $primary_port
            assert_repl_compression_sync $primary $replica \
                "Replica digest mismatch after compressed diskless full sync loaded from disk"

            wait_for_condition 50 100 {
                [repl_compression_log_matches $primary_log "*target: replicas sockets*with compression: lz4*"]
            } else {
                fail "Primary did not log compressed diskless full sync"
            }
        }
    }
}

start_server {overrides {save ""}} {
    set replica [srv 0 client]
    set replica_log [srv 0 stdout]
    $replica config set replcompression yes
    $replica config set repl-diskless-load swapdb

    start_server {overrides {save "" rdbchecksum no}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_log [srv 0 stdout]

        test {Diskless full sync compression works when RDB checksum is disabled} {
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $primary config set replcompression yes
            write_repl_compression_dataset $primary compressed-full-sync-no-rdb-checksum 300

            $replica replicaof $primary_host $primary_port
            assert_repl_compression_sync $primary $replica \
                "Replica digest mismatch after compressed diskless full sync without RDB checksum"

            wait_for_condition 50 100 {
                [repl_compression_log_matches $primary_log "*target: replicas sockets*with compression: lz4*"]
            } else {
                fail "Primary did not log compressed diskless full sync"
            }
            wait_for_condition 50 100 {
                [repl_compression_log_matches $replica_log "*Loading compressed RDB (algo=lz4) from primary*"]
            } else {
                fail "Replica did not log compressed RDB load from primary"
            }

            set rdb_only_replica [valkey_deferring_client_by_addr $primary_host $primary_port]
            $rdb_only_replica replconf capa eof
            assert_equal OK [$rdb_only_replica read]
            $rdb_only_replica replconf capa compression
            assert_equal OK [$rdb_only_replica read]
            $rdb_only_replica replconf rdb-only 1
            assert_equal OK [$rdb_only_replica read]
            $rdb_only_replica psync ? -1

            set payload [read_repl_compression_eof_payload $rdb_only_replica]
            $rdb_only_replica close

            assert_repl_compression_payload_codec_checksum $payload 1
        }
    }
}

start_server {overrides {save ""}} {
    set replica [srv 0 client]
    set replica_log [srv 0 stdout]
    $replica config set replcompression yes
    $replica config set repl-diskless-load swapdb
    $replica config set dual-channel-replication-enabled yes

    start_server {overrides {save ""}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_log [srv 0 stdout]

        test {Dual-channel diskless full sync RDB payload is compressed when both endpoints enable replication compression} {
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $primary config set dual-channel-replication-enabled yes
            $primary config set replcompression yes
            write_repl_compression_dataset $primary dual-channel-compressed-full-sync 300

            $replica replicaof $primary_host $primary_port
            assert_repl_compression_sync $primary $replica \
                "Replica digest mismatch after compressed dual-channel full sync"

            wait_for_condition 50 100 {
                [repl_compression_log_matches $primary_log "*using: dual-channel*with compression: lz4*"]
            } else {
                fail "Primary did not log compressed dual-channel full sync"
            }
            wait_for_condition 50 100 {
                [repl_compression_log_matches $replica_log "*Loading compressed RDB (algo=lz4) from primary*"]
            } else {
                fail "Replica did not log compressed dual-channel RDB load from primary"
            }
        }
    }
}

start_server {overrides {save ""}} {
    set replica [srv 0 client]
    $replica config set replcompression yes
    $replica config set repl-diskless-load disabled
    $replica config set dual-channel-replication-enabled yes

    start_server {overrides {save ""}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_log [srv 0 stdout]

        test {Dual-channel diskless full sync compression works when replica writes transfer to disk first} {
            $primary config set repl-diskless-sync yes
            $primary config set repl-diskless-sync-delay 0
            $primary config set dual-channel-replication-enabled yes
            $primary config set replcompression yes
            write_repl_compression_dataset $primary dual-channel-disk-load-compressed-full-sync 300

            $replica replicaof $primary_host $primary_port
            assert_repl_compression_sync $primary $replica \
                "Replica digest mismatch after compressed dual-channel full sync loaded from disk"

            wait_for_condition 50 100 {
                [repl_compression_log_matches $primary_log "*using: dual-channel*with compression: lz4*"]
            } else {
                fail "Primary did not log compressed dual-channel full sync"
            }
        }
    }
}

}
