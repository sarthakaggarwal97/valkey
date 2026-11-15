# JSON documents (built-in JSON module) in a cluster: failover.
#
#   - A manual CLUSTER FAILOVER and an automatic failover (the primary stops)
#     promote a replica that holds JSON documents. All documents are equal on
#     the new primary, writes continue there, and the old primary rejoins as
#     a replica and catches up.

proc json_fo_tag {lo hi} {
    for {set i 0} {$i < 100000} {incr i} {
        set slot [R 0 CLUSTER KEYSLOT "{f$i}"]
        if {$slot >= $lo && $slot <= $hi} { return [list "{f$i}" $slot] }
    }
    fail "no hash tag in slots $lo-$hi"
}

proc json_fo_stat {node field} {
    return [getInfoProperty [R $node info json_core_metrics] json_$field]
}

proc json_fo_role {node} {
    return [lindex [R $node ROLE] 0]
}

# Waits until node has the replication role, its link is up when a replica,
# and every node of the cluster agrees on the configuration.
proc json_fo_wait_role {node role} {
    wait_for_condition 200 100 {
        [json_fo_role $node] eq $role
    } else {
        fail "node $node did not become a $role: [R $node ROLE]"
    }
    if {$role eq "slave"} {
        wait_for_condition 200 100 {
            [s -$node master_link_status] eq "up"
        } else {
            fail "node $node: the link to the primary is not up"
        }
    }
    wait_for_cluster_propagation
}

# Documents of every shape on keys "${tag}doc:<name>".
proc json_fo_write_documents {node tag} {
    set l {}
    for {set i 0} {$i < 300} {incr i} { lappend l "\"m$i\":{\"id\":$i,\"v\":\[$i,$i.5,\"s$i\"\]}" }
    set ints {}
    for {set i 0} {$i < 3000} {incr i} { lappend ints [expr {$i * 1000003}] }
    set docs [list \
        small {{"a":1,"b":"x","c":[1,2,3],"d":{"e":null}}} \
        tree "{[join $l ,]}" \
        numbers {[0.1,19.90,-0.0,1e-7,9223372036854775807,9223372036854775808,123456789012345678901234567890]} \
        deep "[string repeat {[} 128][string repeat {]} 128]" \
        text "{\"\xc3\xa9\":\"\xe2\x82\xac\xf0\x9f\x98\x80\",\"esc\":\"\\u00e9\"}" \
        ints "\[[join $ints ,]\]" \
        scalar {"a string"}]
    set keys {}
    foreach {name text} $docs {
        assert_equal OK [R $node JSON.SET "${tag}doc:$name" . $text]
        lappend keys "${tag}doc:$name"
    }
    R $node EXPIRE "${tag}doc:small" 100000
    return $keys
}

proc json_fo_snapshot {node keys} {
    set snap {}
    R $node READONLY
    foreach key $keys {
        dict set snap $key [list [R $node JSON.GET $key $] [R $node DEBUG DIGEST-VALUE $key] [R $node PEXPIRETIME $key]]
    }
    R $node READWRITE
    return $snap
}

proc json_fo_assert_snapshot {node snap} {
    R $node READONLY
    dict for {key val} $snap {
        lassign $val text digest at
        assert_equal ReJSON-RL [R $node TYPE $key] "type of $key on node $node"
        assert_equal $text [R $node JSON.GET $key $] "text of $key on node $node"
        assert_equal $digest [R $node DEBUG DIGEST-VALUE $key] "digest of $key on node $node"
        assert_equal $at [R $node PEXPIRETIME $key] "expire time of $key on node $node"
    }
    R $node READWRITE
}

# Counters and arrays written through the primary, with their expected text.
proc json_fo_counters_init {node tag n} {
    set ::json_fo_counters {}
    for {set i 0} {$i < $n} {incr i} {
        assert_equal OK [R $node JSON.SET "${tag}c:$i" $ {{"n":0,"arr":[],"s":{"v":0}}}]
        dict set ::json_fo_counters "${tag}c:$i" [list 0 {} 0]
    }
}

proc json_fo_counters_write {node rounds} {
    for {set r 0} {$r < $rounds} {incr r} {
        dict for {key val} $::json_fo_counters {
            lassign $val n arr s
            incr n
            assert_equal "\[$n\]" [R $node JSON.NUMINCRBY $key $.n 1]
            lappend arr $n
            assert_equal [llength $arr] [R $node JSON.ARRAPPEND $key $.arr $n]
            set s [expr {$n * 10}]
            assert_equal OK [R $node JSON.SET $key $.s.v $s]
            dict set ::json_fo_counters $key [list $n $arr $s]
        }
    }
}

proc json_fo_counters_verify {node} {
    R $node READONLY
    dict for {key val} $::json_fo_counters {
        lassign $val n arr s
        assert_equal "{\"n\":$n,\"arr\":\[[join $arr ,]\],\"s\":{\"v\":$s}}" [R $node JSON.GET $key] "$key on node $node"
    }
    R $node READWRITE
}

# Replica caught up: same offset, same data, statistics consistent.
proc json_fo_assert_caught_up {primary replica} {
    wait_for_ofs_sync [Rn $primary] [Rn $replica]
    assert_equal [R $primary DBSIZE] [R $replica DBSIZE]
    assert_equal [R $primary DEBUG DIGEST] [R $replica DEBUG DIGEST]
    wait_for_condition 100 50 {
        [json_fo_stat $replica num_documents] == [json_fo_stat $primary num_documents]
    } else {
        fail "json_num_documents: [json_fo_stat $replica num_documents] on replica $replica, [json_fo_stat $primary num_documents] on primary $primary"
    }
}

start_cluster 3 3 {tags {external:skip cluster json} overrides {cluster-allow-replica-migration no}} {

    # Node 0 serves slots 0-5460, node 1 5461-10922, node 2 10923-16383.
    # Node i+3 is the replica of node i.

    test "JSON: manual CLUSTER FAILOVER promotes a replica with every document" {
        lassign [json_fo_tag 5461 10922] tag slot
        set keys [json_fo_write_documents 1 $tag]
        json_fo_counters_init 1 $tag 20
        json_fo_counters_write 1 5
        set snap [json_fo_snapshot 1 $keys]
        json_fo_assert_caught_up 1 4

        assert_equal OK [R 4 CLUSTER FAILOVER]
        json_fo_wait_role 4 master
        json_fo_wait_role 1 slave
        assert_equal "MOVED $slot 127.0.0.1:[srv -4 port]" [catch {R 1 JSON.GET [lindex $keys 0]} e; set e]

        json_fo_assert_snapshot 4 $snap
        json_fo_counters_verify 4
        # Writes continue on the new primary and reach the old one.
        json_fo_counters_write 4 5
        json_fo_counters_verify 4
        json_fo_assert_caught_up 4 1
        json_fo_assert_snapshot 1 $snap
        json_fo_counters_verify 1

        # And back.
        assert_equal OK [R 1 CLUSTER FAILOVER]
        json_fo_wait_role 1 master
        json_fo_wait_role 4 slave
        json_fo_counters_write 1 2
        json_fo_assert_caught_up 1 4
        json_fo_assert_snapshot 4 $snap
        json_fo_counters_verify 4
    }

    test "JSON: automatic failover after the primary stops; it rejoins as a replica and catches up" {
        lassign [json_fo_tag 10923 16383] tag slot
        set keys [json_fo_write_documents 2 $tag]
        json_fo_counters_init 2 $tag 20
        json_fo_counters_write 2 5
        set snap [json_fo_snapshot 2 $keys]
        json_fo_assert_caught_up 2 5

        set pid [srv -2 pid]
        pause_process $pid
        wait_for_condition 300 100 {
            [json_fo_role 5] eq "master"
        } else {
            resume_process $pid
            fail "replica 5 was not promoted"
        }
        wait_for_condition 300 100 {
            [CI 0 cluster_state] eq "ok" && [CI 5 cluster_state] eq "ok"
        } else {
            resume_process $pid
            fail "the cluster is not ok after the failover"
        }
        json_fo_assert_snapshot 5 $snap
        json_fo_counters_verify 5
        # Writes on the new primary while the old one is still stopped.
        json_fo_counters_write 5 5
        # A JSON document written only on the new primary.
        assert_equal OK [R 5 JSON.SET "${tag}after" $ {{"written":"after the failover"}}]

        resume_process $pid
        json_fo_wait_role 2 slave
        json_fo_wait_role 5 master
        json_fo_assert_caught_up 5 2
        json_fo_assert_snapshot 2 $snap
        json_fo_counters_verify 2
        R 2 READONLY
        assert_equal {{"written":"after the failover"}} [R 2 JSON.GET "${tag}after"]
        R 2 READWRITE
        json_fo_counters_write 5 2
        json_fo_assert_caught_up 5 2
        json_fo_counters_verify 2
    }

    test "JSON: a restarted replica gets every document back" {
        # Node 2 is now the replica of node 5. Without its data, it needs a
        # full sync, which carries the documents in the RDB form.
        lassign [json_fo_tag 10923 16383] tag slot
        set keys [R 5 CLUSTER GETKEYSINSLOT $slot 1000]
        set snap [json_fo_snapshot 5 $keys]
        set full [s -5 sync_full]
        # No RDB to load at start: a file name that does not exist, and no save.
        R 2 CONFIG SET save ""
        R 2 CONFIG SET dbfilename json-failover-none.rdb
        R 2 CONFIG REWRITE
        restart_server -2 true false
        json_fo_wait_role 2 slave
        json_fo_assert_caught_up 5 2
        assert_morethan [s -5 sync_full] $full
        json_fo_assert_snapshot 2 $snap
        json_fo_counters_verify 2
        json_fo_counters_write 5 1
        json_fo_assert_caught_up 5 2
        json_fo_counters_verify 2
    }
}
