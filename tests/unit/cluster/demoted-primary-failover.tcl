# Check that a primary demoted by a failover takes its slots back when the new
# primary fails before their first sync completes, and that it still refuses
# to fail over when its data is older than the replica validity factor allows.

# Keep the old primary from ever syncing with the new primary #id: it has no
# primaryauth, so once #id requires a password its PSYNC gets NOAUTH.
proc block_first_sync_from {id} {
    R $id CONFIG SET requirepass first-sync-blocker
}

proc wait_for_demotion {primary replica} {
    wait_for_condition 1000 50 {
        [s -$primary role] eq {slave} &&
        [s -$primary master_port] == [srv -$replica port]
    } else {
        fail "#$primary did not become a replica of #$replica"
    }
    assert_equal down [s -$primary master_link_status]
}

# With these settings the data of #id is too old to fail over after node
# timeout (3s) + repl-ping-replica-period (1s) + node timeout * validity factor
# (3s) = 7s.
proc lower_data_validity {id} {
    R $id CONFIG SET cluster-replica-validity-factor 1
    R $id CONFIG SET repl-ping-replica-period 1
}

proc assert_no_failover {id from_line} {
    wait_for_log_messages -$id {"*Currently unable to failover: Disconnected from primary for longer than allowed*"} $from_line 1000 50
    assert_equal slave [s -$id role]
}

start_cluster 3 3 {tags {external:skip cluster}} {
    test "Cluster is up" {
        wait_for_cluster_state ok
    }

    test "Demoted primary fails over when the new primary fails before the first sync" {
        block_first_sync_from 3
        R 3 CLUSTER FAILOVER
        wait_for_demotion 0 3
        pause_process [srv -3 pid]

        wait_for_condition 1000 50 {
            [s 0 role] eq {master}
        } else {
            fail "The demoted primary did not fail over"
        }
        resume_process [srv -3 pid]
    }

    test "Demoted primary does not fail over after staying disconnected for too long" {
        wait_for_cluster_state ok
        lower_data_validity 1
        block_first_sync_from 4
        R 4 CLUSTER FAILOVER
        wait_for_demotion 1 4
        after 8000
        set loglines [count_log_lines -1]
        pause_process [srv -4 pid]

        assert_no_failover 1 $loglines
        resume_process [srv -4 pid]
    }

    test "Primary that was stopped for too long does not fail over after its demotion" {
        # Stop the primary for longer than the data validity, so that its
        # replica fails it over meanwhile, and stop the new primary as soon as
        # the old one is demoted.
        wait_for_cluster_state ok
        lower_data_validity 2
        block_first_sync_from 5
        pause_process [srv -2 pid]
        wait_for_condition 1000 50 {
            [s -5 role] eq {master}
        } else {
            fail "#5 did not fail over"
        }
        after 8000
        set loglines [count_log_lines -2]
        resume_process [srv -2 pid]
        wait_for_demotion 2 5
        pause_process [srv -5 pid]

        assert_no_failover 2 $loglines
        resume_process [srv -5 pid]
    }
}
