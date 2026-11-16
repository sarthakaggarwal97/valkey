# JSON documents (built-in JSON module) in a cluster: slot migration.
#
#   - Documents of every shape, some with a TTL, move with the classic
#     MIGRATE-based flow and with atomic slot migration (CLUSTER MIGRATESLOTS).
#     Afterwards every document is equal (JSON.GET, DEBUG DIGEST-VALUE), keeps
#     its TTL and its type, the replicas agree, and json_num_documents and
#     json_total_memory_bytes move from the source to the target.
#   - A client keeps writing to keys of the slot while it moves (JSON.SET at a
#     sub-path, JSON.NUMINCRBY, JSON.ARRAPPEND, JSON.DEL), following MOVED and
#     ASK. Afterwards no write is lost or applied twice.
#   - Redirections and multi-key rules of JSON commands: ASK, MOVED, TRYAGAIN,
#     CROSSSLOT, hash tags.

# ---------------------------------------------------------------------------
# Helpers. All are prefixed json_cl_ so they do not clash with procs of other
# cluster test files.
# ---------------------------------------------------------------------------

# A hash tag "{...}" whose slot lies in [lo, hi] and is not in $avoid.
proc json_cl_tag {lo hi {avoid {}}} {
    for {set i 0} {$i < 100000} {incr i} {
        set slot [R 0 CLUSTER KEYSLOT "{j$i}"]
        if {$slot >= $lo && $slot <= $hi && [lsearch -exact $avoid $slot] < 0} {
            return [list "{j$i}" $slot]
        }
    }
    fail "no hash tag in slots $lo-$hi"
}

# Server index of the node listening on "host:port".
proc json_cl_node_by_addr {addr} {
    set port [lindex [split $addr :] end]
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        if {[srv -$i port] == $port} { return $i }
    }
    fail "no server listens on $addr"
}

proc json_cl_stat {node field} {
    return [getInfoProperty [R $node info json_core_metrics] json_$field]
}

# Number of JSON keys a node holds, by SCAN with TYPE.
proc json_cl_count_json_keys {node} {
    set n 0
    set cursor 0
    while 1 {
        lassign [R $node SCAN $cursor COUNT 1000 TYPE ReJSON-RL] cursor keys
        incr n [llength $keys]
        if {$cursor == 0} break
    }
    return $n
}

# Waits until json_num_documents of a node equals the number of JSON keys it
# holds. A large document is freed on the lazy-free thread, so it is awaited.
proc json_cl_wait_docs_consistent {node} {
    wait_for_condition 100 50 {
        [json_cl_stat $node num_documents] == [json_cl_count_json_keys $node]
    } else {
        fail "node $node: json_num_documents is [json_cl_stat $node num_documents], but it holds [json_cl_count_json_keys $node] JSON keys"
    }
}

# Waits until the two statistics of a node have the given values.
proc json_cl_wait_stats {node docs bytes {what ""}} {
    wait_for_condition 100 50 {
        [json_cl_stat $node num_documents] == $docs &&
        [json_cl_stat $node total_memory_bytes] == $bytes
    } else {
        fail "node $node: json_num_documents [json_cl_stat $node num_documents] (expected $docs), json_total_memory_bytes [json_cl_stat $node total_memory_bytes] (expected $bytes) $what"
    }
}

# json_total_memory_bytes above base, less the memory of each JSON document
# the node holds (JSON.DEBUG MEMORY): the storage of the member names.
#
# A document's own memory is not a function of its text alone. Member names
# are stored as IDs from a per-node table, and the width of an ID depends on
# which names that node interned before. So the same document can take more
# or less memory on the target than on the source, while the names take the
# same storage on both.
proc json_cl_name_bytes {node base} {
    set sum 0
    set cursor 0
    R $node READONLY
    while 1 {
        lassign [R $node SCAN $cursor COUNT 1000 TYPE ReJSON-RL] cursor keys
        foreach k $keys { incr sum [R $node JSON.DEBUG MEMORY $k] }
        if {$cursor == 0} break
    }
    R $node READWRITE
    return [expr {[json_cl_stat $node total_memory_bytes] - $base - $sum}]
}

# Waits until node holds docs documents and its name storage is names bytes.
# Name storage is counted in allocator usable sizes. jemalloc gives the same
# size for the same request on every node. glibc malloc may hand out a chunk
# 16 bytes larger, depending on the heap's history, so there the two nodes
# need only agree within 10%.
proc json_cl_wait_target_stats {node docs base names {what ""}} {
    set slack [expr {[string match {jemalloc*} [getInfoProperty [R $node INFO memory] mem_allocator]] ? 0 : $names / 10}]
    wait_for_condition 100 50 {
        [json_cl_stat $node num_documents] == $docs &&
        abs([json_cl_name_bytes $node $base] - $names) <= $slack
    } else {
        fail "node $node: json_num_documents [json_cl_stat $node num_documents] (expected $docs), name storage [json_cl_name_bytes $node $base] (expected $names) $what"
    }
}

proc json_cl_deep_array {n} {
    return "[string repeat {[} $n][string repeat {]} $n]"
}
proc json_cl_deep_object {n} {
    return "[string repeat "\{\"a\":" $n]1[string repeat "\}" $n]"
}

# Documents of every shape, as a list of key suffix and text.
proc json_cl_documents {} {
    set docs {}
    lappend docs small {{"a":1,"b":"x","c":[1,2,3],"d":{"e":null,"f":true}}}
    # Over json.max-packed-bytes (4096) and json.max-packed-entries (128): a tree.
    set l {}
    for {set i 0} {$i < 400} {incr i} {
        lappend l "\"member$i\":{\"id\":$i,\"name\":\"name-$i\",\"tags\":\[\"t$i\",$i,$i.5\],\"on\":[expr {$i % 2 ? "true" : "false"}]}"
    }
    lappend docs tree "{[join $l ,]}"
    lappend docs decimals {[0.1,19.90,-3.14159,1e-7,2.50,1.5E+21,-0.0,0.000001,123456.789012,{"price":10.00}]}
    lappend docs bigints {[9223372036854775807,-9223372036854775808,9223372036854775808,-9223372036854775809,123456789012345678901234567890,18446744073709551616]}
    lappend docs deep128 [json_cl_deep_array 128]
    lappend docs deepobj127 [json_cl_deep_object 127]
    # e-acute, euro sign, an emoji, Cyrillic and CJK as UTF-8 bytes, and escapes.
    lappend docs nonascii "{\"\xc3\xa9t\xc3\xa9\":\"\xe2\x82\xac\xf0\x9f\x98\x80\",\"esc\":\"\\u00e9\\ud83d\\ude00\\n\\t\",\"\xe5\x90\x8d\":\[\"\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\"\]}"
    set l {}
    for {set i 0} {$i < 5000} {incr i} { lappend l [expr {$i * 7919 - 20000}] }
    lappend docs intarray "\[[join $l ,]\]"
    set l {}
    for {set i 0} {$i < 2000} {incr i} { lappend l "\"s$i\"" }
    lappend docs strarray "\[[join $l ,]\]"
    set l {}
    for {set i 0} {$i < 1000} {incr i} { lappend l [format %.6f [expr {sin($i)}]] }
    lappend docs doublearray "\[[join $l ,]\]"
    set l {}
    for {set i 0} {$i < 1500} {incr i} { lappend l "\[$i,\"x$i\",null,{\"k\":$i.25}\]" }
    lappend docs mixedarray "\[[join $l ,]\]"
    lappend docs longstring "\"[string repeat {abcdefghij} 3000]\""
    lappend docs scalar_str {"just a string"}
    lappend docs scalar_int 42
    lappend docs scalar_null null
    lappend docs scalar_bool true
    lappend docs empty_obj {{}}
    lappend docs empty_arr {[]}
    return $docs
}

# Writes the documents into keys "$tag<suffix>" on node, a TTL on every other
# key, and one document 129 levels deep, beyond json.max-path-limit (128),
# made by appending to a document 128 levels deep. Returns the key names.
proc json_cl_write_documents {node tag} {
    set keys {}
    set i 0
    foreach {suffix text} [json_cl_documents] {
        set key "${tag}doc:$suffix"
        assert_equal OK [R $node JSON.SET $key . $text]
        if {$i % 2} { assert_equal 1 [R $node EXPIRE $key [expr {10000 + $i}]] }
        lappend keys $key
        incr i
    }
    set key "${tag}doc:deep129"
    assert_equal OK [R $node JSON.SET $key $ [json_cl_deep_array 128]]
    assert_equal 1 [R $node JSON.ARRAPPEND $key "\$[string repeat {[0]} 127]" {[1]}]
    assert_equal 129 [R $node JSON.DEBUG DEPTH $key]
    lappend keys $key
    # A key name that is not ASCII.
    set key "${tag}\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87"
    assert_equal OK [R $node JSON.SET $key . {{"k":"v"}}]
    assert_equal 1 [R $node PEXPIRE $key 20000000]
    lappend keys $key
    return $keys
}

# Reads, for each key, the text, the digest and the expire time.
proc json_cl_snapshot {node keys} {
    set snap {}
    R $node READONLY
    foreach key $keys {
        assert_equal ReJSON-RL [R $node TYPE $key]
        dict set snap $key [list [R $node JSON.GET $key $] [R $node DEBUG DIGEST-VALUE $key] [R $node PEXPIRETIME $key]]
    }
    R $node READWRITE
    return $snap
}

# Asserts that node holds every key of the snapshot, equal.
proc json_cl_assert_snapshot {node snap} {
    R $node READONLY
    dict for {key val} $snap {
        lassign $val text digest expiretime
        assert_equal ReJSON-RL [R $node TYPE $key] "type of $key on node $node"
        assert_equal $text [R $node JSON.GET $key $] "text of $key on node $node"
        assert_equal $digest [R $node DEBUG DIGEST-VALUE $key] "digest of $key on node $node"
        set et [R $node PEXPIRETIME $key]
        if {$expiretime < 0} {
            assert_equal $expiretime $et "expire time of $key on node $node"
        } else {
            # MIGRATE carries a relative TTL, so allow for the time it took.
            assert_range $et [expr {$expiretime - 1000}] [expr {$expiretime + 1000}]
        }
    }
    R $node READWRITE
}

# The classic flow: IMPORTING and MIGRATING, MIGRATE key by key, SETSLOT NODE.
# $between runs in the caller after each MIGRATE.
proc json_cl_migrate_classic {src dst slot {between {}}} {
    set src_id [R $src CLUSTER MYID]
    set dst_id [R $dst CLUSTER MYID]
    assert_equal OK [R $dst CLUSTER SETSLOT $slot IMPORTING $src_id]
    assert_equal OK [R $src CLUSTER SETSLOT $slot MIGRATING $dst_id]
    while 1 {
        set keys [R $src CLUSTER GETKEYSINSLOT $slot 1]
        if {$keys eq {}} break
        assert_equal OK [R $src MIGRATE [srv -$dst host] [srv -$dst port] "" 0 10000 KEYS {*}$keys]
        if {$between ne {}} { uplevel 1 $between }
    }
    assert_equal OK [R $dst CLUSTER SETSLOT $slot NODE $dst_id]
    assert_equal OK [R $src CLUSTER SETSLOT $slot NODE $dst_id]
    wait_for_cluster_propagation
}

proc json_cl_slot_ranges_contain {ranges slot} {
    foreach r $ranges {
        lassign [split $r -] start end
        if {$end eq {}} { set end $start }
        if {$slot >= $start && $slot <= $end} { return 1 }
    }
    return 0
}

# Whether node, in its own view, owns slot.
proc json_cl_owns_slot {node slot} {
    set myid [R $node CLUSTER MYID]
    foreach n [get_cluster_nodes $node] {
        if {[dict get $n id] eq $myid} {
            return [json_cl_slot_ranges_contain [dict get $n slots] $slot]
        }
    }
    return 0
}

proc json_cl_asm_job {node slot} {
    foreach m [R $node CLUSTER GETSLOTMIGRATIONS] {
        if {[json_cl_slot_ranges_contain [dict get $m slot_ranges] $slot]} { return [dict get $m name] }
    }
    return ""
}

proc json_cl_asm_state {node job} {
    foreach m [R $node CLUSTER GETSLOTMIGRATIONS] {
        if {[dict get $m name] eq $job} { return [dict get $m state] }
    }
    return ""
}

proc json_cl_wait_asm_state {node job state} {
    wait_for_condition 200 50 {
        [json_cl_asm_state $node $job] eq $state
    } else {
        fail "migration $job on node $node is '[json_cl_asm_state $node $job]', expected $state"
    }
}

# Atomic slot migration of one slot, waiting for success on both sides.
proc json_cl_migrate_atomic {src dst slot} {
    set dst_id [R $dst CLUSTER MYID]
    assert_equal OK [R $src CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE $dst_id]
    set job [json_cl_asm_job $src $slot]
    json_cl_wait_asm_state $src $job success
    json_cl_wait_asm_state $dst $job success
    wait_for_condition 200 50 {
        [json_cl_owns_slot $dst $slot]
    } else {
        fail "node $dst does not own slot $slot"
    }
    wait_for_cluster_propagation
}

proc json_cl_set_prevent_pause {value} {
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        assert_equal OK [R $i DEBUG SLOTMIGRATION PREVENT-PAUSE $value]
    }
    # Should the test fail, the later tests run without it.
    if {$value} { test_defer_fail { json_cl_set_prevent_pause 0 } }
}

# Runs a command for one key the way a cluster client does, starting at node
# $start: it follows MOVED and ASK (with ASKING) and retries on TRYAGAIN.
# Counts the redirections in ::json_cl_redirects. Returns {node reply}.
proc json_cl_exec {start args} {
    set node $start
    set asking 0
    for {set try 0} {$try < 100} {incr try} {
        if {$asking} { assert_equal OK [R $node ASKING] }
        if {![catch {R $node {*}$args} res]} {
            return [list $node $res]
        }
        if {[string match "MOVED *" $res]} {
            dict incr ::json_cl_redirects moved
            set node [json_cl_node_by_addr [lindex $res 2]]
            set asking 0
        } elseif {[string match "ASK *" $res]} {
            dict incr ::json_cl_redirects ask
            set node [json_cl_node_by_addr [lindex $res 2]]
            set asking 1
        } elseif {[string match "TRYAGAIN *" $res]} {
            dict incr ::json_cl_redirects tryagain
            set asking 0
            after 5
        } else {
            error $res
        }
    }
    fail "too many redirections for '$args'"
}

# --- The write workload: a model of each key, kept in ::json_cl_model. ---

proc json_cl_pad {} {
    set l {}
    for {set i 0} {$i < 200} {incr i} { lappend l $i }
    return "\[[join $l ,]\]"
}

# A key's initial document. Keys whose index is a multiple of 3 also carry an
# array of 200 integers, so the document is over json.max-packed-entries.
proc json_cl_initial {i} {
    if {$i % 3 == 0} {
        return [dict create n 0 arr {} s 0 pad 1 tmp {}]
    }
    return [dict create n 0 arr {} s 0 pad 0 tmp {}]
}

proc json_cl_model_text {m} {
    set t "{\"n\":[dict get $m n],\"arr\":\[[join [dict get $m arr] ,]\],\"s\":{\"v\":[dict get $m s]}"
    if {[dict get $m pad]} { append t ",\"pad\":[json_cl_pad]" }
    if {[dict get $m tmp] ne {}} { append t ",\"tmp\":{\"x\":[dict get $m tmp]}" }
    append t "}"
    return $t
}

# Creates nkeys keys "${tag}w:$i" on node and their model.
proc json_cl_workload_init {node tag nkeys} {
    set ::json_cl_model {}
    set ::json_cl_seq 0
    set ::json_cl_redirects {}
    for {set i 0} {$i < $nkeys} {incr i} {
        set m [json_cl_initial $i]
        dict set ::json_cl_model "${tag}w:$i" $m
        assert_equal OK [R $node JSON.SET "${tag}w:$i" $ [json_cl_model_text $m]]
    }
}

# One random write, sent the way a client that believes $start owns the slot
# would send it. The reply is checked against the model, then the model is
# updated.
proc json_cl_workload_step {start} {
    set keys [dict keys $::json_cl_model]
    set key [lindex $keys [expr {int(rand() * [llength $keys])}]]
    set m [dict get $::json_cl_model $key]
    set v [incr ::json_cl_seq]
    set op [expr {int(rand() * 21)}]
    if {$op < 5} {
        lassign [json_cl_exec $start JSON.NUMINCRBY $key $.n 1] node res
        dict incr m n
        assert_equal "\[[dict get $m n]\]" $res "JSON.NUMINCRBY $key"
    } elseif {$op < 10} {
        lassign [json_cl_exec $start JSON.ARRAPPEND $key $.arr $v] node res
        dict lappend m arr $v
        assert_equal [llength [dict get $m arr]] $res "JSON.ARRAPPEND $key"
    } elseif {$op < 15} {
        lassign [json_cl_exec $start JSON.SET $key $.s.v $v] node res
        dict set m s $v
        assert_equal OK $res "JSON.SET $key"
    } elseif {$op < 20} {
        if {[dict get $m tmp] eq {}} {
            lassign [json_cl_exec $start JSON.SET $key $.tmp "{\"x\":$v}"] node res
            dict set m tmp $v
            assert_equal OK $res "JSON.SET $key \$.tmp"
        } else {
            lassign [json_cl_exec $start JSON.DEL $key $.tmp] node res
            dict set m tmp {}
            assert_equal 1 $res "JSON.DEL $key \$.tmp"
        }
    } else {
        # Delete the whole key and write it again: during a migration the
        # second write goes to the target, by ASK, once the key is gone.
        lassign [json_cl_exec $start JSON.DEL $key] node res
        assert_equal 1 $res "JSON.DEL $key"
        set m [json_cl_initial [lindex [split $key :] end]]
        lassign [json_cl_exec $start JSON.SET $key $ [json_cl_model_text $m]] node res
        assert_equal OK $res "JSON.SET $key \$"
    }
    dict set ::json_cl_model $key $m
}

# Asserts that every key on node equals its model.
proc json_cl_workload_verify {node} {
    dict for {key m} $::json_cl_model {
        assert_equal [json_cl_model_text $m] [R $node JSON.GET $key] "document $key on node $node"
    }
}

# Waits until a replica has applied all of its primary's stream, then asserts
# that both hold the same data and the replica's statistics are consistent.
proc json_cl_assert_replica_same {primary replica} {
    wait_for_ofs_sync [Rn $primary] [Rn $replica]
    assert_equal [R $primary DBSIZE] [R $replica DBSIZE]
    assert_equal [R $primary DEBUG DIGEST] [R $replica DEBUG DIGEST]
    json_cl_wait_docs_consistent $replica
}

start_cluster 3 3 {tags {external:skip cluster json} overrides {cluster-allow-replica-migration no cluster-node-timeout 15000}} {

    # Slots: node 0 serves 0-5460, node 1 5461-10922, node 2 10923-16383.
    # Node i+3 is the replica of node i.
    set used_slots {}

    test "JSON: documents of every shape move with MIGRATE, statistics follow" {
        lassign [json_cl_tag 0 5460 $used_slots] tag slot
        lappend used_slots $slot

        foreach n {0 1} {
            R $n FLUSHALL SYNC
            wait_lazyfree_done [Rn $n]
        }
        set base0 [json_cl_stat 0 total_memory_bytes]
        set base1 [json_cl_stat 1 total_memory_bytes]
        assert_equal 0 [json_cl_stat 0 num_documents]
        assert_equal 0 [json_cl_stat 1 num_documents]

        set keys [json_cl_write_documents 0 $tag]
        set nkeys [llength $keys]
        assert_equal $nkeys [json_cl_stat 0 num_documents]
        assert_morethan [expr {[json_cl_stat 0 total_memory_bytes] - $base0}] 100000
        set names [json_cl_name_bytes 0 $base0]
        assert_morethan $names 0
        set snap [json_cl_snapshot 0 $keys]
        json_cl_assert_replica_same 0 3

        json_cl_migrate_classic 0 1 $slot

        assert_equal 0 [R 0 CLUSTER COUNTKEYSINSLOT $slot]
        assert_equal $nkeys [R 1 CLUSTER COUNTKEYSINSLOT $slot]
        json_cl_assert_snapshot 1 $snap
        # The source returns exactly to its base, the target holds the
        # documents and the same member names.
        json_cl_wait_stats 0 0 $base0 "on the source after MIGRATE"
        json_cl_wait_target_stats 1 $nkeys $base1 $names "on the target after MIGRATE"
        json_cl_assert_replica_same 1 4
        json_cl_assert_replica_same 0 3
        json_cl_assert_snapshot 4 $snap
        assert_equal 0 [json_cl_stat 3 num_documents]
        assert_equal $nkeys [json_cl_stat 4 num_documents]
    }

    test "JSON: the same documents move back with atomic slot migration" {
        set slot [lindex $used_slots end]
        lassign [json_cl_tag $slot $slot] tag
        set keys [json_cl_write_documents 1 "${tag}again:"]
        set nkeys [R 1 CLUSTER COUNTKEYSINSLOT $slot]
        set allkeys [R 1 CLUSTER GETKEYSINSLOT $slot 1000]
        set snap [json_cl_snapshot 1 $allkeys]
        set names [json_cl_name_bytes 1 $base1]

        json_cl_migrate_atomic 1 0 $slot
        json_cl_wait_stats 1 0 $base1 "on the source after MIGRATESLOTS"
        json_cl_wait_target_stats 0 $nkeys $base0 $names "on the target after MIGRATESLOTS"

        assert_equal 0 [R 1 CLUSTER COUNTKEYSINSLOT $slot]
        assert_equal $nkeys [R 0 CLUSTER COUNTKEYSINSLOT $slot]
        json_cl_assert_snapshot 0 $snap
        json_cl_wait_docs_consistent 0
        json_cl_wait_docs_consistent 1
        assert_equal $nkeys [json_cl_stat 0 num_documents]
        assert_equal 0 [json_cl_stat 1 num_documents]
        json_cl_assert_replica_same 0 3
        json_cl_assert_replica_same 1 4
        json_cl_assert_snapshot 3 $snap
    }

    test "JSON: the statistics follow the documents to a third node and back" {
        # Node 2 holds nothing yet; node 0 holds only the slot's documents and
        # was at $base0 (first test) before it received them.
        set slot [lindex $used_slots end]
        R 2 FLUSHALL SYNC
        wait_lazyfree_done [Rn 2]
        set base2 [json_cl_stat 2 total_memory_bytes]
        set nkeys [R 0 CLUSTER COUNTKEYSINSLOT $slot]
        set snap [json_cl_snapshot 0 [R 0 CLUSTER GETKEYSINSLOT $slot 1000]]
        set names [json_cl_name_bytes 0 $base0]

        json_cl_migrate_atomic 0 2 $slot
        json_cl_assert_snapshot 2 $snap
        json_cl_wait_stats 0 0 $base0 "on the source after MIGRATESLOTS"
        json_cl_wait_target_stats 2 $nkeys $base2 $names "on the target after MIGRATESLOTS"

        json_cl_migrate_classic 2 0 $slot
        json_cl_assert_snapshot 0 $snap
        json_cl_wait_stats 2 0 $base2 "on node 2 after the slot left"
        json_cl_wait_target_stats 0 $nkeys $base0 $names "on node 0 after the slot came back"
        json_cl_assert_replica_same 0 3
        json_cl_assert_replica_same 2 5
    }

    test "JSON: a target with other caps and a lower path limit takes every document" {
        set slot [lindex $used_slots end]
        set keys [R 0 CLUSTER GETKEYSINSLOT $slot 1000]
        set snap [json_cl_snapshot 0 $keys]
        set small [lsearch -inline -glob $keys "*doc:small"]
        set packed_mem [R 0 JSON.DEBUG MEMORY $small]
        # Every container a tree node on node 1, and documents 128 and 129
        # levels deep over its json.max-path-limit.
        R 1 CONFIG SET json.max-packed-bytes 0
        R 1 CONFIG SET json.max-packed-entries 0
        R 1 CONFIG SET json.max-path-limit 64
        json_cl_migrate_atomic 0 1 $slot
        json_cl_assert_snapshot 1 $snap
        assert_morethan [R 1 JSON.DEBUG MEMORY $small] $packed_mem
        json_cl_migrate_classic 1 0 $slot
        json_cl_assert_snapshot 0 $snap
        R 1 CONFIG SET json.max-packed-bytes 4096
        R 1 CONFIG SET json.max-packed-entries 128
        R 1 CONFIG SET json.max-path-limit 128
        json_cl_wait_docs_consistent 0
        json_cl_wait_docs_consistent 1
        R 0 FLUSHALL SYNC
        R 1 FLUSHALL SYNC
    }

    test "JSON: a cancelled atomic slot migration leaves no documents on the target" {
        lassign [json_cl_tag 10923 16383 $used_slots] tag slot
        lappend used_slots $slot
        R 0 FLUSHALL SYNC
        wait_lazyfree_done [Rn 0]
        set base0 [json_cl_stat 0 total_memory_bytes]
        set keys [json_cl_write_documents 2 $tag]
        set snap [json_cl_snapshot 2 $keys]

        json_cl_set_prevent_pause 1
        assert_equal OK [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE [R 0 CLUSTER MYID]]
        set job [json_cl_asm_job 2 $slot]
        json_cl_wait_asm_state 2 $job waiting-to-pause
        # The target has loaded the snapshot, but does not serve the keys yet.
        wait_for_condition 100 50 {
            [json_cl_stat 0 num_documents] == [llength $keys]
        } else {
            fail "the target did not load the snapshot: [json_cl_stat 0 num_documents] documents"
        }
        assert_equal 0 [R 0 DBSIZE]
        assert_error "MOVED*" {R 0 JSON.GET [lindex $keys 0]}
        assert_equal OK [R 2 CLUSTER CANCELSLOTMIGRATIONS]
        json_cl_set_prevent_pause 0
        assert_equal cancelled [json_cl_asm_state 2 $job]
        json_cl_wait_asm_state 0 $job failed
        json_cl_wait_stats 0 0 $base0 "on the target after the cancel"
        assert_equal 0 [R 0 CLUSTER COUNTKEYSINSLOT $slot]
        json_cl_assert_snapshot 2 $snap
        json_cl_assert_replica_same 0 3
        json_cl_assert_replica_same 2 5
        R 2 FLUSHALL SYNC
    }

    test "JSON: ASK, MOVED and TRYAGAIN for JSON commands while a slot is migrating" {
        lassign [json_cl_tag 0 5460 $used_slots] tag slot
        lappend used_slots $slot
        set port0 [srv 0 port]
        set port1 [srv -1 port]
        R 0 JSON.SET "${tag}a" $ {{"v":1,"arr":[1]}}
        R 0 JSON.SET "${tag}b" $ {{"v":2,"arr":[2]}}
        assert_equal OK [R 1 CLUSTER SETSLOT $slot IMPORTING [R 0 CLUSTER MYID]]
        assert_equal OK [R 0 CLUSTER SETSLOT $slot MIGRATING [R 1 CLUSTER MYID]]
        assert_equal OK [R 0 MIGRATE [srv -1 host] $port1 "" 0 10000 KEYS "${tag}a"]

        # A key still on the source is served there; a moved one is an ASK.
        assert_equal {[2]} [R 0 JSON.GET "${tag}b" $.v]
        assert_equal "ASK $slot 127.0.0.1:$port1" [catch {R 0 JSON.GET "${tag}a" $.v} e; set e]
        assert_equal "ASK $slot 127.0.0.1:$port1" [catch {R 0 JSON.NUMINCRBY "${tag}a" $.v 1} e; set e]
        assert_equal "ASK $slot 127.0.0.1:$port1" [catch {R 0 JSON.ARRAPPEND "${tag}a" $.arr 3} e; set e]
        assert_equal "ASK $slot 127.0.0.1:$port1" [catch {R 0 JSON.SET "${tag}new" $ 1} e; set e]
        # The target answers with MOVED without ASKING, and serves the key with it.
        assert_equal "MOVED $slot 127.0.0.1:$port0" [catch {R 1 JSON.GET "${tag}a" $.v} e; set e]
        assert_equal OK [R 1 ASKING]
        assert_equal {[2]} [R 1 JSON.NUMINCRBY "${tag}a" $.v 1]
        assert_equal OK [R 1 ASKING]
        assert_equal {[2]} [R 1 JSON.GET "${tag}a" $.v]
        # A multi-key command over one key on each side: TRYAGAIN on the source;
        # on the target too, with ASKING, because one key is missing there.
        assert_match "TRYAGAIN*" [catch {R 0 JSON.MGET "${tag}a" "${tag}b" $.v} e; set e]
        assert_match "TRYAGAIN*" [catch {R 0 JSON.MSET "${tag}a" $.v 5 "${tag}b" $.v 6} e; set e]
        assert_equal OK [R 1 ASKING]
        assert_match "TRYAGAIN*" [catch {R 1 JSON.MGET "${tag}a" "${tag}b" $.v} e; set e]
        # JSON.MGET of a key that is on the target, with ASKING.
        assert_equal OK [R 1 ASKING]
        assert_equal {{[2]}} [R 1 JSON.MGET "${tag}a" $.v]

        json_cl_migrate_classic 0 1 $slot
        assert_equal "MOVED $slot 127.0.0.1:$port1" [catch {R 0 JSON.GET "${tag}a"} e; set e]
        assert_equal "MOVED $slot 127.0.0.1:$port1" [catch {R 0 JSON.MGET "${tag}a" "${tag}b" $} e; set e]
        assert_equal "MOVED $slot 127.0.0.1:$port1" [catch {R 0 JSON.MSET "${tag}a" $.v 5 "${tag}b" $.v 6} e; set e]
        assert_equal OK [R 1 JSON.MSET "${tag}a" $.v 5 "${tag}b" $.v 6]
        assert_equal {{[5]} {[6]}} [R 1 JSON.MGET "${tag}a" "${tag}b" $.v]
        R 1 FLUSHALL SYNC
    }

    test "JSON: JSON.MGET and JSON.MSET across slots are CROSSSLOT, with a hash tag they work" {
        lassign [json_cl_tag 5461 10922 $used_slots] tag slot
        R 1 JSON.SET "${tag}x" $ {{"a":1}}
        R 1 JSON.SET "${tag}y" $ {{"a":2}}
        # Two keys of node 1 in different slots.
        set other {}
        for {set i 0} {$other eq {}} {incr i} {
            set s [R 1 CLUSTER KEYSLOT "k$i"]
            if {$s >= 5461 && $s <= 10922 && $s != $slot} { set other "k$i" }
        }
        R 1 JSON.SET $other $ {{"a":3}}
        assert_error "CROSSSLOT*" {R 1 JSON.MGET "${tag}x" $other $.a}
        assert_error "CROSSSLOT*" {R 1 JSON.MSET "${tag}x" $.a 9 $other $.a 9}
        assert_error "CROSSSLOT*" {R 1 JSON.MSET "${tag}x" $.a 9 "${tag}y" $.a 9 $other $.a 9}
        # Nothing was written by the rejected JSON.MSET.
        assert_equal {{[1]} {[2]}} [R 1 JSON.MGET "${tag}x" "${tag}y" $.a]
        assert_equal {[3]} [R 1 JSON.GET $other $.a]
        assert_equal OK [R 1 JSON.MSET "${tag}x" $.a 7 "${tag}y" $.a 8 "${tag}z" $ {{"a":9}}]
        assert_equal {{[7]} {[8]} {[9]}} [R 1 JSON.MGET "${tag}x" "${tag}y" "${tag}z" $.a]
        # A key of another node is MOVED for a single key and for a hash-tagged set.
        lassign [json_cl_tag 0 5460] tag0 slot0
        assert_equal "MOVED $slot0 127.0.0.1:[srv 0 port]" [catch {R 1 JSON.MGET "${tag0}p" "${tag0}q" $} e; set e]
        assert_equal "MOVED $slot0 127.0.0.1:[srv 0 port]" [catch {R 1 JSON.MSET "${tag0}p" $ 1 "${tag0}q" $ 2} e; set e]
        R 1 FLUSHALL SYNC
    }

    test "JSON: writes during a classic migration are neither lost nor applied twice" {
        lassign [json_cl_tag 0 5460 $used_slots] tag slot
        lappend used_slots $slot
        json_cl_workload_init 0 $tag 60
        for {set i 0} {$i < 100} {incr i} { json_cl_workload_step 0 }
        # Between two MIGRATE calls, five writes, sent as a client that still
        # believes node 0 owns the slot.
        json_cl_migrate_classic 0 1 $slot {
            for {set w 0} {$w < 5} {incr w} { json_cl_workload_step 0 }
        }
        assert_morethan [dict get $::json_cl_redirects ask] 10
        for {set i 0} {$i < 100} {incr i} { json_cl_workload_step 0 }
        assert_morethan [dict get $::json_cl_redirects moved] 0
        assert_equal 0 [R 0 CLUSTER COUNTKEYSINSLOT $slot]
        assert_equal 60 [R 1 CLUSTER COUNTKEYSINSLOT $slot]
        json_cl_workload_verify 1
        json_cl_assert_replica_same 1 4
        json_cl_assert_replica_same 0 3
        json_cl_wait_docs_consistent 0
        json_cl_wait_docs_consistent 1
    }

    test "JSON: writes during an atomic slot migration are neither lost nor applied twice" {
        set slot [lindex $used_slots end]
        # The keys of the previous test are on node 1. Move them to node 2
        # while writing: during the snapshot, while the target streams, and
        # through the pause and the hand-over.
        set ::json_cl_redirects {}
        json_cl_set_prevent_pause 1
        assert_equal OK [R 1 CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE [R 2 CLUSTER MYID]]
        set job [json_cl_asm_job 1 $slot]
        for {set i 0} {$i < 100} {incr i} { json_cl_workload_step 1 }
        json_cl_wait_asm_state 1 $job waiting-to-pause
        for {set i 0} {$i < 300} {incr i} { json_cl_workload_step 1 }
        # A document made 129 levels deep while the target streams the
        # writes: the target applies a write that a client could not send.
        assert_equal OK [R 1 JSON.SET "${tag}deep" $ [json_cl_deep_array 128]]
        assert_equal 1 [R 1 JSON.ARRAPPEND "${tag}deep" "\$[string repeat {[0]} 127]" {[1]}]
        set deeptext [R 1 JSON.GET "${tag}deep"]
        # A target does not serve an importing slot.
        set port1 [srv -1 port]
        assert_equal "MOVED $slot 127.0.0.1:$port1" [catch {R 2 JSON.GET [lindex [dict keys $::json_cl_model] 0] $} e; set e]
        json_cl_set_prevent_pause 0
        set i 0
        while {![json_cl_owns_slot 2 $slot] && $i < 20000} {
            json_cl_workload_step 1
            incr i
        }
        json_cl_wait_asm_state 1 $job success
        json_cl_wait_asm_state 2 $job success
        wait_for_cluster_propagation
        for {set i 0} {$i < 100} {incr i} { json_cl_workload_step 1 }
        assert_morethan [dict get $::json_cl_redirects moved] 0
        assert_equal 0 [R 1 CLUSTER COUNTKEYSINSLOT $slot]
        assert_equal 61 [R 2 CLUSTER COUNTKEYSINSLOT $slot]
        json_cl_workload_verify 2
        assert_equal $deeptext [R 2 JSON.GET "${tag}deep"]
        assert_equal 129 [R 2 JSON.DEBUG DEPTH "${tag}deep"]
        json_cl_assert_replica_same 2 5
        json_cl_assert_replica_same 1 4
        json_cl_wait_docs_consistent 1
        json_cl_wait_docs_consistent 2
    }
}
