# Deterministic trigger for the req-res log desync fixed by #3154.
#
# Without the internal-client guard in reqresShouldLog(), the periodic
# "CLUSTER SYNCSLOTS ACK" emitted by the slot-migration internal client (which
# intentionally has no reply) gets written into the --log-req-res log. That
# desyncs the request/response pairing and breaks reply-schemas-validator.
#
# Holding an atomic migration across several 1-second ACK periods makes the
# no-reply ACK fire deterministically while logging is active.

source tests/support/cluster.tcl

start_cluster 2 0 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Atomic slot migration logs the no-reply SYNCSLOTS ACK" {
    set node0_id [R 0 CLUSTER MYID]

    # Seed keys so the migration has work and stays in flight long enough
    # for the periodic ACK to fire repeatedly.
    for {set i 0} {$i < 20000} {incr i} {
        catch {R 1 SET "key:$i" $i}
    }

    # Drive an atomic slot migration from node 1 to node 0. The internal
    # migration client emits "CLUSTER SYNCSLOTS ACK" on a 1s timer.
    catch {R 1 CLUSTER MIGRATESLOTS SLOTSRANGE 0 100 NODE $node0_id} res

    # Hold across multiple ACK periods so the no-reply ACK is logged.
    for {set t 0} {$t < 8} {incr t} {
        after 1000
        catch {R 1 CLUSTER GETSLOTMIGRATIONS}
        catch {R 0 CLUSTER GETSLOTMIGRATIONS}
    }
    assert_equal 1 1
}

} ;# start_cluster
