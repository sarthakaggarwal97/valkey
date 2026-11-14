# A node must never close a loop in the replicaof chain of its view of the
# cluster, whatever the order in which it processes the packets of a failover.
#
# A node only updates the primary of another node when it processes a packet
# from that node, so its view of a shard can lag behind: after two failovers it
# may still know the latest primary as a replica of an older one. A packet from
# that older primary naming the latest primary as its primary would then close
# a loop, whether the packet is an old one that was delayed, or a new one that
# overtakes the promotion announcement of the latest primary.
#
# The nodes of the shard are paused and the packets they would have sent are
# injected into the observer from a plain TCP socket, which is the only way to
# get a deterministic processing order.

# Build a PING as the node 'sender_id' would send it, with no gossip section
# and no extensions. 'primary_id' is the primary the sender claims to replicate
# from, or "" if it claims to be a primary. 'slots' is the list of slot ranges
# claimed for the sender's shard. The layout matches packet.tcl.
proc create_cluster_ping_packet {sender_id sender_port epoch primary_id slots} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTER_NODE_PRIMARY 1
    set CLUSTER_NODE_REPLICA 2

    set bits [string repeat 0 $CLUSTER_SLOTS]
    foreach range $slots {
        lassign [split $range -] first last
        if {$last eq ""} {set last $first}
        set bits [string replace $bits $first $last [string repeat 1 [expr {$last - $first + 1}]]]
    }

    set packet ""
    append packet "RCmb"
    append packet [binary format I 0]                      ;# totlen, patched below
    append packet [binary format S 1]                      ;# ver
    append packet [binary format S $sender_port]           ;# port
    append packet [binary format S $CLUSTERMSG_TYPE_PING]
    append packet [binary format S 0]                      ;# count
    append packet [binary format W $epoch]                 ;# currentEpoch
    append packet [binary format W $epoch]                 ;# configEpoch
    append packet [binary format W 0]                      ;# offset
    append packet $sender_id
    append packet [binary format b$CLUSTER_SLOTS $bits]    ;# myslots
    if {$primary_id eq ""} {
        append packet [string repeat "\x00" $CLUSTER_NAMELEN]
        set flags $CLUSTER_NODE_PRIMARY
    } else {
        append packet $primary_id
        set flags $CLUSTER_NODE_REPLICA
    }
    append packet [string repeat "\x00" $NET_IP_STR_LEN]   ;# myip
    append packet [binary format S 0]                      ;# extensions
    append packet [string repeat "\x00" 30]                ;# notused1
    append packet [binary format S 0]                      ;# pport
    append packet [binary format S [expr {$sender_port + 10000}]] ;# cport
    append packet [binary format S $flags]
    append packet [binary format c 0]                      ;# state
    append packet [binary format ccc 0 0 0]                ;# mflags
    return [string replace $packet 4 7 [binary format I [string length $packet]]]
}

proc send_cluster_ping_packet {cport packet} {
    set sock [socket 127.0.0.1 $cport]
    fconfigure $sock -translation binary -buffering none -blocking 1
    puts -nonewline $sock $packet
    flush $sock
    close $sock
}

# Node 1 is the observer. Node 0 is the primary of the other shard, nodes 2 and
# 4 are its replicas.
start_cluster 2 4 {tags {external:skip cluster tls:skip}} {
    set observer_cport [expr {[srv -1 port] + 10000}]
    set id0 [R 0 cluster myid]
    set id2 [R 2 cluster myid]
    set id4 [R 4 cluster myid]
    set port0 [srv 0 port]
    set port2 [srv -2 port]
    set port4 [srv -4 port]
    set slots [dict get [cluster_get_myself 0] slots]

    test "Observer processes the failover to node 4 before node 2 reports its new primary" {
        wait_for_condition 1000 50 {
            [cluster_node_is_replica_of 1 $id2 $id0] &&
            [cluster_node_is_replica_of 1 $id4 $id0]
        } else {
            fail "Observer does not see nodes 2 and 4 as replicas of node 0"
        }
        set old_epoch [dict get [cluster_get_node_by_id 1 $id0] config_epoch]
        set epoch [expr {[CI 1 cluster_current_epoch] + 1}]

        # From now on the observer only learns about this shard from us.
        pause_process_persistent [srv 0 pid]
        pause_process_persistent [srv -2 pid]
        pause_process_persistent [srv -4 pid]

        # Node 4 wins a failover and node 0 follows it.
        send_cluster_ping_packet $observer_cport [create_cluster_ping_packet $id4 $port4 $epoch "" $slots]
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 1 $id4] master]
        } else {
            fail "Observer does not see node 4 as a primary"
        }
        send_cluster_ping_packet $observer_cport [create_cluster_ping_packet $id0 $port0 $epoch $id4 $slots]
        wait_for_condition 1000 50 {
            [cluster_node_is_replica_of 1 $id0 $id4]
        } else {
            fail "Observer does not see node 0 as a replica of node 4"
        }

        # The observer's view is now 2 -> 0 -> 4.
        assert {[cluster_node_is_replica_of 1 $id2 $id0]}
    }

    test "A primary change that would close a replicaof loop is ignored" {
        set pattern "*Ignore primary change of node $id0 * to node $id2 *replication loop*"

        # An old packet of node 0, from when node 2 was its primary.
        set from_line [count_log_lines -1]
        send_cluster_ping_packet $observer_cport [create_cluster_ping_packet $id0 $port0 $old_epoch $id2 $slots]
        wait_for_log_messages -1 [list $pattern] $from_line 1000 50

        # Node 2 wins a failover and node 0 follows it, but the packet of node
        # 0 is processed before the one of node 2.
        set from_line [count_log_lines -1]
        incr epoch
        send_cluster_ping_packet $observer_cport [create_cluster_ping_packet $id0 $port0 $epoch $id2 $slots]
        wait_for_log_messages -1 [list $pattern] $from_line 1000 50

        assert_equal {PONG} [R 1 ping]
        assert {[cluster_node_is_replica_of 1 $id0 $id4]}
        assert {[cluster_node_is_replica_of 1 $id2 $id0]}
    }

    test "The primary change is applied once the claimed primary is known as a primary" {
        send_cluster_ping_packet $observer_cport [create_cluster_ping_packet $id2 $port2 $epoch "" $slots]
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 1 $id2] master]
        } else {
            fail "Observer does not see node 2 as a primary"
        }
        send_cluster_ping_packet $observer_cport [create_cluster_ping_packet $id0 $port0 $epoch $id2 $slots]
        wait_for_condition 1000 50 {
            [cluster_node_is_replica_of 1 $id0 $id2]
        } else {
            fail "Observer does not see node 0 as a replica of node 2"
        }
    }
}
