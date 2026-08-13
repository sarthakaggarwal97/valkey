tags {"repl repl-compression external:skip"} {

# uncompressed_bytes= from the replica line of the primary's INFO replication.
proc replica_line_uncompressed_bytes {primary} {
    set info [$primary info replication]
    assert {[regexp {uncompressed_bytes=([0-9]+)} $info -> ub]}
    return $ub
}

# ============================================================
# Config CRUD — single-server tests, no replication needed
# ============================================================

start_server {overrides {save "" repl-compression no}} {

    test {Repl compression config defaults are correct} {
        assert_equal "no" [lindex [r config get repl-compression] 1]
    }

    test {repl-compression can be toggled on and off} {
        r config set repl-compression lz4
        assert_equal "lz4" [lindex [r config get repl-compression] 1]
        r config set repl-compression yes
        assert_equal "yes" [lindex [r config get repl-compression] 1]
        r config set repl-compression no
        assert_equal "no" [lindex [r config get repl-compression] 1]
    }

    test {Repl compression configs survive CONFIG REWRITE and restart} {
        r config set repl-compression lz4
        r config rewrite

        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get repl-compression] 1]

        # Restore default
        r config set repl-compression no
    }
}

# ============================================================
# Replication handshake behavior — primary + replica tests
# ============================================================

start_server {tags {"repl"} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    # Capability negotiation (AND rule): compression activates only when BOTH
    # primary and replica run repl-compression lz4. Folds the old per-config
    # handshake tests (replica off, replica lz4 diskless, replica lz4
    # disk-backed, primary-side no-op full sync, and the backward-compat
    # non-capable replica) into one table. Each row seeds a dataset so the full
    # sync carries data, then writes an incremental key to exercise the stream.
    foreach {label primary_comp replica_cfg expect_compressed} {
        "both plaintext"       no  {save "" repl-compression no}                              0
        "lz4 diskless load"    lz4 {save "" repl-compression lz4 repl-diskless-load swapdb}   1
        "lz4 disk-backed load" lz4 {save "" repl-compression lz4 repl-diskless-load disabled} 1
        "yes default algo"     yes {save "" repl-compression yes repl-diskless-load swapdb}   1
        "non-capable replica"  lz4 {save "" repl-compression no}                              0
    } {
        test "Capability negotiation: $label" {
            $primary config set repl-compression $primary_comp
            $primary flushall
            for {set i 0} {$i < 100} {incr i} {
                $primary set "nego:$i" [string repeat "value$i " 10]
            }

            start_server [list overrides $replica_cfg] {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                if {$expect_compressed} {
                    wait_for_condition 50 200 {
                        [regexp -all "compression=lz4" [$primary info replication]] >= 1
                    } else {
                        fail "Compression not negotiated for $label"
                    }
                } else {
                    assert_equal 0 [regexp -all "compression=lz4" [$primary info replication]]
                }

                # Full sync carried the seeded dataset intact.
                assert_equal 100 [$replica dbsize]
                assert_equal [string repeat "value42 " 10] [$replica get nego:42]

                # Post-sync incremental write round-trips over the negotiated link.
                $primary set nego:incr incrval
                wait_for_condition 50 100 {
                    [$replica get nego:incr] eq {incrval}
                } else {
                    fail "Incremental write did not reach replica for $label"
                }
                assert_equal [$primary debug digest] [$replica debug digest]

                $replica replicaof no one
            }
            $primary config set repl-compression no
        }
    }

    test {Toggling repl-compression mid-runtime affects the next handshake} {
        # First sync with compression disabled
        start_server {overrides {save "" repl-compression no repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started with repl-compression no"
            }

            # Toggle compression on at runtime
            $replica config set repl-compression lz4

            # Disconnect and reconnect to trigger a new handshake
            $replica replicaof no one
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started after toggling repl-compression lz4"
            }

            $replica replicaof no one
        }
    }

    # Data integrity across payload shapes over one compressed incremental link.
    # Folds the small-multi-key, large-compressible, and large-incompressible
    # round-trip tests. One shared replica; flushall between shapes. The batch
    # limit is REPL_COMPRESSION_BATCH_LIMIT (1 MB); the large shapes cross it.
    $primary config set repl-compression lz4
    $primary flushall
    start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
        set replica [srv 0 client]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        wait_for_condition 50 200 {
            [string match {*state=online*compression=lz4*} [$primary info replication]]
        } else {
            fail "Compression not active on replica"
        }

        foreach {label kind} {
            "small multi-key"            small
            "large compressible value"   compressible
            "large incompressible value" incompressible
        } {
            test "Compressed incremental replication: $label" {
                $primary flushall
                wait_for_condition 50 100 {
                    [$replica dbsize] == 0
                } else {
                    fail "Replica did not observe flush before $label"
                }

                # Build the payload set for this shape.
                unset -nocomplain kv
                if {$kind eq "small"} {
                    for {set i 0} {$i < 50} {incr i} {
                        lappend kv "small:$i" [string repeat "payload$i " 20]
                    }
                } elseif {$kind eq "compressible"} {
                    # ~4 MB value, well past the 1 MB batch limit.
                    lappend kv bigkey [string repeat "abcdefghij0123456789" 209715]
                } else {
                    # Deterministic pseudo-random binary payload >= 1.5 MiB built
                    # in 4096-byte chunks from a seeded PRNG so failures are
                    # reproducible; exercises the codec's ratio~1 expansion path
                    # across multiple dispatches.
                    expr {srand(424242)}
                    set payload ""
                    while {[string length $payload] < 1572864} {
                        set chunk ""
                        for {set i 0} {$i < 4096} {incr i} {
                            append chunk [format %c [expr {int(rand()*256)}]]
                        }
                        append payload $chunk
                    }
                    lappend kv incompressible_key $payload
                }

                foreach {k v} $kv {
                    $primary set $k $v
                }

                # Every key round-trips intact.
                foreach {k v} $kv {
                    wait_for_condition 50 200 {
                        [$replica get $k] eq $v
                    } else {
                        fail "Value for $k did not replicate intact ($label)"
                    }
                }

                # Link stayed compressed and reported no codec errors. Per-replica
                # lines carry no error field; the server-global counter is always
                # emitted and reads zero.
                set info [$primary info replication]
                assert_match "*compression=lz4*" $info
                assert_equal 0 [string match "*compression_errors=*" $info]
                assert_match "*repl_compression_errors:0*" $info
            }
        }

        $replica replicaof no one
    }
    $primary config set repl-compression no

    test {Backlog cursor stays pinned until the compressed batch fully drains} {
        # The cursor advances only on full out_buf drain: pause the replica and
        # uncompressed_bytes must freeze while master_repl_offset grows.
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }
            wait_for_condition 50 200 {
                [string match {*state=online*compression=lz4*} [$primary info replication]]
            } else {
                fail "Compression not active on replica"
            }

            $primary set pin:baseline baseline_val
            wait_for_ofs_sync $primary $replica
            set base_ub [replica_line_uncompressed_bytes $primary]
            set base_off [status $primary master_repl_offset]
            # The link must survive on the same connection (no resync).
            set sync_full_before [status $primary sync_full]
            set sync_partial_before [status $primary sync_partial_ok]

            pause_process $replica_pid

            # Pseudo-random 100KB block (past LZ4's 64KB window): ratio ~1
            # overfills the socket buffers and leaves out_buf mid-batch.
            expr {srand(51555)}
            set payload ""
            while {[string length $payload] < 102400} {
                set chunk ""
                for {set i 0} {$i < 4096} {incr i} {
                    append chunk [format %c [expr {int(rand()*256)}]]
                }
                append payload $chunk
            }
            for {set i 0} {$i < 200} {incr i} {
                $primary set "pin:burst:$i" $payload
            }

            # Wait for the residual kernel-buffer drain to settle.
            set prev [replica_line_uncompressed_bytes $primary]
            set settled 0
            for {set i 0} {$i < 100} {incr i} {
                after 100
                set cur [replica_line_uncompressed_bytes $primary]
                if {$cur == $prev} {
                    set settled 1
                    break
                }
                set prev $cur
            }
            assert {$settled == 1}

            # Frozen cursor: two samples with writes in between must be equal.
            set ub1 [replica_line_uncompressed_bytes $primary]
            set off1 [status $primary master_repl_offset]
            for {set i 0} {$i < 20} {incr i} {
                $primary set "pin:tick:$i" tick_val
            }
            after 300
            set ub2 [replica_line_uncompressed_bytes $primary]
            set off2 [status $primary master_repl_offset]

            assert {$off2 > $off1}
            assert_equal $ub1 $ub2
            # The 3/4 margin absorbs socket buffer sizing, no exact values.
            assert {($ub2 - $base_ub) * 4 < ($off2 - $base_off) * 3}

            # Resume: pinned batches drain, cursor advances, data intact.
            resume_process $replica_pid
            wait_for_ofs_sync $primary $replica
            assert {[$replica get pin:burst:199] eq $payload}
            assert_equal tick_val [$replica get pin:tick:19]
            assert {[replica_line_uncompressed_bytes $primary] > $ub2}
            assert_equal $sync_full_before [status $primary sync_full]
            assert_equal $sync_partial_before [status $primary sync_partial_ok]

            $replica replicaof no one
        }

        $primary config set repl-compression no
    }

    test {Partial resync with compression delivers correct data} {
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            # Write initial data
            $primary set key1 value1

            wait_for_condition 50 100 {
                [$replica get key1] eq {value1}
            } else {
                fail "Initial replication failed"
            }

            # Break the replication link from the primary side. The replica keeps
            # its cached primary + offset and auto-reconnects, which exercises the
            # compressed *partial* resync path (REPLICAOF NO ONE would force a full
            # resync instead).
            set partial_before [status $primary sync_partial_ok]
            $primary client kill type replica

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Reconnection failed"
            }

            # Confirm a partial resync actually happened (not a silent full sync)
            wait_for_condition 50 100 {
                [status $primary sync_partial_ok] > $partial_before
            } else {
                fail "Expected a partial resync after reconnect, but none occurred"
            }

            # Write more data after partial resync
            $primary set key2 value2
            $primary set key3 [string repeat "x" 1000]

            wait_for_condition 50 100 {
                [$replica get key3] eq [string repeat "x" 1000]
            } else {
                fail "Post-partial-resync replication failed"
            }

            assert_equal value1 [$replica get key1]
            assert_equal value2 [$replica get key2]

            $replica replicaof no one
        }

        $primary config set repl-compression no
    }

    test {Replica with repl-compression lz4 handles a plaintext primary (passthrough)} {
        # Primary has compression OFF, replica ON: the replica advertises the
        # capability but the primary sends plaintext, so the replica must pass
        # the stream through untouched rather than expecting a VCS envelope.
        $primary config set repl-compression no
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started (primary plaintext, replica compression on)"
            }

            # Incremental writes arrive as plaintext; passthrough must deliver them.
            for {set i 0} {$i < 50} {incr i} {
                $primary set "pt:$i" [string repeat "payload$i " 20]
            }
            wait_for_condition 50 100 {
                [$replica get pt:49] eq [string repeat "payload49 " 20]
            } else {
                fail "Replica did not receive plaintext data via passthrough"
            }
            assert_equal [$primary dbsize] [$replica dbsize]

            # Primary never compressed (its config is off).
            assert_equal 0 [string match {*compression=lz4*} [$primary info replication]]

            $replica replicaof no one
        }
    }

    test {Replica repl-compression flip renegotiates upstream without manual reconnect} {
        $primary config set repl-compression lz4

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            # Flipping compression OFF on the replica must, on its own, drop and
            # reconnect the upstream link so it re-advertises capa without
            # compression. No manual replicaof is issued.
            $replica config set repl-compression no

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                ![string match {*compression=lz4*} [$primary info replication]]
            } else {
                fail "Replica did not renegotiate to plaintext after flip"
            }

            # Data still flows after the renegotiation.
            $primary set flipkey flipval
            wait_for_condition 50 100 {
                [$replica get flipkey] eq {flipval}
            } else {
                fail "Data not replicated after replica-side flip"
            }

            $replica replicaof no one
        }
    }

    test {Multiple compressed replicas receive replication correctly} {
        # Verifies that multiple compressed replicas can connect to the same
        # primary and all receive replication data on the main-thread write
        # path (io-threads multi-replica coverage lives in a later test).
        $primary config set repl-compression lz4

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica1 [srv 0 client]
            $replica1 replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replica 1 not started"
            }

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set replica2 [srv 0 client]
                $replica2 replicaof $primary_host $primary_port

                wait_for_condition 50 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "Replica 2 not started"
                }

                # Write data and verify both replicas receive it
                for {set i 0} {$i < 30} {incr i} {
                    $primary set "multi_repl:$i" "value_$i"
                }

                wait_for_condition 50 100 {
                    [$replica1 get "multi_repl:29"] eq {value_29} &&
                    [$replica2 get "multi_repl:29"] eq {value_29}
                } else {
                    fail "Not all replicas caught up"
                }

                # Verify both have compression active
                set info [$primary info replication]
                set matches [regexp -all "compression=lz4" $info]
                assert {$matches >= 2}

                $replica2 replicaof no one
            }
            $replica1 replicaof no one
        }
        $primary config set repl-compression no
    }

    test {Compressed replication works with io-threads enabled on the replica} {
        $primary config set repl-compression lz4
        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb io-threads 4 io-threads-always-active yes}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }

            # The link is compressed while the replica runs io threads; the
            # primary-link decode stays on the replica's main thread.
            wait_for_condition 50 200 {
                [string match {*state=online*compression=lz4*} [$primary info replication]]
            } else {
                fail "Compression not active on replica"
            }

            $primary set io_test_key "io_test_value"
            wait_for_condition 50 100 {
                [$replica get io_test_key] eq {io_test_value}
            } else {
                fail "Initial replication failed"
            }

            for {set i 0} {$i < 20} {incr i} {
                $primary set "io_threads:$i" "value_$i"
            }
            wait_for_condition 50 100 {
                [$replica get "io_threads:19"] eq {value_19}
            } else {
                fail "Replication with replica io-threads failed"
            }

            $replica replicaof no one
        }
        $primary config set repl-compression no
    }

    test {Dual-channel full sync with compression delivers writes made during load} {
        $primary config set repl-compression lz4
        $primary config set dual-channel-replication-enabled yes
        $primary config set rdb-key-save-delay 100
        $primary flushall
        $primary debug populate 10000 dc: 100

        start_server {overrides {save "" repl-compression lz4 dual-channel-replication-enabled yes}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            # rdb-key-save-delay stretches the RDB stage; catch the sync window.
            wait_for_condition 500 10 {
                [s 0 master_sync_in_progress] == 1
            } else {
                fail "Dual-channel sync did not start"
            }

            # Writes made during load reach the replica via the compressed main
            # channel: +CONTINUE starts compression, put-online must not restart
            # it (a second init would emit a new envelope mid-frame).
            for {set i 0} {$i < 200} {incr i} {
                $primary set "during_load:$i" "value_$i"
            }

            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not up after dual-channel sync"
            }

            wait_for_condition 50 100 {
                [$replica get "during_load:199"] eq {value_199}
            } else {
                fail "Writes made during load did not reach the replica"
            }
            for {set i 0} {$i < 200} {incr i} {
                assert_equal "value_$i" [$replica get "during_load:$i"]
            }
            assert_match "*compression=lz4*" [$primary info replication]
            wait_for_ofs_sync $primary $replica

            # A double init would emit a second envelope mid-frame on the first
            # post-online write, corrupting the replica and forcing a resync.
            # Stable sync counters prove the link survived that first write.
            set sync_full_before [s -1 sync_full]
            set sync_partial_before [s -1 sync_partial_ok]
            $primary set post_online_probe delivered
            wait_for_condition 50 100 {
                [$replica get post_online_probe] eq {delivered}
            } else {
                fail "Post-online write did not reach the replica"
            }
            assert_equal $sync_full_before [s -1 sync_full]
            assert_equal $sync_partial_before [s -1 sync_partial_ok]

            $replica replicaof no one
        }
        $primary config set rdb-key-save-delay 0
        $primary config set dual-channel-replication-enabled no
        $primary config set repl-compression no
    }

    # Flipping repl-compression while a dual-channel replica is still loading
    # freezes the link's decision at +CONTINUE; put-online sees the config
    # diverge, drops the link, and the replica renegotiates via a single partial
    # resync (no second RDB load). Folds the enable- and disable-during-load
    # cases into one direction table.
    foreach {label initial flipped expect_compressed_after} {
        "enable during load"  no  lz4 1
        "disable during load" lz4 no  0
    } {
        test "Dual-channel repl-compression $label converges after put-online" {
            $primary config set repl-compression $initial
            $primary config set dual-channel-replication-enabled yes
            $primary config set rdb-key-save-delay 100
            $primary flushall
            $primary debug populate 10000 midload: 100

            start_server {overrides {save "" repl-compression lz4 dual-channel-replication-enabled yes}} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                # Catch the window where the RDB still streams but the main
                # channel already got +CONTINUE.
                wait_for_condition 500 10 {
                    [s 0 master_sync_in_progress] == 1 &&
                    [string match "*state=bg_transfer*" [$primary info replication]]
                } else {
                    fail "Dual-channel sync window not reached ($label)"
                }

                # Traffic on the frozen-decision main channel before the flip.
                for {set i 0} {$i < 20} {incr i} {
                    $primary set "during_load:$i" "value_$i"
                }
                # bg_transfer implies the handshake (full resync + main-channel
                # +CONTINUE) is already counted, so later movement comes from the
                # renegotiation alone.
                set sync_full_before [s -1 sync_full]
                set sync_partial_before [s -1 sync_partial_ok]
                $primary config set repl-compression $flipped

                # Put-online sees the frozen decision diverge from the config and
                # drops the link; the replica renegotiates via one partial resync
                # (no second RDB load).
                wait_for_condition 100 100 {
                    [s -1 sync_partial_ok] == $sync_partial_before + 1
                } else {
                    fail "Renegotiation partial resync did not happen ($label)"
                }
                assert_equal $sync_full_before [s -1 sync_full]

                # Reconnected link comes up with the post-flip compression state.
                if {$expect_compressed_after} {
                    wait_for_condition 100 100 {
                        [s 0 master_link_status] eq {up} &&
                        [string match "*compression=lz4*" [$primary info replication]]
                    } else {
                        fail "Reconnected link did not come up compressed ($label)"
                    }
                } else {
                    wait_for_condition 100 100 {
                        [s 0 master_link_status] eq {up}
                    } else {
                        fail "Reconnected link did not come up ($label)"
                    }
                    assert_equal 0 [string match "*compression=lz4*" [$primary info replication]]
                }

                # Pre-flip traffic survived the reconnect; new writes still flow.
                wait_for_condition 50 100 {
                    [$replica get "during_load:19"] eq {value_19}
                } else {
                    fail "Pre-flip writes did not reach the replica ($label)"
                }
                for {set i 0} {$i < 20} {incr i} {
                    assert_equal "value_$i" [$replica get "during_load:$i"]
                }
                $primary set midload_probe delivered
                wait_for_condition 50 100 {
                    [$replica get midload_probe] eq {delivered}
                } else {
                    fail "Post-reconnect write did not reach the replica ($label)"
                }
                assert_equal [$primary dbsize] [$replica dbsize]

                # Exactly one renegotiation, and the RDB load was not redone.
                assert_equal [expr {$sync_partial_before + 1}] [s -1 sync_partial_ok]
                assert_equal $sync_full_before [s -1 sync_full]

                $replica replicaof no one
            }
            $primary config set rdb-key-save-delay 0
            $primary config set dual-channel-replication-enabled no
            $primary config set repl-compression no
        }
    }
}

# ============================================================
# Multi-replica compressed replication tests
# ============================================================

# Runtime repl-compression flips on the primary disconnect the affected replicas
# so they reconnect in the new mode. The disconnect/reconnect is deferred until
# after CONFIG SET commits (a rolled-back multi-option CONFIG SET drops/reconnects
# nothing). Folds the symmetric disable- and enable-at-runtime cases into one
# table sharing a single primary + replica.
start_server {tags {"repl"} overrides {save "" repl-compression no}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
        set replica [srv 0 client]

        foreach {label initial flipped expect_after} {
            "disable disconnects compressed replicas" lz4 no  0
            "enable reconnects capable replicas"      no  lz4 1
        } {
            test "Runtime repl-compression: $label" {
                $replica replicaof no one
                $primary config set repl-compression $initial
                $primary flushall
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                # Initial negotiated state matches the primary's starting mode.
                if {$initial eq "lz4"} {
                    wait_for_condition 50 100 {
                        [string match {*state=online*compression=lz4*} [$primary info replication]]
                    } else {
                        fail "Compression not active before flip ($label)"
                    }
                } else {
                    wait_for_condition 50 100 {
                        [status $replica master_link_status] eq "up"
                    } else {
                        fail "Replica did not sync ($label)"
                    }
                    assert_equal 0 [regexp -all "compression=lz4" [$primary info replication]]
                }

                # Flip on the primary: the affected replica is dropped and
                # reconnects in the new mode.
                $primary config set repl-compression $flipped

                if {$expect_after} {
                    wait_for_condition 50 100 {
                        [regexp -all "compression=lz4" [$primary info replication]] >= 1
                    } else {
                        fail "Capable replica did not reconnect compressed after enable"
                    }
                } else {
                    wait_for_condition 50 100 {
                        [regexp -all "compression=lz4" [$primary info replication]] == 0
                    } else {
                        fail "Compressed replica was not disconnected after disable"
                    }
                }
                wait_for_condition 50 100 {
                    [status $replica master_link_status] eq "up"
                } else {
                    fail "Replica did not reconnect after flip ($label)"
                }

                # Data flows correctly over the reconnected link.
                $primary set flip_probe flip_val
                wait_for_condition 50 100 {
                    [$replica get flip_probe] eq "flip_val"
                } else {
                    fail "Replication did not deliver after flip ($label)"
                }
                assert_equal [$primary debug digest] [$replica debug digest]
            }
        }
        $replica replicaof no one
    }
}

# Test 4: Multiple replicas distribute across threads and stay in sync.
# io-threads-always-active starts off during the handshakes: an offloaded
# REPLCONF reply can leave pending output that makes the primary reject PSYNC,
# and the legacy-SYNC fallback suppresses the ACK that diskless sync waits for
# (upstream race). It is enabled once all replicas are streaming.
start_server {tags {"repl"} overrides {save "" io-threads 4 repl-compression lz4}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Multiple replicas all stay in sync under load} {
        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica1 [srv 0 client]
            $replica1 replicaof $primary_host $primary_port
            wait_for_sync $replica1

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set replica2 [srv 0 client]
                $replica2 replicaof $primary_host $primary_port
                wait_for_sync $replica2

                start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                    set replica3 [srv 0 client]
                    $replica3 replicaof $primary_host $primary_port
                    wait_for_sync $replica3

                    # Wait for all replicas to have compression active
                    wait_for_condition 50 200 {
                        [regexp -all "compression=lz4" [$primary info replication]] >= 3
                    } else {
                        fail "Not all replicas have compression active"
                    }

                    # Handshakes are done; run the load phase on IO threads.
                    $primary config set io-threads-always-active yes

                    # Generate sustained load
                    for {set i 0} {$i < 500} {incr i} {
                        $primary set "multi_repl:$i" [string repeat "x" 100]
                    }

                    # Wait for all replicas to catch up
                    wait_for_condition 100 200 {
                        [$replica1 dbsize] == [$primary dbsize] &&
                        [$replica2 dbsize] == [$primary dbsize] &&
                        [$replica3 dbsize] == [$primary dbsize]
                    } else {
                        fail "Not all replicas caught up: r1=[$replica1 dbsize] r2=[$replica2 dbsize] r3=[$replica3 dbsize] primary=[$primary dbsize]"
                    }

                    # Verify data integrity
                    set primary_digest [$primary debug digest]
                    assert_equal $primary_digest [$replica1 debug digest]
                    assert_equal $primary_digest [$replica2 debug digest]
                    assert_equal $primary_digest [$replica3 debug digest]

                    $replica3 replicaof no one
                }
                $replica2 replicaof no one
            }
            $replica1 replicaof no one
        }
    }
}

# Test 5: Compressed replication survives replica disconnect/reconnect
start_server {tags {"repl"} overrides {save "" io-threads 4 io-threads-always-active yes repl-compression lz4}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Compressed replication survives replica disconnect and reconnect} {
        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica1 [srv 0 client]
            $replica1 replicaof $primary_host $primary_port
            wait_for_sync $replica1

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set replica2 [srv 0 client]
                $replica2 replicaof $primary_host $primary_port
                wait_for_sync $replica2

                # Drive traffic, verify both in sync
                for {set i 0} {$i < 200} {incr i} {
                    $primary set "pre_disconnect:$i" [string repeat "x" 50]
                }

                wait_for_condition 50 200 {
                    [$replica1 dbsize] == [$primary dbsize] &&
                    [$replica2 dbsize] == [$primary dbsize]
                } else {
                    fail "Replicas not in sync before disconnect"
                }

                # Disconnect replica1
                $replica1 replicaof no one

                # Drive more traffic — replica2 should stay connected and in sync
                for {set i 0} {$i < 200} {incr i} {
                    $primary set "post_disconnect:$i" [string repeat "x" 50]
                }

                wait_for_condition 50 200 {
                    [$replica2 get "post_disconnect:199"] eq [string repeat "x" 50]
                } else {
                    fail "Replica2 did not stay in sync after replica1 disconnect"
                }

                # Reconnect replica1
                $replica1 replicaof $primary_host $primary_port

                wait_for_condition 50 200 {
                    [status $replica1 master_link_status] eq "up"
                } else {
                    fail "Replica1 did not reconnect"
                }

                # Wait for replica1 to catch up
                wait_for_condition 50 200 {
                    [$replica1 dbsize] == [$primary dbsize]
                } else {
                    fail "Replica1 did not re-sync: replica1=[$replica1 dbsize] primary=[$primary dbsize]"
                }

                # Verify compression is re-negotiated on replica1
                wait_for_condition 50 200 {
                    [regexp -all "compression=lz4" [$primary info replication]] >= 2
                } else {
                    fail "Compression not re-negotiated after reconnect"
                }

                # Verify data integrity
                assert_equal [$primary debug digest] [$replica1 debug digest]
                assert_equal [$primary debug digest] [$replica2 debug digest]

                $replica2 replicaof no one
            }
            $replica1 replicaof no one
        }
    }
}

# Chained replication: each hop negotiates compression independently, and the
# middle node simultaneously decodes its primary link on the main thread while
# encoding for its own replica on IO threads.
start_server {tags {"repl"} overrides {save "" repl-compression lz4}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Chained replication compresses each hop independently} {
        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb io-threads 4 io-threads-always-active yes}} {
            set middle [srv 0 client]
            set middle_host [srv 0 host]
            set middle_port [srv 0 port]

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set leaf [srv 0 client]

                $middle replicaof $primary_host $primary_port
                wait_for_sync $middle
                $leaf replicaof $middle_host $middle_port
                wait_for_sync $leaf

                # Both hops negotiated compression.
                wait_for_condition 100 200 {
                    [regexp -all "compression=lz4" [$primary info replication]] >= 1 &&
                    [regexp -all "compression=lz4" [$middle info replication]] >= 1
                } else {
                    fail "Compression not active on both hops"
                }

                # Writes flow primary -> middle -> leaf across two compressed hops.
                for {set i 0} {$i < 200} {incr i} {
                    $primary set "chain:$i" "chain_value_$i"
                }
                wait_for_condition 100 200 {
                    [$leaf dbsize] == [$primary dbsize]
                } else {
                    fail "Leaf did not catch up: leaf=[$leaf dbsize] primary=[$primary dbsize]"
                }
                assert_equal "chain_value_0" [$leaf get chain:0]
                assert_equal "chain_value_99" [$leaf get chain:99]
                assert_equal "chain_value_199" [$leaf get chain:199]

                # Flip compression off on the leaf only and force a clean
                # reconnect: hop2 renegotiates plaintext, hop1 stays compressed.
                $leaf config set repl-compression no
                $leaf replicaof no one
                $leaf replicaof $middle_host $middle_port
                wait_for_sync $leaf

                wait_for_condition 100 200 {
                    [regexp -all "compression=lz4" [$middle info replication]] == 0
                } else {
                    fail "Hop2 still compressed after leaf disabled repl-compression"
                }
                assert {[regexp -all "compression=lz4" [$primary info replication]] >= 1}

                # Data still flows end-to-end over mixed hops.
                $primary set chain:final final_val
                wait_for_condition 100 200 {
                    [$leaf get chain:final] eq {final_val}
                } else {
                    fail "Write did not reach leaf after hop2 renegotiated plaintext"
                }

                $leaf replicaof no one
            }
            $middle replicaof no one
        }
    }
}


}
