# JSON documents (built-in JSON module) in a cluster: keyspace commands and
# replica reads.
#
#   - DUMP on one node and RESTORE on another (TTL, ABSTTL, REPLACE).
#   - COPY within a node, RENAME within a slot, and the CROSSSLOT errors when
#     the two keys are in different slots.
#   - SCAN with TYPE ReJSON-RL on every node, and CLUSTERSCAN with TYPE
#     ReJSON-RL over the whole cluster.
#   - Replicas serve JSON reads after READONLY, and redirect writes and keys
#     of other shards.

# A hash tag "{...}" whose slot lies in [lo, hi] and is not $avoid.
proc json_ks_tag {lo hi {avoid -1}} {
    for {set i 0} {$i < 100000} {incr i} {
        set slot [R 0 CLUSTER KEYSLOT "{k$i}"]
        if {$slot >= $lo && $slot <= $hi && $slot != $avoid} { return [list "{k$i}" $slot] }
    }
    fail "no hash tag in slots $lo-$hi"
}

proc json_ks_stat {node field} {
    return [getInfoProperty [R $node info json_core_metrics] json_$field]
}

proc json_ks_wait_docs {node docs} {
    wait_for_condition 100 50 {
        [json_ks_stat $node num_documents] == $docs
    } else {
        fail "node $node: json_num_documents is [json_ks_stat $node num_documents], expected $docs"
    }
}

proc json_ks_node_by_addr {addr} {
    set port [lindex [split $addr :] end]
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        if {[srv -$i port] == $port} { return $i }
    }
    fail "no server listens on $addr"
}

# Sends a command to node and follows MOVED. Returns the reply.
proc json_ks_routed {node args} {
    for {set try 0} {$try < 10} {incr try} {
        if {![catch {R $node {*}$args} res]} { return $res }
        if {![string match "MOVED *" $res]} { error $res }
        set node [json_ks_node_by_addr [lindex $res 2]]
    }
    fail "too many redirections for '$args'"
}

# Some documents: one packed, one tree, numbers, text that is not ASCII.
proc json_ks_documents {} {
    set l {}
    for {set i 0} {$i < 300} {incr i} { lappend l "\"m$i\":\[$i,\"v$i\",{\"d\":$i.50}\]" }
    return [list \
        small {{"a":1,"b":[true,false,null],"c":{"d":"e"}}} \
        tree "{[join $l ,]}" \
        numbers {[19.90,-0.0,1e-7,9223372036854775808,123456789012345678901234567890]} \
        text "{\"\xc3\xa9\":\"\xe2\x82\xac\xf0\x9f\x98\x80\",\"esc\":\"\\u00e9\\n\"}"]
}

proc json_ks_wait_sync {primary replica} {
    wait_for_ofs_sync [Rn $primary] [Rn $replica]
}

start_cluster 3 3 {tags {external:skip cluster json} overrides {cluster-allow-replica-migration no}} {

    # Node 0 serves slots 0-5460, node 1 5461-10922, node 2 10923-16383.
    # Node i+3 is the replica of node i.
    lassign [json_ks_tag 0 5460] tag0 slot0
    lassign [json_ks_tag 5461 10922] tag1 slot1
    lassign [json_ks_tag 5461 10922 $slot1] tag1b slot1b
    lassign [json_ks_tag 10923 16383] tag2 slot2

    test "JSON: DUMP on one node, RESTORE on another" {
        set docs0 [json_ks_stat 0 num_documents]
        set docs1 [json_ks_stat 1 num_documents]
        foreach {name text} [json_ks_documents] {
            assert_equal OK [R 0 JSON.SET "${tag0}d:$name" . $text]
            set payload [R 0 DUMP "${tag0}d:$name"]
            set digest [R 0 DEBUG DIGEST-VALUE "${tag0}d:$name"]
            set got [R 0 JSON.GET "${tag0}d:$name" $]

            # The key must be served by the node it is restored on.
            assert_equal "MOVED $slot0 127.0.0.1:[srv 0 port]" [catch {R 1 RESTORE "${tag0}r:$name" 0 $payload} e; set e]
            assert_equal OK [R 1 RESTORE "${tag1}r:$name" 0 $payload]
            assert_equal ReJSON-RL [R 1 TYPE "${tag1}r:$name"]
            assert_equal $got [R 1 JSON.GET "${tag1}r:$name" $]
            assert_equal $digest [R 1 DEBUG DIGEST-VALUE "${tag1}r:$name"]
            assert_equal -1 [R 1 TTL "${tag1}r:$name"]

            # A TTL, an absolute expire time, and REPLACE over a document.
            assert_equal OK [R 1 RESTORE "${tag1}t:$name" 100000 $payload]
            assert_range [R 1 PTTL "${tag1}t:$name"] 90000 100000
            set at [expr {[clock milliseconds] + 200000}]
            assert_equal OK [R 1 RESTORE "${tag1}a:$name" $at $payload ABSTTL]
            assert_equal $at [R 1 PEXPIRETIME "${tag1}a:$name"]
            assert_error "BUSYKEY*" {R 1 RESTORE "${tag1}r:$name" 0 $payload}
            R 1 JSON.SET "${tag1}x:$name" $ {{"other":1}}
            assert_equal OK [R 1 RESTORE "${tag1}x:$name" 0 $payload REPLACE]
            assert_equal $got [R 1 JSON.GET "${tag1}x:$name" $]
        }
        set n [llength [dict keys [json_ks_documents]]]
        json_ks_wait_docs 0 [expr {$docs0 + $n}]
        json_ks_wait_docs 1 [expr {$docs1 + 4 * $n}]
        json_ks_wait_sync 1 4
        assert_equal [R 1 DEBUG DIGEST] [R 4 DEBUG DIGEST]
        json_ks_wait_docs 4 [expr {$docs1 + 4 * $n}]
    }

    test "JSON: COPY within a node is a deep copy; across slots it is CROSSSLOT" {
        set docs [json_ks_stat 1 num_documents]
        foreach {name text} [json_ks_documents] {
            set src "${tag1}c:$name"
            assert_equal OK [R 1 JSON.SET $src . $text]
            R 1 EXPIRE $src 5000
            assert_equal 1 [R 1 COPY $src "${tag1}c2:$name"]
            assert_equal [R 1 JSON.GET $src $] [R 1 JSON.GET "${tag1}c2:$name" $]
            assert_equal [R 1 DEBUG DIGEST-VALUE $src] [R 1 DEBUG DIGEST-VALUE "${tag1}c2:$name"]
            assert_range [R 1 TTL "${tag1}c2:$name"] 4900 5000
            # A change in place to the copy does not reach the original.
            set before [R 1 JSON.GET $src $]
            assert_equal 1 [R 1 JSON.CLEAR "${tag1}c2:$name" $]
            assert_equal $before [R 1 JSON.GET $src $]
            # COPY onto an existing key needs REPLACE.
            assert_equal 0 [R 1 COPY $src "${tag1}c2:$name"]
            assert_equal 1 [R 1 COPY $src "${tag1}c2:$name" REPLACE]
            assert_equal $before [R 1 JSON.GET "${tag1}c2:$name" $]
            # A change in place to the original does not reach the copy.
            assert_equal 1 [R 1 JSON.CLEAR $src $]
            assert_equal $before [R 1 JSON.GET "${tag1}c2:$name" $]
            assert_equal 1 [R 1 COPY "${tag1}c2:$name" $src REPLACE]
            # Another slot of the same node, and a slot of another node.
            assert_error "CROSSSLOT*" {R 1 COPY $src "${tag1b}c3:$name"}
            assert_error "CROSSSLOT*" {R 1 COPY $src "${tag2}c3:$name"}
        }
        set n [llength [dict keys [json_ks_documents]]]
        json_ks_wait_docs 1 [expr {$docs + 2 * $n}]
        json_ks_wait_sync 1 4
        assert_equal [R 1 DEBUG DIGEST] [R 4 DEBUG DIGEST]
        json_ks_wait_docs 4 [json_ks_stat 1 num_documents]
    }

    test "JSON: RENAME within a slot keeps the document and its TTL; across slots it is CROSSSLOT" {
        set docs [json_ks_stat 2 num_documents]
        foreach {name text} [json_ks_documents] {
            set src "${tag2}n:$name"
            assert_equal OK [R 2 JSON.SET $src . $text]
            R 2 PEXPIRE $src 7000000
            set got [R 2 JSON.GET $src $]
            set digest [R 2 DEBUG DIGEST-VALUE $src]
            set at [R 2 PEXPIRETIME $src]
            assert_equal OK [R 2 RENAME $src "${tag2}n2:$name"]
            assert_equal 0 [R 2 EXISTS $src]
            assert_equal ReJSON-RL [R 2 TYPE "${tag2}n2:$name"]
            assert_equal $got [R 2 JSON.GET "${tag2}n2:$name" $]
            assert_equal $digest [R 2 DEBUG DIGEST-VALUE "${tag2}n2:$name"]
            assert_equal $at [R 2 PEXPIRETIME "${tag2}n2:$name"]
            # RENAMENX onto an existing key does nothing; RENAME over a document replaces it.
            R 2 JSON.SET "${tag2}n3:$name" $ 1
            assert_equal 0 [R 2 RENAMENX "${tag2}n2:$name" "${tag2}n3:$name"]
            assert_equal OK [R 2 RENAME "${tag2}n2:$name" "${tag2}n3:$name"]
            assert_equal $got [R 2 JSON.GET "${tag2}n3:$name" $]
            assert_error "CROSSSLOT*" {R 2 RENAME "${tag2}n3:$name" "${tag0}n4:$name"}
            assert_equal 1 [R 2 EXISTS "${tag2}n3:$name"]
        }
        set n [llength [dict keys [json_ks_documents]]]
        json_ks_wait_docs 2 [expr {$docs + $n}]
        json_ks_wait_sync 2 5
        assert_equal [R 2 DEBUG DIGEST] [R 5 DEBUG DIGEST]
        json_ks_wait_docs 5 [json_ks_stat 2 num_documents]
    }

    test "JSON: SCAN and CLUSTERSCAN with TYPE ReJSON-RL find every document of the cluster" {
        foreach n {0 1 2} { R $n FLUSHALL SYNC }
        set expected {}
        for {set i 0} {$i < 300} {incr i} {
            set slot [R 0 CLUSTER KEYSLOT "scan:$i"]
            set node [expr {$slot <= 5460 ? 0 : ($slot <= 10922 ? 1 : 2)}]
            if {$i % 3 == 0} {
                R $node SET "scan:$i" str
            } elseif {$i % 3 == 1} {
                R $node HSET "scan:$i" f v
            } else {
                R $node JSON.SET "scan:$i" $ "{\"i\":$i}"
                lappend expected "scan:$i"
            }
        }
        set expected [lsort $expected]

        # Each node, primaries and replicas, returns the documents of its shard.
        set found {}
        foreach n {0 1 2} {
            json_ks_wait_sync $n [expr {$n + 3}]
            foreach node [list $n [expr {$n + 3}]] {
                set keys {}
                set cursor 0
                while 1 {
                    lassign [R $node SCAN $cursor COUNT 50 TYPE ReJSON-RL] cursor batch
                    lappend keys {*}$batch
                    if {$cursor == 0} break
                }
                set keys [lsort -unique $keys]
                assert_equal [json_ks_stat $node num_documents] [llength $keys]
                if {$node == $n} {
                    set primary_keys $keys
                    lappend found {*}$keys
                } else {
                    assert_equal $primary_keys $keys "SCAN on replica $node"
                }
            }
        }
        assert_equal $expected [lsort $found]

        # CLUSTERSCAN, following MOVED from one node to the next.
        set found {}
        set cursor 0
        for {set iter 0} {$iter < 20000} {incr iter} {
            lassign [json_ks_routed 0 CLUSTERSCAN $cursor COUNT 20 TYPE ReJSON-RL] cursor batch
            foreach k $batch {
                assert_equal ReJSON-RL [json_ks_routed 0 TYPE $k]
            }
            lappend found {*}$batch
            if {$cursor eq "0"} break
        }
        assert_equal $expected [lsort -unique $found]
        # MATCH with TYPE.
        set found {}
        set cursor 0
        for {set iter 0} {$iter < 20000} {incr iter} {
            lassign [json_ks_routed 0 CLUSTERSCAN $cursor MATCH scan:1* TYPE ReJSON-RL] cursor batch
            lappend found {*}$batch
            if {$cursor eq "0"} break
        }
        assert_equal [lsearch -all -inline $expected scan:1*] [lsort -unique $found]
    }

    test "JSON: a replica serves JSON reads after READONLY and redirects the rest" {
        R 0 JSON.SET "${tag0}ro" $ {{"n":1,"s":"str","arr":[1,2,3],"o":{"k":"v"},"f":19.90}}
        R 0 JSON.SET "${tag0}ro2" $ {{"n":2}}
        json_ks_wait_sync 0 3
        set port0 [srv 0 port]
        # Without READONLY a replica redirects reads to its primary.
        assert_equal "MOVED $slot0 127.0.0.1:$port0" [catch {R 3 JSON.GET "${tag0}ro"} e; set e]
        assert_equal OK [R 3 READONLY]
        foreach cmd [list \
                [list JSON.GET "${tag0}ro"] \
                [list JSON.GET "${tag0}ro" $.o $.arr] \
                [list JSON.MGET "${tag0}ro" "${tag0}ro2" $.n] \
                [list JSON.TYPE "${tag0}ro" $.f] \
                [list JSON.STRLEN "${tag0}ro" $.s] \
                [list JSON.ARRLEN "${tag0}ro" $.arr] \
                [list JSON.ARRINDEX "${tag0}ro" $.arr 2] \
                [list JSON.OBJLEN "${tag0}ro" $.o] \
                [list JSON.OBJKEYS "${tag0}ro"] \
                [list JSON.RESP "${tag0}ro" $.o] \
                [list JSON.DEBUG FIELDS "${tag0}ro"]] {
            assert_equal [R 0 {*}$cmd] [R 3 {*}$cmd] "$cmd on the replica"
        }
        # Writes go to the primary, also after READONLY.
        foreach cmd [list \
                [list JSON.SET "${tag0}ro" $.n 5] \
                [list JSON.NUMINCRBY "${tag0}ro" $.n 1] \
                [list JSON.ARRAPPEND "${tag0}ro" $.arr 4] \
                [list JSON.DEL "${tag0}ro" $.o] \
                [list JSON.MSET "${tag0}ro" $.n 1 "${tag0}ro2" $.n 1]] {
            assert_equal "MOVED $slot0 127.0.0.1:$port0" [catch {R 3 {*}$cmd} e; set e] "$cmd on the replica"
        }
        # A key of another shard is MOVED to that shard's primary.
        assert_equal "MOVED $slot1 127.0.0.1:[srv -1 port]" [catch {R 3 JSON.GET "${tag1}ro"} e; set e]
        # A write on the primary is visible on the replica.
        assert_equal {[2]} [R 0 JSON.NUMINCRBY "${tag0}ro" $.n 1]
        json_ks_wait_sync 0 3
        assert_equal {[2]} [R 3 JSON.GET "${tag0}ro" $.n]
        assert_equal OK [R 3 READWRITE]
        assert_equal "MOVED $slot0 127.0.0.1:$port0" [catch {R 3 JSON.GET "${tag0}ro"} e; set e]
    }
}
