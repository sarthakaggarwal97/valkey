# Focused harness for the "diskless no replicas drop during rdb pipe" case.
# This avoids unrelated replication.tcl setup/tests when looping in CI.
start_server {tags {"repl external:skip"} overrides {save ""}} {
    set master [srv 0 client]
    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 5
    $master config set repl-diskless-sync-max-replicas 2
    $master config set dual-channel-replication-enabled no
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    # Match the original test's dataset size so the pipe transfer lasts long
    # enough to exercise the blocked-writer path.
    $master debug populate 20000 test 10000
    $master config set rdbcompression no

    test "diskless no replicas drop during rdb pipe" {
        set replicas {}
        start_server {overrides {save ""}} {
            lappend replicas [srv 0 client]
            start_server {overrides {save ""}} {
                lappend replicas [srv 0 client]

                set loglines [count_log_lines -2]
                [lindex $replicas 0] config set repl-diskless-load swapdb
                # Slow one replica down so the master blocks on the RDB pipe.
                [lindex $replicas 0] config set key-load-delay 100
                [lindex $replicas 0] replicaof $master_host $master_port
                [lindex $replicas 1] replicaof $master_host $master_port

                # Wait until the slow replica starts consuming the streamed RDB.
                wait_for_log_messages -1 {"*Loading DB in memory*"} 0 1500 10

                after 500
                $master incr no

                wait_for_condition 1800 100 {
                    [s -2 rdb_bgsave_in_progress] == 0
                } else {
                    fail "rdb child didn't terminate"
                }

                wait_for_log_messages -2 {"*Diskless rdb transfer, done reading from pipe, 2 replicas still up*"} $loglines 1 1

                foreach replica $replicas {
                    wait_for_condition 150 100 {
                        [lindex [$replica role] 3] eq {connected}
                    } else {
                        fail "replicas still not connected after some time"
                    }

                    wait_for_condition 50 100 {
                        [$master dbsize] == [$replica dbsize]
                    } else {
                        fail "Different number of keys between master and replicas after too long time."
                    }

                    set digest [$master debug digest]
                    set replica_digest [$replica debug digest]
                    assert {$digest ne 0000000000000000000000000000000000000000}
                    assert {$digest eq $replica_digest}
                }
            }
        }
    }
}
