# JSON (built-in module): JSON.DEBUG, duplicate member names, the nesting and size limits,
# JSON.MSET, MULTI/EXEC, digest, memory accounting, arity and key extraction.
#
# Ported from the valkey-json module's tst/integration/test_json_basic.py. Expected values are
# the module's, except where VALKEY_JSON_DESIGN.md section 2.3 lists a deliberate change. Those
# tests carry the change number (B7, B8, ...) in their name.

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

# {"a":{"a":...{}...}} with $depth levels of "a".
proc json_misc_nested_objects {depth} {
    return "[string repeat "{\"a\":" $depth]{}[string repeat "}" $depth]"
}

# A JSON string whose text is exactly $size bytes long.
proc json_misc_string_of_size {size} {
    return "\"[string repeat a [expr {$size - 2}]]\""
}

proc json_misc_info_field {field} {
    return [getInfoProperty [r info json_core_metrics] $field]
}

# Waits until the module's statistics show $docs documents and $bytes bytes. A large
# document is freed on the lazy-free thread and the counters move when that free runs,
# so after a delete, an overwrite or a flush they are awaited, not read once.
proc json_misc_wait_stats {docs bytes {what ""}} {
    wait_for_condition 100 50 {
        [json_misc_info_field json_num_documents] == $docs &&
        [json_misc_info_field json_total_memory_bytes] == $bytes
    } else {
        fail "json_num_documents is [json_misc_info_field json_num_documents] (expected $docs) and json_total_memory_bytes is [json_misc_info_field json_total_memory_bytes] (expected $bytes) $what"
    }
}

# Empties the keyspace and waits for every pending lazy free, so that the statistics
# read next are settled.
proc json_misc_flush_and_settle {} {
    r flushall sync
    wait_lazyfree_done r
}

set ::json_misc_wikipedia {
{
  "firstName": "John",
  "lastName": "Smith",
  "age": 27,
  "weight": 135.17,
  "isAlive": true,
  "address": {
    "street": "21 2nd Street",
    "city": "New York",
    "state": "NY",
    "zipcode": "10021-3100"
  },
  "phoneNumbers": [
    {
      "type": "home",
      "number": "212 555-1234"
    },
    {
      "type": "office",
      "number": "646 555-4567"
    }
  ],
  "children": [],
  "spouse": null,
  "groups": {}
}
}

start_server {tags {"json"}} {
    test {JSON.DEBUG HELP lists the subcommands that are kept (B8)} {
        set help [join [r json.debug help] "\n"]
        foreach sub {MEMORY DEPTH FIELDS HELP MAX-DEPTH-KEY MAX-SIZE-KEY} {
            assert_match "*JSON.DEBUG $sub*" $help
        }
        foreach sub {KEYTABLE-CHECK KEYTABLE-CORRUPT KEYTABLE-DISTRIBUTION TEST-SHARED-API} {
            assert {![string match "*$sub*" $help]}
        }
    }

    test {JSON.DEBUG KEYTABLE-*, TEST-SHARED-API are gone (B8)} {
        r json.set k1 . {{"0":0,"1":true,"2":"string","3":[0.0,1.1,2.0],"4":{},"5":{"0":1,"1":"string"}}}
        assert_error "ERR unknown subcommand 'keytable-check'. Try JSON.DEBUG HELP." {r json.debug keytable-check}
        assert_error "ERR unknown subcommand 'KEYTABLE-CORRUPT'. Try JSON.DEBUG HELP." {r json.debug KEYTABLE-CORRUPT k1}
        assert_error "ERR unknown subcommand 'keytable-distribution'. Try JSON.DEBUG HELP." {r json.debug keytable-distribution 3}
        assert_error "ERR unknown subcommand 'test-shared-api'. Try JSON.DEBUG HELP." {r json.debug test-shared-api k1 {$.0}}
        # The module's test_json_shared_api compared these reads with the shared API. The reads
        # themselves are unchanged.
        foreach {path exp} {
            $.0 {[0]}
            $.1 {[true]}
            $.2 {["string"]}
            $.3 {[[0.0,1.1,2.0]]}
            $.4 {[{}]}
            $.5 {[{"0":1,"1":"string"}]}
            $5.0 {[1]}
            $5.1 {["string"]}
        } {
            assert_equal $exp [r json.get k1 $path]
        }
        r del k1
    }

    test {JSON.DEBUG unknown subcommand and wrong arity} {
        r json.set k1 . {{"a":1}}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG' command" {r json.debug}
        assert_error "ERR unknown subcommand 'MEMORY123'. Try JSON.DEBUG HELP." {r json.debug MEMORY123 k1}
        assert_error "ERR unknown subcommand 'M'. Try JSON.DEBUG HELP." {r json.debug M k1}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|MEMORY' command" {r json.debug memory}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|MEMORY' command" {r json.debug memory k1 . extra}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|FIELDS' command" {r json.debug fields}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|FIELDS' command" {r json.debug fields k1 . extra}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|DEPTH' command" {r json.debug depth}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|DEPTH' command" {r json.debug depth k1 extra}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|HELP' command" {r json.debug help extra}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|MAX-DEPTH-KEY' command" {r json.debug max-depth-key extra}
        assert_error "ERR wrong number of arguments for 'JSON.DEBUG|MAX-SIZE-KEY' command" {r json.debug max-size-key extra}
        r del k1
    }

    test {JSON.DEBUG on a key that is not a JSON document} {
        r set str_key {{"firstName":"John","lastName":"Smith"}}
        assert_error "WRONGTYPE*" {r json.debug memory str_key}
        assert_error "WRONGTYPE*" {r json.debug memory str_key .}
        assert_error "WRONGTYPE*" {r json.debug fields str_key}
        assert_error "WRONGTYPE*" {r json.debug depth str_key}
        r del str_key
    }

    test {COMMAND INFO arity of every JSON command} {
        foreach {cmd arity} {
            SET -4 MERGE 4 GET -2 MGET -3 DEL -2 FORGET -2 NUMINCRBY 4 NUMMULTBY 4
            STRLEN -2 STRAPPEND -3 TOGGLE -2 OBJLEN -2 OBJKEYS -2 ARRLEN -2 ARRAPPEND -4
            ARRPOP -2 ARRINSERT -5 ARRTRIM 5 CLEAR -2 ARRINDEX -4 TYPE -2 RESP -2 DEBUG -2
            MSET -4
        } {
            set info [lindex [r command info JSON.$cmd] 0]
            assert_equal "JSON.$cmd" [lindex $info 0]
            assert_equal $arity [lindex $info 1] "arity of JSON.$cmd"
        }
    }

    test {COMMAND INFO JSON.DEBUG subcommands and their arity (B8)} {
        # The module registers ten subcommands. B8 removes KEYTABLE-CHECK, KEYTABLE-CORRUPT,
        # KEYTABLE-DISTRIBUTION and TEST-SHARED-API.
        array set expected {
            JSON.DEBUG|MEMORY -3 JSON.DEBUG|FIELDS -3 JSON.DEBUG|DEPTH 3 JSON.DEBUG|HELP 2
            JSON.DEBUG|MAX-DEPTH-KEY 2 JSON.DEBUG|MAX-SIZE-KEY 2
        }
        set subcommands [lindex [lindex [r command info JSON.DEBUG] 0] 9]
        assert_equal [array size expected] [llength $subcommands]
        foreach sub $subcommands {
            set name [lindex $sub 0]
            assert {[info exists expected($name)]}
            assert_equal $expected($name) [lindex $sub 1] "arity of $name"
        }
    }

    test {COMMAND GETKEYS for JSON.DEBUG subcommands} {
        assert_equal {k1} [r command getkeys json.debug memory k1]
        assert_equal {k1} [r command getkeys json.debug memory k1 .]
        assert_equal {k1} [r command getkeys json.debug fields k1]
        assert_equal {k1} [r command getkeys json.debug depth k1]
        assert_error "ERR Invalid number of arguments specified for command" {r command getkeys json.debug}
        assert_error "ERR Invalid number of arguments specified for command" {r command getkeys json.debug memory}
        assert_error "ERR Invalid command specified" {r command getkeys json.debug mem}
        assert_error "ERR Invalid command specified" {r command getkeys json.debug memofieldsdepthhelp}
        assert_error "ERR The command has no key arguments" {r command getkeys json.debug help}
        assert_error "ERR The command has no key arguments" {r command getkeys json.debug max-depth-key}
        assert_error "ERR The command has no key arguments" {r command getkeys json.debug max-size-key}
        # B8: the removed subcommands are not commands any more.
        assert_error "ERR Invalid command specified" {r command getkeys json.debug keytable-corrupt k1}
        assert_error "ERR Invalid command specified" {r command getkeys json.debug test-shared-api k1 .}
    }

    test {COMMAND GETKEYS for the JSON commands} {
        assert_equal {k1} [r command getkeys json.set k1 . 1]
        assert_equal {k1} [r command getkeys json.get k1 .a .b]
        assert_equal {k1} [r command getkeys json.merge k1 . 1]
        assert_equal {k1 k2 k3} [r command getkeys json.mget k1 k2 k3 .]
        assert_equal {k1 k2} [r command getkeys json.mset k1 . 1 k2 .a 2]
        assert_equal {k1 k1} [r command getkeys json.mset k1 . 1 k1 .a 2]
        foreach cmd {del forget type resp strlen objlen objkeys arrlen clear toggle arrpop} {
            assert_equal {k1} [r command getkeys json.$cmd k1 .]
        }
        assert_equal {k1} [r command getkeys json.numincrby k1 . 1]
        assert_equal {k1} [r command getkeys json.nummultby k1 . 1]
        assert_equal {k1} [r command getkeys json.strappend k1 . {"a"}]
        assert_equal {k1} [r command getkeys json.arrappend k1 . 1 2]
        assert_equal {k1} [r command getkeys json.arrinsert k1 . 0 1]
        assert_equal {k1} [r command getkeys json.arrtrim k1 . 0 1]
        assert_equal {k1} [r command getkeys json.arrindex k1 . 1]
    }

    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall
        r config set json.max-path-limit 128
        r config set json.max-document-size 0

        test "JSON.DEBUG MEMORY errors and missing key (caps $caps)" {
            r json.set wikipedia . $::json_misc_wikipedia
            assert_equal {} [r json.debug memory nonexistentkey]
            assert_equal {} [r json.debug memory nonexistentkey .]
            assert_equal {} [r json.debug memory nonexistentkey {$.a}]
            assert_error "NONEXISTENT JSON path does not exist" {r json.debug memory wikipedia nonexistentpath}
            assert_error "NONEXISTENT JSON path does not exist" {r json.debug memory wikipedia .nonexistentpath}
            assert_equal {} [r json.debug memory wikipedia {$.nonexistentpath}]
        }

        test "JSON.DEBUG MEMORY document size equals the size of the root path (caps $caps)" {
            # The module's test: the per-document tracked size matches the size found by walking
            # the document.
            set tracked [r json.debug memory wikipedia]
            assert_morethan $tracked 0
            assert_equal $tracked [r json.debug memory wikipedia .]
            assert_equal [list $tracked] [r json.debug memory wikipedia {$}]
        }

        test "JSON.DEBUG MEMORY of sub-paths, legacy and \$ (caps $caps)" {
            set whole [r json.debug memory wikipedia]
            set address [r json.debug memory wikipedia .address]
            assert_morethan $address 0
            assert_lessthan $address $whole
            assert_equal [list $address] [r json.debug memory wikipedia {$.address}]
            # One number per match, in match order.
            set sizes [r json.debug memory wikipedia {$.phoneNumbers[*]}]
            assert_equal 2 [llength $sizes]
            foreach size $sizes {
                assert_morethan $size 0
                assert_lessthan $size $whole
            }
            # A legacy path with several matches reports the first one.
            assert_equal [lindex $sizes 0] [r json.debug memory wikipedia {.phoneNumbers[*]}]
        }

        test "JSON.DEBUG MEMORY grows and shrinks with the document (caps $caps)" {
            r json.set mem . {{"a":[]}}
            set small [r json.debug memory mem]
            for {set j 0} {$j < 200} {incr j} {
                r json.arrappend mem .a "\"[string repeat x 100]\""
            }
            set large [r json.debug memory mem]
            assert_morethan $large [expr {$small + 200 * 100}]
            assert_equal $large [r json.debug memory mem .]
            # MEMORY USAGE includes the document.
            assert_morethan [r memory usage mem] [expr {200 * 100}]
            r json.set mem . {{"a":[]}}
            assert_lessthan [r json.debug memory mem] [expr {$small + 1024}]
            assert_equal {{"a":[]}} [r json.get mem]
            r del mem
        }

        test "JSON.DEBUG MEMORY stays bounded when a number flips between integer and double (caps $caps)" {
            # The module's test_doc_mem_size_with_numincrby, with fewer rounds.
            r json.set k1 . {{"a":1955439684,"b":5398,"c":[],"d":{"e":0,"f":2.7233281135559082,"g":1732953600,"h":false,"i":true}}}
            for {set j 0} {$j < 300} {incr j} {
                r json.numincrby k1 .d.e 7.055081799084578998899889890890
                assert_equal OK [r json.set k1 .d.e 0]
                set tracked [r json.debug memory k1]
                assert_lessthan $tracked 1000
                assert_equal $tracked [r json.debug memory k1 .]
            }
            assert_equal {{"a":1955439684,"b":5398,"c":[],"d":{"e":0,"f":2.7233281135559082,"g":1732953600,"h":false,"i":true}}} [r json.get k1]
            r del k1
        }

        test "JSON.DEBUG FIELDS (caps $caps)" {
            assert_equal {} [r json.debug fields nonexistentkey]
            assert_equal {} [r json.debug fields nonexistentkey .]
            r json.set f . {{"x":[1,2,3]}}
            r json.set g . {{"a":{"b":{"c":1}}}}
            assert_equal 4 [r json.debug fields f]
            assert_equal 4 [r json.debug fields f .]
            assert_equal {4} [r json.debug fields f {$}]
            assert_equal 3 [r json.debug fields f .x]
            assert_equal {1 1 1} [r json.debug fields f {$.x[*]}]
            assert_equal 1 [r json.debug fields f {.x[*]}]
            assert_equal 3 [r json.debug fields g]
            assert_equal 2 [r json.debug fields g .a]
            assert_equal {2 1 1} [r json.debug fields g {$..*}]
            assert_equal {} [r json.debug fields f {$.nope}]
            assert_error "NONEXISTENT JSON path does not exist" {r json.debug fields f .nope}
            foreach {doc fields} {
                1 1
                {"a"} 1
                null 1
                true 1
                {{}} 0
                {[]} 0
                {{"a":0}} 1
                {[0,[1,2]]} 4
            } {
                r json.set f . $doc
                assert_equal $fields [r json.debug fields f] "fields of $doc"
            }
            r del f
            r del g
        }

        test "JSON.DEBUG DEPTH (caps $caps)" {
            foreach {key val depth} {
                k1 1 0
                k2 {"a"} 0
                k3 null 0
                k4 true 0
                k5 {{}} 0
                k6 {{"a":0}} 1
                k7 {{"a":{"a":0}}} 2
                k7 {{"a":{"a":{"a":0}}}} 3
                k8 {[]} 0
                k9 {[0]} 1
                k10 {[[0]]} 2
                k11 {[[0],[[0]]]} 3
            } {
                assert_equal OK [r json.set $key {$} $val]
                assert_equal $depth [r json.debug depth $key] "depth of $val"
            }
            # what if the key does not exist?
            assert_equal {} [r json.debug depth foobar]
            # The depth follows sub-path writes in both directions.
            r json.set k6 .a {{"b":{"c":[1]}}}
            assert_equal 4 [r json.debug depth k6]
            r json.del k6 .a.b
            assert_equal {{"a":{}}} [r json.get k6]
            assert_equal 1 [r json.debug depth k6]
            foreach key {k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11} { r del $key }
        }

        test "JSON.DEBUG MAX-DEPTH-KEY and MAX-SIZE-KEY (caps $caps)" {
            r flushall
            assert_equal {0 {}} [r json.debug max-depth-key]
            assert_equal {0 {}} [r json.debug max-size-key]
            r set str_key foo
            r json.set shallow . "\[\"[string repeat x 2000]\"\]"
            r json.set deep . {{"a":{"b":{"c":{"d":1}}}}}
            r json.set scalar . 1
            assert_equal {4 deep} [r json.debug max-depth-key]
            lassign [r json.debug max-size-key] size key
            assert_equal shallow $key
            assert_equal [r json.debug memory shallow] $size
            r del shallow
            assert_equal [list [r json.debug memory deep] deep] [r json.debug max-size-key]
            r flushall
        }

        test "JSON.DEBUG reply shapes with RESP3 (caps $caps)" {
            r json.set f . {{"x":[1,2,3]}}
            r hello 3
            set memory [r json.debug memory f]
            assert {[string is integer -strict $memory]}
            assert_equal 4 [r json.debug fields f]
            assert_equal {1 1 1} [r json.debug fields f {$.x[*]}]
            assert_equal 2 [r json.debug depth f]
            assert_equal {2 f} [r json.debug max-depth-key]
            assert_equal [list $memory f] [r json.debug max-size-key]
            assert_equal {} [r json.debug memory nonexistentkey]
            assert_equal {} [r json.debug depth nonexistentkey]
            assert_equal 7 [llength [r json.debug help]]
            r hello 2
            assert_equal $memory [r json.debug memory f]
            r del f
        }

        test "JSON object with duplicate member names keeps the last value (caps $caps)" {
            r json.set k1 . {{"a":0, "a":1}}
            assert_equal {{"a":1}} [r json.get k1 .]
            assert_equal {a} [r json.objkeys k1]
            assert_equal 1 [r json.get k1 a]
            assert_equal 1 [r json.objlen k1]
            r json.set k1 a 2
            assert_equal {{"a":2}} [r json.get k1 .]
            r json.numincrby k1 a 2
            assert_equal {{"a":4}} [r json.get k1 .]
            r json.nummultby k1 a 2
            assert_equal {{"a":8}} [r json.get k1 .]
            r json.del k1 a
            assert_equal {{}} [r json.get k1 .]
        }

        test "JSON duplicate member names: position of the first, value of the last, at every level (caps $caps)" {
            r json.set k1 . {{"a":0, "b":5, "a":1, "b":{"c":1,"c":2}}}
            assert_equal {{"a":1,"b":{"c":2}}} [r json.get k1 .]
            r json.set k1 . {{"a":0,"b":1,"a":2}}
            assert_equal {{"a":2,"b":1}} [r json.get k1 .]
            assert_equal {a b} [r json.objkeys k1]
            assert_equal 2 [r json.debug fields k1]
            # Many duplicates, enough to leave the packed form under the small caps.
            set members {}
            for {set j 0} {$j < 300} {incr j} { lappend members "\"k[expr {$j % 10}]\":$j" }
            r json.set k1 . "{[join $members ,]}"
            assert_equal 10 [r json.objlen k1]
            assert_equal {{"k0":290,"k1":291,"k2":292,"k3":293,"k4":294,"k5":295,"k6":296,"k7":297,"k8":298,"k9":299}} [r json.get k1]
            # A merge patch with duplicate names behaves the same way.
            r json.set k1 . {{"a":1}}
            r json.merge k1 . {{"b":1,"b":2,"a":null,"a":3}}
            assert_equal {{"a":3,"b":2}} [r json.get k1]
            r del k1
        }

        test "JSON.SET nesting limit json.max-path-limit (caps $caps)" {
            r config set json.max-path-limit 128
            # json not too deep: ok
            assert_equal OK [r json.set k . [json_misc_nested_objects 127]]
            assert_equal 127 [r json.debug depth k]
            # error condition: json is too deep
            assert_error "LIMIT Document path nesting limit is exceeded" {
                r json.set k . [json_misc_nested_objects 200000]
            }
            assert_error "LIMIT Document path nesting limit is exceeded" {
                r json.set k . [json_misc_nested_objects 128]
            }
            assert_error "LIMIT Document path nesting limit is exceeded" {
                r json.set k . [string repeat {[} 200000]
            }
            # The failed writes left the document alone.
            assert_equal 127 [r json.debug depth k]
            assert_equal [json_misc_nested_objects 127] [r json.get k]
            r del k
        }

        test "json.max-document-size: a string below the limit is stored, one above is refused (B7) (caps $caps)" {
            with_config json.max-document-size 0 {
                # The module's test uses 64 MB, 33 MB and 64 MB. Scaled down by 64.
                set MB [expr {2**20}]
                r config set json.max-document-size $MB
                assert_equal OK [r json.set k . [json_misc_string_of_size [expr {$MB / 2}]]]
                assert_error "LIMIT Document size limit is exceeded" {
                    r json.set k . [json_misc_string_of_size [expr {2 * $MB}]]
                }
                assert_equal [expr {$MB / 2}] [string length [r json.get k]]
                # 0 means unlimited.
                r config set json.max-document-size 0
                assert_equal OK [r json.set k . [json_misc_string_of_size [expr {2 * $MB}]]]
                assert_equal [expr {2 * $MB}] [string length [r json.get k]]
                r del k
            }
        }

        test "json.max-document-size is checked on every write that grows a document (B7) (caps $caps)" {
            with_config json.max-document-size 0 {
                set doc {{"a":[1,2,3],"s":"abc","o":{"x":1},"n":1}}
                r config set json.max-document-size 0
                r json.set d{t} . $doc
                # Room for about 2 KB more than the document needs now.
                r config set json.max-document-size [expr {[r json.debug memory d{t}] + 2048}]
                set small {"0123456789"}
                set large "\"[string repeat y 8192]\""

                # Each of these grows the document by a few bytes and is accepted...
                assert_equal OK [r json.set d{t} .s $small]
                assert_equal OK [r json.set d{t} .new $small]
                assert_equal 4 [r json.arrappend d{t} .a $small]
                assert_equal 5 [r json.arrinsert d{t} .a 0 $small]
                assert_equal 20 [r json.strappend d{t} .s $small]
                assert_equal OK [r json.merge d{t} .o "{\"m\":$small}"]
                assert_equal OK [r json.mset d{t} .o.x $small]
                set before [r json.get d{t}]
                assert_equal {{"a":["0123456789",1,2,3,"0123456789"],"s":"01234567890123456789","o":{"x":"0123456789","m":"0123456789"},"n":1,"new":"0123456789"}} $before

                # ...and each of these would grow it by 8 KB and is refused, changing nothing.
                assert_error "LIMIT Document size limit is exceeded" {r json.set d{t} .s $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.set d{t} .new2 $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.set d{t} {$.a[*]} $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.set d{t} . "\[$large\]"}
                assert_error "LIMIT Document size limit is exceeded" {r json.arrappend d{t} .a $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.arrappend d{t} .a 1 2 $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.arrinsert d{t} .a 0 $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.strappend d{t} .s $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.merge d{t} . "{\"m\":$large}"}
                assert_error "LIMIT Document size limit is exceeded" {r json.merge d{t} .o "{\"m\":$large}"}
                assert_error "LIMIT Document size limit is exceeded" {r json.mset d{t} .o.x $large}
                assert_equal $before [r json.get d{t}]

                # Writes that do not grow the document still work at the limit.
                assert_equal 2 [r json.numincrby d{t} .n 1]
                assert_equal 1 [r json.del d{t} .new]
                assert_equal 1 [r json.clear d{t} .o]

                # A new key is held to the limit as well, by JSON.SET, JSON.MERGE and JSON.MSET.
                assert_error "LIMIT Document size limit is exceeded" {r json.set fresh{t} . $large}
                assert_error "LIMIT Document size limit is exceeded" {r json.merge fresh{t} . "{\"m\":$large}"}
                assert_error "LIMIT Document size limit is exceeded" {r json.mset fresh{t} . $large}
                assert_equal 0 [r exists fresh{t}]

                # With the limit lifted every one of them is accepted.
                r config set json.max-document-size 0
                assert_equal OK [r json.set d{t} .s $large]
                assert_equal 6 [r json.arrappend d{t} .a $large]
                assert_equal 7 [r json.arrinsert d{t} .a 0 $large]
                assert_equal 16384 [r json.strappend d{t} .s $large]
                assert_equal OK [r json.merge d{t} .o "{\"m\":$large}"]
                assert_equal OK [r json.mset d{t} .o.x $large fresh{t} . $large]
                assert_equal 8194 [string length [r json.get fresh{t}]]
                assert_morethan [r json.debug memory d{t}] [expr {5 * 8192}]
                r del d{t} fresh{t}
            }
        }

        test "json.max-document-size: JSON.MSET is refused as a whole when one document would be too large (B7) (caps $caps)" {
            with_config json.max-document-size 0 {
                r config set json.max-document-size 0
                r json.set d{t} . {{"a":1}}
                r config set json.max-document-size [expr {[r json.debug memory d{t}] + 2048}]
                set large "\"[string repeat y 8192]\""
                assert_error "LIMIT Document size limit is exceeded" {r json.mset n1{t} . 1 d{t} .a 2 n2{t} . $large n3{t} . 3}
                assert_error "LIMIT Document size limit is exceeded" {r json.mset n1{t} . 1 d{t} .b $large n3{t} . 3}
                assert_equal 0 [r exists n1{t} n2{t} n3{t}]
                assert_equal {{"a":1}} [r json.get d{t}]
                r config set json.max-document-size 0
                r del d{t}
            }
        }

        test "json.max-document-size CONFIG SET and GET round trip (caps $caps)" {
            with_config json.max-document-size 0 {
                assert_equal {json.max-document-size 0} [r config get json.max-document-size]
                r config set json.max-document-size 65536
                assert_equal {json.max-document-size 65536} [r config get json.max-document-size]
                r config set json.max-document-size 64mb
                assert_equal {json.max-document-size 67108864} [r config get json.max-document-size]
                assert_error "ERR CONFIG SET failed*" {r config set json.max-document-size -1}
                assert_error "ERR CONFIG SET failed*" {r config set json.max-document-size abc}
                assert_equal {json.max-document-size 67108864} [r config get json.max-document-size]
                r config set json.max-document-size 0
            }
        }

        test "JSON.MSET supports all data types on a sub-path (caps $caps)" {
            r json.set wikipedia . $::json_misc_wikipedia
            foreach {path value} {
                .address.city {"Boston"}
                .age 30
                .foo {[1,2,3]}
                .phoneNumbers[0].number {"1234567"}
                .foo {{"a":"b"}}
                .lastName null
                .isAlive false
            } {
                assert_equal OK [r json.mset wikipedia $path $value]
                assert_equal $value [r json.get wikipedia $path]
            }
            assert_equal {{"firstName":"John","lastName":null,"age":30,"weight":135.17,"isAlive":false,"address":{"street":"21 2nd Street","city":"Boston","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[{"type":"home","number":"1234567"},{"type":"office","number":"646 555-4567"}],"children":[],"spouse":null,"groups":{},"foo":{"a":"b"}}} [r json.get wikipedia]
        }

        test "JSON.MSET root documents (caps $caps)" {
            assert_equal OK [r json.mset \
                k1{t} . {"Boston"} \
                k2{t} . 123 \
                k3{t} . {["Seattle","Boston"]} \
                k4{t} . {[1,2,3]} \
                k5{t} . {{"a":"b"}} \
                k6{t} . {{}} \
                k7{t} . null \
                k8{t} . false]
            foreach {key value} {
                k1{t} {"Boston"}
                k2{t} 123
                k3{t} {["Seattle","Boston"]}
                k4{t} {[1,2,3]}
                k5{t} {{"a":"b"}}
                k6{t} {{}}
                k7{t} null
                k8{t} false
            } {
                assert_equal $value [r json.get $key]
                assert_equal ReJSON-RL [r type $key]
            }
            # A $ root path works the same way.
            assert_equal OK [r json.mset k1{t} {$} 1 k9{t} {$} {[true]}]
            assert_equal 1 [r json.get k1{t}]
            assert_equal {[true]} [r json.get k9{t}]
            r del k9{t}
        }

        test "JSON.MSET error conditions (caps $caps)" {
            r json.set wikipedia{t} . $::json_misc_wikipedia
            # A new Valkey key's path must be root
            assert_error "SYNTAXERR Command syntax error" {r json.mset foo{t} .bar {"bar"}}
            assert_error "SYNTAXERR Command syntax error" {r json.mset foo{t} {$.bar} {"bar"}}
            assert_equal 0 [r exists foo{t}]
            assert_error "ERR wrong number of arguments for 'JSON.MSET' command" {r json.mset}
            assert_error "ERR wrong number of arguments for 'JSON.MSET' command" {r json.mset k1{t}}
            assert_error "ERR wrong number of arguments for 'JSON.MSET' command" {r json.mset k1{t} .}
            assert_error "ERR wrong number of arguments for 'JSON.MSET' command" {r json.mset k1{t} . 1 k2{t}}
            assert_error "ERR wrong number of arguments for 'JSON.MSET' command" {r json.mset k1{t} . 1 k2{t} .}
            assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.mset k1{t} . 1 k2{t} . nope}
            assert_error "NONEXISTENT*" {r json.mset k1{t} . 1 wikipedia{t} .no.such.parent 1}
            r set str_key{t} foo
            assert_error "WRONGTYPE Not a JSON document key" {r json.mset k1{t} . 1 str_key{t} . 1}
            assert_error "WRONGTYPE Not a JSON document key" {r json.mset str_key{t} .a 1}
            assert_equal foo [r get str_key{t}]
            # None of the failed commands changed k1.
            assert_equal 1 [r json.get k1{t}]
            r del str_key{t}
        }

        test "JSON.MSET mixing root and sub-path writes (caps $caps)" {
            r json.set wikipedia{t} . $::json_misc_wikipedia
            assert_equal OK [r json.mset \
                k1{t} . {"Boston"} \
                wikipedia{t} .address.city {"Boston"} \
                k2{t} . 123 \
                wikipedia{t} .age 30 \
                k3{t} . {["Seattle","Boston"]} \
                k4{t} . {[1,2,3]} \
                wikipedia{t} {.phoneNumbers[0].number} {"1234567"} \
                k5{t} . {{"a":"b"}} \
                wikipedia{t} .foo {{"a":"b"}} \
                k6{t} . {{}} \
                wikipedia{t} .lastName null \
                k7{t} . null \
                wikipedia{t} .isAlive false \
                k8{t} . false]
            foreach {key path value} {
                k1{t} . {"Boston"}
                k2{t} . 123
                k3{t} . {["Seattle","Boston"]}
                k4{t} . {[1,2,3]}
                k5{t} . {{"a":"b"}}
                k6{t} . {{}}
                k7{t} . null
                k8{t} . false
                wikipedia{t} .address.city {"Boston"}
                wikipedia{t} .age 30
                wikipedia{t} .phoneNumbers[0].number {"1234567"}
                wikipedia{t} .foo {{"a":"b"}}
                wikipedia{t} .lastName null
                wikipedia{t} .isAlive false
            } {
                assert_equal $value [r json.get $key $path]
            }
        }

        test "JSON.MSET is all or nothing (caps $caps)" {
            # Continues from the previous test. The triple for the new key foo is invalid, so
            # none of the others may be applied.
            assert_error "SYNTAXERR Command syntax error" {
                r json.mset \
                    k1{t} . {"Seattle"} \
                    wikipedia{t} .address.city {"Seattle"} \
                    k2{t} . 456 \
                    wikipedia{t} .age 40 \
                    k3{t} . {["Seattle","Chicago"]} \
                    k4{t} . {[4,5,6]} \
                    wikipedia{t} {.phoneNumbers[0].number} {"56789"} \
                    k5{t} . {{"c":"d"}} \
                    wikipedia{t} .foo {{"c":"d"}} \
                    k6{t} . {{}} \
                    wikipedia{t} .lastName null \
                    k7{t} . null \
                    foo{t} .bar {"bar"} \
                    k8{t} . true
            }
            foreach {key path value} {
                k1{t} . {"Boston"}
                k2{t} . 123
                k3{t} . {["Seattle","Boston"]}
                k4{t} . {[1,2,3]}
                k5{t} . {{"a":"b"}}
                k6{t} . {{}}
                k7{t} . null
                k8{t} . false
                wikipedia{t} .address.city {"Boston"}
                wikipedia{t} .age 30
                wikipedia{t} .phoneNumbers[0].number {"1234567"}
                wikipedia{t} .foo {{"a":"b"}}
                wikipedia{t} .lastName null
                wikipedia{t} .isAlive false
            } {
                assert_equal $value [r json.get $key $path]
            }
            assert_equal 0 [r exists foo{t}]
            # A failure in the last triple, after new keys earlier in the command.
            assert_error "SYNTAXERR*" {r json.mset new1{t} . 1 new2{t} . 2 k1{t} . {"x"} new3{t} . nope}
            assert_equal 0 [r exists new1{t} new2{t} new3{t}]
            assert_equal {"Boston"} [r json.get k1{t}]
            r del k1{t} k2{t} k3{t} k4{t} k5{t} k6{t} k7{t} k8{t}
        }

        test "JSON.MSET naming one key twice: independent sub-paths (caps $caps)" {
            r json.set m{t} . {{"a":1}}
            assert_equal OK [r json.mset m{t} .a 2 m{t} .b 3]
            assert_equal {{"a":2,"b":3}} [r json.get m{t}]
            r json.set m{t} . {{"a":{"b":1},"c":[1]}}
            assert_equal OK [r json.mset m{t} .a.b 2 m{t} .c {[1,2]} m{t} .a.d 3 m{t} {$.c[0]} 9]
            assert_equal {{"a":{"b":2,"d":3},"c":[9,2]}} [r json.get m{t}]
        }

        test "JSON.MSET naming one key twice: later triples see earlier ones (caps $caps)" {
            # Root written three times: the last value wins.
            assert_equal OK [r json.mset m{t} . 1 m{t} . 2 m{t} . 3]
            assert_equal 3 [r json.get m{t}]
            # A new key written twice.
            assert_equal OK [r json.mset n{t} . {{"a":1}} n{t} . {{"b":2}}]
            assert_equal {{"b":2}} [r json.get n{t}]
            # The parent replaced first, then a member set inside the new parent.
            r json.set m{t} . {{"a":{"b":1},"c":[9,2]}}
            assert_equal OK [r json.mset m{t} .a {{}} m{t} .a.b 7]
            assert_equal {{"a":{"b":7},"c":[9,2]}} [r json.get m{t}]
            # The child first, then its parent replaced.
            r json.set m{t} . {{"a":{"b":1}}}
            assert_equal OK [r json.mset m{t} .a.b 6 m{t} .a 5]
            assert_equal {{"a":5}} [r json.get m{t}]
            # Root replaced, then a member of the new root.
            r json.set m{t} . {{"a":{"b":1}}}
            assert_equal OK [r json.mset m{t} . {{"x":1}} m{t} .x 2]
            assert_equal {{"x":2}} [r json.get m{t}]
            r del n{t}
        }

        test "JSON.MSET naming one key twice: a triple invalidated by an earlier one fails the whole command (caps $caps)" {
            # The module hits an assert on these (design 2.5). Here the command returns the
            # error a standalone JSON.SET would give and changes nothing, on any key.
            r json.set m{t} . {{"a":{"b":1}}}
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.mset m{t} .a 5 m{t} .a.b 6}
            assert_equal {{"a":{"b":1}}} [r json.get m{t}]
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.mset m{t} . {[1,2]} m{t} .a 3}
            assert_equal {{"a":{"b":1}}} [r json.get m{t}]
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.mset other{t} . 1 m{t} . {[1,2]} m{t} .a.b 3 other2{t} . 2}
            assert_equal {{"a":{"b":1}}} [r json.get m{t}]
            assert_equal 0 [r exists other{t} other2{t}]
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.mset m{t} {$.a.b} 1 m{t} .a {[]} m{t} {$.a.b} 2}
            assert_equal {{"a":{"b":1}}} [r json.get m{t}]
        }

        test "JSON.MSET naming one key twice: every triple is first checked against the stored document (caps $caps)" {
            # As in the module, which validates each triple against the keyspace as it was
            # before the command: a path whose parent only exists after an earlier triple is
            # refused, and so is a sub-path on a key the same command creates.
            r json.set m{t} . {{"a":2,"b":3}}
            assert_error "NONEXISTENT JSON path does not exist" {r json.mset m{t} . {{"x":{}}} m{t} .x.y 5}
            assert_equal {{"a":2,"b":3}} [r json.get m{t}]
            assert_error "SYNTAXERR Command syntax error" {r json.mset fresh{t} . {{"a":[]}} fresh{t} .a {[1]}}
            assert_error "SYNTAXERR Command syntax error" {r json.mset fresh{t} .a 1 fresh{t} . {{}}}
            assert_equal 0 [r exists fresh{t}]
            r del m{t}
        }

        test "JSON commands inside MULTI/EXEC (caps $caps)" {
            r json.set wikipedia . $::json_misc_wikipedia
            r multi
            r json.del wikipedia .address.street
            r json.del wikipedia .address.zipcode
            r json.set wikipedia .address.region {"US East"}
            assert_equal {1 1 OK} [r exec]
            assert_equal {{"city":"New York","state":"NY","region":"US East"}} [r json.get wikipedia .address]

            r multi
            r json.arrpop wikipedia .phoneNumbers
            r json.arrpop wikipedia .phoneNumbers
            r json.arrappend wikipedia .phoneNumbers 123
            r json.arrappend wikipedia .phoneNumbers 456
            assert_equal {{{"type":"office","number":"646 555-4567"}} {{"type":"home","number":"212 555-1234"}} 1 2} [r exec]
            assert_equal {[123,456]} [r json.get wikipedia .phoneNumbers]
        }

        test "JSON MULTI/EXEC: a failing command does not undo the others, DISCARD drops all (caps $caps)" {
            r json.set w . {{"a":{"b":1}}}
            r multi
            r json.set w .a.b 5
            r json.set w .zz.yy 5
            r json.numincrby w .a.b 1
            catch {r exec} err
            assert_match "*NONEXISTENT*" $err
            assert_equal {{"a":{"b":6}}} [r json.get w]

            r multi
            r json.set w . 1
            r json.del w
            assert_equal OK [r discard]
            assert_equal {{"a":{"b":6}}} [r json.get w]

            # A command rejected at queue time aborts the transaction.
            r multi
            r json.set w .a.b 7
            assert_error "ERR wrong number of arguments*" {r json.set w}
            assert_error "EXECABORT*" {r exec}
            assert_equal {{"a":{"b":6}}} [r json.get w]
            r del w
        }

        test "DEBUG DIGEST with JSON documents (caps $caps)" {
            r json.set wikipedia . $::json_misc_wikipedia
            set digest [debug_digest]
            assert {$digest ne [string repeat 0 40]}
            # A change to the document changes the digest, and undoing it restores it.
            r json.set wikipedia .age 28
            assert {$digest ne [debug_digest]}
            r json.set wikipedia .age 27
            assert_equal $digest [debug_digest]
            r flushall
            assert_equal [string repeat 0 40] [debug_digest]
        }

        test "json_total_memory_bytes: documents without object keys are released completely (caps $caps)" {
            # The module's test_big_dup (a leak check), with generated documents in place of
            # its data files. These documents intern no member names, so nothing is retained.
            json_misc_flush_and_settle
            set base [json_misc_info_field json_total_memory_bytes]
            assert_equal 0 [json_misc_info_field json_num_documents]
            set numbers {}
            for {set j 0} {$j < 3000} {incr j} { lappend numbers [expr {$j * 0.25}] }
            set docs [list \
                "\[[join $numbers ,]\]" \
                "\"[string repeat s 100000]\"" \
                "[string repeat {[} 100]1[string repeat {]} 100]" \
                "\[[join [lrepeat 2000 {["a",true,null,[1,2.5]]}] ,]\]" \
                1 null {[]} {not json}]
            foreach doc $docs {
                catch {r json.set x . $doc}
                if {[r exists x]} {
                    # A scalar root may need no memory of its own.
                    assert {[json_misc_info_field json_total_memory_bytes] >= $base}
                    if {[string length $doc] > 100} {
                        assert_morethan [json_misc_info_field json_total_memory_bytes] $base
                    }
                    assert_equal 1 [json_misc_info_field json_num_documents]
                }
                catch {r json.del x .}
                json_misc_wait_stats 0 $base "after JSON.DEL"
            }
            # The same through DEL, UNLINK and an overwrite by another type.
            foreach how {del unlink overwrite} {
                r json.set x . [lindex $docs 0]
                switch $how {
                    del { r del x }
                    unlink { r unlink x }
                    overwrite { r set x foo; r del x }
                }
                json_misc_wait_stats 0 $base "after $how"
            }
        }

        test "json_total_memory_bytes returns exactly to its previous value when documents with keys are deleted (caps $caps)" {
            # A member name is counted only while a document refers to it. The commands
            # here use root paths only: a path that names a member is kept in the cache of
            # compiled paths and keeps that name counted (see the next test).
            json_misc_flush_and_settle
            set base [json_misc_info_field json_total_memory_bytes]
            assert_equal 0 [json_misc_info_field json_num_documents]

            # A small document is freed by the command that removes it.
            set small {{"id":1,"name":"small","tags":["a","b"],"nested":{"k":true,"n":null},"score":19.90}}
            foreach how {del jsondel jsondel-v2 forget overwrite-json overwrite-string mset flushall} {
                assert_equal OK [r json.set x . $small]
                assert_morethan [json_misc_info_field json_total_memory_bytes] $base
                assert_equal 1 [json_misc_info_field json_num_documents]
                switch $how {
                    del { r del x }
                    jsondel { r json.del x }
                    jsondel-v2 { r json.del x $ }
                    forget { r json.forget x . }
                    overwrite-json { r json.set x . 1; r del x }
                    overwrite-string { r set x foo; r del x }
                    mset { r json.mset x . {[]}; r del x }
                    flushall { r flushall sync }
                }
                assert_equal 0 [json_misc_info_field json_num_documents]
                assert_equal $base [json_misc_info_field json_total_memory_bytes] "after $how"
            }
            # Two documents with the same names: the names are released with the last one.
            r json.set x . $small
            set one [json_misc_info_field json_total_memory_bytes]
            r json.set y . $small
            assert_morethan [json_misc_info_field json_total_memory_bytes] $one
            r del x
            assert_morethan [json_misc_info_field json_total_memory_bytes] $base
            r del y
            assert_equal $base [json_misc_info_field json_total_memory_bytes]

            # A large document may be freed on the lazy-free thread.
            set records {}
            for {set j 0} {$j < 500} {incr j} {
                lappend records "{\"id\":$j,\"name\":\"user$j\",\"score\":$j.5,\"tags\":\[\"a\",\"b$j\"\],\"active\":[expr {$j % 2 ? "true" : "false"}],\"ref\":null}"
            }
            set members {}
            for {set j 0} {$j < 2000} {incr j} { lappend members "\"member_$j\":{\"v\":$j}" }
            foreach doc [list "\[[join $records ,]\]" "{[join $members ,]}"] {
                foreach how {del unlink jsondel overwrite expire flushall-async del unlink} {
                    r json.set x . $doc
                    assert_equal 1 [json_misc_info_field json_num_documents]
                    assert_morethan [json_misc_info_field json_total_memory_bytes] [expr {$base + 10000}]
                    switch $how {
                        del { r del x }
                        unlink { r unlink x }
                        jsondel { r json.del x }
                        overwrite { r json.set x . 1; r del x }
                        expire { r pexpire x 1 }
                        flushall-async { r flushall async }
                    }
                    wait_for_condition 100 50 {
                        [r exists x] == 0
                    } else {
                        fail "the key is still there after $how"
                    }
                    json_misc_wait_stats 0 $base "after $how"
                    assert_equal 0 [json_misc_info_field json_num_documents]
                    assert_equal $base [json_misc_info_field json_total_memory_bytes] "after $how"
                }
            }
        }

        test "json_total_memory_bytes after writes through paths: exact once the paths are in the path cache (caps $caps)" {
            # A compiled path of up to 128 bytes stays in a cache of 256 entries and holds a
            # reference on each member name it uses, so such a name stays counted after the
            # last document with it is gone. Use every path of this test once first, then
            # the set, write and delete cycle must return exactly to where it started.
            json_misc_flush_and_settle
            set doc {{"pc_arr":[1,2,3],"pc_num":1,"pc_str":"s","pc_obj":{"pc_in":true},"pc_del":0}}
            set cycle {
                r json.set x . $doc
                r json.arrappend x .pc_arr 4 {"five"} {{"pc_in":6}}
                r json.arrinsert x $.pc_arr 0 0
                r json.arrpop x .pc_arr
                r json.arrtrim x .pc_arr 0 2
                r json.numincrby x .pc_num 1.5
                r json.nummultby x $.pc_num 2
                r json.strappend x .pc_str "\"[string repeat y 300]\""
                r json.toggle x .pc_obj.pc_in
                r json.merge x .pc_obj {{"pc_new":[1,2,3],"pc_in":null}}
                r json.set x $.pc_obj.pc_added "\"[string repeat z 5000]\""
                r json.clear x .pc_obj
                r json.del x .pc_del
                r json.get x .pc_arr $.pc_num
                r json.del x
            }
            eval $cycle
            wait_lazyfree_done r
            assert_equal 0 [json_misc_info_field json_num_documents]
            set base [json_misc_info_field json_total_memory_bytes]
            for {set round 0} {$round < 5} {incr round} {
                eval $cycle
                json_misc_wait_stats 0 $base "in round $round"
                assert_equal $base [json_misc_info_field json_total_memory_bytes] "round $round"
            }
            # Sub-path growth and shrinkage is tracked while the document lives.
            r json.set x . {{"pc_arr":[]}}
            set one [json_misc_info_field json_total_memory_bytes]
            for {set j 0} {$j < 300} {incr j} { r json.arrappend x .pc_arr "\"[string repeat v 50]\"" $j }
            assert_morethan [json_misc_info_field json_total_memory_bytes] [expr {$one + 300 * 50}]
            r json.set x .pc_arr {[]}
            assert_lessthan [json_misc_info_field json_total_memory_bytes] [expr {$one + 1024}]
            r del x
            json_misc_wait_stats 0 $base "at the end"
        }

        test "JSON.DEBUG MEMORY of every document equals the size of its root path, via SCAN TYPE (caps $caps)" {
            # The module's test_verify_doc_mem_size, with generated documents.
            r flushall
            r json.set wikipedia . $::json_misc_wikipedia
            r json.set scalar . 3.25
            r json.set str . {"hello"}
            r json.set arr . "\[[join [lrepeat 500 {{"a":1,"b":[true,null]}}] ,]\]"
            r json.set obj . {{"a":{"b":{"c":{"d":[1,2,3]}}}}}
            r set str_key foo
            set count 0
            set cursor 0
            while 1 {
                lassign [r scan $cursor type ReJSON-RL] cursor keys
                foreach key $keys {
                    set tracked [r json.debug memory $key]
                    assert_equal $tracked [r json.debug memory $key .] "memory of $key"
                    assert_morethan $tracked 0
                    incr count
                }
                if {$cursor == 0} break
            }
            assert_equal 5 $count
            r flushall
        }
    }
    json_set_caps {4096 128}
}
