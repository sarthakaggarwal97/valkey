# Tests for the built-in JSON module: the in-memory forms of a document.
#
# A document is stored packed (one allocation) or as a tree whose containers
# may hold packed children, and an array of numbers of one kind is stored as a
# typed array (design section 5). Which form is used depends on the caps
# json.max-packed-bytes and json.max-packed-entries and on the document. No
# command reports the form, so these tests force each form through the caps
# and document sizes, move documents between forms in both directions, and
# compare the document text (JSON.GET key $ and the legacy form) with the
# expected text after every group of changes. JSON.DEBUG MEMORY is used only
# for relations that hold by construction (a typed array of small integers
# takes less than a pointer per element, a tree takes more than a packed node).

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

# Asserts the text of a document with both path syntaxes.
proc json_assert_doc {key text} {
    assert_equal $text [r json.get $key]
    assert_equal "\[$text\]" [r json.get $key $]
}

# "[e0,e1,...]" from a list of element texts.
proc json_array_text {elements} {
    return "\[[join $elements ,]\]"
}

# n elements, element i is the result of the expression `expr` over i.
proc json_gen_elements {n expr} {
    set l {}
    for {set i 0} {$i < $n} {incr i} { lappend l [expr $expr] }
    return $l
}

# n elements, element i is the result of the script `script` over i.
proc json_gen_script {n script} {
    set l {}
    for {set i 0} {$i < $n} {incr i} { lappend l [eval $script] }
    return $l
}

# Text of the model used by the random-operations test: a dict whose values
# are lists of element texts.
proc json_model_text {model} {
    set parts {}
    dict for {name elements} $model {
        lappend parts "\"$name\":[json_array_text $elements]"
    }
    return "{[join $parts ,]}"
}

# A random JSON value as compact text. Integers stay far inside int64.
proc json_random_element {} {
    switch [expr {int(rand() * 12)}] {
        0 { return [expr {int(rand() * 200) - 100}] }
        1 { return [expr {int(rand() * 60000) - 30000}] }
        2 { return [expr {int(rand() * 4000000000) - 2000000000}] }
        3 { return [expr {int(rand() * 9000000000000000) - 4500000000000000}] }
        4 { return [expr {int(rand() * 1000)}].5 }
        5 { return [expr {int(rand() * 1000)}].25 }
        6 { return "\"s[expr {int(rand() * 1000)}]\"" }
        7 { return "\"[string repeat [format %c [expr {97 + int(rand() * 26)}]] [expr {int(rand() * 120)}]]\"" }
        8 { return [lindex {true false null} [expr {int(rand() * 3)}]] }
        9 { return "{\"x\":[expr {int(rand() * 100)}],\"y\":\"v\"}" }
        10 { return "\[[expr {int(rand() * 100)}],\"w\",\[\]\]" }
        11 { return [expr {int(rand() * 100)}] }
    }
}

start_server {tags {"json"}} {
    # ---------------------------------------------------------------------
    # Packed and tree forms
    # ---------------------------------------------------------------------

    test {JSON encodings: the caps decide between the packed and the tree form of a new document} {
        r flushall
        set small {{"a":1,"b":"x","c":[1,2,3],"d":{"e":null}}}
        set mem {}
        foreach caps [json_caps_list] {
            json_set_caps $caps
            r json.set doc . $small
            json_assert_doc doc $small
            lappend mem [r json.debug memory doc]
        }
        lassign $mem packed mixed tree
        # Default caps: one packed node. Caps of 64 bytes and 4 entries: the
        # document has 4 members and fits. Zero caps: every container is a
        # tree node, which takes more memory.
        assert_morethan $tree $packed
        assert {$mixed <= $tree}
        json_set_caps {4096 128}
    }

    test {JSON encodings: CONFIG SET of the caps applies to documents written afterwards, not to stored ones} {
        r flushall
        set text {{"a":1,"b":"x","c":[1,2,3],"d":{"e":null}}}
        json_set_caps {4096 128}
        r json.set packed . $text
        set before [r json.debug memory packed]
        json_set_caps {0 0}
        # Reading does not convert.
        json_assert_doc packed $text
        assert_equal $before [r json.debug memory packed]
        r json.set tree . $text
        assert_morethan [r json.debug memory tree] $before
        # Both are the same document.
        assert_equal [debug_digest_value packed] [debug_digest_value tree]
        json_set_caps {4096 128}
        json_assert_doc tree $text
        # A document written whole under the new caps takes the packed form again.
        r json.set tree . $text
        assert_equal $before [r json.debug memory tree]
    } {} {needs:debug}

    test {JSON encodings: a document over json.max-packed-bytes or json.max-packed-entries is a tree with packed children} {
        r flushall
        json_set_caps {4096 128}
        # 200 members is over the entry cap, 100 members of 100 bytes over the byte cap.
        set many {}
        for {set i 0} {$i < 200} {incr i} { lappend many "\"m$i\":$i" }
        set many "{[join $many ,]}"
        set wide {}
        for {set i 0} {$i < 100} {incr i} { lappend wide "\"w$i\":\"[string repeat x 100]\"" }
        set wide "{[join $wide ,]}"
        set nested {}
        for {set i 0} {$i < 150} {incr i} { lappend nested "{\"id\":$i,\"tags\":\[\"a\",\"b\"\],\"pos\":{\"x\":$i,\"y\":-[expr {$i + 1}]}}" }
        set nested "{\"items\":\[[join $nested ,]\]}"
        foreach {key text} [list many $many wide $wide nested $nested] {
            r json.set $key . $text
            json_assert_doc $key $text
        }
        assert_equal 200 [r json.objlen many]
        assert_equal 199 [r json.get many .m199]
        assert_equal 100 [r json.strlen wide .w99]
        assert_equal 150 [r json.arrlen nested .items]
        assert_equal {{"x":149,"y":-150}} [r json.get nested {.items[149].pos}]
        assert_equal {[149]} [r json.get nested {$.items[?(@.id==149)].pos.x}]
        # Writes into a packed child of a tree parent, and into the tree parent.
        r json.set nested {.items[7].pos.x} {"moved"}
        r json.arrappend nested {.items[7].tags} {"c"} {"d"} {"e"} {"f"}
        r json.del nested {.items[8]}
        r json.set many .m100 {{"now":"an object"}}
        r json.numincrby many .m101 1000000000000
        assert_equal {{"id":7,"tags":["a","b","c","d","e","f"],"pos":{"x":"moved","y":-8}}} [r json.get nested {.items[7]}]
        assert_equal {{"id":9,"tags":["a","b"],"pos":{"x":9,"y":-10}}} [r json.get nested {.items[8]}]
        assert_equal 149 [r json.arrlen nested .items]
        assert_equal {{"now":"an object"}} [r json.get many .m100]
        assert_equal 1000000000101 [r json.get many .m101]
        assert_equal 200 [r json.objlen many]
    }

    # ---------------------------------------------------------------------
    # Typed arrays
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        test "JSON encodings: arrays of numbers of each kind keep their text (caps $caps)" {
            set n 1000
            set arrays [dict create \
                int8      [json_gen_elements $n {($i % 256) - 128}] \
                int16     [json_gen_elements $n {($i * 65) - 32768}] \
                int32     [json_gen_elements $n {($i * 4294967) - 2147483648}] \
                int64     [json_gen_elements $n {($i - 500) * 18446744073709551}] \
                bounds    {-9223372036854775808 9223372036854775807 0 -1 1 127 128 -128 -129 32767 32768 -32768 -32769 2147483647 2147483648 -2147483648 -2147483649} \
                double    [json_gen_script $n {format %d.5 $i}] \
                fraction  [json_gen_script $n {format %.1f [expr {$i / 10.0}]}] \
                fixed2    [json_gen_script $n {format %.2f [expr {$i / 7.0}]}] \
                fixed6    [json_gen_script $n {format %.6f [expr {sin($i)}]}] \
                exponent  [json_gen_script $n {format %de5 $i}] \
                precise   [json_gen_script $n {format %.17g [expr {sqrt($i + 2)}]}] \
                huge      [json_gen_script $n {format 1234567890123456789012345%d $i}] \
                intdouble [json_gen_script $n {format [expr {$i % 2 ? "%d" : "%d.5"}] $i}] \
                mixed     [json_gen_elements $n {$i % 3 == 0 ? "\"s$i\"" : ($i % 3 == 1 ? $i : "null")}] \
                bools     [json_gen_elements $n {$i % 2 ? "true" : "false"}] \
                strings   [json_gen_elements $n {"\"str$i\""}] \
            ]
            dict for {name elements} $arrays {
                set text [json_array_text $elements]
                r json.set $name . $text
                json_assert_doc $name $text
                assert_equal [llength $elements] [r json.arrlen $name]
                # Element access from both ends.
                assert_equal [lindex $elements 0] [r json.get $name {[0]}]
                assert_equal [lindex $elements end] [r json.get $name {[-1]}]
                set mid [expr {[llength $elements] / 2}]
                assert_equal "\[[lindex $elements $mid]\]" [r json.get $name "\$\[$mid\]"]
                # Inside an object too.
                r json.set holder:$name . "{\"before\":1,\"arr\":$text,\"after\":\"x\"}"
                json_assert_doc holder:$name "{\"before\":1,\"arr\":$text,\"after\":\"x\"}"
            }
            # JSON.TYPE of the elements.
            assert_equal integer [r json.type int8 {[5]}]
            assert_equal integer [r json.type int64 {[5]}]
            assert_equal integer [r json.type bounds {[0]}]
            assert_equal integer [r json.type bounds {[1]}]
            assert_equal number [r json.type double {[5]}]
            assert_equal number [r json.type fixed2 {[5]}]
            assert_equal number [r json.type exponent {[5]}]
            assert_equal number [r json.type huge {[5]}]
        }

        test "JSON encodings: an array of small integers is stored typed, narrower than a slot per element (caps $caps)" {
            set m8 [r json.debug memory int8]
            set m16 [r json.debug memory int16]
            set m32 [r json.debug memory int32]
            set m64 [r json.debug memory int64]
            set mstr [r json.debug memory strings]
            # 1000 elements: one, two, four and eight bytes each.
            assert_lessthan $m8 2000
            assert_lessthan $m8 $m16
            assert_lessthan $m16 $m32
            assert_lessthan $m32 $m64
            assert_lessthan $m32 8000
            assert {$m64 <= $mstr}
        }

        test "JSON encodings: reads on typed arrays (caps $caps)" {
            assert_equal 5 [r json.arrindex int8 . -123]
            assert_equal -1 [r json.arrindex int8 . 4000]
            assert_equal 3 [r json.arrindex int16 . [expr {3 * 65 - 32768}]]
            assert_equal 2 [r json.arrindex double . 2.5]
            assert_equal 7 [r json.arrindex fixed2 . 1.00]
            assert_equal 0 [r json.arrindex bounds . -9223372036854775808]
            assert_equal 1 [r json.arrindex bounds . 9223372036854775807]
            assert_equal {[-128,-127,-126]} [r json.get int8 {$[0:3]}]
            assert_equal {[-126,-124]} [r json.get int8 {$[2:6:2]}]
            assert_equal {[127,127,127]} [r json.get int8 {$[?(@>126)]}]
            assert_equal 1001 [llength [r json.resp int8]]
            assert_equal 1001 [llength [r json.resp double]]
            assert_equal [list {[} -128 -127] [lrange [r json.resp int8] 0 2]
            assert_equal [list {[} 0.5 1.5] [lrange [r json.resp double] 0 2]
            assert_equal [lrepeat 1 integer] [r json.type int8 {$[0]}]
            assert_equal 1000 [r json.debug fields int8]
        }

        test "JSON encodings: a typed array widens and becomes a plain array, step by step (caps $caps)" {
            set elements [json_gen_elements 300 {$i % 100}]
            r json.set arr . [json_array_text $elements]
            set narrow [r json.debug memory arr]

            # Still one byte each.
            lappend elements 127 -128
            assert_equal 302 [r json.arrappend arr . 127 -128]
            json_assert_doc arr [json_array_text $elements]

            # Two bytes: by append, then by insert at the front.
            lappend elements 128
            assert_equal 303 [r json.arrappend arr . 128]
            set elements [linsert $elements 0 -32768]
            assert_equal 304 [r json.arrinsert arr . 0 -32768]
            json_assert_doc arr [json_array_text $elements]

            # Four bytes: by overwriting one element.
            lset elements 10 2147483647
            assert_equal OK [r json.set arr {[10]} 2147483647]
            json_assert_doc arr [json_array_text $elements]
            assert_morethan [r json.debug memory arr] $narrow

            # Eight bytes: by arithmetic on one element.
            lset elements 10 2147483648
            assert_equal 2147483648 [r json.numincrby arr {[10]} 1]
            lset elements 11 [expr {[lindex $elements 11] * 1000000000000}]
            assert_equal [lindex $elements 11] [r json.nummultby arr {[11]} 1000000000000]
            lappend elements 9223372036854775807 -9223372036854775808
            assert_equal 306 [r json.arrappend arr . 9223372036854775807 -9223372036854775808]
            json_assert_doc arr [json_array_text $elements]

            # A double among the integers.
            lset elements 20 0.5
            assert_equal OK [r json.set arr {$[20]} 0.5]
            lappend elements 1.25
            r json.arrappend arr . 1.25
            json_assert_doc arr [json_array_text $elements]
            assert_equal number [r json.type arr {[20]}]
            assert_equal integer [r json.type arr {[21]}]

            # A number kept as text, and one beyond int64.
            lappend elements 1e5 19.90 9223372036854775808
            r json.arrappend arr . 1e5 19.90 9223372036854775808
            json_assert_doc arr [json_array_text $elements]

            # Values that are not numbers.
            lappend elements {"str"} null true {{"k":[1]}} {[]}
            assert_equal 315 [r json.arrappend arr . {"str"} null true {{"k":[1]}} {[]}]
            json_assert_doc arr [json_array_text $elements]
            assert_equal {[1]} [r json.get arr {[-2].k}]
        }

        test "JSON encodings: a plain array becomes all numbers again and stays correct (caps $caps)" {
            # Continue from the array of the previous test: drop what is not an integer.
            set elements [json_gen_elements 300 {$i % 100}]
            r json.arrtrim arr . 1 300
            # Positions 9, 10 and 19 (after the trim) hold the wide values and the double.
            foreach i {9 10 19} { r json.set arr "\[$i\]" [lindex $elements $i] }
            json_assert_doc arr [json_array_text $elements]
            # A fresh document with the same text is the same document.
            r json.set fresh . [json_array_text $elements]
            assert_equal [debug_digest_value fresh] [debug_digest_value arr]
            # It keeps working as an integer array.
            assert_equal 301 [r json.arrappend arr . 5]
            assert_equal 5 [r json.arrpop arr]
            assert_equal 0 [r json.arrpop arr . 0]
            assert_equal 99 [r json.arrpop arr . -1]
            assert_equal [lrepeat 3 [expr {1 + 1}]] [list [r json.numincrby arr {[0]} 1] [r json.get arr {[0]}] [r json.get arr {[1]}]]
            assert_equal 298 [r json.arrlen arr]
            assert_equal 3 [r json.arrtrim arr . 0 2]
            json_assert_doc arr {[2,2,3]}
            assert_equal 1 [r json.clear arr]
            json_assert_doc arr {[]}
            assert_equal 3 [r json.arrappend arr . 1 2 3]
            json_assert_doc arr {[1,2,3]}
        } {} {needs:debug}

        test "JSON encodings: every element of a typed array can be popped, inserted and trimmed away (caps $caps)" {
            foreach {name gen} {
                i8  {$i - 50}
                i16 {$i * 300}
                i32 {$i * 70000}
                i64 {$i * 5000000000}
                f64 {"$i.5"}
            } {
                set elements [json_gen_elements 100 $gen]
                r json.set t . [json_array_text $elements]
                # Pop from the back, the front and the middle.
                assert_equal [lindex $elements end] [r json.arrpop t]
                assert_equal [lindex $elements 0] [r json.arrpop t . 0]
                assert_equal [lindex $elements 50] [r json.arrpop t . 49]
                set elements [lreplace [lrange $elements 1 end-1] 49 49]
                json_assert_doc t [json_array_text $elements]
                # Insert in the middle, several values.
                set elements [linsert $elements 40 7 8 9]
                assert_equal 100 [r json.arrinsert t . 40 7 8 9]
                json_assert_doc t [json_array_text $elements]
                # A negative insert index counts from the end: before the element at len-2.
                set elements [linsert $elements [expr {[llength $elements] - 2}] 77]
                assert_equal 101 [r json.arrinsert t . -2 77]
                json_assert_doc t [json_array_text $elements]
                # Trim to a middle range, then to one element, then to nothing.
                set elements [lrange $elements 10 89]
                assert_equal 80 [r json.arrtrim t . 10 89]
                json_assert_doc t [json_array_text $elements]
                assert_equal 1 [r json.arrtrim t . 5 5]
                json_assert_doc t "\[[lindex $elements 5]\]"
                assert_equal 0 [r json.arrtrim t . 1 0]
                json_assert_doc t {[]}
                # Delete elements through a slice and a filter.
                set elements [json_gen_elements 100 $gen]
                r json.set t . [json_array_text $elements]
                assert_equal 10 [r json.del t {$[10:20]}]
                set elements [lreplace $elements 10 19]
                json_assert_doc t [json_array_text $elements]
                assert_equal 45 [r json.del t {$[0:100:2]}]
                set kept {}
                for {set i 1} {$i < [llength $elements]} {incr i 2} { lappend kept [lindex $elements $i] }
                json_assert_doc t [json_array_text $kept]
            }
        }

        test "JSON encodings: arithmetic on every element of a typed array through a wildcard (caps $caps)" {
            set elements [json_gen_elements 200 {$i - 100}]
            r json.set t . [json_array_text $elements]
            # Stays in one byte.
            set elements [lmap e $elements {expr {$e + 1}}]
            assert_equal [json_array_text $elements] [r json.numincrby t {$[*]} 1]
            json_assert_doc t [json_array_text $elements]
            # Grows to two, four and eight bytes.
            foreach factor {100 1000 100000} {
                set elements [lmap e $elements {expr {$e * $factor}}]
                assert_equal [json_array_text $elements] [r json.nummultby t {$[*]} $factor]
                json_assert_doc t [json_array_text $elements]
                assert_equal integer [r json.type t {[0]}]
            }
            # Becomes doubles (B3: a whole double result keeps its fraction).
            r json.numincrby t {$[*]} 0.5
            assert_equal number [r json.type t {[0]}]
            assert_equal 200 [r json.arrlen t]
            assert_equal {[20000000000.5]} [r json.get t {$[101]}]
            assert_equal {[-9999999999.5]} [r json.get t {$[98]}]
            assert_equal {[0.5]} [r json.get t {$[99]}]
            assert_equal {[-989999999999.5]} [r json.get t {$[0]}]
        }
    }
    json_set_caps {4096 128}

    # ---------------------------------------------------------------------
    # Transitions between the packed and the tree form
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON encodings: an object grows past the entry cap and shrinks back (caps $caps)" {
            r flushall
            r json.set o . {{}}
            set parts {}
            for {set i 0} {$i < 300} {incr i} {
                assert_equal OK [r json.set o .m$i $i]
                lappend parts "\"m$i\":$i"
                # Check around the caps of every setting.
                if {$i in {0 3 4 5 127 128 129 299}} {
                    json_assert_doc o "{[join $parts ,]}"
                    assert_equal [expr {$i + 1}] [r json.objlen o]
                }
            }
            # Delete from the front, the back and the middle, down to nothing.
            for {set i 0} {$i < 100} {incr i} { assert_equal 1 [r json.del o .m$i] }
            for {set i 299} {$i >= 200} {incr i -1} { assert_equal 1 [r json.del o "\$.m$i"] }
            set parts [lrange $parts 100 199]
            json_assert_doc o "{[join $parts ,]}"
            for {set i 100} {$i < 196} {incr i} { r json.del o .m$i }
            json_assert_doc o {{"m196":196,"m197":197,"m198":198,"m199":199}}
            assert_equal {m196 m197 m198 m199} [r json.objkeys o]
            # After shrinking it is the same document as one written whole.
            r json.set fresh . {{"m196":196,"m197":197,"m198":198,"m199":199}}
            assert_equal [debug_digest_value fresh] [debug_digest_value o]
            for {set i 196} {$i < 200} {incr i} { r json.del o .m$i }
            json_assert_doc o {{}}
            # And it grows again.
            r json.set o .again {[1,2,3]}
            json_assert_doc o {{"again":[1,2,3]}}
        } {} {needs:debug}

        test "JSON encodings: an array grows past the entry cap and shrinks back (caps $caps)" {
            r flushall
            r json.set a . {[]}
            set elements {}
            for {set i 0} {$i < 300} {incr i} {
                set e [expr {$i % 3 == 0 ? "\"e$i\"" : $i}]
                lappend elements $e
                assert_equal [expr {$i + 1}] [r json.arrappend a . $e]
                if {$i in {0 3 4 5 127 128 129 299}} { json_assert_doc a [json_array_text $elements] }
            }
            while {[llength $elements] > 2} {
                if {[llength $elements] % 2} {
                    assert_equal [lindex $elements end] [r json.arrpop a]
                    set elements [lrange $elements 0 end-1]
                } else {
                    assert_equal [lindex $elements 0] [r json.arrpop a . 0]
                    set elements [lrange $elements 1 end]
                }
                if {[llength $elements] in {200 129 128 127 5 4 3}} { json_assert_doc a [json_array_text $elements] }
            }
            json_assert_doc a [json_array_text $elements]
            r json.arrpop a
            r json.arrpop a
            json_assert_doc a {[]}
            assert_equal {} [r json.arrpop a]
        }

        test "JSON encodings: a string grows past the byte cap and shrinks back (caps $caps)" {
            r flushall
            r json.set s . {{"before":1,"s":"","after":[true]}}
            set s ""
            foreach n {1 6 1 8 40 20 100 4000 100 20000} {
                append s [string repeat [string index "abcdefghij" [expr {$n % 10}]] $n]
                assert_equal [string length $s] [r json.strappend s .s "\"[string repeat [string index "abcdefghij" [expr {$n % 10}]] $n]\""]
                json_assert_doc s "{\"before\":1,\"s\":\"$s\",\"after\":\[true\]}"
            }
            assert_equal [string length $s] [r json.strlen s .s]
            # Replace by shorter and shorter strings.
            foreach n {5000 4096 4095 100 65 64 63 8 7 1 0} {
                set s [string repeat z $n]
                assert_equal OK [r json.set s .s "\"$s\""]
                json_assert_doc s "{\"before\":1,\"s\":\"$s\",\"after\":\[true\]}"
            }
            # A root string.
            r json.set root . {""}
            foreach n {7 1 60 4100} {
                r json.strappend root "\"[string repeat q $n]\""
            }
            assert_equal 4168 [r json.strlen root]
            assert_equal "\"[string repeat q 4168]\"" [r json.get root]
        }

        test "JSON encodings: the root changes between scalar, packed and tree (caps $caps)" {
            r flushall
            set big {}
            for {set i 0} {$i < 400} {incr i} { lappend big "{\"i\":$i,\"s\":\"v$i\"}" }
            set big [json_array_text $big]
            foreach text [list 1 {"s"} {{"a":1}} $big null {[1,2,3]} $big {{"a":{"b":{"c":[1,{"d":2}]}}}} true 1.5 $big {[]} {{}}] {
                assert_equal OK [r json.set root . $text]
                json_assert_doc root $text
                assert_equal OK [r json.set root $ $text]
                json_assert_doc root $text
            }
            # A small child replaced by a big one and back, at several depths.
            r json.set root . {{"a":{"b":{"c":1}},"z":"end"}}
            foreach path {.a.b.c .a.b .a} {
                r json.set root $path $big
                assert_equal $big [r json.get root $path]
                assert_equal {"end"} [r json.get root .z]
                assert_equal 400 [r json.arrlen root $path]
                r json.set root $path 1
                assert_equal 1 [r json.get root $path]
            }
            json_assert_doc root {{"a":1,"z":"end"}}
        }

        test "JSON encodings: a packed child inside a tree parent outgrows the caps and shrinks back (caps $caps)" {
            r flushall
            set items {}
            for {set i 0} {$i < 200} {incr i} { lappend items "{\"id\":$i,\"tags\":\[\]}" }
            r json.set doc . "{\"items\":[json_array_text $items]}"
            # Grow the tags of every tenth item far past any cap.
            for {set i 0} {$i < 200} {incr i 10} {
                for {set j 0} {$j < 150} {incr j} { r json.arrappend doc ".items\[$i\].tags" "\"t$j\"" }
                r json.set doc ".items\[$i\].note" "\"[string repeat n 5000]\""
            }
            assert_equal 150 [r json.arrlen doc {.items[190].tags}]
            assert_equal {"t149"} [r json.get doc {.items[190].tags[-1]}]
            assert_equal {[]} [r json.get doc {.items[191].tags}]
            assert_equal 5000 [r json.strlen doc {.items[0].note}]
            assert_equal [lrepeat 20 150] [r json.arrlen doc {$.items[?(@.note)].tags}]
            # Shrink them back.
            for {set i 0} {$i < 200} {incr i 10} {
                assert_equal 0 [r json.arrtrim doc ".items\[$i\].tags" 1 0]
                assert_equal 1 [r json.del doc ".items\[$i\].note"]
            }
            json_assert_doc doc "{\"items\":[json_array_text $items]}"
            r json.set fresh . "{\"items\":[json_array_text $items]}"
            assert_equal [debug_digest_value fresh] [debug_digest_value doc]
        } {} {needs:debug}

        test "JSON encodings: member names of every length, shared between documents (caps $caps)" {
            r flushall
            set names [list "" a ab [string repeat k 7] [string repeat k 8] [string repeat k 63] [string repeat k 64] \
                           [string repeat k 255] [string repeat k 256] [string repeat k 5000] "sp ace" "quo\\\"te" "uni\\u00e9"]
            set parts {}
            set i 0
            foreach name $names {
                lappend parts "\"$name\":[incr i]"
            }
            set text "{[join $parts ,]}"
            for {set d 0} {$d < 20} {incr d} { r json.set doc:$d . $text }
            # The escape for U+00E9 is stored decoded and printed as UTF-8 (two bytes).
            set printed [string map [list "\\u00e9" [binary format H* c3a9]] $text]
            assert_equal $printed [r json.get doc:0]
            assert_equal [llength $names] [r json.objlen doc:19]
            assert_equal {[1]} [r json.get doc:5 {$[""]}]
            assert_equal {[10]} [r json.get doc:5 "\$\[\"[string repeat k 5000]\"\]"]
            # Deleting some documents and some members leaves the others intact.
            for {set d 0} {$d < 20} {incr d 2} { r del doc:$d }
            r json.del doc:1 "\$\[\"[string repeat k 5000]\"\]"
            r json.del doc:3 {$[""]}
            assert_equal $printed [r json.get doc:5]
            assert_equal [expr {[llength $names] - 1}] [r json.objlen doc:1]
            assert_equal {[]} [r json.get doc:3 {$[""]}]
            assert_equal {[1]} [r json.get doc:7 {$[""]}]
            # The same names in a new document after the others are gone.
            r flushall
            r json.set again . $text
            assert_equal $printed [r json.get again]
        }
    }
    json_set_caps {4096 128}

    # ---------------------------------------------------------------------
    # Random writes checked against a model, with the caps changing
    # ---------------------------------------------------------------------

    foreach start_caps [json_caps_list] {
        test "JSON encodings: random writes with the caps changing between them match a model (start caps $start_caps)" {
            r flushall
            expr {srand(20261114)}
            set all_caps {{4096 128} {64 4} {0 0} {4096 4} {64 128} {16 2} {65535 65535}}
            json_set_caps $start_caps
            set model [dict create]
            for {set i 0} {$i < 6} {incr i} {
                set elements {}
                set n [expr {int(rand() * 12)}]
                for {set j 0} {$j < $n} {incr j} { lappend elements [json_random_element] }
                dict set model k$i $elements
            }
            r json.set doc . [json_model_text $model]
            set fresh_name 0
            set ops 1500
            if {$::accurate} { set ops 20000 }
            for {set op 0} {$op < $ops} {incr op} {
                if {$op % 20 == 19} { json_set_caps [lindex $all_caps [expr {int(rand() * [llength $all_caps])}]] }
                set names [dict keys $model]
                set name [lindex $names [expr {int(rand() * [llength $names])}]]
                set elements [dict get $model $name]
                set len [llength $elements]
                set idx [expr {$len ? int(rand() * $len) : 0}]
                set what "?"
                switch [expr {int(rand() * 13)}] {
                    0 - 1 {
                        set new {}
                        set n [expr {1 + int(rand() * 3)}]
                        for {set j 0} {$j < $n} {incr j} { lappend new [json_random_element] }
                        set what "arrappend .$name $new"
                        lappend elements {*}$new
                        assert_equal [llength $elements] [r json.arrappend doc .$name {*}$new] $what
                    }
                    2 {
                        set new [json_random_element]
                        set what "arrinsert .$name $idx $new"
                        set elements [linsert $elements $idx $new]
                        assert_equal [llength $elements] [r json.arrinsert doc .$name $idx $new] $what
                    }
                    3 {
                        if {$len == 0} continue
                        set what "arrpop .$name $idx"
                        assert_equal [lindex $elements $idx] [r json.arrpop doc .$name $idx] $what
                        set elements [lreplace $elements $idx $idx]
                    }
                    4 {
                        if {$len < 2} continue
                        set stop [expr {$idx + int(rand() * ($len - $idx))}]
                        set what "arrtrim .$name $idx $stop"
                        set elements [lrange $elements $idx $stop]
                        assert_equal [llength $elements] [r json.arrtrim doc .$name $idx $stop] $what
                    }
                    5 - 6 {
                        if {$len == 0} continue
                        set new [json_random_element]
                        set what "set .$name\[$idx\] $new"
                        lset elements $idx $new
                        assert_equal OK [r json.set doc ".$name\[$idx\]" $new] $what
                    }
                    7 {
                        set elements {}
                        set n [expr {int(rand() * 40)}]
                        for {set j 0} {$j < $n} {incr j} { lappend elements [json_random_element] }
                        set what "set .$name (array of $n)"
                        assert_equal OK [r json.set doc "\$.$name" [json_array_text $elements]] $what
                    }
                    8 {
                        if {[llength $names] <= 2} continue
                        set what "del .$name"
                        assert_equal 1 [r json.del doc .$name] $what
                        dict unset model $name
                        set name ""
                    }
                    9 {
                        if {[llength $names] >= 12} continue
                        set name n[incr fresh_name]
                        set elements [list [json_random_element]]
                        set what "set .$name (new member)"
                        assert_equal OK [r json.set doc .$name [json_array_text $elements]] $what
                    }
                    10 - 11 {
                        if {$len == 0 || ![string is entier -strict [lindex $elements $idx]]} continue
                        set delta [expr {int(rand() * 2000000) - 1000000}]
                        set what "numincrby .$name\[$idx\] $delta"
                        lset elements $idx [expr {[lindex $elements $idx] + $delta}]
                        assert_equal [lindex $elements $idx] [r json.numincrby doc ".$name\[$idx\]" $delta] $what
                    }
                    12 {
                        set what "clear .$name"
                        assert_equal [expr {$len > 0}] [r json.clear doc .$name] $what
                        set elements {}
                    }
                }
                if {$name ne ""} { dict set model $name $elements }
                if {$op % 10 == 0 || $op == $ops - 1} {
                    assert_equal [json_model_text $model] [r json.get doc] "after operation $op: $what"
                }
            }
            # The final document equals one written whole, and survives a reload.
            set text [json_model_text $model]
            json_assert_doc doc $text
            r json.set fresh . $text
            assert_equal [debug_digest_value fresh] [debug_digest_value doc]
            r debug reload
            json_assert_doc doc $text
            json_set_caps {4096 128}
        } {OK} {needs:debug}
    }
}

# ---------------------------------------------------------------------------
# Decimals and arrays of short strings
# ---------------------------------------------------------------------------
#
# A number written in plain notation with 1 to 15 fraction digits and at most
# 15 significant digits is a decimal: it is stored as a scaled integer, and an
# array of decimals of one scale is a typed array whose element width follows
# the largest scaled value (design 5.4). An array of strings of at most 15
# bytes that is too big for the packed form stores them inline, 16 bytes each.
# In both cases the text that JSON.GET returns is the text that was written.

# Random array writes on the root array `key`, checked against the list of
# element texts `elements`. `gen` is a script that returns a new element text.
# Returns the final list.
proc json_array_model_ops {key elements gen count} {
    for {set op 0} {$op < $count} {incr op} {
        set len [llength $elements]
        set idx [expr {$len ? int(rand() * $len) : 0}]
        set what "?"
        switch [expr {int(rand() * 20)}] {
            0 - 1 - 2 - 3 - 4 {
                set new {}
                set n [expr {1 + int(rand() * 3)}]
                for {set j 0} {$j < $n} {incr j} { lappend new [uplevel 1 $gen] }
                set what "arrappend $new"
                lappend elements {*}$new
                assert_equal [llength $elements] [r json.arrappend $key . {*}$new] $what
            }
            5 - 6 - 7 {
                set new [uplevel 1 $gen]
                set what "arrinsert $idx $new"
                set elements [linsert $elements $idx $new]
                assert_equal [llength $elements] [r json.arrinsert $key . $idx $new] $what
            }
            8 - 9 - 10 - 11 {
                if {$len == 0} continue
                set what "arrpop $idx"
                assert_equal [lindex $elements $idx] [r json.arrpop $key . $idx] $what
                set elements [lreplace $elements $idx $idx]
            }
            12 - 13 - 14 {
                if {$len == 0} continue
                set what "del \$\[$idx\]"
                assert_equal 1 [r json.del $key "\$\[$idx\]"] $what
                set elements [lreplace $elements $idx $idx]
            }
            15 - 16 - 17 - 18 {
                if {$len == 0} continue
                set new [uplevel 1 $gen]
                set what "set \[$idx\] $new"
                lset elements $idx $new
                assert_equal OK [r json.set $key "\[$idx\]" $new] $what
            }
            19 {
                if {$len < 40 || rand() > 0.2} continue
                set stop [expr {$len - 1 - int(rand() * 20)}]
                set start [expr {int(rand() * 20)}]
                set what "arrtrim $start $stop"
                set elements [lrange $elements $start $stop]
                assert_equal [llength $elements] [r json.arrtrim $key . $start $stop] $what
            }
        }
        if {$op % 10 == 0 || $op == $count - 1} {
            assert_equal [json_array_text $elements] [r json.get $key] "after operation $op: $what"
        }
    }
    return $elements
}

# A JSON string, as text in the form JSON.GET prints it, whose content is
# exactly `bytes` bytes long. Some have characters that need an escape and
# some have non-ASCII characters.
proc json_string_of_bytes {bytes} {
    set pieces [list a 1 b 1 Z 1 7 1 { } 1 - 1 {\"} 1 {\\} 1 {\n} 1 {\t} 1 / 1 \
                    [binary format H* c3a9] 2 [binary format H* e282ac] 3 [binary format H* f09f9880] 4]
    set s ""
    while {$bytes > 0} {
        set i [expr {2 * int(rand() * ([llength $pieces] / 2))}]
        # Plain letters most of the time.
        if {rand() < 0.6} { set i [expr {2 * int(rand() * 4)}] }
        if {[lindex $pieces [expr {$i + 1}]] > $bytes} continue
        append s [lindex $pieces $i]
        incr bytes -[lindex $pieces [expr {$i + 1}]]
    }
    return "\"$s\""
}

# An element for an array of short strings: nearly always a string of 0 to 15
# bytes, now and then a longer one or a value of another type.
proc json_short_string_element {} {
    set r [expr {rand()}]
    if {$r < 0.02} { return [json_string_of_bytes [expr {16 + int(rand() * 30)}]] }
    if {$r < 0.03} { return [lindex {7 null true 1.50 {[]} {{"k":"v"}}} [expr {int(rand() * 6)}]] }
    return [json_string_of_bytes [expr {int(rand() * 16)}]]
}

# An element for an array of decimals with two fraction digits: nearly always
# such a decimal, now and then a number of another scale or kind.
proc json_decimal_element {} {
    set r [expr {rand()}]
    if {$r < 0.02} { return [lindex {0.5 0.125 7 1e2 -0.0 12345678901.234567 {"s"} null 3.000} [expr {int(rand() * 9)}]] }
    switch [expr {int(rand() * 4)}] {
        0 { return [format %.2f [expr {rand() * 2.5 - 1.25}]] }
        1 { return [format %.2f [expr {rand() * 600 - 300}]] }
        2 { return [format %.2f [expr {rand() * 40000000 - 20000000}]] }
        3 { return [format %.2f [expr {rand() * 9000000000000 - 4500000000000}]] }
    }
}

start_server {tags {"json"}} {
    # ---------------------------------------------------------------------
    # Decimals
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall
        set json_decimal_docs {}

        test "JSON decimals of every scale from 1 to 15 keep their text (caps $caps)" {
            for {set k 1} {$k <= 15} {incr k} {
                set digits [string range 123456789123456 0 [expr {$k - 1}]]
                set values [list \
                    0.$digits \
                    -0.$digits \
                    0.[string repeat 0 [expr {$k - 1}]]1 \
                    1.[string repeat 0 $k] \
                    -7.[string repeat 0 [expr {$k - 1}]]5 \
                    0.[string repeat 0 $k] \
                    0.[string repeat 9 $k]]
                # As many integer digits as 15 significant digits leave room for.
                if {$k < 15} { lappend values [string range 987654321987654 0 [expr {14 - $k}]].$digits }
                if {$k < 14} { lappend values -[string range 4000000000000 0 [expr {13 - $k}]].[string repeat 0 [expr {$k - 1}]]1 }
                foreach v $values {
                    assert_equal OK [r json.set scalar . $v]
                    assert_equal $v [r json.get scalar] "scale $k"
                    assert_equal number [r json.type scalar]
                    assert_equal OK [r json.set member . "{\"n\":$v,\"after\":true}"]
                    assert_equal "{\"n\":$v,\"after\":true}" [r json.get member]
                    assert_equal $v [r json.get member .n]
                }
                # All of one scale in a small array and in a big one.
                set small [json_array_text $values]
                set big [json_array_text [concat {*}[lrepeat 40 $values]]]
                r json.set dec:small:$k . $small
                r json.set dec:big:$k . $big
                json_assert_doc dec:small:$k $small
                json_assert_doc dec:big:$k $big
                assert_equal [lrepeat [llength $values] number] [r json.type dec:small:$k {$[*]}]
                assert_equal [lindex $values 2] [r json.get dec:big:$k {[2]}]
                assert_equal [lindex $values end] [r json.get dec:big:$k {[-1]}]
                lappend json_decimal_docs dec:small:$k $small dec:big:$k $big
            }
        }

        test "JSON numbers of 15 and of 16 significant digits keep their text (caps $caps)" {
            # 15 digits is the most a decimal holds. One more and the number is kept another
            # way, with the same guarantee for its text.
            set fifteen {1.23456789012345 12345678.9012345 0.123456789012345 99999999999999.9 -99999999999999.9
                         0.000000000000001 0.999999999999999 123456789.012345 -1234567890123.45 0.000000123456789
                         100000000000000.0 1.00000000000000 0.00000000000000}
            set sixteen {1.234567890123456 123456789012345.6 0.1234567890123456 999999999999999.9 99999999999999.99
                         -99999999999999.99 0.0000000000000001 1234567890123456.7 0.0000000000000000 1.0000000000000000
                         12345678901234567890.5 0.12345678901234567890}
            foreach v [concat $fifteen $sixteen] {
                assert_equal OK [r json.set scalar . $v]
                assert_equal $v [r json.get scalar]
                assert_equal number [r json.type scalar]
            }
            foreach {key values} [list dec:fifteen $fifteen dec:sixteen $sixteen dec:both [concat $fifteen $sixteen]] {
                set text [json_array_text [concat {*}[lrepeat 30 $values]]]
                r json.set $key . $text
                json_assert_doc $key $text
                assert_equal [lrepeat [llength $values] number] [lrange [r json.type $key {$[*]}] 0 [expr {[llength $values] - 1}]]
                lappend json_decimal_docs $key $text
            }
            # Two numbers that differ only in the 16th digit are two texts and one value.
            r json.set k . {[99999999999999.99,99999999999999.98]}
            assert_equal {[99999999999999.99,99999999999999.98]} [r json.get k]
            assert_equal 0 [r json.arrindex k . 99999999999999.98]
            assert_equal {[ 99999999999999.98 99999999999999.98} [string map {\{ {} \} {}} [r json.resp k]]
        }

        test "JSON typed decimal arrays: the element width follows the scaled value (caps $caps)" {
            set n 1000
            # Scaled values within 1, 2, 4 and 8 bytes.
            set arrays [dict create \
                d8  [json_gen_script $n {format %d.%d [expr {$i % 10}] [expr {1 + $i % 9}]}] \
                d8n [json_gen_script $n {format -%d.%d [expr {$i % 10}] [expr {1 + $i % 9}]}] \
                d16 [json_gen_script $n {format %.2f [expr {$i / 100.0}]}] \
                d32 [json_gen_script $n {format %.2f [expr {$i * 1000.01}]}] \
                d64 [json_gen_script $n {format %.6f [expr {$i * 100000.000001}]}] \
                d15 [json_gen_script $n {format 0.%015ld [expr {$i * 999999999999}]}] \
                mixedscale [json_gen_script $n {format %.[expr {1 + $i % 2}]f [expr {$i / 100.0}]}] \
                withint [json_gen_script $n {format [expr {$i % 2 ? "%d" : "%d.5"}] $i}] \
            ]
            dict for {name elements} $arrays {
                set text [json_array_text $elements]
                r json.set dec:$name . $text
                json_assert_doc dec:$name $text
                assert_equal $n [r json.arrlen dec:$name]
                assert_equal [lindex $elements 500] [r json.get dec:$name {[500]}]
                assert_equal number [r json.type dec:$name {[0]}]
                lappend json_decimal_docs dec:$name $text
            }
            set m8 [r json.debug memory dec:d8]
            set m16 [r json.debug memory dec:d16]
            set m32 [r json.debug memory dec:d32]
            set m64 [r json.debug memory dec:d64]
            assert_lessthan $m8 2000
            assert_equal $m8 [r json.debug memory dec:d8n]
            assert_lessthan $m8 $m16
            assert_lessthan $m16 $m32
            assert_lessthan $m32 $m64
            assert_equal $m64 [r json.debug memory dec:d15]
            # Two scales, or an integer among the decimals: not one typed array.
            assert_morethan [r json.debug memory dec:mixedscale] $m16
            assert_morethan [r json.debug memory dec:withint] $m16
            assert_equal integer [r json.type dec:withint {[1]}]
            # Reads.
            assert_equal 250 [r json.arrindex dec:d16 . 2.50]
            assert_equal 250 [r json.arrindex dec:d16 . 2.5]
            assert_equal 250 [r json.arrindex dec:d16 . 25e-1]
            assert_equal -1 [r json.arrindex dec:d16 . 2.505]
            assert_equal {[0.00,0.01,0.02]} [r json.get dec:d16 {$[0:3]}]
            assert_equal {[9.98,9.99]} [r json.get dec:d16 {$[?(@>9.975)]}]
            assert_equal [list {[} 0.0 0.01 0.02] [lrange [r json.resp dec:d16] 0 3]
        }

        test "JSON typed decimal arrays widen from 1 to 2, 4 and 8 bytes per element (caps $caps)" {
            set elements [json_gen_script 300 {format %.2f [expr {($i % 128) / 100.0}]}]
            r json.set arr . [json_array_text $elements]
            set narrow [r json.debug memory arr]
            assert_lessthan $narrow 600

            # Still one byte: the ends of its range.
            lappend elements 1.27 -1.28
            assert_equal 302 [r json.arrappend arr . 1.27 -1.28]
            json_assert_doc arr [json_array_text $elements]
            # Two bytes, by append.
            lappend elements 1.28 -327.68 327.67
            assert_equal 305 [r json.arrappend arr . 1.28 -327.68 327.67]
            json_assert_doc arr [json_array_text $elements]
            # Four bytes, by overwriting one element.
            lset elements 5 327.68
            assert_equal OK [r json.set arr {[5]} 327.68]
            lset elements 6 -21474836.48
            assert_equal OK [r json.set arr {$[6]} -21474836.48]
            json_assert_doc arr [json_array_text $elements]
            # Eight bytes, by insert at the front, up to 15 digits.
            set elements [linsert $elements 0 21474836.48 9999999999999.99 -9999999999999.99]
            assert_equal 308 [r json.arrinsert arr . 0 21474836.48 9999999999999.99 -9999999999999.99]
            json_assert_doc arr [json_array_text $elements]
            assert_morethan [r json.debug memory arr] $narrow
            assert_equal [lrepeat 308 number] [r json.type arr {$[*]}]
            # 16 digits with the same two fraction digits: no longer a decimal.
            lappend elements 99999999999999.99
            assert_equal 309 [r json.arrappend arr . 99999999999999.99]
            json_assert_doc arr [json_array_text $elements]
            # Take the wide ones out again.
            assert_equal 99999999999999.99 [r json.arrpop arr]
            assert_equal 6 [r json.arrtrim arr . 3 8]
            json_assert_doc arr [json_array_text [lrange $elements 3 8]]
            r copy arr dec:widened replace
            lappend json_decimal_docs dec:widened [json_array_text [lrange $elements 3 8]]
        }

        test "JSON decimal array: another scale or kind makes it a plain array and the text stays exact (caps $caps)" {
            set elements [json_gen_script 300 {format %.2f [expr {$i / 7.0}]}]
            set text [json_array_text $elements]
            r json.set fresh . $text
            lappend json_decimal_docs fresh $text
            foreach intruder {0.5 0.125 3.000 7 -7 1e2 1.5e3 -0.0 0.0 12345678901.234567 9223372036854775808
                              {"1.50"} null true {[1.50]} {{"a":1.50}}} {
                r json.set arr . $text
                # At the end, at the front and in the middle.
                assert_equal 301 [r json.arrappend arr . $intruder]
                assert_equal 302 [r json.arrinsert arr . 0 $intruder]
                assert_equal OK [r json.set arr {[100]} $intruder]
                set changed [linsert [lreplace $elements 99 99 $intruder] 0 $intruder]
                lappend changed $intruder
                json_assert_doc arr [json_array_text $changed]
                assert_equal $intruder [r json.get arr {[-1]}]
                assert_equal [lindex $elements 150] [r json.get arr {[151]}]
                # Out again: the same document as before.
                assert_equal $intruder [r json.arrpop arr]
                assert_equal $intruder [r json.arrpop arr . 0]
                assert_equal OK [r json.set arr {[99]} [lindex $elements 99]]
                json_assert_doc arr $text
                assert_equal [debug_digest_value fresh] [debug_digest_value arr]
            }
        } {} {needs:debug}

        test "JSON decimal array: arithmetic stores the canonical text of the result - B3 (caps $caps)" {
            set elements [concat {*}[lrepeat 100 {1.50 2.25 3.00}]]
            r json.set arr . [json_array_text $elements]
            # One element at a time: the others keep their text.
            assert_equal {[1.75]} [r json.numincrby arr {$[0]} 0.25]
            assert_equal {[2.375]} [r json.numincrby arr {$[1]} 0.125]
            assert_equal {[4.0]} [r json.numincrby arr {$[2]} 1]
            assert_equal 3.0 [r json.nummultby arr {[3]} 2]
            assert_equal 0.0 [r json.nummultby arr {[4]} 0]
            assert_equal -3.0 [r json.nummultby arr {[5]} -1]
            set elements [lreplace $elements 0 5 1.75 2.375 4.0 3.0 0.0 -3.0]
            json_assert_doc arr [json_array_text $elements]
            assert_equal [lrepeat 300 number] [r json.type arr {$[*]}]
            # All of them: every element is now a computed double in canonical text. The
            # expected texts are spelled out, not computed by Tcl.
            set doubled {1.75 3.5 2.375 4.75 4.0 8.0 3.0 6.0 0.0 0.0 -3.0 -6.0 1.50 3.0 2.25 4.5 3.00 6.0}
            set elements [lmap e $elements {dict get $doubled $e}]
            assert_equal [json_array_text $elements] [r json.nummultby arr {$[*]} 2]
            json_assert_doc arr [json_array_text $elements]
            set plus {3.5 3.75 4.75 5.0 8.0 8.25 6.0 6.25 0.0 0.25 -6.0 -5.75 3.0 3.25 4.5 4.75}
            set elements [lmap e $elements {dict get $plus $e}]
            assert_equal [json_array_text $elements] [r json.numincrby arr {$[*]} 0.25]
            json_assert_doc arr [json_array_text $elements]
            assert_equal {[3.75,5.0,8.25]} [r json.get arr {$[0:3]}]
            # An integer operand does not make an integer of a decimal (the type is stable).
            r json.set arr . {[1.50,2.50,-0.50]}
            assert_equal {[3.0,5.0,-1.0]} [r json.nummultby arr {$[*]} 2]
            assert_equal {number number number} [r json.type arr {$[*]}]
            assert_equal {[3.0,5.0,-1.0]} [r json.get arr]
            r copy arr dec:computed replace
            lappend json_decimal_docs dec:computed {[3.0,5.0,-1.0]}
            # A decimal scalar and member.
            r json.set k . 19.90
            assert_equal 20.9 [r json.numincrby k . 1]
            assert_equal 20.9 [r json.get k]
            r json.set k . {{"price":19.90,"qty":2}}
            assert_equal 39.8 [r json.nummultby k .price 2]
            assert_equal {{"price":39.8,"qty":2}} [r json.get k]
            r copy k dec:price replace
            lappend json_decimal_docs dec:price {{"price":39.8,"qty":2}}
        }

        test "JSON -0.0 keeps its text and its sign (caps $caps)" {
            foreach v {-0.0 -0.00 -0.000000000000000 0.0 0.00} {
                r json.set k . $v
                assert_equal $v [r json.get k]
                assert_equal number [r json.type k]
            }
            set text {[-0.0,0.0,-0.00,0.000,1.0,-1.0,0.10,-0.10]}
            r json.set negzero . $text
            json_assert_doc negzero $text
            assert_equal [lrepeat 8 number] [r json.type negzero {$[*]}]
            foreach v {0 -0 0.0 -0.0 0e0} { assert_equal 0 [r json.arrindex negzero . $v] }
            assert_equal 4 [r json.arrindex negzero . 1]
            assert_equal 6 [r json.arrindex negzero . 0.1]
            assert_equal [list {[} -0.0 0.0 -0.0 0.0 1.0 -1.0 0.1 -0.1] [r json.resp negzero]
            # In a big array of decimals too.
            set elements [json_gen_script 300 {format %s%d.%d [expr {$i % 3 ? "" : "-"}] [expr {$i % 4}] [expr {$i % 10}]}]
            assert_equal -0.0 [lindex $elements 0]
            r json.set negzero:big . [json_array_text $elements]
            json_assert_doc negzero:big [json_array_text $elements]
            # Arithmetic follows IEEE 754 and prints canonically.
            r json.set k . -0.0
            assert_equal -0.0 [r json.nummultby k . 1]
            assert_equal -0.0 [r json.numincrby k . -0.0]
            assert_equal 0.0 [r json.numincrby k . 0]
            r json.set k . -0.0
            assert_equal 1.5 [r json.numincrby k . 1.5]
            lappend json_decimal_docs negzero $text negzero:big [json_array_text $elements]
        }

        test "JSON decimals: random array writes match a model (caps $caps)" {
            expr {srand(20261115)}
            set elements {}
            for {set i 0} {$i < 300} {incr i} { lappend elements [format %.2f [expr {rand() * 2.5 - 1.25}]] }
            r json.set dec:random . [json_array_text $elements]
            set count 1500
            if {$::accurate} { set count 15000 }
            set elements [json_array_model_ops dec:random $elements json_decimal_element $count]
            json_assert_doc dec:random [json_array_text $elements]
            lappend json_decimal_docs dec:random [json_array_text $elements]
        }

        test "JSON decimals: exact text and JSON.TYPE number after DEBUG RELOAD under every cap setting (saved with caps $caps)" {
            assert_morethan [llength $json_decimal_docs] 80
            set digest [debug_digest]
            foreach load_caps [json_caps_list] {
                json_set_caps $load_caps
                r debug reload
                foreach {key text} $json_decimal_docs {
                    assert_equal $text [r json.get $key] "$key loaded with caps $load_caps"
                }
                assert_equal $digest [debug_digest]
                assert_equal [lrepeat 15 number] [lrange [r json.type dec:fifteen {$[*]}] 0 14]
                assert_equal [lrepeat 12 number] [lrange [r json.type dec:sixteen {$[*]}] 0 11]
                assert_equal number [r json.type dec:d8 {[7]}]
                assert_equal number [r json.type dec:d64 {[7]}]
                assert_equal number [r json.type dec:small:15 {[0]}]
                # A copy and a restored dump are the same document.
                r copy dec:d16 copy:d16 replace
                r del restored:d32
                r restore restored:d32 0 [r dump dec:d32]
                assert_equal [r json.get dec:d16] [r json.get copy:d16]
                assert_equal [r json.get dec:d32] [r json.get restored:d32]
                assert_equal [debug_digest_value dec:d32] [debug_digest_value restored:d32]
                r del copy:d16 restored:d32
            }
            json_set_caps $caps
        } {OK} {needs:debug}
    }
    json_set_caps {4096 128}

    # ---------------------------------------------------------------------
    # Arrays of short strings
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        test "JSON arrays of strings of 0, 7, 8, 15 and 16 bytes keep their text (caps $caps)" {
            set n 1000
            set arrays [dict create \
                len0  [lrepeat $n {""}] \
                len7  [json_gen_script $n {format {"%07d"} $i}] \
                len8  [json_gen_script $n {format {"%08d"} $i}] \
                len15 [json_gen_script $n {format {"%015d"} $i}] \
                len16 [json_gen_script $n {format {"%016d"} $i}] \
                mix15 [json_gen_script $n {format {"%0*d"} [expr {$i % 16}] $i}] \
                mix16 [json_gen_script $n {format {"%0*d"} [expr {$i % 17}] $i}] \
                few15 [json_gen_script 3 {format {"%015d"} $i}] \
            ]
            dict for {name elements} $arrays {
                set text [json_array_text $elements]
                r json.set str:$name . $text
                json_assert_doc str:$name $text
                assert_equal [llength $elements] [r json.arrlen str:$name]
                assert_equal [lindex $elements end] [r json.get str:$name {[-1]}]
                assert_equal string [r json.type str:$name {[1]}]
                assert_equal [expr {[string length [lindex $elements 2]] - 2}] [r json.strlen str:$name {[2]}]
                assert_equal [lsearch -exact $elements [lindex $elements 2]] [r json.arrindex str:$name . [lindex $elements 2]]
                # The same array as an object member.
                r json.set holder . "{\"before\":\"x\",\"arr\":$text,\"after\":1}"
                json_assert_doc holder "{\"before\":\"x\",\"arr\":$text,\"after\":1}"
            }
            # Strings of up to 15 bytes are stored inline, 16 bytes each. One byte more and
            # each needs an allocation of its own.
            set m8 [r json.debug memory str:len8]
            set m15 [r json.debug memory str:len15]
            set m16 [r json.debug memory str:len16]
            assert_equal $m8 $m15
            assert_equal $m15 [r json.debug memory str:mix15]
            assert_lessthan $m15 [expr {$n * 16 + 1024}]
            assert_lessthan $m15 $m16
            assert_lessthan $m15 [r json.debug memory str:mix16]
            # Reads.
            assert_equal {["000000000000007"]} [r json.get str:len15 {$[7]}]
            assert_equal {["000000000000998","000000000000999"]} [r json.get str:len15 {$[-2:]}]
            assert_equal {["000000000000500"]} [r json.get str:len15 {$[?(@=="000000000000500")]}]
            assert_equal 999 [r json.arrindex str:len15 . {"000000000000999"}]
            assert_equal -1 [r json.arrindex str:len15 . {"0000000000009990"}]
            assert_equal -1 [r json.arrindex str:len15 . 999]
            assert_equal [lrepeat 1000 15] [r json.strlen str:len15 {$[*]}]
            assert_equal [list {[} 000000000000000 000000000000001] [lrange [r json.resp str:len15] 0 2]
            assert_equal 1000 [r json.debug fields str:len15]
        }

        test "JSON arrays of short strings with escapes and non-ASCII characters (caps $caps)" {
            set e [binary format H* c3a9]      ;# 2 bytes
            set euro [binary format H* e282ac] ;# 3 bytes
            set grin [binary format H* f09f9880] ;# 4 bytes
            set base [list \
                {""} {"a"} {"\""} {"\\"} {"\n"} {"\t\r\b\f"} {"a\u0000b"} {"\u0001\u001F"} {"/"} {"q\"q\\q"} \
                "\"$e\"" "\"$euro\"" "\"$grin\"" "\"a${e}b${euro}c${grin}d\"" \
                "\"[string repeat $e 7]x\"" "\"[string repeat $euro 5]\"" "\"[string repeat $grin 3]abc\"" \
                {"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\""} {"\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000"} \
                {"0123456789abcde"}]
            set lengths {0 1 1 1 1 4 3 2 1 5 2 3 4 13 15 15 15 15 15 15}
            set elements [concat {*}[lrepeat 20 $base]]
            set text [json_array_text $elements]
            r json.set esc . $text
            json_assert_doc esc $text
            assert_equal [concat {*}[lrepeat 20 $lengths]] [r json.strlen esc {$[*]}]
            for {set i 0} {$i < [llength $base]} {incr i} {
                set element [lindex $base $i]
                assert_equal $i [r json.arrindex esc . $element] "ARRINDEX of element $i"
                assert_equal $element [r json.get esc "\[[expr {380 + $i}]\]"]
            }
            # Escapes written another way are the same strings.
            assert_equal 1 [r json.arrindex esc . {"\u0061"}]
            assert_equal 2 [r json.arrindex esc . {"\u0022"}]
            assert_equal 8 [r json.arrindex esc . {"\/"}]
            assert_equal 10 [r json.arrindex esc . {"\u00e9"}]
            assert_equal 12 [r json.arrindex esc . {"\ud83d\ude00"}]
            # The decoded strings, through JSON.RESP.
            set resp [r json.resp esc]
            assert_equal {} [lindex $resp 1]
            assert_equal "\"" [lindex $resp 3]
            assert_equal "\n" [lindex $resp 5]
            assert_equal "a\x00b" [lindex $resp 7]
            assert_equal "a${e}b${euro}c${grin}d" [lindex $resp 14]
            # One character more than fits (16 bytes), written each way.
            foreach long [list "\"[string repeat $e 8]\"" {"0123456789abcdef"} "\"[string repeat $grin 4]\""] {
                r json.set esc . $text
                assert_equal 401 [r json.arrappend esc . $long]
                assert_equal OK [r json.set esc {[3]} $long]
                json_assert_doc esc [json_array_text [concat [lreplace $elements 3 3 $long] [list $long]]]
                assert_equal 16 [r json.strlen esc {[3]}]
            }
            r json.set esc . $text
        }

        test "JSON array of short strings: every write command, including those that make it a plain array (caps $caps)" {
            set elements [json_gen_script 300 {format {"s%014d"} $i}]
            set text [json_array_text $elements]
            r json.set arr . $text
            r json.set fresh . $text
            set inline [r json.debug memory arr]

            # Commands that keep every element a string of at most 15 bytes.
            lappend elements {"tail"} {""} {"0123456789abcde"}
            assert_equal 303 [r json.arrappend arr . {"tail"} {""} {"0123456789abcde"}]
            set elements [linsert $elements 0 {"head"}]
            assert_equal 304 [r json.arrinsert arr . 0 {"head"}]
            set elements [linsert $elements 150 {"mid1"} {"mid2"}]
            assert_equal 306 [r json.arrinsert arr . 150 {"mid1"} {"mid2"}]
            lset elements 7 {"replaced"}
            assert_equal OK [r json.set arr {[7]} {"replaced"}]
            lset elements 8 {"dollar"}
            assert_equal OK [r json.set arr {$[8]} {"dollar"}]
            lset elements 0 {"header"}
            assert_equal 6 [r json.strappend arr {[0]} {"er"}]
            lset elements 9 {""}
            assert_equal 1 [r json.clear arr {$[9]}]
            lset elements 10 {"merged"}
            assert_equal OK [r json.merge arr {$[10]} {"merged"}]
            lset elements 11 {"mset"}
            assert_equal OK [r json.mset arr {[11]} {"mset"}]
            json_assert_doc arr [json_array_text $elements]
            assert_equal {"0123456789abcde"} [r json.arrpop arr]
            assert_equal {"header"} [r json.arrpop arr . 0]
            assert_equal {"mid1"} [r json.arrpop arr . 149]
            set elements [lreplace [lrange $elements 1 end-1] 149 149]
            assert_equal 1 [r json.del arr {$[149]}]
            assert_equal 1 [r json.forget arr {[-1]}]
            set elements [lrange [lreplace $elements 149 149] 0 end-1]
            json_assert_doc arr [json_array_text $elements]
            assert_equal 10 [r json.del arr {$[20:30]}]
            set elements [lreplace $elements 20 29]
            assert_equal 3 [r json.del arr {$[?(@=="tail"||@=="replaced"||@=="dollar")]}]
            set elements [lsearch -all -inline -not -regexp $elements {^"(tail|replaced|dollar)"$}]
            json_assert_doc arr [json_array_text $elements]
            assert_equal 200 [r json.arrtrim arr . 50 249]
            set elements [lrange $elements 50 249]
            json_assert_doc arr [json_array_text $elements]
            # Commands that do not apply to strings change nothing.
            assert_equal [lrepeat 200 {}] [r json.toggle arr {$[*]}]
            assert_equal "\[[join [lrepeat 200 null] ,]\]" [r json.numincrby arr {$[*]} 1]
            assert_error "WRONGTYPE*" {r json.numincrby arr {[0]} 1}
            assert_error "WRONGTYPE*" {r json.arrappend arr {[0]} 1}
            json_assert_doc arr [json_array_text $elements]

            # Commands that make it a plain array: a longer string, and another type.
            set short $elements
            lset elements 0 "\"[string range [lindex $elements 0] 1 end-1]+\""
            assert_equal 16 [r json.strappend arr {[0]} {"+"}]
            json_assert_doc arr [json_array_text $elements]
            assert_equal OK [r json.set arr {[0]} [lindex $short 0]]
            set elements $short
            foreach intruder [list {"0123456789abcdef"} "\"[string repeat long 100]\"" 7 1.50 null true {[]} {{"k":"v"}} {["nested"]}] {
                lappend elements $intruder
                assert_equal 201 [r json.arrappend arr . $intruder]
                json_assert_doc arr [json_array_text $elements]
                assert_equal $intruder [r json.arrpop arr]
                set elements $short
                set elements [linsert $elements 100 $intruder]
                assert_equal 201 [r json.arrinsert arr . 100 $intruder]
                json_assert_doc arr [json_array_text $elements]
                assert_equal 1 [r json.del arr {$[100]}]
                set elements $short
                lset elements 199 $intruder
                assert_equal OK [r json.set arr {[199]} $intruder]
                json_assert_doc arr [json_array_text $elements]
                assert_equal OK [r json.set arr {[199]} [lindex $short 199]]
                set elements $short
                json_assert_doc arr [json_array_text $elements]
            }
            # All elements at once, through a wildcard.
            assert_equal [lrepeat 200 16] [r json.strappend arr {$[*]} {"!"}]
            set elements [lmap e $elements {string cat [string range $e 0 end-1] "!\""}]
            json_assert_doc arr [json_array_text $elements]
            assert_equal OK [r json.set arr {$[*]} {"same"}]
            json_assert_doc arr [json_array_text [lrepeat 200 {"same"}]]
            assert_equal 200 [r json.clear arr {$[*]}]
            json_assert_doc arr [json_array_text [lrepeat 200 {""}]]
            assert_equal 1 [r json.clear arr]
            json_assert_doc arr {[]}
            # Back to the first document, by one command and element by element.
            r json.set arr . $text
            assert_equal $inline [r json.debug memory arr]
            assert_equal [debug_digest_value fresh] [debug_digest_value arr]
        } {} {needs:debug}

        test "JSON array of short strings: random writes match a model, then DEBUG RELOAD, COPY and the digest of an element-by-element build (caps $caps)" {
            expr {srand(20261116)}
            set elements {}
            for {set i 0} {$i < 300} {incr i} { lappend elements [json_string_of_bytes [expr {int(rand() * 16)}]] }
            r json.set str:random . [json_array_text $elements]
            set count 1500
            if {$::accurate} { set count 15000 }
            set elements [json_array_model_ops str:random $elements json_short_string_element $count]
            set text [json_array_text $elements]
            json_assert_doc str:random $text

            # A document of short strings only, big enough for the inline form.
            set strings {}
            for {set i 0} {$i < 500} {incr i} { lappend strings [json_string_of_bytes [expr {$i % 16}]] }
            set stext [json_array_text $strings]
            r json.set str:whole . $stext
            # The same two documents built element by element.
            foreach {key list} [list str:random $elements str:whole $strings] {
                r json.set built:$key . {[]}
                foreach element $list { r json.arrappend built:$key . $element }
                assert_equal [r json.get $key] [r json.get built:$key]
                assert_equal [debug_digest_value $key] [debug_digest_value built:$key]
                # And built from the back by inserts at the front.
                r json.set front:$key . {[]}
                foreach element [lreverse $list] { r json.arrinsert front:$key . 0 $element }
                assert_equal [debug_digest_value $key] [debug_digest_value front:$key]
            }
            # COPY.
            assert_equal 1 [r copy str:whole str:copy]
            assert_equal 1 [r copy str:random str:copy2]
            json_assert_doc str:copy $stext
            json_assert_doc str:copy2 $text
            assert_equal 501 [r json.arrappend str:copy . {"only the copy"}]
            assert_equal OK [r json.set str:whole {[0]} {"only the source"}]
            assert_equal [lindex $strings 0] [r json.get str:copy {[0]}]
            assert_equal 500 [r json.arrlen str:whole]
            assert_equal OK [r json.set str:whole {[0]} [lindex $strings 0]]
            assert_equal {"only the copy"} [r json.arrpop str:copy]
            assert_equal [debug_digest_value str:whole] [debug_digest_value str:copy]
            # DUMP and RESTORE, and DEBUG RELOAD under every cap setting.
            assert_equal OK [r restore str:restored 0 [r dump str:whole]]
            json_assert_doc str:restored $stext
            set digest [debug_digest]
            foreach load_caps [json_caps_list] {
                json_set_caps $load_caps
                r debug reload
                assert_equal $digest [debug_digest]
                json_assert_doc str:whole $stext
                json_assert_doc str:copy $stext
                json_assert_doc str:restored $stext
                json_assert_doc str:random $text
                json_assert_doc built:str:random $text
                json_assert_doc esc [r json.get esc]
                assert_equal 15 [r json.strlen str:len15 {[999]}]
                assert_equal {"0000000000000999"} [r json.get str:len16 {[999]}]
                # Loaded arrays take writes.
                assert_equal 501 [r json.arrappend str:whole . {"after reload"}]
                assert_equal {"after reload"} [r json.arrpop str:whole]
            }
            json_set_caps $caps
        } {OK} {needs:debug}
    }
    json_set_caps {4096 128}
}
