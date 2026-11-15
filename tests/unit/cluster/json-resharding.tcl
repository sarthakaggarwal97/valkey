# JSON documents (built-in JSON module) in a cluster: live resharding with
# valkey-cli --cluster reshard, with MIGRATE and with atomic slot migration,
# while a client writes to JSON documents all over the cluster. Afterwards
# every document equals the client's model of it, the replicas agree with
# their primaries, and json_num_documents matches the keys of each node.

proc json_rs_node_by_addr {addr} {
    set port [lindex [split $addr :] end]
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        if {[srv -$i port] == $port} { return $i }
    }
    fail "no server listens on $addr"
}

proc json_rs_stat {node field} {
    return [getInfoProperty [R $node info json_core_metrics] json_$field]
}

proc json_rs_count_json_keys {node} {
    set n 0
    set cursor 0
    while 1 {
        lassign [R $node SCAN $cursor COUNT 1000 TYPE ReJSON-RL] cursor keys
        incr n [llength $keys]
        if {$cursor == 0} break
    }
    return $n
}

# Number of slots node serves, in its own view.
proc json_rs_slot_count {node} {
    set myid [R $node CLUSTER MYID]
    set count 0
    foreach n [get_cluster_nodes $node] {
        if {[dict get $n id] ne $myid} continue
        foreach r [dict get $n slots] {
            if {[string index $r 0] eq "\["} continue
            lassign [split $r -] start end
            if {$end eq {}} { set end $start }
            incr count [expr {$end - $start + 1}]
        }
    }
    return $count
}

# Runs a command for a key like a cluster client: it starts at the node it
# last saw serving the key's slot, follows MOVED (and remembers the new
# owner) and ASK (with ASKING), and retries on TRYAGAIN.
proc json_rs_exec {key args} {
    set slot $::json_rs_slot($key)
    set node [dict get $::json_rs_owner $slot]
    set asking 0
    for {set try 0} {$try < 200} {incr try} {
        if {$asking} { R $node ASKING }
        if {![catch {R $node {*}$args} res]} { return $res }
        if {[string match "MOVED *" $res]} {
            dict incr ::json_rs_redirects moved
            set node [json_rs_node_by_addr [lindex $res 2]]
            dict set ::json_rs_owner $slot $node
            set asking 0
        } elseif {[string match "ASK *" $res]} {
            dict incr ::json_rs_redirects ask
            set node [json_rs_node_by_addr [lindex $res 2]]
            set asking 1
        } elseif {[string match "TRYAGAIN *" $res] || [string match "CLUSTERDOWN *" $res]} {
            dict incr ::json_rs_redirects tryagain
            set asking 0
            after 10
        } else {
            error $res
        }
    }
    fail "too many redirections for '$args'"
}

proc json_rs_text {m} {
    lassign $m n arr s tmp
    set t "{\"n\":$n,\"arr\":\[[join $arr ,]\],\"s\":{\"v\":$s}"
    if {$tmp ne {}} { append t ",\"tmp\":{\"x\":$tmp}" }
    return "$t}"
}

# Creates nkeys documents spread over the whole cluster.
proc json_rs_init {nkeys} {
    set ::json_rs_model {}
    set ::json_rs_owner {}
    set ::json_rs_redirects {}
    set ::json_rs_seq 0
    for {set s 0} {$s < 16384} {incr s} {
        dict set ::json_rs_owner $s [expr {$s <= 5460 ? 0 : ($s <= 10922 ? 1 : 2)}]
    }
    for {set i 0} {$i < $nkeys} {incr i} {
        set key "rs:$i"
        set ::json_rs_slot($key) [R 0 CLUSTER KEYSLOT $key]
        set m [list 0 {} 0 {}]
        dict set ::json_rs_model $key $m
        assert_equal OK [json_rs_exec $key JSON.SET $key $ [json_rs_text $m]]
    }
}

# One random write, checked against the model.
proc json_rs_step {} {
    set keys [dict keys $::json_rs_model]
    set key [lindex $keys [expr {int(rand() * [llength $keys])}]]
    lassign [dict get $::json_rs_model $key] n arr s tmp
    set v [incr ::json_rs_seq]
    set op [expr {int(rand() * 21)}]
    if {$op < 5} {
        incr n
        assert_equal "\[$n\]" [json_rs_exec $key JSON.NUMINCRBY $key $.n 1] "JSON.NUMINCRBY $key"
    } elseif {$op < 10} {
        lappend arr $v
        assert_equal [llength $arr] [json_rs_exec $key JSON.ARRAPPEND $key $.arr $v] "JSON.ARRAPPEND $key"
    } elseif {$op < 15} {
        set s $v
        assert_equal OK [json_rs_exec $key JSON.SET $key $.s.v $v] "JSON.SET $key"
    } elseif {$op < 20} {
        if {$tmp eq {}} {
            set tmp $v
            assert_equal OK [json_rs_exec $key JSON.SET $key $.tmp "{\"x\":$v}"] "JSON.SET $key \$.tmp"
        } else {
            set tmp {}
            assert_equal 1 [json_rs_exec $key JSON.DEL $key $.tmp] "JSON.DEL $key \$.tmp"
        }
    } else {
        assert_equal 1 [json_rs_exec $key JSON.DEL $key] "JSON.DEL $key"
        lassign {0 {} 0 {}} n arr s tmp
        assert_equal OK [json_rs_exec $key JSON.SET $key $ [json_rs_text [list $n $arr $s $tmp]]] "JSON.SET $key \$"
    }
    dict set ::json_rs_model $key [list $n $arr $s $tmp]
}

proc json_rs_verify {} {
    dict for {key m} $::json_rs_model {
        assert_equal [json_rs_text $m] [json_rs_exec $key JSON.GET $key] "document $key"
    }
}

# Every primary's replica holds the same data, and every node's document
# counter matches its JSON keys.
proc json_rs_verify_nodes {} {
    set total 0
    foreach p {0 1 2} {
        set r [expr {$p + 3}]
        wait_for_ofs_sync [Rn $p] [Rn $r]
        assert_equal [R $p DEBUG DIGEST] [R $r DEBUG DIGEST] "digest of primary $p and replica $r"
        foreach node [list $p $r] {
            wait_for_condition 100 50 {
                [json_rs_stat $node num_documents] == [json_rs_count_json_keys $node]
            } else {
                fail "node $node: json_num_documents [json_rs_stat $node num_documents], JSON keys [json_rs_count_json_keys $node]"
            }
        }
        incr total [R $p DBSIZE]
    }
    assert_equal [dict size $::json_rs_model] $total
}

# Starts valkey-cli --cluster reshard in the background: nslots slots from
# all primaries to node `to`. Returns the process id.
proc json_rs_start_reshard {to nslots extra} {
    return [lindex [exec $::VALKEY_CLI_BIN --cluster reshard 127.0.0.1:[srv 0 port] \
                        --cluster-from all \
                        --cluster-to [R $to CLUSTER MYID] \
                        --cluster-slots $nslots \
                        --cluster-yes \
                        {*}$extra \
                        {*}[valkeycli_tls_config "./tests"] \
                        > /dev/null 2>@1 &] 0]
}

# Writes while the resharding runs, then at least `after` more writes.
proc json_rs_write_while {pid {after 200}} {
    set ops 0
    while {[process_is_alive $pid]} {
        for {set j 0} {$j < 20} {incr j} { json_rs_step }
        incr ops 20
        if {$ops > 200000} { fail "the resharding does not end" }
    }
    for {set j 0} {$j < $after} {incr j} { json_rs_step }
    return $ops
}

start_cluster 3 3 {tags {external:skip cluster json valgrind:skip} overrides {cluster-allow-replica-migration no}} {

    json_rs_init 400

    test "JSON: documents stay consistent through valkey-cli --cluster reshard with MIGRATE" {
        for {set j 0} {$j < 500} {incr j} { json_rs_step }
        set before [json_rs_slot_count 2]
        set pid [json_rs_start_reshard 2 400 {--cluster-pipeline 5}]
        set ops [json_rs_write_while $pid]
        wait_for_cluster_propagation
        assert_equal [expr {$before + 400}] [json_rs_slot_count 2]
        assert_morethan $ops 0
        assert_morethan [dict get $::json_rs_redirects moved] 0
        json_rs_verify
        json_rs_verify_nodes
    }

    test "JSON: documents stay consistent through valkey-cli --cluster reshard with atomic slot migration" {
        set ::json_rs_redirects {}
        set before [json_rs_slot_count 0]
        set pid [json_rs_start_reshard 0 400 {--cluster-use-atomic-slot-migration}]
        set ops [json_rs_write_while $pid]
        wait_for_cluster_propagation
        assert_equal [expr {$before + 400}] [json_rs_slot_count 0]
        assert_morethan [dict get $::json_rs_redirects moved] 0
        json_rs_verify
        json_rs_verify_nodes
    }
}
