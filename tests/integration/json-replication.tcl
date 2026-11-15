# Tests for the built-in JSON module: replication.
#
#   - A replica receives JSON documents through a full sync: disk-based and
#     diskless on the primary, repl-diskless-load disabled and swapdb on the
#     replica, and with dual-channel replication.
#   - A replica receives every write command through the command stream,
#     verbatim, in exactly the cases in which the external valkey-json module
#     propagates it (design 2.4). That includes writes that match nothing: with
#     a $ path they are propagated, with a legacy path they are an error or 0
#     and are not.
#   - DEBUG DIGEST is equal on both sides, and WAIT sees the replica's
#     acknowledgement.
#   - When the external valkey-json module is available (see
#     json_external_module_path), the built-in module replicates from it and
#     to it, including what only the external module accepts from a client
#     (invalid UTF-8 and out-of-range numbers: B1 and B2 are not applied to
#     the replication stream).

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }

# Asserts that the module counts `expected` JSON documents. A large document
# is freed on the lazy-free thread and the counter moves when that free runs,
# so after a delete, an overwrite or a flush the value is awaited, not read once.
proc json_assert_num_documents {expected {client r}} {
    wait_for_condition 100 50 {
        [getInfoProperty [$client info json] json_num_documents] == $expected
    } else {
        fail "json_num_documents is [getInfoProperty [$client info json] json_num_documents], expected $expected"
    }
}

proc json_external_module_path {} {
    set candidates {}
    if {[info exists ::env(VALKEY_JSON_EXTERNAL_MODULE)]} {
        lappend candidates $::env(VALKEY_JSON_EXTERNAL_MODULE)
    }
    foreach path $candidates {
        if {[file exists $path]} { return [file normalize $path] }
    }
    return ""
}

# Documents of every shape, as a list of key and text.
proc json_repl_documents {} {
    set docs {}
    lappend docs doc:int 17
    lappend docs doc:string {"a\"b\\c d"}
    lappend docs doc:numbers {[0,-1,19.90,1e5,1.0E3,-0.0,9223372036854775807,9223372036854775808,1234567890123456789012345,0.1,1.5e+21]}
    lappend docs doc:object {{"id":7,"name":"seven","tags":["a","b"],"nested":{"x":{"y":[true,false,null]}},"empty":{},"none":[]}}
    set l {}
    for {set i 0} {$i < 500} {incr i} { lappend l "\"member$i\":{\"id\":$i,\"tags\":\[\"t$i\",$i,$i.5\],\"on\":[expr {$i % 2 ? "true" : "false"}]}" }
    lappend docs doc:bigobject "{[join $l ,]}"
    set l {}
    for {set i 0} {$i < 3000} {incr i} { lappend l [expr {$i * 1000003}] }
    lappend docs doc:bigints "\[[join $l ,]\]"
    set l {}
    for {set i 0} {$i < 300} {incr i} { lappend l [format %.6f [expr {sin($i)}]] }
    lappend docs doc:doubles "\[[join $l ,]\]"
    lappend docs doc:longstring "\"[string repeat {abcdefghij} 3000]\""
    lappend docs doc:deep "[string repeat {[} 100]\"x\"[string repeat {]} 100]"
    lappend docs doc:null null
    return $docs
}

# The same table as in unit/type/json-keyspace.tcl: write commands on keys k
# and k2 with the reply of the external module ("nil" a null reply,
# "E:<prefix>" an error) and whether that module propagates the command.
proc json_repl_write_table {} {
    return {
        {{json.set k . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1}}}} OK 1}
        {{json.set k $.a 2}                 OK                1}
        {{json.set k .a 3}                  OK                1}
        {{json.set k $.zz.yy 1}             E:NONEXISTENT     0}
        {{json.set k {$.arr[*].q} 1}        OK                1}
        {{json.set k {.arr[*].q} 1}         OK                1}
        {{json.set k . 1 NX}                nil               0}
        {{json.set k3 . 1 XX}               nil               0}
        {{json.mset k $.a 4 k2 . {{"z":1}}} OK                1}
        {{json.mset k $.zz.yy 4 k2 . {{"z":2}}} E:NONEXISTENT 0}
        {{json.merge k $.o {{"y":2}}}       OK                1}
        {{json.merge k $.nomatch.x 1}       E:NONEXISTENT     0}
        {{json.numincrby k $.a 1}           {[5]}             1}
        {{json.numincrby k $.zz 1}          {[]}              1}
        {{json.numincrby k .zz 1}           E:NONEXISTENT     0}
        {{json.numincrby k $.s 1}           {[null]}          1}
        {{json.numincrby k .s 1}            E:WRONGTYPE       0}
        {{json.nummultby k $.a 2}           {[10]}            1}
        {{json.nummultby k $.zz 2}          {[]}              1}
        {{json.nummultby k .zz 2}           E:NONEXISTENT     0}
        {{json.strappend k $.s {"y"}}       2                 1}
        {{json.strappend k $.zz {"y"}}      {}                1}
        {{json.strappend k .zz {"y"}}       E:NONEXISTENT     0}
        {{json.strappend k $.a {"y"}}       {{}}              1}
        {{json.toggle k $.b}                0                 1}
        {{json.toggle k $.zz}               {}                1}
        {{json.toggle k .zz}                E:NONEXISTENT     0}
        {{json.toggle k $.a}                {{}}              1}
        {{json.arrappend k $.arr 4}         4                 1}
        {{json.arrappend k $.zz 4}          {}                1}
        {{json.arrappend k .zz 4}           E:NONEXISTENT     0}
        {{json.arrinsert k $.arr 0 0}       5                 1}
        {{json.arrinsert k $.zz 0 0}        {}                1}
        {{json.arrinsert k .zz 0 0}         E:NONEXISTENT     0}
        {{json.arrpop k $.arr}              4                 1}
        {{json.arrpop k $.zz}               {}                1}
        {{json.arrpop k .zz}                E:NONEXISTENT     0}
        {{json.arrpop k $.a}                {{}}              1}
        {{json.arrtrim k $.arr 0 1}         2                 1}
        {{json.arrtrim k $.zz 0 1}          {}                1}
        {{json.arrtrim k .zz 0 1}           E:NONEXISTENT     0}
        {{json.clear k $.o}                 1                 1}
        {{json.clear k $.o}                 0                 1}
        {{json.clear k $.zz}                0                 1}
        {{json.clear k .zz}                 0                 1}
        {{json.del k $.o}                   1                 1}
        {{json.del k $.zz}                  0                 1}
        {{json.del k .zz}                   0                 0}
        {{json.forget k $.s}                1                 1}
        {{json.forget k .zz}                0                 0}
        {{json.del nokey}                   0                 0}
        {{json.del k}                       1                 1}
        {{json.forget k2}                   1                 1}
    }
}

# N levels of nested arrays, the innermost empty, and N levels of nested
# objects under the member "a", the innermost value 1.
proc json_deep_array {n} {
    return "[string repeat {[} $n][string repeat {]} $n]"
}
proc json_deep_object {n} {
    return "[string repeat "\{\"a\":" $n]1[string repeat "\}" $n]"
}

# Documents nested deeper than json.max-path-limit (128), written through
# client `c` in the two ways that are possible: under a raised limit that is
# then lowered again, and the way that also works in the external module,
# 128 nested arrays and then one more level appended at the bottom. Returns
# the {key text} list.
proc json_write_deep_documents {c {raise_limit 1}} {
    set docs {}
    if {$raise_limit} {
        $c config set json.max-path-limit 2000
        foreach {key text} [list deep:array600 [json_deep_array 600] deep:object1024 [json_deep_object 1024] \
                                 deep:array1024 [json_deep_array 1024]] {
            assert_equal OK [$c json.set $key . $text]
            lappend docs $key $text
        }
        $c config set json.max-path-limit 128
    }
    assert_equal OK [$c json.set deep:129 $ [json_deep_array 128]]
    assert_equal 1 [$c json.arrappend deep:129 "\$[string repeat {[0]} 127]" {[1]}]
    assert_equal 129 [$c json.debug depth deep:129]
    lappend docs deep:129 "[string repeat {[} 128]\[1\][string repeat {]} 128]"
    return $docs
}

# Runs one command on client c and checks the reply against the table form.
proc json_repl_check_reply {c cmd expected} {
    set failed [catch {$c {*}$cmd} res]
    if {[string match "E:*" $expected]} {
        if {!$failed} { fail "'$cmd' replied '$res', expected an error '[string range $expected 2 end]'" }
        assert_match "[string range $expected 2 end]*" $res
    } elseif {$failed} {
        fail "'$cmd' failed: $res"
    } elseif {$expected eq "nil"} {
        assert_equal {} $res "reply of '$cmd'"
    } else {
        assert_equal $expected $res "reply of '$cmd'"
    }
}

# Waits until the replica has applied everything the primary wrote.
proc json_repl_sync {primary replica} {
    $primary wait 1 0
    wait_for_ofs_sync $primary $replica
}

# Asserts that both sides hold the same keys with the same JSON text and the
# same dataset digest.
proc json_repl_assert_same {primary replica} {
    json_repl_sync $primary $replica
    assert_equal [$primary dbsize] [$replica dbsize]
    foreach key [$primary keys *] {
        if {[$primary type $key] eq "ReJSON-RL"} {
            assert_equal ReJSON-RL [$replica type $key] "type of $key on the replica"
            assert_equal [$primary json.get $key] [$replica json.get $key] "text of $key"
        }
    }
    assert_equal [$primary debug digest] [$replica debug digest]
}

# A random write command on one of four keys. Many of them fail or match
# nothing, which is part of the test.
proc json_repl_random_command {} {
    set key rnd:[expr {int(rand() * 4)}]
    set n [expr {int(rand() * 1000)}]
    set f [format %.2f [expr {rand() * 100}]]
    set name [lindex {a b c arr o s n} [expr {int(rand() * 7)}]]
    set idx [expr {int(rand() * 6) - 2}]
    set value [lindex [list $n $f "\"s$n\"" true null "\[$n,$f\]" "{\"a\":$n}" "{\"arr\":\[1,2,$n\],\"o\":{\"n\":$f}}" 1e5 -0.0] [expr {int(rand() * 10)}]]
    set path [lindex [list ".$name" "\$.$name" "\$..$name" "\$.*" ".arr\[$idx\]" "\$.arr\[$idx\]" "\$.o.$name" ".zz" "\$.zz" "\$..arr\[*\]" . $] [expr {int(rand() * 12)}]]
    switch [expr {int(rand() * 16)}] {
        0 - 1 - 2 { return [list json.set $key $path $value] }
        3 { return [list json.set $key . "{\"a\":$n,\"b\":true,\"s\":\"x\",\"n\":$f,\"arr\":\[1,2,3\],\"o\":{\"a\":1}}"] }
        4 { return [list json.mset $key $path $value rnd:[expr {int(rand() * 4)}] ".$name" $n] }
        5 { return [list json.merge $key $path $value] }
        6 { return [list json.numincrby $key $path [lindex [list $n $f -1 0.1] [expr {int(rand() * 4)}]]] }
        7 { return [list json.nummultby $key $path [lindex [list 2 $f -1 0.5] [expr {int(rand() * 4)}]]] }
        8 { return [list json.strappend $key $path "\"x$n\""] }
        9 { return [list json.toggle $key $path] }
        10 { return [list json.arrappend $key $path $value $n] }
        11 { return [list json.arrinsert $key $path $idx $value] }
        12 { return [list json.arrpop $key $path $idx] }
        13 { return [list json.arrtrim $key $path 0 [expr {int(rand() * 5)}]] }
        14 { return [list json.clear $key $path] }
        15 { return [list [lindex {json.del json.forget} [expr {int(rand() * 2)}]] $key [lindex [list $path $path $path .] [expr {int(rand() * 4)}]]] }
    }
}

# -------------------------------------------------------------------------
# What the primary propagates
# -------------------------------------------------------------------------

start_server {tags {"json repl external:skip"} overrides {save ""}} {
    test {JSON write commands are propagated verbatim, in the cases the module propagates them} {
        r flushall
        r select 9
        set repl [attach_to_replication_stream]
        set expected {{select 9}}
        foreach row [json_repl_write_table] {
            lassign $row cmd reply propagated
            json_repl_check_reply r $cmd $reply
            if {$propagated} { lappend expected $cmd }
        }
        r set marker 1
        lappend expected {set marker 1}
        set got {}
        while 1 {
            set cmd [read_from_replication_stream $repl]
            if {$cmd eq ""} break
            if {$cmd eq "ping"} continue
            lappend got $cmd
            if {$cmd eq {set marker 1}} break
        }
        close_replication_stream $repl
        assert_equal [llength $expected] [llength $got] "propagated: $got"
        foreach e $expected g $got {
            assert_equal [lrange $e 0 end] [lrange $g 0 end]
        }
    }

    test {JSON commands in MULTI/EXEC and in a script are propagated as the commands themselves} {
        r flushall
        r select 9
        set repl [attach_to_replication_stream]
        r multi
        r json.set tx . {{"a":1}}
        r json.numincrby tx .a 1
        r json.get tx
        r exec
        r eval {
            server.call('JSON.SET', KEYS[1], '.', '{"n":1}')
            server.call('JSON.GET', KEYS[1])
            return server.call('JSON.NUMINCRBY', KEYS[1], '$.n', 2)
        } 1 lua
        assert_replication_stream $repl {
            {multi}
            {select *}
            {json.set tx . {{"a":1}}}
            {json.numincrby tx .a 1}
            {exec}
            {multi}
            {json.set lua . {{"n":1}}}
            {json.numincrby lua {$.n} 2}
            {exec}
        }
        close_replication_stream $repl
    }
}

# -------------------------------------------------------------------------
# Full sync and command stream to a replica
# -------------------------------------------------------------------------

foreach {mdl sdl dual} {no disabled no  yes disabled no  no swapdb no  yes swapdb no  yes swapdb yes} {
    start_server [list tags {"json repl external:skip"} overrides [list save "" enable-debug-command yes \
                                                                       dual-channel-replication-enabled $dual]] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        $primary config set repl-diskless-sync $mdl
        $primary config set repl-diskless-sync-delay 0

        start_server [list tags {"json"} overrides [list save "" enable-debug-command yes dual-channel-replication-enabled $dual]] {
            set replica [srv 0 client]
            $replica config set repl-diskless-load $sdl
            # The caps are local to a server: the replica uses others than the primary.
            $replica config set json.max-packed-bytes 0 json.max-packed-entries 0
            set label "diskless sync $mdl, diskless load $sdl, dual channel $dual"

            test "A replica receives JSON documents through a full sync ($label)" {
                # Documents written under each cap setting on the primary.
                set n 0
                foreach caps [json_caps_list] {
                    $primary config set json.max-packed-bytes [lindex $caps 0] json.max-packed-entries [lindex $caps 1]
                    foreach {key text} [json_repl_documents] {
                        assert_equal OK [$primary json.set $key:$n . $text]
                    }
                    incr n
                }
                $primary json.set doc:ttl . {{"expires":"in 2100"}}
                $primary expireat doc:ttl 4102444800
                $primary set plain:string hello
                # The replica holds data of its own, which the sync replaces.
                $replica json.set stale . {{"old":[1,2,3]}}
                $replica json.set doc:int:0 . {"stale"}

                $replica replicaof $primary_host $primary_port
                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "the replica did not finish the sync"
                }
                json_repl_assert_same $primary $replica
                assert_equal 0 [$replica exists stale]
                for {set i 0} {$i < 3} {incr i} {
                    foreach {key text} [json_repl_documents] {
                        assert_equal $text [$replica json.get $key:$i]
                    }
                }
                assert_equal 4102444800 [$replica expiretime doc:ttl]
                json_assert_num_documents [expr {[$primary dbsize] - 1}] $replica
                assert_equal 1 [s -1 sync_full]
            }

            test "A replica is read-only for JSON commands and serves reads ($label)" {
                assert_error "READONLY*" {$replica json.set doc:int:0 . 1}
                assert_error "READONLY*" {$replica json.numincrby doc:int:0 . 1}
                assert_error "READONLY*" {$replica json.del doc:int:0}
                assert_error "READONLY*" {$replica json.arrappend doc:numbers:0 . 1}
                assert_equal 17 [$replica json.get doc:int:0]
                assert_equal {[19.90]} [$replica json.get doc:numbers:0 {$[2]}]
                assert_equal integer [$replica json.type doc:int:0]
                assert_equal 6 [$replica json.objlen doc:object:1]
                assert_equal {{17 17 17}} [list [$replica json.mget doc:int:0 doc:int:1 doc:int:2 .]]
                assert_morethan [$replica json.debug memory doc:bigobject:2] 0
            }

            test "A replica applies every JSON write command from the command stream ($label)" {
                foreach row [json_repl_write_table] {
                    lassign $row cmd reply propagated
                    set offset [status $primary master_repl_offset]
                    json_repl_check_reply $primary $cmd $reply
                    if {$propagated} {
                        assert_morethan [status $primary master_repl_offset] $offset
                    } else {
                        assert_equal $offset [status $primary master_repl_offset] "'$cmd' moved the replication offset"
                    }
                    assert_equal 1 [$primary wait 1 10000] "WAIT after '$cmd'"
                    foreach key {k k2} {
                        assert_equal [$primary json.get $key] [$replica json.get $key] "$key after '$cmd'"
                    }
                }
                json_repl_assert_same $primary $replica
            }

            test "WAIT returns once the replica acknowledged a JSON write ($label)" {
                $primary json.set w . {{"n":0}}
                for {set i 1} {$i <= 20} {incr i} {
                    $primary json.numincrby w .n 1
                    assert_equal 1 [$primary wait 1 10000]
                    assert_equal $i [$replica json.get w .n]
                }
                # WAITAOF and WAIT with no pending write return at once.
                assert_equal 1 [$primary wait 1 10000]
            }

            test "TTL changes made by JSON writes reach the replica ($label)" {
                $primary json.set t . {{"a":1}}
                $primary expireat t 4102444800
                $primary json.set t .a 2
                $primary json.numincrby t .a 1
                json_repl_sync $primary $replica
                assert_equal 4102444800 [$replica expiretime t]
                assert_equal {{"a":3}} [$replica json.get t]
                $primary json.set t . {{"a":4}}
                json_repl_sync $primary $replica
                assert_equal -1 [$replica ttl t]
                assert_equal {{"a":4}} [$replica json.get t]
                # A document that expires on the primary is deleted on the replica.
                $primary json.set gone . {[1,2,3]}
                $primary pexpire gone 1
                wait_for_condition 100 50 {
                    [$replica exists gone] == 0
                } else {
                    fail "the expired key was not deleted on the replica"
                }
            }

            test "Transactions and scripts with JSON commands reach the replica ($label)" {
                $primary multi
                $primary json.set tx . {{"a":1,"arr":[]}}
                $primary json.arrappend tx .arr 1 2 3
                $primary json.numincrby tx .a 41
                $primary exec
                $primary eval {
                    server.call('JSON.SET', KEYS[1], '.', '{"n":1.5}')
                    return server.call('JSON.NUMINCRBY', KEYS[1], '$.n', 1.5)
                } 1 lua
                $primary json.set m1 . {{"x":1}}
                $primary json.mset m1 .y 2 m2 . {[1,2]} m1 .x 3
                json_repl_sync $primary $replica
                assert_equal {{"a":42,"arr":[1,2,3]}} [$replica json.get tx]
                assert_equal {{"n":3.0}} [$replica json.get lua]
                assert_equal {{"x":3,"y":2}} [$replica json.get m1]
                assert_equal {[1,2]} [$replica json.get m2]
            }

            test "Random JSON writes leave the primary and the replica identical ($label)" {
                expr {srand(20261114)}
                set count 3000
                if {$::accurate} { set count 30000 }
                set failed 0
                for {set i 0} {$i < $count} {incr i} {
                    if {$i % 500 == 0} {
                        set caps [lindex [json_caps_list] [expr {($i / 500) % 3}]]
                        $primary config set json.max-packed-bytes [lindex $caps 0] json.max-packed-entries [lindex $caps 1]
                    }
                    if {[catch {$primary {*}[json_repl_random_command]}]} { incr failed }
                }
                # The workload is meant to have both kinds.
                assert_morethan $failed 0
                assert_lessthan $failed $count
                json_repl_assert_same $primary $replica
                foreach key {rnd:0 rnd:1 rnd:2 rnd:3} {
                    assert_equal [$primary json.get $key] [$replica json.get $key]
                    assert_equal [$primary debug digest-value $key] [$replica debug digest-value $key]
                }
                $primary config set json.max-packed-bytes 4096 json.max-packed-entries 128
            }

            test "Documents deeper than json.max-path-limit reach the replica through the command stream and a full sync ($label)" {
                # The replica keeps the default limit of 128. What its primary sends is
                # held to max(json.max-path-limit, 1024) levels only.
                assert_equal {json.max-path-limit 128} [$replica config get json.max-path-limit]
                set docs [json_write_deep_documents $primary]
                json_repl_sync $primary $replica
                foreach {key text} $docs {
                    assert_equal $text [$replica json.get $key] "$key through the command stream"
                }
                assert_equal 129 [$replica json.debug depth deep:129]
                # Writes into them.
                assert_equal 2 [$primary json.arrappend deep:array600 {$[0]} {"after"}]
                assert_equal {[2]} [$primary json.numincrby deep:object1024 "\$[string repeat .a 1024]" 1]
                assert_equal 2 [$primary json.arrappend deep:129 "\$[string repeat {[0]} 128]" 2]
                json_repl_assert_same $primary $replica
                # The replica itself refuses such a write from a client once promoted, and
                # takes the documents again through a full sync.
                $replica replicaof no one
                assert_error "LIMIT*" {$replica json.set mine . [json_deep_array 129]}
                $replica flushall
                $replica replicaof $primary_host $primary_port
                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "the replica did not finish the sync"
                }
                json_repl_assert_same $primary $replica
                foreach key {deep:array600 deep:object1024 deep:array1024 deep:129} {
                    assert_equal [$primary json.get $key] [$replica json.get $key] "$key through a full sync"
                }
                assert_equal "[string repeat {[} 128]\[1,2\][string repeat {]} 128]" [$replica json.get deep:129]
                assert_equal 1024 [$replica json.debug depth deep:object1024]
            }

            test "Lazy deletes, FLUSHALL and a second full sync ($label)" {
                $primary unlink doc:bigobject:0 doc:bigints:0
                $primary json.del doc:bigobject:1
                json_repl_assert_same $primary $replica
                $primary flushall
                json_repl_sync $primary $replica
                assert_equal 0 [$replica dbsize]
                wait_lazyfree_done $replica
                json_assert_num_documents 0 $replica

                # Sync again from scratch, with the replica holding documents.
                $replica replicaof no one
                foreach {key text} [json_repl_documents] {
                    $primary json.set $key . $text
                    $replica json.set $key:old . $text
                }
                $replica replicaof $primary_host $primary_port
                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "the replica did not finish the second sync"
                }
                json_repl_assert_same $primary $replica
                foreach {key text} [json_repl_documents] {
                    assert_equal $text [$replica json.get $key]
                    assert_equal 0 [$replica exists $key:old]
                }
            }

            test "A promoted replica takes JSON writes ($label)" {
                $replica replicaof no one
                assert_equal 18 [$replica json.numincrby doc:int . 1]
                assert_equal 12 [$replica json.arrappend doc:numbers . 2.5]
                assert_equal OK [$replica json.set fresh . {{"promoted":true}}]
                assert_equal 17 [$primary json.get doc:int]
            }
        }
    }
}

# -------------------------------------------------------------------------
# Between the external module and the built-in one
# -------------------------------------------------------------------------

set json_external [json_external_module_path]

if {$json_external eq ""} {
    start_server {tags {"json repl external:skip"}} {
        test {Replication between the external valkey-json module and the built-in module} {
            puts "    \[skipped\]: external valkey-json module not found (set VALKEY_JSON_EXTERNAL_MODULE to its shared library)"
        }
    }
} else {
    # A primary with the external module, a replica with the built-in one.
    start_server [list tags {"json repl external:skip"} overrides [list save "" json-enabled no loadmodule $json_external enable-debug-command yes]] {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        start_server {tags {"json"} overrides {save "" enable-debug-command yes}} {
            set replica [srv 0 client]

            test {The built-in module replicates from a primary running the external module: full sync} {
                assert_match "*KEYTABLE-CHECK*" [$primary json.debug help]
                assert_no_match "*KEYTABLE-CHECK*" [$replica json.debug help]
                foreach {key text} [json_repl_documents] {
                    assert_equal OK [$primary json.set $key . $text]
                }
                $replica replicaof $primary_host $primary_port
                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "the replica did not finish the sync"
                }
                json_repl_assert_same $primary $replica
                foreach {key text} [json_repl_documents] {
                    assert_equal $text [$replica json.get $key]
                }
            }

            test {The built-in module replicates from a primary running the external module: command stream} {
                # The table has no computed double and no nested match, where
                # the two implementations differ by design (B3, B4).
                foreach row [json_repl_write_table] {
                    lassign $row cmd reply propagated
                    json_repl_check_reply $primary $cmd $reply
                    assert_equal 1 [$primary wait 1 10000]
                    foreach key {k k2} {
                        assert_equal [$primary json.get $key] [$replica json.get $key] "$key after '$cmd'"
                    }
                }
                json_repl_assert_same $primary $replica
            }

            test {A document of depth 129 built the module's way replicates from the external module} {
                # 128 nested arrays, then one more level appended at the bottom: the
                # external module allows it, and so the replica has to take it.
                set docs [json_write_deep_documents $primary 0]
                json_repl_sync $primary $replica
                foreach {key text} $docs {
                    assert_equal $text [$primary json.get $key]
                    assert_equal $text [$replica json.get $key]
                }
                assert_equal 129 [$replica json.debug depth deep:129]
                json_repl_assert_same $primary $replica
            }

            test {A replica accepts from the stream what the external module accepted from a client - B1, B2} {
                # Invalid UTF-8 and a number beyond the range of a double. The
                # built-in module rejects both from a client, and must take
                # them from its primary.
                set bad [binary format H* fffe80c0]
                assert_equal OK [$primary json.set utf8 . "{\"s\":\"$bad\",\"$bad\":1}"]
                assert_equal 5 [$primary json.strappend utf8 .s "\"[binary format H* ff]\""]
                assert_equal OK [$primary json.set range . {[1.8e308,-1.8e308,1.7976931348623159e308,1]}]
                assert_equal 5 [$primary json.arrappend range . 2e308]
                json_repl_sync $primary $replica
                assert_equal [$primary json.get utf8] [$replica json.get utf8]
                assert_equal "{\"s\":\"$bad[binary format H* ff]\",\"$bad\":1}" [$replica json.get utf8]
                assert_equal {[1.8e308,-1.8e308,1.7976931348623159e308,1,2e308]} [$replica json.get range]
                assert_equal {[2]} [$primary json.numincrby range {$[3]} 1]
                json_repl_sync $primary $replica
                assert_equal {[1.8e308,-1.8e308,1.7976931348623159e308,2,2e308]} [$replica json.get range]
                # And through a full sync.
                $replica replicaof no one
                $replica flushall
                $replica replicaof $primary_host $primary_port
                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "the replica did not finish the sync"
                }
                json_repl_sync $primary $replica
                assert_equal [$primary json.get utf8] [$replica json.get utf8]
                assert_equal {[1.8e308,-1.8e308,1.7976931348623159e308,2,2e308]} [$replica json.get range]
                # Promoted, it serves them and still rejects such input from a client.
                $replica replicaof no one
                assert_error "SYNTAXERR*" {$replica json.set utf8 .s "\"$bad\""}
                assert_error "SYNTAXERR*" {$replica json.set range {[0]} 1.8e308}
                assert_error "OVERFLOW*" {$replica json.numincrby range {$[0]} 1}
                assert_equal {[1.8e308,-1.8e308,1.7976931348623159e308,2,2e308]} [$replica json.get range]
            }
        }
    }

    # A primary with the built-in module, a replica with the external one.
    start_server {tags {"json repl external:skip"} overrides {save "" enable-debug-command yes}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        start_server [list tags {"json"} overrides [list save "" json-enabled no loadmodule $json_external enable-debug-command yes]] {
            set replica [srv 0 client]

            test {The external module replicates from a primary running the built-in module} {
                assert_match "*KEYTABLE-CHECK*" [$replica json.debug help]
                foreach {key text} [json_repl_documents] {
                    assert_equal OK [$primary json.set $key . $text]
                }
                $replica replicaof $primary_host $primary_port
                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "the replica did not finish the sync"
                }
                json_repl_assert_same $primary $replica
                foreach row [json_repl_write_table] {
                    lassign $row cmd reply propagated
                    json_repl_check_reply $primary $cmd $reply
                    assert_equal 1 [$primary wait 1 10000]
                    foreach key {k k2} {
                        assert_equal [$primary json.get $key] [$replica json.get $key] "$key after '$cmd'"
                    }
                }
                json_repl_assert_same $primary $replica
                foreach {key text} [json_repl_documents] {
                    assert_equal $text [$replica json.get $key]
                }
            }
        }
    }
}
