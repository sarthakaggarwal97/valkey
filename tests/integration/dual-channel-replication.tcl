proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

# Wait until the process enters a paused state.
proc wait_process_paused idx {
    set pid [srv $idx pid]
    wait_for_condition 50 1000 {
        [string match "T*" [exec ps -o state= -p $pid]]
    } else {
        fail "Process $pid didn't stop, current state is [exec ps -o state= -p $pid]"
    }
}

# Wait until the process enters a paused state, then resume the process.
proc wait_and_resume_process idx {
    set pid [srv $idx pid]
    wait_process_paused $idx
    resume_process $pid
}

start_server {tags {"dual-channel-replication external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set backlog_size [expr {10 ** 5}]
        set loglines [count_log_lines -1]

        $primary config set repl-diskless-sync yes
        $primary config set dual-channel-replication-enabled yes
        $primary config set repl-backlog-size $backlog_size
        $primary config set loglevel debug
        $primary config set repl-diskless-sync-delay 0
        if {$::valgrind} {
            $primary config set repl-timeout 100
            $replica config set repl-timeout 100
        } else {
            $primary config set repl-timeout 30
            $replica config set repl-timeout 30
        }

        # Avoids timeout by keeping the RDB child alive longer while the replica is inactive
        $primary config set rdb-key-save-delay 1000
        populate 10000 primary 10000
        
        set load_handle1 [start_one_key_write_load $primary_host $primary_port 100 "mykey1"]
        set load_handle2 [start_one_key_write_load $primary_host $primary_port 100 "mykey2"]
        set load_handle3 [start_one_key_write_load $primary_host $primary_port 100 "mykey3"]

        $replica config set dual-channel-replication-enabled yes
        $replica config set loglevel debug
        
        # Pause replica after primary fork
        $replica debug pause-after-fork 1

        test "dual-channel-replication: Primary COB growth with inactive replica" {
            $replica replicaof $primary_host $primary_port
            # Verify repl backlog can grow
            wait_for_condition 2000 10 {
                [s 0 mem_total_replication_buffers] > [expr {2 * $backlog_size}]
            } else {
                set cur [s 0 mem_total_replication_buffers]
                fail "Primary should allow backlog (have=$cur, need>[expr {2 * $backlog_size}]) to grow beyond its limits during dual-channel-replication sync handshake"
            }
            wait_and_resume_process -1

            set t0 [clock milliseconds]

            verify_replica_online $primary 0 5000
            wait_for_condition 50 1000 {
                [status $replica master_link_status] == "up"
            } else {
                fail "Replica is not synced"
            }

            set elapsed [expr {[clock milliseconds] - $t0}]
            puts "Replica master_link_status==up after ${elapsed} ms"

        }

        stop_write_load $load_handle1
        stop_write_load $load_handle2
        stop_write_load $load_handle3

    }
}