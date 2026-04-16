# Focused harness for the diskless replication subcases that exercise
# rdb-pipe behavior with two replicas.
start_server {tags {"repl external:skip"} overrides {save ""}} {
    set master [srv 0 client]
    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 5
    $master config set repl-diskless-sync-max-replicas 2
    $master config set dual-channel-replication-enabled no
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set master_pid [srv 0 pid]

    # Match the real test's dataset so the streamed RDB is large enough to
    # exercise the blocked-writer path.
    $master debug populate 20000 test 10000
    $master config set rdbcompression no

    set os [catch {exec uname}]
    set measure_time [expr {$os == "Linux"} ? 1 : 0]

    foreach all_drop {no slow fast all timeout} {
        test "diskless $all_drop replicas drop during rdb pipe" {
            set replicas {}
            set replicas_alive {}
            start_server {overrides {save ""}} {
                lappend replicas [srv 0 client]
                lappend replicas_alive [srv 0 client]
                start_server {overrides {save ""}} {
                    lappend replicas [srv 0 client]
                    lappend replicas_alive [srv 0 client]

                    set loglines [count_log_lines -2]
                    [lindex $replicas 0] config set repl-diskless-load swapdb
                    [lindex $replicas 0] replicaof $master_host $master_port
                    [lindex $replicas 1] replicaof $master_host $master_port

                    wait_for_log_messages -1 {"*Loading DB in memory*"} 0 1500 10

                    if {$measure_time} {
                        set master_statfile "/proc/$master_pid/stat"
                        set master_start_metrics [get_cpu_metrics $master_statfile]
                        set start_time [clock seconds]
                    }

                    # Use the same bounded slow-reader simulation as the real
                    # test so each subcase exercises the same timing path.
                    set slow_replica_pid [srv -1 pid]
                    pause_process $slow_replica_pid

                    after 500
                    $master incr $all_drop

                    if {$all_drop == "no" || $all_drop == "fast"} {
                        set slow_replica_resume_delay [expr {$all_drop == "no" ? 1000 : 1500}]
                        after $slow_replica_resume_delay
                        resume_process $slow_replica_pid
                    }

                    if {$all_drop == "all" || $all_drop == "slow"} {
                        resume_process $slow_replica_pid
                    }

                    if {$all_drop == "all" || $all_drop == "fast"} {
                        exec kill [srv 0 pid]
                        set replicas_alive [lreplace $replicas_alive 1 1]
                    }
                    if {$all_drop == "all" || $all_drop == "slow"} {
                        exec kill [srv -1 pid]
                        set replicas_alive [lreplace $replicas_alive 0 0]
                    }
                    if {$all_drop == "timeout"} {
                        $master config set repl-timeout 2
                        pause_process [srv -1 pid]
                        after 2000
                    }

                    wait_for_condition 2400 100 {
                        [s -2 rdb_bgsave_in_progress] == 0
                    } else {
                        fail "rdb child didn't terminate"
                    }

                    if {$all_drop == "all"} {
                        wait_for_log_messages -2 {"*Diskless rdb transfer, last replica dropped, killing fork child*"} $loglines 1 1
                    }
                    if {$all_drop == "no"} {
                        wait_for_log_messages -2 {"*Diskless rdb transfer, done reading from pipe, 2 replicas still up*"} $loglines 1 1
                    }
                    if {$all_drop == "slow" || $all_drop == "fast"} {
                        wait_for_log_messages -2 {"*Diskless rdb transfer, done reading from pipe, 1 replicas still up*"} $loglines 1 1
                    }
                    if {$all_drop == "timeout"} {
                        wait_for_log_messages -2 {"*Disconnecting timedout replica (full sync)*"} $loglines 1 1
                        wait_for_log_messages -2 {"*Diskless rdb transfer, done reading from pipe, 1 replicas still up*"} $loglines 1 1
                        set replicas_alive [lreplace $replicas_alive 0 0]
                        resume_process [srv -1 pid]
                    }

                    if {$measure_time} {
                        set master_end_metrics [get_cpu_metrics $master_statfile]
                        set time_elapsed [expr {[clock seconds]-$start_time}]
                        set master_cpu [compute_cpu_usage $master_start_metrics $master_end_metrics]
                        set master_utime [lindex $master_cpu 0]
                        set master_stime [lindex $master_cpu 1]
                        if {$::verbose} {
                            puts "elapsed: $time_elapsed"
                            puts "master utime: $master_utime"
                            puts "master stime: $master_stime"
                        }
                        if {!$::no_latency && ($all_drop == "all" || $all_drop == "slow" || $all_drop == "timeout")} {
                            assert {$master_utime < 70}
                            assert {$master_stime < 70}
                        }
                        if {!$::no_latency && ($all_drop == "none" || $all_drop == "fast")} {
                            assert {$master_utime < 15}
                            assert {$master_stime < 15}
                        }
                    }

                    foreach replica $replicas_alive {
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
}
