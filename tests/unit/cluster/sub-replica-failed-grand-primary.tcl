start_cluster 3 2 {tags {external:skip cluster}} {
    # This directly exercises the unsafe topology transition guarded by #3468.
    # The original fuzzer scenario relied on a stale same-shard packet, but on
    # current unstable that path is largely blocked by #2811's stale message
    # detection. Here we keep the packet flow real and only manufacture the last
    # step: a healthy primary B claims a failed grand-primary A while C is
    # already replicating B. Before the fix, C rewires itself to failed A.
    test "Node keeps current primary when sub-replica grand-primary is failed" {
        isolate_node 4

        set R0_nodeid [R 0 cluster myid]
        set R3_nodeid [R 3 cluster myid]

        # Turn R3 into an empty primary in its own shard, then add R4 as its
        # replica so we have a live C -> B relationship.
        assert_equal {OK} [R 3 cluster replicate no one]
        wait_for_condition 1000 10 {
            [lindex [R 3 role] 0] eq {master} &&
            [dict get [cluster_get_myself 3] slaveof] eq "-"
        } else {
            puts "R 3 cluster nodes:"
            puts [R 3 cluster nodes]
            fail "R3 did not become an empty primary"
        }

        R 4 cluster meet [srv -3 host] [srv -3 port]
        wait_for_condition 50 100 {
            [cluster_get_node_by_id 4 $R3_nodeid] != {}
        } else {
            fail "Node R4 never learned about node R3"
        }
        R 4 cluster replicate $R3_nodeid
        wait_for_sync [srv -4 client]

        wait_for_condition 1000 10 {
            [dict get [cluster_get_myself 4] slaveof] eq $R3_nodeid
        } else {
            puts "R 4 cluster nodes:"
            puts [R 4 cluster nodes]
            fail "R4 did not start replicating R3"
        }

        # Mark A as FAIL in C's local view.
        set primary0_pid [srv 0 pid]
        pause_process $primary0_pid
        wait_node_marked_fail 4 $R0_nodeid

        set loglines4 [count_log_lines -4]

        # Now make B claim failed A as its primary. Before the fix, R4 updates
        # itself from C -> B to C -> A due to the sub-replica repair path.
        assert_equal {OK} [R 3 cluster replicate $R0_nodeid]

        wait_for_condition 1000 10 {
            [dict get [cluster_get_myself 4] slaveof] eq $R3_nodeid &&
            [dict get [cluster_get_node_by_id 4 $R3_nodeid] slaveof] eq $R0_nodeid &&
            [cluster_has_flag [cluster_get_node_by_id 4 $R0_nodeid] fail] eq 1
        } else {
            puts "R 4 cluster nodes:"
            puts [R 4 cluster nodes]
            fail "R4 rewired itself away from its healthy primary"
        }

        set pattern "*Keeping my current primary $R3_nodeid*"
        verify_log_message -4 $pattern $loglines4

        resume_process $primary0_pid
    }
} ;# start_cluster
