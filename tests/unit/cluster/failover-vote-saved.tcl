# A primary that grants a failover vote must have the vote (lastVoteEpoch) in
# nodes.conf before the candidate can receive it. Otherwise a voter restarted
# after sending the vote but before saving it has forgotten the vote and can
# vote again in the same epoch.
#
# The voter grants the vote while it runs a busy script. It still serves the
# cluster bus from processEventsWhileBlocked(), but clusterBeforeSleep() does
# not run until the script ends. So when the candidate wins while the voter is
# still busy, the vote must already be on disk.

start_cluster 3 3 {tags {external:skip cluster}} {
    test "A failover vote is saved to nodes.conf before it is sent" {
        # R3 replicates R0 and is the candidate. R1 and R2 are the voters,
        # and the candidate needs both votes.
        assert_equal [lindex [R 3 role] 2] [srv 0 port]
        set primary_id [R 0 cluster myid]
        set candidate_id [R 3 cluster myid]
        set voter_conf [cluster_nodes_conf_path 1]

        # Hold the candidate back until the voter is busy.
        R 3 config set cluster-replica-no-failover yes
        set primary_pid [srv 0 pid]
        pause_process $primary_pid
        wait_node_marked_fail 1 $primary_id
        wait_node_marked_fail 2 $primary_id

        R 1 config set busy-reply-threshold 10
        set rd [valkey_deferring_client -1]
        $rd eval {while true do end} 0
        $rd flush
        wait_for_condition 50 100 {
            [catch {R 1 ping} e] == 1 && [string match {BUSY*} $e]
        } else {
            fail "R1 did not enter BUSY state"
        }

        R 3 config set cluster-replica-no-failover no
        wait_for_condition 1000 50 {
            [s -3 role] eq "master"
        } else {
            fail "R3 did not win the election"
        }

        # R1 voted for R3 and is still busy, so the vote can only be in its
        # nodes.conf if it was saved before the ACK was sent.
        assert_error {BUSY*} {R 1 ping}
        verify_log_message -1 "*Failover auth granted to $candidate_id*" 0
        set election_epoch [CI 3 cluster_my_epoch]
        assert {[regexp {lastVoteEpoch (\d+)} [read_file $voter_conf] -> saved_vote_epoch]}
        assert_equal $election_epoch $saved_vote_epoch

        R 1 script kill
        catch {$rd read}
        $rd close
        resume_process $primary_pid
    }
}
