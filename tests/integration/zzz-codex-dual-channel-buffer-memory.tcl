test "Dual channel replication buffer memory fields" {
    start_server {tags {"dual-channel-replication external:skip"}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        set primary_srv_id -1

        $primary config set repl-diskless-sync yes
        $primary config set repl-diskless-sync-delay 0
        $primary config set dual-channel-replication-enabled yes
        $primary config set repl-backlog-size 1
        $primary config set client-output-buffer-limit "replica 0 0 0"

        $primary config set rdb-key-save-delay 2000000
        for {set j 0} {$j < 1000} {incr j} {
            $primary set "key-$j" $j
        }

        start_server {} {
            set replica [srv 0 client]
            set replica_srv_id 0

            $replica config set dual-channel-replication-enabled yes
            $replica config set loading-process-events-interval-bytes 1024
            $replica config set client-output-buffer-limit "replica 0 0 0"

            $replica replicaof $primary_host $primary_port

            wait_for_condition 1000 50 {
                [string match "*slave*,state=wait_bgsave*,type=rdb-channel*" [$primary info replication]] &&
                [string match "*slave*,state=bg_transfer*,type=main-channel*" [$primary info replication]] &&
                [s $primary_srv_id rdb_bgsave_in_progress] eq 1 &&
                [s $replica_srv_id master_sync_in_progress] eq 1
            } else {
                fail "replica didn't start a dual-channel sync session in time"
            }

            set bigstr [string repeat x 1024000]
            for {set j 0} {$j < 50} {incr j} {
                $primary set key $bigstr
            }

            wait_for_condition 1000 50 {
               [s $primary_srv_id mem_total_replication_buffers] < [expr 1024000 * 10] &&
               [s $replica_srv_id mem_total_replication_buffers] > [expr 1024000 * 40]
            } else {
                fail "replica didn't receive the data in time"
            }

            $primary multi
            $primary info
            $primary memory stats
            lassign [$primary exec] primary_info primary_memory_stats

            assert_lessthan_equal [getInfoProperty $primary_info mem_total_replication_buffers] [expr 1024000 * 10]
            assert_equal 0 [getInfoProperty $primary_info mem_replicas_repl_buffer]
            assert_equal 0 [dict get $primary_memory_stats replicas.repl.buffer]

            $replica multi
            $replica info
            $replica memory stats
            lassign [$replica exec] replica_info replica_memory_stats

            assert_morethan_equal [getInfoProperty $replica_info used_memory_overhead] [expr 1024000 * 40]
            assert_equal [getInfoProperty $replica_info used_memory_overhead] [dict get $replica_memory_stats overhead.total]
            assert_morethan_equal [getInfoProperty $replica_info mem_total_replication_buffers] [expr 1024000 * 40]
            assert_equal [getInfoProperty $replica_info mem_replicas_repl_buffer] [getInfoProperty $replica_info mem_total_replication_buffers]
            assert_equal [getInfoProperty $replica_info mem_replicas_repl_buffer] [dict get $replica_memory_stats replicas.repl.buffer]
            assert_morethan_equal [getInfoProperty $replica_info replicas_repl_buffer_size] [expr 1024000 * 40]
            assert_morethan_equal [getInfoProperty $replica_info replicas_repl_buffer_peak] [expr 1024000 * 40]
        }
    }
}
