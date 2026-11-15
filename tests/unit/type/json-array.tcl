# JSON array commands: JSON.ARRLEN, ARRAPPEND, ARRPOP, ARRINSERT, ARRTRIM and ARRINDEX.
#
# Ported from the valkey-json module's tst/integration/test_json_basic.py with the module's
# expected values, except where the built-in module deliberately differs. Those tests carry the
# number of the change in their name:
#   B6   a multi-match write that fails on one match changes nothing
#   2.5  nested matches in one array command (the module reads freed memory there); every match
#        is resolved against the document as it was before the command (design 8.1)

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

# Reads the rest of a reply whose first protocol line is $line, keeping the RESP types:
#   (5) integer, "abc" bulk string, +OK status, -ERR error, nil RESP2 null bulk,
#   nilarray RESP2 null array, null RESP3 null, [a b] array, %{k v} map.
proc json_raw_reply {line} {
    set t [string index $line 0]
    set v [string range $line 1 end]
    switch -exact -- $t {
        _ { return null }
        : { return "($v)" }
        + { return "+$v" }
        - { return "-$v" }
        $ {
            if {$v == -1} { return nil }
            # read the payload from the channel itself: it may hold line breaks or blanks
            set s [read [r channel] [expr {$v + 2}]]
            return "\"[string range $s 0 end-2]\""
        }
        * {
            if {$v == -1} { return nilarray }
            set out {}
            for {set i 0} {$i < $v} {incr i} { lappend out [json_raw_reply [r read]] }
            return "\[[join $out { }]\]"
        }
        % {
            set out {}
            for {set i 0} {$i < 2 * $v} {incr i} { lappend out [json_raw_reply [r read]] }
            return "%{[join $out { }]}"
        }
        default { return "?$line" }
    }
}

# Runs a command and returns its reply in the typed form described above.
proc json_raw {args} {
    r readraw 1
    set res [json_raw_reply [r {*}$args]]
    r readraw 0
    return $res
}

# The module's wikipedia.json sample (tst/integration/data).
set ::json_wikipedia {
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

# Fresh keyspace with the fixtures the module's suite starts every test with.
proc json_array_fixture {} {
    r flushall
    r json.set wikipedia . $::json_wikipedia
    r set str_key {{"firstName":"John","lastName":"Smith"}}
}

# The JSON text of an array whose elements are the JSON texts in $items.
proc json_array_text {items} { return "\[[join $items ,]\]" }

# A new array element for the model test: unique for a given $n in every mode.
proc json_model_value {mode n} {
    switch -exact -- $mode {
        ints { return [expr {$n * 1000003 - 5000}] }
        fixed2 { return [format %.2f [expr {$n / 4.0}]] }
        default {
            switch -exact -- [expr {$n % 9}] {
                0 { return "\"s$n\"" }
                1 { return $n }
                2 { return [format %.2f [expr {$n / 4.0}]] }
                3 { return "1e[expr {$n % 300}]" }
                4 { return "\[$n,\[\"$n\"\]\]" }
                5 { return "{\"k\":$n,\"l\":\[\]}" }
                6 { return true }
                7 { return null }
                default { return "\"\"" }
            }
        }
    }
}

start_server {tags {"json"}} {
    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        # ---------------------------------------------------------------- JSON.ARRLEN

        test "JSON.ARRLEN legacy path (caps $caps)" {
            json_array_fixture
            assert_equal 2 [r json.arrlen wikipedia .phoneNumbers]
            # edge case: empty array
            assert_equal 0 [r json.arrlen wikipedia .children]
            # null if the document key does not exist
            assert_equal nil [json_raw json.arrlen foo .phoneNumbers]
            assert_equal nil [json_raw json.arrlen foo $.phoneNumbers]
            assert_equal nil [json_raw json.arrlen foo]
            # error if the path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.arrlen wikipedia .foo}
            # error condition: the element is not an array
            foreach path {.address .groups .age .weight .isAlive .spouse .firstName .} {
                assert_error "WRONGTYPE JSON element is not an array" {r json.arrlen wikipedia $path}
            }
            assert_error "WRONGTYPE JSON element is not an array" {r json.arrlen wikipedia}
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrlen str_key .a}
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrlen str_key $.a}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.arrlen wikipedia .phoneNumbers extra}
            assert_error "*wrong number of arguments*" {r json.arrlen}
            # a root array, with and without a path
            r json.set arr . {[0, 1, 2, 3, 4]}
            assert_equal 5 [r json.arrlen arr]
            assert_equal 5 [r json.arrlen arr .]
            assert_equal 5 [r json.arrlen arr $]
        }

        test "JSON.ARRLEN \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            assert_equal OK [r json.set k2 . {[[], "a", ["a", "b"], ["a", "b", "c"], 4]}]
            assert_equal {0 1 2 3} [r json.arrlen k1 {$.[*]}]
            assert_equal {0 {} 2 3 {}} [r json.arrlen k2 {$.[*]}]
            assert_equal {[(0) nil (2) (3) nil]} [json_raw json.arrlen k2 {$[*]}]
            assert_equal {[(0) nil (2) (3) nil nil nil nil nil nil]} [json_raw json.arrlen k2 $..*]
            assert_equal {[(5)]} [json_raw json.arrlen k2 $]
            assert_equal {[]} [json_raw json.arrlen k2 $.foo]
            # a legacy path replies with the first array selected
            assert_equal 0 [r json.arrlen k2 {.[*]}]
            assert_equal 2 [r json.arrlen k2 {[2]}]
            assert_error "WRONGTYPE JSON element is not an array" {r json.arrlen k2 {[1]}}
        }

        # ---------------------------------------------------------------- JSON.ARRAPPEND

        test "JSON.ARRAPPEND legacy path (caps $caps)" {
            json_array_fixture
            # edge case: append to an empty array
            assert_equal 1 [r json.arrappend wikipedia .children {"John"}]
            assert_equal {["John"]} [r json.get wikipedia .children]
            # append to a non-empty array
            assert_equal 3 [r json.arrappend wikipedia .children {"Mary"} {"Tom"}]
            assert_equal {["John","Mary","Tom"]} [r json.get wikipedia .children]
            assert_equal 3 [r json.arrlen wikipedia .children]

            # error if the document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.arrappend foo .children {"Mary"}}
            assert_error "NONEXISTENT Document key does not exist" {r json.arrappend foo $.children {"Mary"}}
            assert_equal 0 [r exists foo]
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.arrappend wikipedia .foo {"abc"}}
            # error condition: the element is not an array
            foreach path {.address .groups .age .weight .isAlive .spouse .firstName .} {
                assert_error "WRONGTYPE JSON element is not an array" {r json.arrappend wikipedia $path 123}
            }
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrappend str_key .children 1}
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrappend str_key $.children 1}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.arrappend wikipedia .children}
            assert_error "*wrong number of arguments*" {r json.arrappend wikipedia}
            # a value that is not JSON: nothing is appended, not even the valid values before it
            assert_error "SYNTAXERR*" {r json.arrappend wikipedia .children abc}
            assert_error "SYNTAXERR*" {r json.arrappend wikipedia .children 1 {"x"} abc}
            assert_error "SYNTAXERR*" {r json.arrappend wikipedia $.children 1 {[1,}}
            assert_equal {["John","Mary","Tom"]} [r json.get wikipedia .children]

            # values of every kind
            assert_equal 11 [r json.arrappend wikipedia .children 1 -2.50 1e5 true false null {{"x":[1,2]}} {[null, [] ]}]
            assert_equal {["John","Mary","Tom",1,-2.50,1e5,true,false,null,{"x":[1,2]},[null,[]]]} [r json.get wikipedia .children]
            assert_equal 3 [r json.arrappend wikipedia .phoneNumbers {{"type":"cell","number":"1"}}]
            assert_equal {{"type":"cell","number":"1"}} [r json.get wikipedia {.phoneNumbers[2]}]
        }

        test "JSON.ARRAPPEND \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            assert_equal OK [r json.set k2 . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            foreach {key path val exp new_val} {
                k1 $.[*] {"a"} {1 2 3 4} {[["a"],["a","a"],["a","b","a"],["a","b","c","a"]]}
                k2 $.[*] {""}  {1 2 3 4} {[[""],["a",""],["a","b",""],["a","b","c",""]]}
            } {
                assert_equal $exp [r json.arrappend $key $path $val]
                assert_equal $new_val [r json.get $key $path]
            }

            # an element that is not an array replies null, and no match replies an empty array
            r json.set k3 . {[[], "a", ["a", "b"], ["a", "b", "c"], 4]}
            assert_equal {[(1) nil (3) (4) nil]} [json_raw json.arrappend k3 {$.[*]} {"z"}]
            assert_equal {[]} [json_raw json.arrappend k3 $.foo 1]
            assert_equal {[nil]} [json_raw json.arrappend k3 {$[1]} 1]
            # a legacy wildcard appends to every array and replies with the first new length
            assert_equal {(3)} [json_raw json.arrappend k3 {.[*]} 1 2]
            assert_equal {[["z",1,2],"a",["a","b","z",1,2],["a","b","c","z",1,2],4]} [r json.get k3]
            assert_equal {(9)} [json_raw json.arrappend k3 . {{"x":[1,2]}} {[null]} true 1.50]
            assert_equal {[(10)]} [json_raw json.arrappend k3 $ {"last"}]
            assert_equal {[["z",1,2],"a",["a","b","z",1,2],["a","b","c","z",1,2],4,{"x":[1,2]},[null],true,1.50,"last"]} [r json.get k3]
        }

        # ---------------------------------------------------------------- JSON.ARRPOP

        test "JSON.ARRPOP legacy path (caps $caps)" {
            json_array_fixture
            # edge case: pop an empty array
            assert_equal nil [json_raw json.arrpop wikipedia .children]
            # populate the array
            assert_equal 3 [r json.arrappend wikipedia .children {"John"} {"Mary"} {"Tom"}]
            foreach {idx popped_out new_len new_val} {
                1  {"Mary"} 2 {["John","Tom"]}
                -1 {"Tom"}  1 {["John"]}
                0  {"John"} 0 {[]}
            } {
                assert_equal $popped_out [r json.arrpop wikipedia .children $idx]
                assert_equal $new_len [r json.arrlen wikipedia .children]
                assert_equal $new_val [r json.get wikipedia .children]
            }
            # edge case: pop an empty array
            assert_equal nil [json_raw json.arrpop wikipedia .children]
            assert_equal nil [json_raw json.arrpop wikipedia .children 0]

            # error if the document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.arrpop foo .children}
            assert_error "NONEXISTENT Document key does not exist" {r json.arrpop foo $.children}
            assert_error "NONEXISTENT Document key does not exist" {r json.arrpop foo}
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.arrpop wikipedia .foo}
            # error condition: the element is not an array
            foreach path {.address .groups .age .weight .isAlive .spouse .firstName .} {
                assert_error "WRONGTYPE JSON element is not an array" {r json.arrpop wikipedia $path}
            }
            assert_error "WRONGTYPE JSON element is not an array" {r json.arrpop wikipedia}
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrpop str_key}
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrpop str_key $.a 0}
            # large index: larger than int32
            assert_equal nil [json_raw json.arrpop wikipedia .children 3000000000]
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.arrpop wikipedia .phoneNumbers 0 extra}
            assert_error "*wrong number of arguments*" {r json.arrpop}
            # the index is not an integer
            foreach idx {abc 1.5 99999999999999999999 {}} {
                assert_error "WRONGTYPE Value is not an integer" {r json.arrpop wikipedia .phoneNumbers $idx}
            }
            assert_equal 2 [r json.arrlen wikipedia .phoneNumbers]
            # popping a container replies with its JSON text
            assert_equal {{"type":"home","number":"212 555-1234"}} [r json.arrpop wikipedia .phoneNumbers 0]
            assert_equal {[{"type":"office","number":"646 555-4567"}]} [r json.get wikipedia .phoneNumbers]
        }

        test "JSON.ARRPOP index rules (caps $caps)" {
            # The default index is -1. A negative index counts from the end and an index out of
            # range is rounded to the nearest end.
            r flushall
            r json.set p . {[0,1,2,3,4,5,6,7,8,9]}
            foreach {idx popped} {2 2  -2 8  20 9  -20 0  0 1  -1 7  9223372036854775807 6  -9223372036854775808 3} {
                assert_equal $popped [r json.arrpop p . $idx] "index $idx"
            }
            assert_equal {[4,5]} [r json.get p]
            assert_equal 5 [r json.arrpop p]
            assert_equal {[4]} [r json.get p]
            assert_equal {["4"]} [json_raw json.arrpop p $ 5]
            assert_equal {[]} [r json.get p]
            assert_equal {[nil]} [json_raw json.arrpop p $]
            assert_equal {nil} [json_raw json.arrpop p . 9223372036854775807]
            assert_equal {nil} [json_raw json.arrpop p . -9223372036854775808]
        }

        test "JSON.ARRPOP \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1{t} . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            r copy k1{t} k2{t}
            r copy k1{t} k3{t}
            foreach {key path index exp exp_new_val} {
                k1{t} $.[*] 0  {{} {"a"} {"a"} {"a"}} {[[],[],["b"],["b","c"]]}
                k2{t} $.[*] 1  {{} {"a"} {"b"} {"b"}} {[[],[],["a"],["a","c"]]}
                k3{t} $.[*] -1 {{} {"a"} {"b"} {"c"}} {[[],[],["a"],["a","b"]]}
            } {
                assert_equal $exp [r json.arrpop $key $path $index]
                assert_equal $exp_new_val [r json.get $key .]
            }
            # an empty array and a value that is not an array both reply null
            r json.set k4 . {[["z",1,2],"a",["a","b","z",1,2],4,{"x":[1,2]},[null],true,1.50,[]]}
            assert_equal {["2" nil "2" nil nil "null" nil nil nil]} [json_raw json.arrpop k4 {$[*]}]
            assert_equal {[]} [json_raw json.arrpop k4 $.foo]
            # a legacy wildcard pops from every array and replies with the first value popped
            assert_equal {"1"} [json_raw json.arrpop k4 {.[*]}]
            assert_equal {[["z"],"a",["a","b","z"],4,{"x":[1,2]},[],true,1.50,[]]} [r json.get k4]
            assert_equal {"[]"} [json_raw json.arrpop k4]
            assert_equal {["1.50"]} [json_raw json.arrpop k4 $ -1]
            assert_equal {[["z"],"a",["a","b","z"],4,{"x":[1,2]},[],true]} [r json.get k4]
        }

        # ---------------------------------------------------------------- JSON.ARRINSERT

        test "JSON.ARRINSERT legacy path (caps $caps)" {
            json_array_fixture
            # edge case: insert into an empty array
            assert_equal 1 [r json.arrinsert wikipedia .children 0 {"foo"}]
            assert_equal {["foo"]} [r json.get wikipedia .children]
            assert_equal {"foo"} [r json.arrpop wikipedia .children]
            # populate the array
            assert_equal 3 [r json.arrappend wikipedia .children {"John"} {"Mary"} {"Tom"}]
            assert_equal {["John","Mary","Tom"]} [r json.get wikipedia .children]
            foreach {idx val new_len new_val} {
                0  {"Kathy"} 4 {["Kathy","John","Mary","Tom"]}
                2  {"Rose"}  5 {["Kathy","John","Rose","Mary","Tom"]}
                3  {"Bob"}   6 {["Kathy","John","Rose","Bob","Mary","Tom"]}
                -1 {"Peter"} 7 {["Kathy","John","Rose","Bob","Mary","Peter","Tom"]}
                -1 {"Jane"}  8 {["Kathy","John","Rose","Bob","Mary","Peter","Jane","Tom"]}
                8  {"Grace"} 9 {["Kathy","John","Rose","Bob","Mary","Peter","Jane","Tom","Grace"]}
            } {
                assert_equal $new_len [r json.arrinsert wikipedia .children $idx $val]
                assert_equal $new_val [r json.get wikipedia .children]
            }

            # error if the document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.arrinsert foo .children 0 {"abc"}}
            assert_error "NONEXISTENT Document key does not exist" {r json.arrinsert foo $.children 0 {"abc"}}
            assert_equal 0 [r exists foo]
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.arrinsert wikipedia .foo 0 123}
            # error condition: the element is not an array
            foreach path {.address .groups .age .weight .isAlive .spouse .firstName .} {
                assert_error "WRONGTYPE JSON element is not an array" {r json.arrinsert wikipedia $path 0 1}
            }
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrinsert str_key .children 0 1}
            # error condition: index arg is out of array boundaries
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert wikipedia .children 3000000000 {"a"}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert wikipedia .children 31 {"a"}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert wikipedia $.children 31 {"a"}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert wikipedia .children 10 {"a"}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert wikipedia .children -10 {"a"}}
            # wrong number of arguments, bad index, bad value
            assert_error "*wrong number of arguments*" {r json.arrinsert wikipedia .children 0}
            assert_error "*wrong number of arguments*" {r json.arrinsert wikipedia .children}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrinsert wikipedia .children abc 1}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrinsert wikipedia .children 1.0 1}
            assert_error "SYNTAXERR*" {r json.arrinsert wikipedia .children 0 abc}
            assert_error "SYNTAXERR*" {r json.arrinsert wikipedia .children 0 1 2 {{"a":}}}
            assert_equal {["Kathy","John","Rose","Bob","Mary","Peter","Jane","Tom","Grace"]} [r json.get wikipedia .children]
        }

        test "JSON.ARRINSERT index rules and several values (caps $caps)" {
            # An index from -len to len is accepted. A negative index counts from the end, so
            # -1 inserts before the last element, and len appends.
            r flushall
            r json.set p . {[3,5]}
            assert_equal 4 [r json.arrinsert p . 0 {"a"} {"b"}]
            assert_equal 5 [r json.arrinsert p . 2 {"c"}]
            assert_equal 6 [r json.arrinsert p . -1 {"y"}]
            assert_equal 7 [r json.arrinsert p . -2 {"x"}]
            assert_equal {["a","b","c",3,"x","y",5]} [r json.get p]
            foreach idx {8 10 -8 -13 9223372036854775807 -9223372036854775808} {
                assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert p . $idx 1}
                assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert p $ $idx 1 2}
            }
            assert_equal {["a","b","c",3,"x","y",5]} [r json.get p]
            assert_equal 8 [r json.arrinsert p . 7 {"end"}]
            assert_equal 9 [r json.arrinsert p . -8 {"first"}]
            assert_equal {["first","a","b","c",3,"x","y",5,"end"]} [r json.get p]
            assert_equal {[(11)]} [json_raw json.arrinsert p $ 1 {{"o":[1]}} {[[]]}]
            assert_equal {["first",{"o":[1]},[[]],"a","b","c",3,"x","y",5,"end"]} [r json.get p]
            # an empty array takes index 0 and any negative index
            r json.set p . {[]}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert p . 1 1}
            assert_equal 1 [r json.arrinsert p . -5 1]
            assert_equal {[1]} [r json.get p]
        }

        test "JSON.ARRINSERT \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {[[], [0], [0, 1], [0, 1, 2]]}]
            r json.set k2 . {[[], [0], [0, 1], [0, 1, 2]]}
            r json.set k3 . {[[], [0], [0, 1], [0, 1, 2]]}
            foreach {key path index val exp exp_new_val} {
                k1 $.[*] 0  3 {1 2 3 4} {[[3],[3,0],[3,0,1],[3,0,1,2]]}
                k3 $.[*] -1 3 {1 2 3 4} {[[3],[3,0],[0,3,1],[0,1,3,2]]}
            } {
                assert_equal $exp [r json.arrinsert $key $path $index $val]
                assert_equal $exp_new_val [r json.get $key .]
            }
            # index out of bounds for the first array
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert k2 {$.[*]} 1 3}
            assert_equal {[[],[0],[0,1],[0,1,2]]} [r json.get k2]

            # a value that is not an array replies null, no match replies an empty array
            r json.set k4 . {["a",[0,"z"],4,{"x":[1,2]},[0,"y","z"]]}
            assert_equal {[nil (3) nil nil (4)]} [json_raw json.arrinsert k4 {$[*]} 1 5]
            assert_equal {[]} [json_raw json.arrinsert k4 $.foo 0 1]
            # a legacy wildcard inserts into every array and replies with the first new length
            assert_equal {(4)} [json_raw json.arrinsert k4 {.[*]} 0 true]
            assert_equal {["a",[true,0,5,"z"],4,{"x":[1,2]},[true,0,5,"y","z"]]} [r json.get k4]
        }

        test "JSON.ARRINSERT out of bounds for one of several arrays changes nothing - B6 (caps $caps)" {
            # The module inserts into the arrays before the failing one and then stops.
            r flushall
            r json.set k . {[[0,1],[0],[]]}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert k {$[*]} 1 9}
            assert_equal {[[0,1],[0],[]]} [r json.get k]
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert k {.[*]} 1 9}
            assert_equal {[[0,1],[0],[]]} [r json.get k]
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert k $..* -2 9 10}
            assert_equal {[[0,1],[0],[]]} [r json.get k]
            r json.set k . {{"a":[1,2,3],"b":{"a":[1,2],"c":{"a":[1]}}}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert k $..a 2 {"x"}}
            assert_equal {{"a":[1,2,3],"b":{"a":[1,2],"c":{"a":[1]}}}} [r json.get k]
            assert_equal {3 2 1} [r json.arrlen k $..a]
            # with an index valid for all of them the same write applies everywhere
            assert_equal {4 3 2} [r json.arrinsert k $..a 1 {"x"}]
            assert_equal {{"a":[1,"x",2,3],"b":{"a":[1,"x",2],"c":{"a":[1,"x"]}}}} [r json.get k]
        }

        # ---------------------------------------------------------------- JSON.ARRTRIM

        test "JSON.ARRTRIM legacy path (caps $caps)" {
            json_array_fixture
            # edge case: empty array
            assert_equal 0 [r json.arrtrim wikipedia .children 0 1]
            # populate the array
            assert_equal 3 [r json.arrappend wikipedia .children {"John"} {"Mary"} {"Tom"}]
            assert_equal {["John","Mary","Tom"]} [r json.get wikipedia .children]
            foreach {path start end new_len new_val} {
                .children     1  2 2 {["Mary","Tom"]}
                .children     0  0 1 {["Mary"]}
                .children     -1 5 1 {["Mary"]}
                .phoneNumbers 2  0 0 {[]}
            } {
                assert_equal $new_len [r json.arrtrim wikipedia $path $start $end]
                assert_equal $new_val [r json.get wikipedia $path]
            }

            # error if the document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.arrtrim foo .children 0 1}
            assert_error "NONEXISTENT Document key does not exist" {r json.arrtrim foo $.children 0 1}
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.arrtrim wikipedia .foo 0 3}
            # error condition: the element is not an array
            foreach path {.address .groups .age .weight .isAlive .spouse .firstName .} {
                assert_error "WRONGTYPE JSON element is not an array" {r json.arrtrim wikipedia $path 0 1}
            }
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrtrim str_key .children 0 1}
            # large index: larger than int32
            assert_equal 0 [r json.arrtrim wikipedia .phoneNumbers 3000000000 3000000001]
            # wrong number of arguments, bad indexes
            assert_error "*wrong number of arguments*" {r json.arrtrim wikipedia .phoneNumbers 0 1 extra}
            assert_error "*wrong number of arguments*" {r json.arrtrim wikipedia .phoneNumbers 0}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrtrim wikipedia .children a 1}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrtrim wikipedia .children 0 b}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrtrim wikipedia .children 0 1.5}
            assert_equal {["Mary"]} [r json.get wikipedia .children]
        }

        test "JSON.ARRTRIM range rules (caps $caps)" {
            # A negative start is 0 and a stop past the end is the last index. Neither counts
            # from the end: a start past the end or after the stop empties the array.
            r flushall
            foreach {start stop new_len new_val} {
                2    7    6  {[2,3,4,5,6,7]}
                0    100  10 {[0,1,2,3,4,5,6,7,8,9]}
                0    9    10 {[0,1,2,3,4,5,6,7,8,9]}
                -100 2    3  {[0,1,2]}
                -1   0    1  {[0]}
                9    9    1  {[9]}
                9    100  1  {[9]}
                5    5    1  {[5]}
                1    8    8  {[1,2,3,4,5,6,7,8]}
                10   12   0  {[]}
                3    2    0  {[]}
                -3   -2   0  {[]}
                -4   -2   0  {[]}
                4    -2   0  {[]}
                0    -1   0  {[]}
                -9223372036854775808 9223372036854775807  10 {[0,1,2,3,4,5,6,7,8,9]}
                9223372036854775807  -9223372036854775808 0  {[]}
                9223372036854775807  9223372036854775807  0  {[]}
            } {
                r json.set t . {[0,1,2,3,4,5,6,7,8,9]}
                assert_equal $new_len [r json.arrtrim t . $start $stop] "trim $start $stop"
                assert_equal $new_val [r json.get t] "trim $start $stop"
                r json.set t . {{"a":[0,1,2,3,4,5,6,7,8,9],"b":"x"}}
                assert_equal [list $new_len] [r json.arrtrim t $.a $start $stop] "trim $start $stop"
                assert_equal "{\"a\":$new_val,\"b\":\"x\"}" [r json.get t] "trim $start $stop"
            }
        }

        test "JSON.ARRTRIM \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            assert_equal OK [r json.set k2 . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            assert_equal OK [r json.set k3 . {[[], [0], [0,1], [0,1,2], [0,1,2,3]]}]
            foreach {key path start stop exp exp_new_val} {
                k1 $.[*] 0 1 {0 1 2 2}   {[[],["a"],["a","b"],["a","b"]]}
                k2 $.[*] 1 1 {0 0 1 1}   {[[],[],["b"],["b"]]}
                k3 $.[*] 1 2 {0 0 1 2 2} {[[],[],[1],[1,2],[1,2]]}
            } {
                assert_equal $exp [r json.arrtrim $key $path $start $stop]
                assert_equal $exp_new_val [r json.get $key .]
            }
            r json.set k4 . {["a",[0,5,5,"z"],4,{"x":[1,2]},[0,5,5,"y","z"]]}
            assert_equal {[nil (3) nil nil (3)]} [json_raw json.arrtrim k4 {$[*]} 1 3]
            assert_equal {[]} [json_raw json.arrtrim k4 $.foo 0 0]
            # a legacy wildcard trims every array and replies with the first new length
            assert_equal {(1)} [json_raw json.arrtrim k4 {.[*]} 0 0]
            assert_equal {["a",[5],4,{"x":[1,2]},[5]]} [r json.get k4]
            assert_equal {(2)} [json_raw json.arrtrim k4 . 1 2]
            assert_equal {[[5],4]} [r json.get k4]
        }

        # ---------------------------------------------------------------- JSON.ARRINDEX

        test "JSON.ARRINDEX legacy path (caps $caps)" {
            json_array_fixture
            # edge case: empty array
            assert_equal -1 [r json.arrindex wikipedia .children {"tom"}]
            # populate the array
            assert_equal 5 [r json.arrappend wikipedia .children {"John"} {"Mary"} {"Tom"} {"Paul"} {"Peter"}]
            foreach {val idx} {{"John"} 0 {"Tom"} 2 {"Peter"} 4 {"Peter2"} -1} {
                assert_equal $idx [r json.arrindex wikipedia .children $val]
            }
            foreach {val start stop idx} {
                {"Tom"}  5 0 -1
                {"Paul"} 0 4 3
                {"Paul"} 0 0 3
            } {
                assert_equal $idx [r json.arrindex wikipedia .children $val $start $stop]
            }
            assert_equal OK [r json.set arr . {[0, 1, 2, 3, 4]}]
            assert_equal 1 [r json.arrindex arr . 1]
            assert_equal -1 [r json.arrindex arr . 1 2]

            # error if the document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.arrindex foo .children 0 5}
            assert_error "NONEXISTENT Document key does not exist" {r json.arrindex foo $.children 0}
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.arrindex wikipedia .foo 0 3}
            # error condition: the element is not an array
            foreach path {.address .groups .age .weight .isAlive .spouse .firstName .} {
                assert_error "WRONGTYPE JSON element is not an array" {r json.arrindex wikipedia $path 1}
            }
            assert_error "WRONGTYPE Not a JSON document key" {r json.arrindex str_key .children 0}
            # large index: larger than int32
            assert_equal -1 [r json.arrindex wikipedia .phoneNumbers 1 3000000000 0]
            # wrong number of arguments, a value that is not JSON, bad indexes
            assert_error "*wrong number of arguments*" {r json.arrindex wikipedia .phoneNumbers 1 0 3 extra}
            assert_error "*wrong number of arguments*" {r json.arrindex wikipedia .children}
            assert_error "SYNTAXERR*" {r json.arrindex wikipedia .children abc}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrindex wikipedia .children 1 a}
            assert_error "WRONGTYPE Value is not an integer" {r json.arrindex wikipedia .children 1 0 b}
        }

        test "JSON.ARRINDEX \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {[[], ["a"], ["a", "b"], ["a", "b", "c"]]}]
            assert_equal OK [r json.set k2 . {[[], [0], [0,1], [0,1,2]]}]
            assert_equal OK [r json.set k3 . {[[], [0,true], [0,1,false], [0,1,2,null,true]]}]
            foreach {key path val exp} {
                k1 $.[*] {"a"} {-1 0 0 0}
                k1 $.[*] {"b"} {-1 -1 1 1}
                k1 $.[*] {"c"} {-1 -1 -1 2}
                k2 $.[*] 1     {-1 -1 1 1}
                k2 $.[*] 2     {-1 -1 -1 2}
                k2 $.[*] true  {-1 -1 -1 -1}
                k3 $.[*] true  {-1 1 -1 4}
                k3 $.[*] null  {-1 -1 -1 3}
            } {
                assert_equal $exp [r json.arrindex $key $path $val]
            }
            r json.set k4 . {["a",[0,5,"z"],4,{"x":[1,2]},[5]]}
            assert_equal {[nil (1) nil nil (0)]} [json_raw json.arrindex k4 {$[*]} 5]
            assert_equal {[]} [json_raw json.arrindex k4 $.foo 5]
            assert_equal {[(2)]} [json_raw json.arrindex k4 $ 4]
            # a legacy wildcard replies with the result for the first array
            assert_equal {(1)} [json_raw json.arrindex k4 {.[*]} 5]
            assert_equal {(-1)} [json_raw json.arrindex k4 {.[*]} {"y"}]
        }

        test "JSON.ARRINDEX is not limited to scalar values (caps $caps)" {
            r flushall
            r json.set k1 . {[5, 6, {"a":"b"}, [99,100]]}
            assert_equal 2 [r json.arrindex k1 . {{"a":"b"}} 0 0]
            assert_equal 3 [r json.arrindex k1 . {[99,100]} 0 0]
            assert_equal 0 [r json.arrindex k1 . 5 0 0]
            assert_equal 1 [r json.arrindex k1 . 6 0 0]
        }

        test "JSON.ARRINDEX with a recursive \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {{"level0":{"level1_0":{"level2":[1,2,3, [25, [4,5,{"c":"d"}]]]},"level1_1":{"level2":[[{"a":[2,5]},true,null]]}}}}]
            foreach {path val exp} {
                $..level0.level1_0.. {[4,5,{"c":"d"}]}         {{} -1 1 -1 {}}
                $..level0.level1_0.. {[25,[4,5,{"c":"d"}]]}    {{} 3 -1 -1 {}}
                $..level0.level1_0.. {{"c":"d"}}               {{} -1 -1 2 {}}
                $..level0.level1_1.. {[{"a":[2,5]},true,null]} {{} 0 -1 {} -1}
                $..level0.level1_1.. {[null,true,{"a":[2,5]}]} {{} -1 -1 {} -1}
                $..level0.level1_1.. {[{"a":[2,5]},true]}      {{} -1 -1 {} -1}
                $..level0.level1_0.. {[4,{"c":"d"}]}           {{} -1 -1 -1 {}}
            } {
                assert_equal $exp [r json.arrindex k1 $path $val]
            }
        }

        test "JSON.ARRINDEX equality and range rules (caps $caps)" {
            r flushall
            # Values are compared as JSON values: a number by value whatever its text, an object
            # whatever the order of its members.
            r json.set e . {[{"a":1,"b":2},{"b":2,"a":1},"a\tb","e","E",[1,[2]],[1,[2.0]],1e2,100,"100",true,"true",null,{},[],0]}
            foreach {val args exp} {
                {{"b":2,"a":1}}     {}       0
                {{"a":1,"b":2}}     {}       0
                {{ "a" : 1 , "b" : 2 }} {}   0
                {{"a":1,"b":2}}     1        1
                {{"a":1}}           {}       -1
                {{"a":1,"b":2,"c":3}} {}     -1
                {"a\tb"}            {}       2
                {"a\u0009b"}        {}       2
                {"e"}               {}       3
                {"E"}               {}       4
                {"E"}          {}       4
                {[1,[2]]}           {}       5
                {[1,[2.0]]}         {}       5
                {[ 1 , [ 2 ] ]}     6        6
                {[1,[2,3]]}         {}       -1
                {[[2],1]}           {}       -1
                100                 {}       7
                1e2                 {}       7
                100.0               8        8
                {"100"}             {}       9
                true                {}       10
                {"true"}            {}       11
                null                {}       12
                {{}}                {}       13
                {[]}                {}       14
                0                   {}       15
                -0                  {}       15
                0.0                 {}       15
                -0.0                {}       15
                false               {}       -1
                {""}                {}       -1
            } {
                assert_equal $exp [r json.arrindex e . $val {*}$args] "value $val from $args"
            }
            # The search covers [start, stop). A negative start is 0, a stop of 0 or -1 means the
            # end, a stop past the end is the end, and any other negative stop finds nothing.
            foreach {val start stop exp} {
                0     -1  {}  15
                0     -1  -1  15
                0     15  15  -1
                0     15  16  15
                0     15  17  15
                0     16  {}  -1
                0     16  0   -1
                0     100 0   -1
                true  10  11  10
                true  10  10  -1
                true  11  10  -1
                true  -6  -5  -1
                true  -6  -6  -1
                null  0   -3  -1
                null  0   -4  -1
                null  0   12  -1
                null  0   13  12
                null  12  0   12
                null  13  0   -1
                null  -100 100 12
                null  -9223372036854775808 9223372036854775807 12
                null  9223372036854775807 0 -1
            } {
                set args [list $start]
                if {$stop ne ""} { lappend args $stop }
                assert_equal $exp [r json.arrindex e . $val {*}$args] "value $val in $start..$stop"
            }

            r json.set i . {[0,1,2,3,1,"1",1.0,true,null,[1],{"a":1}]}
            foreach {val args exp} {
                1 {} 1   1 2 4   1 {2 4} -1   1 {2 5} 4   1 -7 1   1 {5 0} 6   1 {5 -1} 6
                1.0 {} 1   {"1"} {} 5   {[1]} {} 9   {{"a":1}} {} 10   {{"a": 1}} {} 10
                1 11 -1   1 {10 0} -1   1 {0 -100} -1
            } {
                assert_equal $exp [r json.arrindex i . $val {*}$args] "value $val from $args"
            }
            assert_equal {4} [r json.arrindex i $ 1 4]
        }

        test "JSON.ARRINDEX compares numbers by value (caps $caps)" {
            r flushall
            # two integers within int64 are compared exactly
            r json.set a . {[9007199254740993,9007199254740992,3]}
            assert_equal 1 [r json.arrindex a . 9007199254740992]
            assert_equal 0 [r json.arrindex a . 9007199254740993]
            assert_equal -1 [r json.arrindex a . 9007199254740994]
            # an integer and a double are compared as doubles
            assert_equal 0 [r json.arrindex a . 9007199254740992.0]
            assert_equal 0 [r json.arrindex a . 9.007199254740993e15]
            r json.set a . {[1.5,9007199254740993.0,3.25]}
            assert_equal 1 [r json.arrindex a . 9007199254740992]
            assert_equal 1 [r json.arrindex a . 9007199254740993]
            # the text of a number does not matter
            r json.set a . {[0.1,19.90,1e2,1.0,"x"]}
            foreach {val exp} {0.10 0  1e-1 0  0.1000000000000000055511151231257827 0  19.9 1  19.900 1
                               1.99e1 1  100 2  100.00 2  1 3  1.00 3  10e-1 3  2 -1  {"x"} 4} {
                assert_equal $exp [r json.arrindex a . $val] "value $val"
            }
            # numbers too long for a double's precision
            r json.set a . {[1234567890123456789012345,"x"]}
            foreach val {1234567890123456789012345 1234567890123456789012346 1.2345678901234568e24 1234567890123456800000000} {
                assert_equal 0 [r json.arrindex a . $val] "value $val"
            }
            assert_equal -1 [r json.arrindex a . 1234567890123456000000000]
        }

        test "JSON.ARRINDEX compares an integer outside int64 as a double, as the module does (caps $caps)" {
            # In the module only two int64 values are compared exactly. Any other pair of
            # numbers is compared as doubles, so 2^63 equals 2^63 - 1 and 2^63 + 1.
            r flushall
            r json.set a . {[9223372036854775808,"x"]}
            assert_equal 0 [r json.arrindex a . 9223372036854775808]
            assert_equal 0 [r json.arrindex a . 9223372036854775808.0]
            assert_equal 0 [r json.arrindex a . 9.223372036854775808e18]
            assert_equal 0 [r json.arrindex a . 9223372036854775807]
            assert_equal 0 [r json.arrindex a . 9223372036854775809]
            assert_equal 0 [r json.arrindex a . 9223372036854776000]
            assert_equal -1 [r json.arrindex a . 9223372036854777000]
            r json.set a . {[9223372036854775807,"x"]}
            assert_equal 0 [r json.arrindex a . 9223372036854775807]
            assert_equal -1 [r json.arrindex a . 9223372036854775806]
            assert_equal 0 [r json.arrindex a . 9223372036854775807.0]
            assert_equal 0 [r json.arrindex a . 9223372036854775808]
            r json.set a . {[9223372036854775808,9223372036854775809]}
            assert_equal 0 [r json.arrindex a . 9223372036854775809]
            assert_equal 0 [r json.arrindex a . 9223372036854775807]
            # Two integers within int64 are compared exactly, so -1 is not 2^64 - 1, while
            # two 25-digit integers that differ in the last digit are the same double.
            r json.set a . {[9223372036854775807,-1,1234567890123456789012345]}
            assert_equal 0 [r json.arrindex a . 9223372036854775808]
            assert_equal 1 [r json.arrindex a . -1]
            assert_equal -1 [r json.arrindex a . 18446744073709551615]
            assert_equal 2 [r json.arrindex a . 1234567890123456789012346]
            assert_equal -1 [r json.arrindex a . 1234567890123456789012345678]
            r json.set a . {[18446744073709551615]}
            assert_equal -1 [r json.arrindex a . -1]
            assert_equal 0 [r json.arrindex a . 18446744073709551615]
            assert_equal 0 [r json.arrindex a . 18446744073709551616]
        }

        # ---------------------------------------------------------------- number text in arrays

        test "JSON array commands keep the text of every number in arrays of numbers (caps $caps)" {
            r flushall
            # an array whose doubles share two fraction digits
            r json.set a . {[1.50,2.25]}
            assert_equal 8 [r json.arrappend a . 1.5 3 4.000 1e2 {"s"} 2.75]
            assert_equal {[1.50,2.25,1.5,3,4.000,1e2,"s",2.75]} [r json.get a]
            assert_equal 10 [r json.arrinsert a . 0 0.10 10]
            assert_equal {[0.10,10,1.50,2.25,1.5,3,4.000,1e2,"s",2.75]} [r json.get a]
            assert_equal 0.10 [r json.arrpop a . 0]
            assert_equal 1.50 [r json.arrpop a . 1]
            assert_equal 2.75 [r json.arrpop a]
            assert_equal {[10,2.25,1.5,3,4.000,1e2,"s"]} [r json.get a]
            foreach {val idx} {1.5 2  1.50 2  4 4  4.0 4  100 5  1e2 5  100.0 5} {
                assert_equal $idx [r json.arrindex a . $val]
            }
            assert_equal 3 [r json.arrtrim a . 1 3]
            assert_equal {[2.25,1.5,3]} [r json.get a]

            # an integer array
            r json.set a . {[1,2,3]}
            assert_equal 4 [r json.arrappend a . 4.5]
            assert_equal {[1,2,3,4.5]} [r json.get a]
            assert_equal 6 [r json.arrappend a . 5.50 6]
            assert_equal {[1,2,3,4.5,5.50,6]} [r json.get a]
            assert_equal 9 [r json.arrinsert a . 1 1e1 {"x"} null]
            assert_equal {[1,1e1,"x",null,2,3,4.5,5.50,6]} [r json.get a]
            assert_equal 1e1 [r json.arrpop a . 1]
            assert_equal {"x"} [r json.arrpop a . 1]
            assert_equal null [r json.arrpop a . 1]
            assert_equal {[1,2,3,4.5,5.50,6]} [r json.get a]
            assert_equal {integer integer integer number number integer} [r json.type a {$[*]}]

            # an array whose doubles share six fraction digits
            r json.set a . {[0.100000,0.250000]}
            assert_equal 5 [r json.arrappend a . 0.5 0.3000000 1]
            assert_equal {[0.100000,0.250000,0.5,0.3000000,1]} [r json.get a]
            assert_equal 6 [r json.arrinsert a . -1 0.123456]
            assert_equal {[0.100000,0.250000,0.5,0.3000000,0.123456,1]} [r json.get a]
            assert_equal 2 [r json.arrtrim a . 0 1]
            assert_equal {[0.100000,0.250000]} [r json.get a]
            assert_equal 3 [r json.arrappend a . 0.333333]
            assert_equal {[0.100000,0.250000,0.333333]} [r json.get a]
            assert_equal 1 [r json.clear a]
            assert_equal {[]} [r json.get a]
            assert_equal 2 [r json.arrappend a . 1.10 2.20]
            assert_equal {[1.10,2.20]} [r json.get a]
            assert_equal 3 [r json.arrappend a . 3]
            assert_equal {[1.10,2.20,3]} [r json.get a]

            # an empty array that becomes typed, then mixed
            r json.set a . {[]}
            assert_equal 3 [r json.arrappend a . 1 2 3]
            assert_equal 4 [r json.arrappend a . 1.5]
            assert_equal 5 [r json.arrappend a . {[1]}]
            assert_equal 7 [r json.arrappend a . 9223372036854775808 -0]
            assert_equal {[1,2,3,1.5,[1],9223372036854775808,0]} [r json.get a]
            assert_equal 5 [r json.arrindex a . 9223372036854775808]
            foreach val {0 -0 0.0 -0.0} { assert_equal 6 [r json.arrindex a . $val] }
            assert_equal 0 [r json.arrpop a]
            assert_equal 9223372036854775808 [r json.arrpop a]
            assert_equal {[1]} [r json.arrpop a]
            assert_equal {[1,2,3,1.5]} [r json.get a]

            # integers of growing width
            r json.set a . {[1,2]}
            assert_equal 7 [r json.arrappend a . 300 -70000 5000000000 -9223372036854775808 9223372036854775807]
            assert_equal {[1,2,300,-70000,5000000000,-9223372036854775808,9223372036854775807]} [r json.get a]
            assert_equal 9223372036854775807 [r json.arrpop a]
            assert_equal -9223372036854775808 [r json.arrpop a . -1]
            assert_equal {[1,2,300,-70000,5000000000]} [r json.get a]
            assert_equal {integer integer integer integer integer} [r json.type a {$[*]}]
        }

        # ---------------------------------------------------------------- nested matches

        test "JSON array commands with nested matches - 2.5 (caps $caps)" {
            # Every match is resolved against the document as it was before the command. A
            # command that only adds applies to every match. Where one match removes a value
            # that holds another match, the outer one wins, and each reply is the one that match
            # would get alone.
            r flushall
            r json.set k . {{"a":[{"a":[1]},5]}}
            assert_equal {3 2} [r json.arrappend k $..a 9]
            assert_equal {{"a":[{"a":[1,9]},5,9]}} [r json.get k]
            assert_equal {3 2} [r json.arrlen k $..a]
            assert_equal {4 3} [r json.arrinsert k $..a 0 7]
            assert_equal {{"a":[7,{"a":[7,1,9]},5,9]}} [r json.get k]
            assert_equal {3 2} [r json.arrindex k $..a 9]
            assert_equal {5 4} [r json.arrinsert k $..a -1 {[8]}]
            assert_equal {{"a":[7,{"a":[7,1,[8],9]},5,[8],9]}} [r json.get k]

            # the outer pop removes the object that holds the inner array
            r json.set k . {{"a":[7,{"a":[7,1,9]},5,9]}}
            assert_equal {{{"a":[7,1,9]}} 1} [r json.arrpop k $..a 1]
            assert_equal {{"a":[7,5,9]}} [r json.get k]
            r json.set k . {{"a":[{"a":[1,2,3]},5,6]}}
            assert_equal {{{"a":[1,2,3]}} 1} [r json.arrpop k $..a 0]
            assert_equal {{"a":[5,6]}} [r json.get k]
            r json.set k . {{"a":[{"a":[1,2,3]},5,6]}}
            assert_equal {{"a":[1,2,3]}} [r json.arrpop k ..a 0]
            assert_equal {{"a":[5,6]}} [r json.get k]
            # the outer pop leaves the inner array in place: both apply
            r json.set k . {{"a":[{"a":[1,2,3]},5,6]}}
            assert_equal {6 3} [r json.arrpop k $..a]
            assert_equal {{"a":[{"a":[1,2]},5]}} [r json.get k]

            # the outer trim removes the inner array
            r json.set k . {{"a":[{"a":[1,2,3]},5,6]}}
            assert_equal {2 2} [r json.arrtrim k $..a 1 2]
            assert_equal {{"a":[5,6]}} [r json.get k]
            # the outer trim keeps it: both apply
            r json.set k . {{"a":[{"a":[1,2,3]},5,6]}}
            assert_equal {1 1} [r json.arrtrim k $..a 0 0]
            assert_equal {{"a":[{"a":[1]}]}} [r json.get k]

            # arrays nested directly in arrays
            r json.set k . {[[1,[2,[3,4]]],[5]]}
            assert_equal {3 2 {} 3 {} 3 {} {} {}} [r json.arrappend k $..* {"x"}]
            assert_equal {[[1,[2,[3,4,"x"],"x"],"x"],[5,"x"]]} [r json.get k]
            r json.set k . {[[1,[2,[3,4]]],[5]]}
            assert_equal {{[2,[3,4]]} 5 {} {[3,4]} {} 4 {} {} {}} [r json.arrpop k $..*]
            assert_equal {[[1],[]]} [r json.get k]
            assert_equal PONG [r ping]
        }

        # ---------------------------------------------------------------- RESP3

        test "JSON array commands RESP3 replies (caps $caps)" {
            json_array_fixture
            r json.set k . {[[], "a", ["a", "b"], 4]}
            r hello 3
            assert_equal {null} [json_raw json.arrlen foo .a]
            assert_equal {null} [json_raw json.arrlen foo $.a]
            assert_equal {(2)} [json_raw json.arrlen wikipedia .phoneNumbers]
            assert_equal {[(2)]} [json_raw json.arrlen wikipedia $.phoneNumbers]
            assert_equal {[]} [json_raw json.arrlen wikipedia $.foo]
            assert_equal {[(0) null (2) null]} [json_raw json.arrlen k {$[*]}]
            assert_equal {[(1) null (3) null]} [json_raw json.arrappend k {$[*]} 1]
            assert_equal {(2)} [json_raw json.arrappend k {.[*]} 2]
            assert_equal {[(3) null (5) null]} [json_raw json.arrinsert k {$[*]} 0 0]
            assert_equal {[(-1) null (1) null]} [json_raw json.arrindex k {$[*]} {"a"}]
            assert_equal {(0)} [json_raw json.arrindex k {.[*]} 0]
            assert_equal {["2" null "2" null]} [json_raw json.arrpop k {$[*]}]
            assert_equal {"1"} [json_raw json.arrpop k {.[*]}]
            assert_equal {[(1) null (2) null]} [json_raw json.arrtrim k {$[*]} 0 1]
            assert_equal {(1)} [json_raw json.arrtrim k {.[*]} 0 0]
            assert_equal {null} [json_raw json.arrpop wikipedia .children]
            assert_equal {[null]} [json_raw json.arrpop wikipedia $.children]
            assert_equal {[null]} [json_raw json.arrpop wikipedia $.address]
            assert_equal {[]} [json_raw json.arrpop wikipedia $.foo]
            assert_equal {[]} [json_raw json.arrappend wikipedia $.foo 1]
            assert_equal {[]} [json_raw json.arrinsert wikipedia $.foo 0 1]
            assert_equal {[]} [json_raw json.arrtrim wikipedia $.foo 0 1]
            assert_equal {[]} [json_raw json.arrindex wikipedia $.foo 1]
            assert_equal {-WRONGTYPE JSON element is not an array} [json_raw json.arrlen wikipedia .address]
            assert_equal {-NONEXISTENT JSON path does not exist} [json_raw json.arrpop wikipedia .foo]
            r hello 2
            assert_equal {[[0],"a",[0],4]} [r json.get k]
        }

        # ---------------------------------------------------------------- model

        foreach mode {ints fixed2 mixed} {
            test "JSON array commands against a list model, $mode values (caps $caps)" {
                # Random appends, inserts, pops and trims on a root array and on an array inside
                # an object, growing past the packed caps and shrinking again. The document is
                # compared with a Tcl list after every command.
                r flushall
                expr {srand(20261114)}
                foreach {key path} {root . nested .arr} {
                    if {$key eq "root"} {
                        r json.set $key . {[]}
                    } else {
                        r json.set $key . {{"pad":"x","arr":[],"tail":[1,2.50,"t"]}}
                    }
                    set model {}
                    set n 0
                    for {set step 0} {$step < 150} {incr step} {
                        set len [llength $model]
                        # grow for the first 100 steps, shrink afterwards
                        set grow [expr {$step < 100}]
                        set dice [expr {int(rand() * 10)}]
                        if {($grow && $dice < 5) || $len == 0} {
                            set vals {}
                            set count [expr {1 + int(rand() * 8)}]
                            for {set j 0} {$j < $count} {incr j} { lappend vals [json_model_value $mode [incr n]] }
                            lappend model {*}$vals
                            assert_equal [llength $model] [r json.arrappend $key $path {*}$vals]
                        } elseif {$grow && $dice < 8} {
                            set vals {}
                            set count [expr {1 + int(rand() * 3)}]
                            for {set j 0} {$j < $count} {incr j} { lappend vals [json_model_value $mode [incr n]] }
                            set idx [expr {int(rand() * (2 * $len + 1)) - $len}]
                            set at [expr {$idx < 0 ? $len + $idx : $idx}]
                            set model [linsert $model $at {*}$vals]
                            assert_equal [llength $model] [r json.arrinsert $key $path $idx {*}$vals] "insert at $idx"
                        } elseif {$dice < 9 || !$grow && $dice < 7} {
                            set idx [expr {int(rand() * (2 * $len + 5)) - $len - 2}]
                            set at [expr {$idx < 0 ? $len + $idx : $idx}]
                            if {$at < 0} { set at 0 }
                            if {$at >= $len} { set at [expr {$len - 1}] }
                            set exp [lindex $model $at]
                            set model [lreplace $model $at $at]
                            assert_equal $exp [r json.arrpop $key $path $idx] "pop at $idx"
                        } else {
                            set start [expr {int(rand() * ($len + 4)) - 2}]
                            set stop [expr {$start + int(rand() * ($len + 2)) - 1}]
                            if {$grow} { set start [expr {int(rand() * 3) - 1}]; set stop [expr {$len - int(rand() * 3)}] }
                            set s [expr {$start < 0 ? 0 : $start}]
                            set e [expr {$stop >= $len ? $len - 1 : $stop}]
                            if {$s >= $len || $s > $e} { set model {} } else { set model [lrange $model $s $e] }
                            assert_equal [llength $model] [r json.arrtrim $key $path $start $stop] "trim $start $stop"
                        }
                        assert_equal [json_array_text $model] [r json.get $key $path] "step $step"
                        assert_equal [llength $model] [r json.arrlen $key $path]
                        if {[llength $model] > 0 && $mode ne "mixed"} {
                            set at [expr {int(rand() * [llength $model])}]
                            assert_equal $at [r json.arrindex $key $path [lindex $model $at]]
                            assert_equal [lindex $model $at] [r json.get $key "$path\[$at\]"]
                        }
                    }
                    if {$key eq "nested"} {
                        assert_equal "{\"pad\":\"x\",\"arr\":[json_array_text $model],\"tail\":\[1,2.50,\"t\"\]}" [r json.get $key]
                    }
                    assert_equal [r json.debug memory $key] [r json.debug memory $key .]
                }
            }
        }
    }
    json_set_caps {4096 128}
}
