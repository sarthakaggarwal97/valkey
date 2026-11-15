# Tests for the built-in JSON module: persistence.
#
#   - RDB: DEBUG RELOAD, SAVE and restart, BGSAVE, with every pair of encoding
#     cap settings on the saving and the loading side.
#   - AOF: commands appended to the AOF, BGREWRITEAOF (with and without an RDB
#     preamble), and a restart from the AOF.
#   - The data type keeps the name and the encoding version of the external
#     valkey-json module (design section 9): an RDB file written by a server
#     running that module (tests/assets/json/valkey-json-module.rdb) loads
#     here with identical documents.
#   - Module configs given in the configuration file and written by CONFIG
#     REWRITE.

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

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

# The documents of the persistence tests, as a list of key and text. They
# cover every kind of value, numbers whose text must be kept exactly, typed
# arrays, and documents small and large enough for every in-memory form.
proc json_persist_documents {} {
    set docs {}
    lappend docs json:int 17
    # "a\"b\\c\u0000d " followed by U+00E9 and U+20AC as UTF-8, as bytes in hex.
    lappend docs json:string [binary format H* 22615c22625c5c635c75303030306420c3a9e282ac22]
    lappend docs json:numbers {[0,-1,19.90,1e5,1.0E3,-0.0,9223372036854775807,9223372036854775808,1234567890123456789012345,0.1,1.5e+21,1e-7,2.5,-128,32767,2147483648]}
    lappend docs json:object {{"id":7,"name":"seven","tags":["a","b"],"nested":{"x":{"y":[true,false,null]}},"empty":{},"none":[],"":0}}
    set l {}
    for {set i 0} {$i < 40} {incr i} { lappend l "{\"k\":\[$i,2,3\],\"s\":\"v$i\"}" }
    lappend docs json:array "\[[join $l ,]\]"
    set l {}
    for {set i 0} {$i < 200} {incr i} { lappend l [expr {$i * 37 - 500}] }
    lappend docs json:ints "\[[join $l ,]\]"
    set l {}
    for {set i 0} {$i < 100} {incr i} { lappend l [format %.2f [expr {$i / 7.0}]] }
    lappend docs json:doubles "\[[join $l ,]\]"
    lappend docs json:null null
    lappend docs json:ttl {{"expires":"in 2100"}}
    return $docs
}

# More documents, not in the asset file: larger ones and other shapes.
proc json_persist_more_documents {} {
    set docs {}
    set l {}
    for {set i 0} {$i < 500} {incr i} { lappend l "\"member$i\":{\"id\":$i,\"tags\":\[\"t$i\",$i,$i.5\],\"on\":[expr {$i % 2 ? "true" : "false"}]}" }
    lappend docs more:bigobject "{[join $l ,]}"
    set l {}
    for {set i 0} {$i < 3000} {incr i} { lappend l [expr {$i * 1000003}] }
    lappend docs more:bigints "\[[join $l ,]\]"
    lappend docs more:longstring "\"[string repeat {abcdefghij} 2000]\""
    lappend docs more:deep "[string repeat {[} 100]\"x\"[string repeat {]} 100]"
    lappend docs more:empty-object {{}}
    lappend docs more:empty-array {[]}
    lappend docs more:empty-string {""}
    lappend docs more:true true
    lappend docs more:float -1.5e-10
    lappend docs more:mixed {[1,"a",null,true,{"k":[]},[[]],1.25,"",{},-7]}
    return $docs
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
        try {
            foreach {key text} [list deep:array600 [json_deep_array 600] deep:object1024 [json_deep_object 1024] \
                                     deep:array1024 [json_deep_array 1024]] {
                assert_equal OK [$c json.set $key . $text]
                lappend docs $key $text
            }
        } finally {
            $c config set json.max-path-limit 128
        }
    }
    assert_equal OK [$c json.set deep:129 $ [json_deep_array 128]]
    assert_equal 1 [$c json.arrappend deep:129 "\$[string repeat {[0]} 127]" {[1]}]
    assert_equal 129 [$c json.debug depth deep:129]
    lappend docs deep:129 "[string repeat {[} 128]\[1\][string repeat {]} 128]"
    return $docs
}

proc json_write_documents {docs} {
    foreach {key text} $docs {
        assert_equal OK [r json.set $key . $text]
    }
}

proc json_assert_documents {docs} {
    foreach {key text} $docs {
        assert_equal ReJSON-RL [r type $key] "type of $key"
        assert_equal $text [r json.get $key] "text of $key"
    }
}

# A sequence of writes of every command on top of the documents above, and
# the documents it leaves. Returns the expected {key text} list.
proc json_apply_writes {} {
    r json.set w:doc . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1},"f":1.5}}
    r json.set w:doc $.a 2
    r json.mset w:doc $.m {"mset"} w:other . {{"z":[1.50,2.25]}}
    r json.merge w:doc $.o {{"y":{"deep":[1]},"x":null}}
    r json.numincrby w:doc $.a 40
    r json.nummultby w:doc $.f 2
    r json.numincrby w:other {$.z[0]} 1
    r json.strappend w:doc $.s {"yz"}
    r json.toggle w:doc $.b
    r json.arrappend w:doc $.arr 4 {"five"} {{"six":6}}
    r json.arrinsert w:doc $.arr 0 0
    r json.arrpop w:doc $.arr 1
    r json.arrtrim w:doc $.arr 0 4
    r json.set w:doc $.gone {[1,2,3]}
    r json.clear w:doc $.gone
    r json.set w:doc $.removed 1
    r json.del w:doc $.removed
    r json.set w:forgotten . 1
    r json.forget w:forgotten
    r json.set w:replaced . {[1,2,3]}
    r json.set w:replaced . {{"now":"object"}}
    return [list \
        w:doc {{"a":42,"b":false,"s":"xyz","arr":[0,2,3,4,"five"],"o":{"y":{"deep":[1]}},"f":3.0,"m":"mset","gone":[]}} \
        w:other {{"z":[2.5,2.25]}} \
        w:replaced {{"now":"object"}}]
}

tags {"json external:skip"} {

# -------------------------------------------------------------------------
# RDB
# -------------------------------------------------------------------------

start_server {tags {"json"} overrides {save ""}} {
    foreach caps [json_caps_list] {
        test "JSON documents survive DEBUG RELOAD (caps $caps)" {
            r flushall
            json_set_caps $caps
            set docs [concat [json_persist_documents] [json_persist_more_documents]]
            json_write_documents $docs
            set docs [concat $docs [json_apply_writes]]
            r expireat json:ttl 4102444800
            set digest [debug_digest]
            set count [r dbsize]
            r debug reload
            assert_equal $count [r dbsize]
            json_assert_documents $docs
            assert_equal $digest [debug_digest]
            assert_equal 4102444800 [r expiretime json:ttl]
            assert_equal 0 [r exists w:forgotten]
            json_assert_num_documents $count
            # Loaded documents take writes like any other.
            assert_equal 43 [r json.numincrby w:doc .a 1]
            assert_equal 201 [r json.arrappend json:ints . 1]
            assert_equal OK [r json.set json:object .nested.x.z {"new"}]
            assert_equal {{"y":[true,false,null],"z":"new"}} [r json.get json:object .nested.x]
        } {} {needs:debug}
    }

    foreach save_caps [json_caps_list] {
        foreach load_caps [json_caps_list] {
            test "JSON documents saved with caps {$save_caps} load with caps {$load_caps}" {
                r flushall
                json_set_caps $save_caps
                set docs [concat [json_persist_documents] [json_persist_more_documents]]
                json_write_documents $docs
                set docs [concat $docs [json_apply_writes]]
                set digest [debug_digest]
                json_set_caps $load_caps
                r debug reload
                json_assert_documents $docs
                assert_equal $digest [debug_digest]
                # Write to the documents loaded under the new caps, and reload once more
                # under the old ones.
                assert_equal 41 [r json.arrappend json:array . {{"k":[40,2,3],"s":"v40"}}]
                assert_equal 1 [r json.del more:bigobject .member0]
                assert_equal 499 [r json.objlen more:bigobject]
                set digest [debug_digest]
                json_set_caps $save_caps
                r debug reload
                assert_equal $digest [debug_digest]
                assert_equal {{"k":[40,2,3],"s":"v40"}} [r json.get json:array {[-1]}]
                assert_equal 499 [r json.objlen more:bigobject]
                assert_equal {{"id":499,"tags":["t499",499,499.5],"on":true}} [r json.get more:bigobject .member499]
            } {} {needs:debug}
        }
    }
    json_set_caps {4096 128}

    test {JSON documents survive SAVE and a restart} {
        r flushall
        set docs [concat [json_persist_documents] [json_persist_more_documents]]
        json_write_documents $docs
        set docs [concat $docs [json_apply_writes]]
        r expireat json:ttl 4102444800
        set digest [debug_digest]
        r save
        restart_server 0 true false
        wait_done_loading r
        assert_equal [expr {[llength $docs] / 2}] [r dbsize]
        json_assert_documents $docs
        assert_equal $digest [debug_digest]
        assert_equal 4102444800 [r expiretime json:ttl]
        json_assert_num_documents [r dbsize]
    } {} {needs:debug}

    test {JSON documents survive BGSAVE and DEBUG RELOAD NOSAVE} {
        r flushall
        set docs [concat [json_persist_documents] [json_persist_more_documents]]
        json_write_documents $docs
        set digest [debug_digest]
        r bgsave
        waitForBgsave r
        # Changes after the snapshot are not in it.
        r json.set json:int . 18
        r json.set after:snapshot . 1
        r debug reload nosave
        json_assert_documents $docs
        assert_equal 0 [r exists after:snapshot]
        assert_equal $digest [debug_digest]
    } {} {needs:debug}

    test {An RDB file with JSON documents passes valkey-check-rdb} {
        r flushall
        json_write_documents [concat [json_persist_documents] [json_persist_more_documents]]
        r save
        set rdb [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
        set out [exec src/valkey-check-rdb $rdb]
        assert_match "*RDB looks OK*" $out
    }
}

# -------------------------------------------------------------------------
# An RDB file written by the external module
# -------------------------------------------------------------------------

set server_path [tmpdir "server.json-module-rdb"]
exec cp -f tests/assets/json/valkey-json-module.rdb $server_path

foreach caps [json_caps_list] {
    lassign $caps bytes entries
    start_server [list tags {"json"} overrides [list dir $server_path dbfilename valkey-json-module.rdb save "" \
                                       json.max-packed-bytes $bytes json.max-packed-entries $entries]] {
        test "An RDB written by the external valkey-json module loads with identical documents (caps $caps)" {
            r select 0
            assert_equal [list json.max-packed-bytes $bytes] [r config get json.max-packed-bytes]
            assert_equal [list json.max-packed-entries $entries] [r config get json.max-packed-entries]
            assert_equal 11 [r dbsize]
            json_assert_documents [json_persist_documents]
            json_assert_num_documents 9
            # The keys of other types and the expiry in the same file.
            assert_equal hello [r get plain:string]
            assert_equal {a b} [r lrange plain:list 0 -1]
            assert_equal 4102444800 [r expiretime json:ttl]
        }

        test "The dataset loaded from the external module's RDB has the digest that server reported (caps $caps)" {
            r select 0
            assert_equal e5205c3124febac6d56b7166cc430cb348c71f7b [debug_digest]
            foreach {key digest} {
                json:int     ec4e8acfadb2b28b1552a64b09d616a265daf09d
                json:string  d72247b7dfa3b6e6928493954e64585d17919c0f
                json:numbers 3109373003324cafc5fc4bf30e3b085e38b141f9
                json:object  e771fa86c1b9d057ced38403f4528d61b3fc406d
                json:array   3156003269e1b2529b558e6daa4608b0b212dcd3
                json:ints    b5d2710cbc6e1a63ab0fa544e957ac31e2f6480c
                json:doubles 26cb13b6eea2767fddab01fc9918e0f74d14866e
                json:null    d5e589ec957770b60455ebf7e2c38cebe7c8c91e
                json:ttl     94d5c874734f38ebb96cdd808e3e9544f298b265
            } {
                assert_equal $digest [debug_digest_value $key] "digest of $key"
            }
        } {} {needs:debug}

        test "Documents loaded from the external module's RDB take writes and are saved again unchanged (caps $caps)" {
            r select 0
            set digest [debug_digest]
            r debug reload
            assert_equal $digest [debug_digest]
            json_assert_documents [json_persist_documents]
            assert_equal 18 [r json.numincrby json:int . 1]
            assert_equal 20.9 [r json.numincrby json:numbers {[2]} 1]
            assert_equal 201 [r json.arrappend json:ints . 7]
            assert_equal 7 [r json.objlen json:object]
            assert_equal 1 [r json.del json:object {$[""]}]
            assert_equal {id name tags nested empty none} [r json.objkeys json:object]
            assert_equal {[1.00]} [r json.get json:doubles {$[7]}]
        } {} {needs:debug}
    }
}

# -------------------------------------------------------------------------
# AOF
# -------------------------------------------------------------------------

foreach preamble {yes no} {
    start_server [list tags {"json"} overrides [list appendonly yes appendfsync always aof-use-rdb-preamble $preamble \
                                       auto-aof-rewrite-percentage 0 save ""]] {
        foreach caps [json_caps_list] {
            test "JSON commands are replayed from the AOF (preamble $preamble, caps $caps)" {
                r flushall
                json_set_caps $caps
                set docs [concat [json_persist_documents] [json_persist_more_documents]]
                json_write_documents $docs
                set docs [concat $docs [json_apply_writes]]
                set digest [debug_digest]
                r debug loadaof
                json_assert_documents $docs
                assert_equal $digest [debug_digest]
                assert_equal 0 [r exists w:forgotten]
            } {} {needs:debug}

            test "JSON documents survive BGREWRITEAOF and writes after it (preamble $preamble, caps $caps)" {
                r bgrewriteaof
                waitForBgrewriteaof r
                assert_equal ok [s aof_last_bgrewrite_status]
                # Writes that land in the incremental file after the rewrite.
                assert_equal 43 [r json.numincrby w:doc .a 1]
                assert_equal OK [r json.set after:rewrite . {{"k":[1,2,3]}}]
                assert_equal 4 [r json.arrappend after:rewrite .k 19.90]
                assert_equal 1 [r json.del json:null]
                set docs [concat [json_persist_documents] [json_persist_more_documents]]
                set expected {}
                foreach {key text} $docs {
                    if {$key ne "json:null"} { lappend expected $key $text }
                }
                lappend expected w:doc {{"a":43,"b":false,"s":"xyz","arr":[0,2,3,4,"five"],"o":{"y":{"deep":[1]}},"f":3.0,"m":"mset","gone":[]}}
                lappend expected w:other {{"z":[2.5,2.25]}} w:replaced {{"now":"object"}}
                lappend expected after:rewrite {{"k":[1,2,3,19.90]}}
                set digest [debug_digest]
                r debug loadaof
                json_assert_documents $expected
                assert_equal 0 [r exists json:null]
                assert_equal $digest [debug_digest]
            } {} {needs:debug}
        }
        json_set_caps {4096 128}

        test "The rewritten AOF holds JSON documents as [expr {$preamble eq "yes" ? "an RDB preamble" : "JSON.SET commands"}] (preamble $preamble)" {
            r flushall
            r json.set one . {{"a":[1,2.50,"x"]}}
            r json.set two . {"second"}
            r bgrewriteaof
            waitForBgrewriteaof r
            set fd [open [get_base_aof_path r] r]
            fconfigure $fd -translation binary
            set base [read $fd]
            close $fd
            if {$preamble eq "yes"} {
                # The module's RDB form: the compact text as one string.
                assert_match "VALKEY*" $base
                assert {[string first {{"a":[1,2.50,"x"]}} $base] >= 0}
                assert {[string first {"second"} $base] >= 0}
                assert_equal 0 [regexp -all {JSON\.SET} $base]
            } else {
                # One JSON.SET per key at the root path, with the compact text.
                assert {[string first "JSON.SET\r\n\$3\r\none\r\n\$1\r\n.\r\n\$18\r\n{\"a\":\[1,2.50,\"x\"\]}\r\n" $base] >= 0}
                assert {[string first "JSON.SET\r\n\$3\r\ntwo\r\n\$1\r\n.\r\n\$8\r\n\"second\"\r\n" $base] >= 0}
                assert_equal 2 [regexp -all {JSON\.SET} $base]
            }
        }

    test "JSON documents with a TTL survive an AOF rewrite (preamble $preamble)" {
            r flushall
            r json.set ttl . {{"a":1}}
            r expireat ttl 4102444800
            r json.set nottl . {{"a":2}}
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            assert_equal 4102444800 [r expiretime ttl]
            assert_equal -1 [r ttl nottl]
            assert_equal {{"a":1}} [r json.get ttl]
        } {} {needs:debug}

        test "JSON documents survive a restart from the AOF (preamble $preamble)" {
            r flushall
            set docs [concat [json_persist_documents] [json_persist_more_documents]]
            json_write_documents $docs
            r bgrewriteaof
            waitForBgrewriteaof r
            set docs [concat $docs [json_apply_writes]]
            set digest [debug_digest]
            restart_server 0 true false
            wait_done_loading r
            assert_equal 1 [s aof_enabled]
            json_assert_documents $docs
            assert_equal $digest [debug_digest]
            json_assert_num_documents [r dbsize]
            # The AOF directory passes valkey-check-aof.
            set dir [lindex [r config get dir] 1]
            set manifest [file join $dir [lindex [r config get appenddirname] 1] [lindex [r config get appendfilename] 1].manifest]
            assert_match "*is valid*" [exec src/valkey-check-aof $manifest]
        } {} {needs:debug}

        test "Documents deeper than json.max-path-limit survive BGREWRITEAOF and a restart from the AOF (preamble $preamble)" {
            r flushall
            set docs [json_write_deep_documents r]
            assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
            json_assert_documents $docs
            set digest [debug_digest]
            # From the commands as they were appended.
            r debug loadaof
            json_assert_documents $docs
            assert_equal $digest [debug_digest]
            # From the rewritten file.
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            r debug loadaof
            json_assert_documents $docs
            assert_equal $digest [debug_digest]
            # With writes after the rewrite, across a restart. The restarted
            # server has the default limit from its configuration file.
            assert_equal 2 [r json.arrappend deep:array600 {$[0]} {"after"}]
            assert_equal {[2]} [r json.numincrby deep:object1024 "\$[string repeat .a 1024]" 1]
            assert_equal 1 [r json.del deep:array1024 "\$[string repeat {[0]} 1023]"]
            assert_equal 2 [r json.arrappend deep:129 "\$[string repeat {[0]} 128]" 2]
            set after {}
            foreach key {deep:array600 deep:object1024 deep:array1024 deep:129} { lappend after $key [r json.get $key] }
            assert_equal "[string repeat {[} 128]\[1,2\][string repeat {]} 128]" [r json.get deep:129]
            set digest [debug_digest]
            restart_server 0 true false
            wait_done_loading r
            assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
            json_assert_documents $after
            assert_equal $digest [debug_digest]
            assert_equal 129 [r json.debug depth deep:129]
            assert_equal 1024 [r json.debug depth deep:object1024]
            # And from an RDB file.
            r debug reload
            json_assert_documents $after
            assert_equal $digest [debug_digest]
            set dir [lindex [r config get dir] 1]
            set manifest [file join $dir [lindex [r config get appenddirname] 1] [lindex [r config get appendfilename] 1].manifest]
            assert_match "*is valid*" [exec src/valkey-check-aof $manifest]
        } {} {needs:debug}
    }
}

start_server {tags {"json"} overrides {appendonly yes appendfsync always auto-aof-rewrite-percentage 0 save ""}} {
    test {JSON writes that change nothing are not written to the AOF, those with a $ path that matches nothing are} {
        # Design 2.4: what the external module propagates is propagated here.
        r flushall
        r json.set k . {{"a":1,"s":"x"}}
        set before [s aof_current_size]
        catch {r json.numincrby k .zz 1}
        catch {r json.set k $.zz.yy 1}
        catch {r json.numincrby k .s 1}
        assert_equal {} [r json.set k . 1 NX]
        assert_equal 0 [r json.del k .zz]
        assert_equal 0 [r json.del nokey]
        assert_equal {{"a":1,"s":"x"}} [r json.get k]
        assert_equal $before [s aof_current_size]
        assert_equal {[]} [r json.numincrby k $.zz 1]
        assert_morethan [s aof_current_size] $before
        set before [s aof_current_size]
        assert_equal 0 [r json.del k $.zz]
        assert_morethan [s aof_current_size] $before
        r debug loadaof
        assert_equal {{"a":1,"s":"x"}} [r json.get k]
    } {} {needs:debug}

    test {The AOF records JSON commands verbatim} {
        r flushall
        r json.set k . {{"a": 1, "n": 19.90}}
        r json.numincrby k $.a 1.5
        r json.arrappend k $.none 1
        set fd [open [get_last_incr_aof_path r] r]
        fconfigure $fd -translation binary
        set incr [read $fd]
        close $fd
        assert_match "*json.set\r\n\$1\r\nk\r\n\$1\r\n.\r\n\$20\r\n{\"a\": 1, \"n\": 19.90}\r\n*" $incr
        assert_match "*json.numincrby\r\n\$1\r\nk\r\n\$3\r\n\$.a\r\n\$3\r\n1.5\r\n*" $incr
        assert_match "*json.arrappend\r\n\$1\r\nk\r\n\$6\r\n\$.none\r\n\$1\r\n1\r\n*" $incr
    }
}

# -------------------------------------------------------------------------
# A hand-written AOF: what the AOF loader accepts
# -------------------------------------------------------------------------
#
# Commands from the AOF, like commands from a primary, skip the checks that an
# ordinary client gets: invalid UTF-8 (B1), a number too large for a double
# (B2) and a NUL after the JSON text, which the external module accepted and
# may have written to an AOF. They are held to the load bound on nesting,
# max(json.max-path-limit, 1024). A built-in primary cannot produce such a
# stream, so the file is written here byte for byte.

proc json_aof_command {args} {
    set cmd "*[llength $args]\r\n"
    foreach a $args {
        append cmd "\$[string length $a]\r\n$a\r\n"
    }
    return $cmd
}

set json_aof_path [tmpdir server.json-aof]
set json_bad_utf8 [binary format H* fffe]
file mkdir [file join $json_aof_path appendonlydir]
set fd [open [file join $json_aof_path appendonlydir appendonly.aof.1.incr.aof] w]
fconfigure $fd -translation binary
puts -nonewline $fd [json_aof_command SELECT 9]
# Invalid UTF-8, in a value and in a member name, at the root and through sub-path commands.
puts -nonewline $fd [json_aof_command JSON.SET utf8 . "{\"s\":\"$json_bad_utf8\",\"$json_bad_utf8\":1,\"arr\":\[\]}"]
puts -nonewline $fd [json_aof_command JSON.STRAPPEND utf8 .s "\"[binary format H* 80]\""]
puts -nonewline $fd [json_aof_command JSON.ARRAPPEND utf8 .arr "\"[binary format H* c0]\"" 1]
puts -nonewline $fd [json_aof_command JSON.MERGE utf8 . "{\"m\":\"[binary format H* ff]\"}"]
puts -nonewline $fd [json_aof_command JSON.SET utf8 "\$.[binary format H* fe]" 2]
puts -nonewline $fd [json_aof_command JSON.MSET m1 . "\"[binary format H* ff]\"" m2 . {[1.8e308]}]
# Numbers beyond the range of a double.
puts -nonewline $fd [json_aof_command JSON.SET range . {[1.8e308,-1.8e308,1e400,1]}]
puts -nonewline $fd [json_aof_command JSON.ARRAPPEND range . 2e308 -1e999]
puts -nonewline $fd [json_aof_command JSON.NUMINCRBY range {$[3]} 1]
# A NUL after the JSON text: the text ends there.
puts -nonewline $fd [json_aof_command JSON.SET nul . "\[1,2\]\x00garbage"]
puts -nonewline $fd [json_aof_command JSON.SET nul2 . "{\"a\":1}\x00"]
puts -nonewline $fd [json_aof_command JSON.ARRAPPEND nul . "3\x00junk" "\"four\"\x00"]
# Nesting: within the load bound although over json.max-path-limit.
puts -nonewline $fd [json_aof_command JSON.SET deep600 . [json_deep_array 600]]
puts -nonewline $fd [json_aof_command JSON.SET deep1024 . [json_deep_object 1024]]
puts -nonewline $fd [json_aof_command JSON.SET deep129 $ [json_deep_array 128]]
puts -nonewline $fd [json_aof_command JSON.ARRAPPEND deep129 "\$[string repeat {[0]} 127]" {[1]}]
# An ordinary command at the end: the loader got this far.
puts -nonewline $fd [json_aof_command JSON.SET last . {{"loaded":true}}]
puts -nonewline $fd [json_aof_command SET plain value]
close $fd
set fd [open [file join $json_aof_path appendonlydir appendonly.aof.manifest] w]
puts -nonewline $fd "file appendonly.aof.1.incr.aof seq 1 type i\n"
close $fd

start_server [list tags {"json"} keep_persistence true overrides [list dir $json_aof_path appendonly yes appendfilename appendonly.aof \
                  appenddirname appendonlydir auto-aof-rewrite-percentage 0 save ""]] {
    test {The AOF loader accepts JSON text with invalid UTF-8, out-of-range numbers and a NUL after the text - B1, B2} {
        wait_done_loading r
        r select 9
        assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
        set bad $json_bad_utf8
        assert_equal value [r get plain]
        assert_equal {{"loaded":true}} [r json.get last]
        assert_equal "{\"s\":\"$bad[binary format H* 80]\",\"$bad\":1,\"arr\":\[\"[binary format H* c0]\",1\],\"m\":\"[binary format H* ff]\",\"[binary format H* fe]\":2}" [r json.get utf8]
        assert_equal "\"[binary format H* ff]\"" [r json.get m1]
        assert_equal {[1.8e308]} [r json.get m2]
        assert_equal {[1.8e308,-1.8e308,1e400,2,2e308,-1e999]} [r json.get range]
        assert_equal {number number number integer number number} [r json.type range {$[*]}]
        assert_equal {[1,2,3,"four"]} [r json.get nul]
        assert_equal {{"a":1}} [r json.get nul2]
    }

    test {The AOF loader accepts documents nested up to max(json.max-path-limit, 1024) levels} {
        assert_equal [json_deep_array 600] [r json.get deep600]
        assert_equal [json_deep_object 1024] [r json.get deep1024]
        assert_equal "[string repeat {[} 128]\[1\][string repeat {]} 128]" [r json.get deep129]
        assert_equal 11 [r dbsize]
        json_assert_num_documents 10
    }

    test {What the AOF loader accepted is still refused from a client, and arithmetic on an out-of-range number is an error} {
        assert_error "SYNTAXERR*" {r json.set utf8 .s "\"$json_bad_utf8\""}
        assert_error "SYNTAXERR*" {r json.set range {[0]} 1.8e308}
        assert_error "SYNTAXERR*" {r json.set nul . "\[1,2\]\x00garbage"}
        assert_error "LIMIT*" {r json.set deep600 . [json_deep_array 600]}
        assert_error "OVERFLOW*" {r json.numincrby range {$[0]} 1}
        assert_equal {[1.8e308,-1.8e308,1e400,2,2e308,-1e999]} [r json.get range]
    }

    foreach preamble {yes no} {
        test "Documents that only the AOF loader accepts survive a rewrite and another load (preamble $preamble)" {
            set before {}
            foreach key [lsort [r keys *]] {
                if {[r type $key] eq "ReJSON-RL"} { lappend before $key [r json.get $key] }
            }
            set digest [debug_digest]
            r config set aof-use-rdb-preamble $preamble
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            r debug loadaof
            json_assert_documents $before
            assert_equal $digest [debug_digest]
            r debug reload
            json_assert_documents $before
            assert_equal $digest [debug_digest]
        } {} {needs:debug}
    }
}

# A command in the AOF whose document is nested deeper than the bound is an
# error for the loader. The test configuration makes a server panic on such
# an error, so this server is told to ignore it, as a server does by default.
set json_aof_path [tmpdir server.json-aof-deep]
file mkdir [file join $json_aof_path appendonlydir]
set fd [open [file join $json_aof_path appendonlydir appendonly.aof.1.incr.aof] w]
fconfigure $fd -translation binary
puts -nonewline $fd [json_aof_command SELECT 9]
puts -nonewline $fd [json_aof_command JSON.SET before . {{"a":1}}]
puts -nonewline $fd [json_aof_command JSON.SET deep1024 . [json_deep_array 1024]]
puts -nonewline $fd [json_aof_command JSON.SET deep1025 . [json_deep_array 1025]]
puts -nonewline $fd [json_aof_command JSON.SET deepo1025 . [json_deep_object 1025]]
puts -nonewline $fd [json_aof_command JSON.SET grows . [json_deep_array 1024]]
puts -nonewline $fd [json_aof_command JSON.ARRAPPEND grows "\$[string repeat {[0]} 1023]" {[1]}]
puts -nonewline $fd [json_aof_command JSON.SET after . {{"b":2}}]
close $fd
set fd [open [file join $json_aof_path appendonlydir appendonly.aof.manifest] w]
puts -nonewline $fd "file appendonly.aof.1.incr.aof seq 1 type i\n"
close $fd

start_server [list tags {"json"} keep_persistence true overrides [list dir $json_aof_path appendonly yes appendfilename appendonly.aof \
                  appenddirname appendonlydir auto-aof-rewrite-percentage 0 save "" propagation-error-behavior ignore]] {
    test {The AOF loader refuses a document nested deeper than max(json.max-path-limit, 1024) levels and goes on} {
        wait_done_loading r
        r select 9
        assert_equal {{"a":1}} [r json.get before]
        assert_equal {{"b":2}} [r json.get after]
        assert_equal [json_deep_array 1024] [r json.get deep1024]
        assert_equal 0 [r exists deep1025 deepo1025]
        assert_equal 1 [r exists grows]
        assert_equal 4 [r dbsize]
        json_assert_num_documents 4
    }

    test {The AOF loader refuses a sub-path command that would nest a stored document deeper than the bound} {
        # JSON.ARRAPPEND of [1] to the innermost array of a document 1,024 levels
        # deep would make 1,025 levels: a document that RDB load and RESTORE refuse.
        assert_equal 1023 [r json.debug depth grows] "JSON.DEBUG DEPTH of the document after loading"
        assert_equal [json_deep_array 1024] [r json.get grows]
        # What the loader left in memory can be saved and loaded again.
        set digest [debug_digest]
        r debug reload
        assert_equal $digest [debug_digest]
        assert_equal 4 [r dbsize]
    } {} {needs:debug}
}

# -------------------------------------------------------------------------
# Module configs in the configuration file
# -------------------------------------------------------------------------

start_server {tags {"json"} overrides {json.max-document-size 1048576 json.max-path-limit 64 json.max-packed-bytes 0 json.max-packed-entries 0 save ""}} {
    test {The json.* configs can be given in the configuration file} {
        assert_equal {json.max-document-size 1048576 json.max-packed-bytes 0 json.max-packed-entries 0 json.max-path-limit 64} \
            [lsort -stride 2 [r config get json.*]]
        assert_error "LIMIT*" {r json.set k . "[string repeat {[} 65][string repeat {]} 65]"}
        assert_equal OK [r json.set k . "[string repeat {[} 64][string repeat {]} 64]"]
    }

    test {CONFIG REWRITE keeps the json.* configs across a restart} {
        r config set json.max-path-limit 99
        r config set json.max-packed-bytes 123
        r config set json.max-packed-entries 45
        r config set json.max-document-size 2097152
        r config rewrite
        restart_server 0 true false
        assert_equal {json.max-document-size 2097152 json.max-packed-bytes 123 json.max-packed-entries 45 json.max-path-limit 99} \
            [lsort -stride 2 [r config get json.*]]
        assert_equal {json-enabled yes} [r config get json-enabled]
    }
}

}
