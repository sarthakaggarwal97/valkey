# JSON string, object and whole-value commands: JSON.STRLEN, STRAPPEND, OBJLEN, OBJKEYS, CLEAR,
# TYPE and RESP. The array commands are in json-array.tcl and the numeric ones in json-num.tcl.
#
# Ported from the valkey-json module's tst/integration/test_json_basic.py with the module's
# expected values, except where the built-in module deliberately differs. Those tests carry the
# number of the change in their name:
#   2.5  module defects that are not reproduced: STRAPPEND cutting both operands at an embedded
#        NUL, and nested matches in JSON.CLEAR (design 2.5, 2.6 Q2 and 8.1)

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

# json_raw for JSON.RESP. That command marks an array with the status "[" and an object with the
# status of an opening brace. An unmatched brace cannot be written inside a braced Tcl word, so
# the two statuses are spelled ARR and OBJ here.
proc json_resp {args} {
    return [string map [list "+\{" OBJ "+\[" ARR] [json_raw {*}$args]]
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
proc json_cmds_fixture {} {
    r flushall
    r json.set wikipedia . $::json_wikipedia
    r set str_key {{"firstName":"John","lastName":"Smith"}}
}

start_server {tags {"json"}} {
    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        # ---------------------------------------------------------------- JSON.STRLEN

        test "JSON.STRLEN legacy path (caps $caps)" {
            json_cmds_fixture
            assert_equal 2 [r json.strlen wikipedia .address.state]
            assert_equal 4 [r json.strlen wikipedia .firstName]
            # edge case: empty string
            assert_equal OK [r json.set wikipedia .foo {""}]
            assert_equal 0 [r json.strlen wikipedia .foo]
            # null if the document key does not exist
            assert_equal nil [json_raw json.strlen foo .firstName]
            assert_equal nil [json_raw json.strlen foo $.firstName]
            assert_equal nil [json_raw json.strlen foo]
            # error if the path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.strlen wikipedia .bar}
            # error condition: the element is not a string
            foreach path {.address .groups .age .isAlive .spouse .children .weight .} {
                assert_error "WRONGTYPE JSON element is not a string" {r json.strlen wikipedia $path}
            }
            # the path defaults to the root
            assert_error "WRONGTYPE JSON element is not a string" {r json.strlen wikipedia}
            assert_error "WRONGTYPE Not a JSON document key" {r json.strlen str_key .a}
            assert_error "WRONGTYPE Not a JSON document key" {r json.strlen str_key}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.strlen wikipedia .firstName extra}
            assert_error "*wrong number of arguments*" {r json.strlen}
            # a root string
            r json.set s . {"root"}
            assert_equal 4 [r json.strlen s]
            assert_equal 4 [r json.strlen s .]
            assert_equal 4 [r json.strlen s $]
        }

        test "JSON.STRLEN legacy and \$ wildcards (caps $caps)" {
            r flushall
            r json.set k1 . {{"a":{"a":"a"}, "b":{"a":""}, "c":{"a":"a", "b":"bb"}, "d":{"a":1, "b":"b", "c":3}}}
            foreach {path exp} {
                $.a.a  1
                $.a.*  1
                $.b.a  0
                $.b.*  0
                $.c.*  {1 2}
                $.c.b  2
                $.d.*  {{} 1 {}}
                .a.a   1
                .a.*   1
                .b.a   0
                .b.*   0
                .c.*   1
                .c.b   2
                .d.*   1
            } {
                assert_equal $exp [r json.strlen k1 $path] "path $path"
            }
            assert_equal {[nil (1) nil]} [json_raw json.strlen k1 $.d.*]
            assert_equal {[nil (1) (0) (1) nil]} [json_raw json.strlen k1 $..a]
            assert_equal {(1)} [json_raw json.strlen k1 ..a]
            assert_equal {[]} [json_raw json.strlen k1 $.zz]
            assert_equal {[nil]} [json_raw json.strlen k1 $.a]
            assert_equal {[nil]} [json_raw json.strlen k1 $]
            assert_error "WRONGTYPE JSON element is not a string" {r json.strlen k1 .a}
            assert_error "WRONGTYPE JSON element is not a string" {r json.strlen k1 .*}
        }

        test "JSON.STRLEN counts bytes of the unescaped string (caps $caps)" {
            r flushall
            # e-acute is 2 bytes, the euro sign 3, an emoji 4
            set u "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"
            r json.set s . "{\"s\":\"\",\"u\":\"$u\",\"e\":\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\",\"z\":\"\\u0041\\u00e9\\ud83d\\ude00\"}"
            assert_equal {0 9 12 7} [r json.strlen s $.*]
            assert_equal 0 [r json.strlen s .s]
            assert_equal 9 [r json.strlen s .u]
            assert_equal 12 [r json.strlen s .e]
            assert_equal 7 [r json.strlen s .z]
            assert_equal "a\"b\\c/d\b\f\n\r\t" [r json.resp s .e]
            assert_equal "A\xc3\xa9\xf0\x9f\x98\x80" [r json.resp s .z]
            assert_equal $u [r json.resp s .u]
        }

        # ---------------------------------------------------------------- JSON.STRAPPEND

        test "JSON.STRAPPEND legacy path (caps $caps)" {
            json_cmds_fixture
            foreach {val new_len new_val} {
                {"son"}      7  {"Johnson"}
                {" Junior"}  14 {"Johnson Junior"}
                {" is"}      17 {"Johnson Junior is"}
                {" my"}      20 {"Johnson Junior is my"}
                {" friend."} 28 {"Johnson Junior is my friend."}
                {""}         28 {"Johnson Junior is my friend."}
            } {
                assert_equal $new_len [r json.strappend wikipedia .firstName $val]
                assert_equal $new_val [r json.get wikipedia .firstName]
            }
            # edge case: appending to an empty string
            assert_equal OK [r json.set wikipedia .foo {""}]
            assert_equal 3 [r json.strappend wikipedia .foo {"abc"}]
            assert_equal {"abc"} [r json.get wikipedia .foo]

            # error condition: document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.strappend foo .firstName {"abc"}}
            assert_error "NONEXISTENT Document key does not exist" {r json.strappend foo $.firstName {"abc"}}
            assert_equal 0 [r exists foo]
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.strappend wikipedia .bar {"abc"}}
            # error condition: attempt to append a non-string value
            foreach val {123 true false null {{}} {[]} 1.5 {["a"]}} {
                assert_error "WRONGTYPE Value is not a string" {r json.strappend wikipedia .firstName $val}
            }
            # a value that is not JSON
            foreach val {abc {"a" "b"} {'a'} {"unterminated} {}} {
                assert_error "SYNTAXERR*" {r json.strappend wikipedia .firstName $val}
            }
            # error condition: attempt to append to a non-string element
            foreach path {.address .groups .age .isAlive .spouse .children .weight .} {
                assert_error "WRONGTYPE JSON element is not a string" {r json.strappend wikipedia $path {"12"}}
            }
            # with two arguments the value is appended to the root
            assert_error "WRONGTYPE JSON element is not a string" {r json.strappend wikipedia {"x"}}
            assert_error "WRONGTYPE Not a JSON document key" {r json.strappend str_key .firstName {"abc"}}
            assert_error "WRONGTYPE Not a JSON document key" {r json.strappend str_key {"abc"}}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.strappend wikipedia .firstName {"abc"} extra}
            assert_error "*wrong number of arguments*" {r json.strappend wikipedia}
            assert_equal {"Johnson Junior is my friend."} [r json.get wikipedia .firstName]

            # a root string, with and without a path; blanks around the value are allowed
            r json.set s . {"a"}
            assert_equal 2 [r json.strappend s {"b"}]
            assert_equal 3 [r json.strappend s . { "c"}]
            assert_equal {4} [r json.strappend s $ {"d" }]
            assert_equal {"abcd"} [r json.get s]
        }

        test "JSON.STRAPPEND legacy and \$ wildcards (caps $caps)" {
            # The expected results account for the outcome of the commands before them.
            r flushall
            r json.set k1 . {{"a":{"a":"a"}, "b":{"a":""}, "c":{"a":"a", "b":"bb"}, "d":{"a":1, "b":"b", "c":3}}}
            foreach {path append exp_ret exp_new_str exp_whole_json} {
                $.a.a {"x"}  2  {["ax"]}    {{"a":{"a":"ax"},"b":{"a":""},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                $.a.a {""}   2  {["ax"]}    {{"a":{"a":"ax"},"b":{"a":""},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                $.a.* {"yz"} 4  {["axyz"]}  {{"a":{"a":"axyz"},"b":{"a":""},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                .a.a  {"a"}  5  {"axyza"}   {{"a":{"a":"axyza"},"b":{"a":""},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                .a.*  {"a"}  6  {"axyzaa"}  {{"a":{"a":"axyzaa"},"b":{"a":""},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                $.b.a {"a"}  1  {["a"]}     {{"a":{"a":"axyzaa"},"b":{"a":"a"},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                $.b.* {""}   1  {["a"]}     {{"a":{"a":"axyzaa"},"b":{"a":"a"},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                $.b.* {"a"}  2  {["aa"]}    {{"a":{"a":"axyzaa"},"b":{"a":"aa"},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                .b.a  {"a"}  3  {"aaa"}     {{"a":{"a":"axyzaa"},"b":{"a":"aaa"},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                .b.*  {"a"}  4  {"aaaa"}    {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"a","b":"bb"},"d":{"a":1,"b":"b","c":3}}}
                $.c.* {"a"}  {2 3} {["aa","bba"]} {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"aa","b":"bba"},"d":{"a":1,"b":"b","c":3}}}
                $.c.b {"a"}  4  {["bbaa"]}  {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"aa","b":"bbaa"},"d":{"a":1,"b":"b","c":3}}}
                .c.*  {"a"}  5  {"aaa"}     {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"aaa","b":"bbaaa"},"d":{"a":1,"b":"b","c":3}}}
                .c.b  {"a"}  6  {"bbaaaa"}  {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"aaa","b":"bbaaaa"},"d":{"a":1,"b":"b","c":3}}}
                $.d.* {"a"}  {{} 2 {}} {[1,"ba",3]} {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"aaa","b":"bbaaaa"},"d":{"a":1,"b":"ba","c":3}}}
                .d.*  {"a"}  3  1           {{"a":{"a":"axyzaa"},"b":{"a":"aaaa"},"c":{"a":"aaa","b":"bbaaaa"},"d":{"a":1,"b":"baa","c":3}}}
            } {
                # A legacy path replies with the length of the last string updated, while
                # JSON.GET with the same path returns the first value selected.
                assert_equal $exp_ret [r json.strappend k1 $path $append] "path $path"
                assert_equal $exp_new_str [r json.get k1 $path] "path $path"
                assert_equal $exp_whole_json [r json.get k1] "path $path"
            }
            assert_equal {[nil (4) nil]} [json_raw json.strappend k1 $.d.* {"a"}]
            assert_equal {[]} [json_raw json.strappend k1 $.zz {"a"}]
            assert_equal {[nil]} [json_raw json.strappend k1 $.a {"a"}]
            assert_equal {(5)} [json_raw json.strappend k1 .d.b {"a"}]

            # recursive descent: every string, nested or not
            r json.set s . {{"a":"x","b":["y",1,"z"],"c":{"d":"w"}}}
            assert_equal {[(2) nil nil (2) nil (2) (2)]} [json_raw json.strappend s $..* {"1"}]
            assert_equal {{"a":"x1","b":["y1",1,"z1"],"c":{"d":"w1"}}} [r json.get s]
            assert_equal {(3)} [json_raw json.strappend s ..* {"2"}]
            assert_equal {{"a":"x12","b":["y12",1,"z12"],"c":{"d":"w12"}}} [r json.get s]
            assert_equal {[(3) nil nil (3) nil (3) (3)]} [json_raw json.strlen s $..*]
            r json.set s . {{"a":"x","b":{"a":"y","c":[{"a":"z"}]}}}
            assert_equal {2 2 2} [r json.strappend s $..a {"!"}]
            assert_equal {{"a":"x!","b":{"a":"y!","c":[{"a":"z!"}]}}} [r json.get s]
        }

        test "JSON.STRAPPEND with escapes and multi-byte characters (caps $caps)" {
            r flushall
            set u "\xc3\xa9\xe2\x82\xac"
            r json.set s . "{\"a\":\"h$u\"}"
            assert_equal 6 [r json.strlen s .a]
            # a surrogate pair written as escapes becomes one 4-byte character
            assert_equal 10 [r json.strappend s .a {"\ud83d\ude00"}]
            assert_equal "\"h$u\xf0\x9f\x98\x80\"" [r json.get s .a]
            assert_equal 18 [r json.strappend s .a {"a\"b\\c\n\/\u0041"}]
            assert_equal "\"h$u\xf0\x9f\x98\x80a\\\"b\\\\c\\n/A\"" [r json.get s .a]
            assert_equal "h$u\xf0\x9f\x98\x80a\"b\\c\n/A" [r json.resp s .a]
            assert_equal 20 [r json.strappend s .a "\"\xc3\xa9\""]
            assert_equal 20 [r json.strlen s .a]
        }

        test "JSON.STRAPPEND keeps an embedded NUL in both operands - 2.5 (caps $caps)" {
            # The module cuts the stored string and the appended one at the first NUL.
            r flushall
            r json.set k . {"abc"}
            assert_equal 6 [r json.strappend k . {"d\u0000e"}]
            assert_equal 6 [r json.strlen k]
            assert_equal {"abcd\u0000e"} [r json.get k]
            assert_equal 7 [r json.strappend k . {"f"}]
            assert_equal {"abcd\u0000ef"} [r json.get k]
            assert_equal "abcd\x00ef" [r json.resp k]

            r json.set k . {{"s":"x\u0000y","t":["\u0000"]}}
            assert_equal 3 [r json.strlen k .s]
            assert_equal 4 [r json.strappend k .s {"z"}]
            assert_equal {2} [r json.strappend k {$.t[0]} {"\u0000"}]
            assert_equal {{"s":"x\u0000yz","t":["\u0000\u0000"]}} [r json.get k]
            assert_equal "x\x00yz" [r json.resp k .s]
        }

        test "JSON.STRAPPEND grows strings past the packed caps (caps $caps)" {
            r flushall
            r json.set k . {{"head":1,"s":"","mid":[1,2,3],"t":"t","tail":{"x":"y"}}}
            set chunk [string repeat "0123456789" 10]
            set exp ""
            for {set i 1} {$i <= 60} {incr i} {
                append exp $chunk
                assert_equal [expr {$i * 100}] [r json.strappend k .s "\"$chunk\""]
            }
            assert_equal 6000 [r json.strlen k .s]
            assert_equal "\"$exp\"" [r json.get k .s]
            assert_equal "{\"head\":1,\"s\":\"$exp\",\"mid\":\[1,2,3\],\"t\":\"t\",\"tail\":{\"x\":\"y\"}}" [r json.get k]
            # every string in the document, the long one included
            assert_equal {{} 6001 {} 2 {} {} {} {} 2} [r json.strappend k $..* {"!"}]
            assert_equal "{\"head\":1,\"s\":\"$exp!\",\"mid\":\[1,2,3\],\"t\":\"t!\",\"tail\":{\"x\":\"y!\"}}" [r json.get k]
            # and back to a short one
            assert_equal OK [r json.set k .s {""}]
            assert_equal 3 [r json.strappend k .s {"abc"}]
            assert_equal {{"head":1,"s":"abc","mid":[1,2,3],"t":"t!","tail":{"x":"y!"}}} [r json.get k]
        }

        # ---------------------------------------------------------------- JSON.OBJLEN

        test "JSON.OBJLEN legacy path (caps $caps)" {
            json_cmds_fixture
            assert_equal 4 [r json.objlen wikipedia .address]
            # edge case: empty object
            assert_equal 0 [r json.objlen wikipedia .groups]
            # the path defaults to the root
            assert_equal 10 [r json.objlen wikipedia]
            assert_equal 10 [r json.objlen wikipedia .]
            assert_equal 10 [r json.objlen wikipedia $]
            # null if the document key does not exist
            assert_equal nil [json_raw json.objlen foo .address]
            assert_equal nil [json_raw json.objlen foo $.address]
            assert_equal nil [json_raw json.objlen foo]
            # error if the path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.objlen wikipedia .foo}
            # error condition: the element is not an object
            foreach path {.children .phoneNumbers .age .weight .isAlive .spouse .firstName} {
                assert_error "WRONGTYPE JSON element is not an object" {r json.objlen wikipedia $path}
            }
            assert_error "WRONGTYPE Not a JSON document key" {r json.objlen str_key .a}
            assert_error "WRONGTYPE Not a JSON document key" {r json.objlen str_key}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.objlen wikipedia . extra}
            assert_error "*wrong number of arguments*" {r json.objlen}
        }

        test "JSON.OBJLEN legacy and \$ wildcards (caps $caps)" {
            r flushall
            r json.set k1 . {{"a":{}, "c":{"a":"a", "b":"bb"}, "d":{"a":1, "b":"b", "c":{"a":3,"b":4}}, "e":1}}
            foreach {path exp} {
                $.a    0
                $.a.*  {}
                .a     0
                $.c    2
                $.c.*  {{} {}}
                .c     2
                $.d    3
                $.d.*  {{} {} 2}
                .d     3
                $.*    {0 2 3 {}}
                .*     0
                .d.*   2
            } {
                assert_equal $exp [r json.objlen k1 $path] "path $path"
            }
            assert_equal {[]} [json_raw json.objlen k1 $.a.*]
            assert_equal {[nil nil]} [json_raw json.objlen k1 $.c.*]
            assert_equal {[(0) (2) (3) nil]} [json_raw json.objlen k1 $.*]
            assert_equal {[(4)]} [json_raw json.objlen k1 $]
            assert_equal {[]} [json_raw json.objlen k1 $.zz]
            assert_error "NONEXISTENT JSON path does not exist" {r json.objlen k1 .a.*}
            assert_error "WRONGTYPE JSON element is not an object" {r json.objlen k1 .c.*}
            assert_error "WRONGTYPE JSON element is not an object" {r json.objlen k1 .e}
        }

        # ---------------------------------------------------------------- JSON.OBJKEYS

        test "JSON.OBJKEYS legacy path (caps $caps)" {
            json_cmds_fixture
            assert_equal {street city state zipcode} [r json.objkeys wikipedia .address]
            # edge case: empty object
            assert_equal {[]} [json_raw json.objkeys wikipedia .groups]
            # the path defaults to the root
            assert_equal {firstName lastName age weight isAlive address phoneNumbers children spouse groups} [r json.objkeys wikipedia]
            assert_equal {{firstName lastName age weight isAlive address phoneNumbers children spouse groups}} [r json.objkeys wikipedia $]
            # null if the document key does not exist
            assert_equal nil [json_raw json.objkeys foo .address]
            assert_equal nil [json_raw json.objkeys foo $.address]
            assert_equal nil [json_raw json.objkeys foo]
            # null if the path does not exist
            assert_equal nil [json_raw json.objkeys wikipedia .foo]
            # error condition: the element is not an object
            foreach path {.children .phoneNumbers .age .weight .isAlive .spouse .firstName} {
                assert_error "WRONGTYPE JSON element is not an object" {r json.objkeys wikipedia $path}
            }
            assert_error "WRONGTYPE Not a JSON document key" {r json.objkeys str_key .a}
            assert_error "WRONGTYPE Not a JSON document key" {r json.objkeys str_key}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.objkeys wikipedia . extra}
            assert_error "*wrong number of arguments*" {r json.objkeys}
        }

        test "JSON.OBJKEYS legacy and \$ wildcards (caps $caps)" {
            # The module's suite lists these cases without running them. The values are the
            # module's replies.
            r flushall
            r json.set k1 . {{"a":{}, "c":{"a":"a", "b":"bb"}, "d":{"a":1, "b":"b", "c":{"a":3,"b":4}}, "e":1}}
            foreach {path exp} {
                $.a    {[[]]}
                $.a.*  {[]}
                .a     {[]}
                $.c    {[["a" "b"]]}
                .c     {["a" "b"]}
                $.d    {[["a" "b" "c"]]}
                $.d.*  {[[] [] ["a" "b"]]}
                .d     {["a" "b" "c"]}
                .d.*   {["a" "b"]}
                $.*    {[[] ["a" "b"] ["a" "b" "c"] []]}
                .*     {["a" "b"]}
                $.c.*  {[[] []]}
                $.e    {[[]]}
                $.zz   {[]}
                .zz    {nil}
                .a.*   {nil}
                $      {[["a" "c" "d" "e"]]}
                .      {["a" "c" "d" "e"]}
            } {
                assert_equal $exp [json_raw json.objkeys k1 $path] "path $path"
            }
            assert_error "WRONGTYPE JSON element is not an object" {r json.objkeys k1 .c.*}
            assert_error "WRONGTYPE JSON element is not an object" {r json.objkeys k1 .e}
            assert_error "WRONGTYPE JSON element is not an object" {r json.objlen k1 .c.*}
            assert_error "WRONGTYPE JSON element is not an object" {r json.objlen k1 .e}
        }

        test "JSON.OBJKEYS replies with unescaped names in insertion order (caps $caps)" {
            r flushall
            set e "\xc3\xa9"
            r json.set o . "{\"a\\\"b\":1,\"$e\":2,\"\\u00e9x\":3,\"\":4,\"a b\":5,\"\\n\":6,\"\\u0000\":7,\"a/b\":8,\"~\":9,\"\\\\\":10,\"\\t\":11}"
            assert_equal [list "a\"b" $e "${e}x" "" "a b" "\n" "\x00" "a/b" "~" "\\" "\t"] [r json.objkeys o]
            assert_equal [list [list "a\"b" $e "${e}x" "" "a b" "\n" "\x00" "a/b" "~" "\\" "\t"]] [r json.objkeys o $]
            assert_equal 11 [r json.objlen o]
            assert_equal "{\"a\\\"b\":1,\"$e\":2,\"${e}x\":3,\"\":4,\"a b\":5,\"\\n\":6,\"\\u0000\":7,\"a/b\":8,\"~\":9,\"\\\\\":10,\"\\t\":11}" [r json.get o]

            # a name given twice is one member holding the last value
            r json.set o . {{"a":1,"a":2,"b":3}}
            assert_equal 2 [r json.objlen o]
            assert_equal {a b} [r json.objkeys o]
        }

        test "JSON.OBJLEN and OBJKEYS on objects larger than the packed caps (caps $caps)" {
            r flushall
            r json.set o . {{"small":{"x":1},"big":{}}}
            set names {}
            for {set i 0} {$i < 300} {incr i} {
                lappend names "member-$i"
                r json.set o ".big.member-$i" $i
                if {$i % 50 == 0} {
                    assert_equal [expr {$i + 1}] [r json.objlen o .big]
                    assert_equal $names [r json.objkeys o .big]
                }
            }
            assert_equal 300 [r json.objlen o .big]
            assert_equal $names [r json.objkeys o .big]
            assert_equal [list 1 300] [r json.objlen o $.*]
            assert_equal [list x $names] [r json.objkeys o $.*]
            # remove every other member, then all but three
            for {set i 0} {$i < 300} {incr i 2} { assert_equal 1 [r json.del o ".big.member-$i"] }
            assert_equal 150 [r json.objlen o .big]
            assert_equal [lmap n $names { if {[lindex [split $n -] 1] % 2 == 0} continue; set n }] [r json.objkeys o .big]
            for {set i 1} {$i < 295} {incr i 2} { assert_equal 1 [r json.del o ".big.member-$i"] }
            assert_equal {member-295 member-297 member-299} [r json.objkeys o .big]
            assert_equal {{"small":{"x":1},"big":{"member-295":295,"member-297":297,"member-299":299}}} [r json.get o]
        }

        # ---------------------------------------------------------------- JSON.CLEAR

        test "JSON.CLEAR legacy path (caps $caps)" {
            json_cmds_fixture
            # populate the array to be cleared
            assert_equal 3 [r json.arrappend wikipedia .children {"John"} {"Mary"} {"Tom"}]
            assert_equal {["John","Mary","Tom"]} [r json.get wikipedia .children]
            # clears and counts 1, 0 remain
            assert_equal 1 [r json.clear wikipedia .children]
            assert_equal 0 [r json.arrlen wikipedia .children]
            # clears an empty array
            assert_equal 0 [r json.clear wikipedia .children]
            assert_equal 0 [r json.arrlen wikipedia .children]
            # if the path does not exist, the command returns 0
            assert_equal 0 [r json.clear wikipedia .foo]
            assert_equal 0 [r json.clear wikipedia $.foo]
            # a false boolean is already clear
            assert_equal OK [r json.set wikipedia .foobool false]
            assert_equal 0 [r json.clear wikipedia .foobool]
            # scalars are cleared to false, 0 and the empty string; null is left alone
            assert_equal 1 [r json.clear wikipedia .isAlive]
            assert_equal 1 [r json.clear wikipedia .age]
            assert_equal 0 [r json.clear wikipedia .age]
            assert_equal 1 [r json.clear wikipedia $.firstName]
            assert_equal 0 [r json.clear wikipedia .spouse]
            assert_equal 1 [r json.clear wikipedia .weight]
            assert_equal 1 [r json.clear wikipedia .phoneNumbers]
            assert_equal {{"firstName":"","lastName":"Smith","age":0,"weight":0.0,"isAlive":false,"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[],"children":[],"spouse":null,"groups":{},"foobool":false}} [r json.get wikipedia]

            # error conditions
            assert_error "NONEXISTENT Document key does not exist" {r json.clear foo}
            assert_error "NONEXISTENT Document key does not exist" {r json.clear foo $.a}
            assert_error "WRONGTYPE Not a JSON document key" {r json.clear str_key}
            assert_error "WRONGTYPE Not a JSON document key" {r json.clear str_key $.a}
            assert_error "*wrong number of arguments*" {r json.clear wikipedia . extra}
            assert_error "*wrong number of arguments*" {r json.clear}

            # clear the wikipedia object entirely
            assert_equal 11 [r json.objlen wikipedia]
            assert_equal 1 [r json.clear wikipedia]
            assert_equal 0 [r json.objlen wikipedia]
            assert_equal {{}} [r json.get wikipedia]
            assert_equal 0 [r json.clear wikipedia]
            # the key stays
            assert_equal 1 [r exists wikipedia]
            assert_equal object [r json.type wikipedia]
        }

        test "JSON.CLEAR \$ path and wildcards (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {{"a":{}, "b":{"a": 1, "b": null, "c": true}, "c":1, "d":true, "e":null, "f":"d"}}]
            assert_equal OK [r json.set k2 . {[[], [0], [0,1], [0,1,2], 1, true, null, "d"]}]
            foreach {key path exp exp_new_val} {
                k1 $.*  4 {{"a":{},"b":{},"c":0,"d":false,"e":null,"f":""}}
                k2 $[*] 6 {[[],[],[],[],0,false,null,""]}
            } {
                assert_equal $exp [r json.clear $key $path]
                assert_equal $exp_new_val [r json.get $key .]
                # everything is clear now
                assert_equal 0 [r json.clear $key $path]
                assert_equal $exp_new_val [r json.get $key .]
            }
            assert_equal {(0)} [json_raw json.clear k1 $.zz]
            assert_equal {(0)} [json_raw json.clear k1 .zz]
            # a legacy wildcard clears every match
            r json.set k1 . {{"a":{},"b":{"a":1,"b":null,"c":true},"c":1,"d":true,"e":null,"f":"d","g":1.5,"h":[1,[2]]}}
            assert_equal {(6)} [json_raw json.clear k1 .*]
            assert_equal {{"a":{},"b":{},"c":0,"d":false,"e":null,"f":"","g":0.0,"h":[]}} [r json.get k1]
            assert_equal 1 [r json.clear k1 .]
            assert_equal 0 [r json.clear k1 .]
            assert_equal {{}} [r json.get k1]
            # a container counts once, whatever it holds
            r json.set k1 . {[1,[2,[3]],{"a":[4]},"x"]}
            assert_equal 4 [r json.clear k1 {.[*]}]
            assert_equal {[0,[],{},""]} [r json.get k1]
            r json.set k1 . {[1,[2,[3]],{"a":[4]},"x"]}
            assert_equal 1 [r json.clear k1 $]
            assert_equal {[]} [r json.get k1]
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.clear k1 {$[0]}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.clear k1 {.[0]}}
        }

        test "JSON.CLEAR value rules: integer to 0, double to 0.0, zero and null untouched (caps $caps)" {
            r flushall
            # value, cleared value, count
            foreach {val cleared count} {
                5                     0      1
                -1                    0      1
                0                     0      0
                -0                    0      0
                9223372036854775807   0      1
                1.5                   0.0    1
                -2.50                 0.0    1
                1e5                   0.0    1
                19.90                 0.0    1
                9223372036854775808   0.0    1
                1234567890123456789012345 0.0 1
                5e-324                0.0    1
                1e-320                0.0    1
                0.0                   0.0    0
                -0.0                  -0.0   0
                0e0                   0e0    0
                0.00                  0.00   0
                true                  false  1
                false                 false  0
                null                  null   0
                {"str"}               {""}   1
                {""}                  {""}   0
                {[1,2]}               {[]}   1
                {[]}                  {[]}   0
                {{"a":{"b":1}}}       {{}}   1
                {{}}                  {{}}   0
            } {
                r json.set s . $val
                assert_equal $count [r json.clear s] "value $val"
                assert_equal $cleared [r json.get s] "value $val"
                assert_equal 0 [r json.clear s] "value $val"

                # the same inside a document
                set stored $val
                if {$val eq "-0"} { set stored 0 }
                r json.set s . "{\"a\":\[1,$val\],\"b\":$val,\"c\":\"x\"}"
                assert_equal $count [r json.clear s {$.a[1]}] "value $val in an array"
                assert_equal "{\"a\":\[1,$cleared\],\"b\":$stored,\"c\":\"x\"}" [r json.get s] "value $val in an array"
                assert_equal $count [r json.clear s .b] "value $val in an object"
                assert_equal "{\"a\":\[1,$cleared\],\"b\":$cleared,\"c\":\"x\"}" [r json.get s] "value $val in an object"
            }
            assert_equal PONG [r ping]
        }

        test "JSON.CLEAR with nested matches clears the outermost - 2.5 (caps $caps)" {
            # The module reads freed memory here. Design 2.6 Q2: the outermost match wins and
            # every match counts as it would alone.
            r flushall
            r json.set k . {{"a":[{"a":[1,2,3]},5,6]}}
            assert_equal 2 [r json.clear k $..a]
            assert_equal {{"a":[]}} [r json.get k]
            r json.set k . {{"a":{"a":{"a":1,"b":"x"}}}}
            assert_equal 3 [r json.clear k $..a]
            assert_equal {{"a":{}}} [r json.get k]
            r json.set k . {{"a":{"a":{"a":1,"b":"x"}}}}
            assert_equal 3 [r json.clear k ..a]
            assert_equal {{"a":{}}} [r json.get k]
            r json.set k . {{"x":{"y":[1,{"z":"s"}]},"n":7}}
            assert_equal 6 [r json.clear k $..*]
            assert_equal {{"x":{},"n":0}} [r json.get k]
            assert_equal 0 [r json.clear k $..*]
            assert_equal PONG [r ping]
        }

        # ---------------------------------------------------------------- JSON.TYPE

        test "JSON.TYPE legacy path (caps $caps)" {
            json_cmds_fixture
            foreach {path type} {
                .              object
                .groups        object
                .phoneNumbers  array
                .children      array
                .isAlive       boolean
                .spouse        null
                .address.city  string
                .age           integer
                .weight        number
                {.phoneNumbers[0]}      object
                {.phoneNumbers[1].type} string
            } {
                assert_equal $type [r json.type wikipedia $path]
            }
            # the path defaults to the root
            assert_equal object [r json.type wikipedia]
            assert_equal {+object} [json_raw json.type wikipedia]
            # null if the document key does not exist
            assert_equal nil [json_raw json.type foo]
            assert_equal nil [json_raw json.type foo .a]
            assert_equal nil [json_raw json.type foo $.a]
            # null if the path does not exist
            assert_equal nil [json_raw json.type wikipedia .foo]
            assert_error "WRONGTYPE Not a JSON document key" {r json.type str_key}
            assert_error "WRONGTYPE Not a JSON document key" {r json.type str_key $.a}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.type wikipedia .phoneNumbers extra}
            assert_error "*wrong number of arguments*" {r json.type}
        }

        test "JSON.TYPE \$ path (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {{"a":1, "b":2.3, "c":"foo", "d":true, "e":null, "f":{}, "g":[]}}]
            assert_equal OK [r json.set k2 . {[1, 2.3, "foo", true, null, {}, []]}]
            assert_equal {integer number string boolean null object array} [r json.type k1 $.*]
            assert_equal {integer number string boolean null object array} [r json.type k2 {$[*]}]
            assert_equal {[+integer +number +string +boolean +null +object +array]} [json_raw json.type k1 $.*]
            assert_equal {[+integer +number +string +boolean +null +object +array]} [json_raw json.type k1 $..*]
            assert_equal {[+object]} [json_raw json.type k1 $]
            assert_equal {[+array]} [json_raw json.type k2 $]
            assert_equal {[]} [json_raw json.type k1 $.zz]
            # a legacy path replies with the type of the first value selected
            assert_equal {+integer} [json_raw json.type k1 .*]
            assert_equal {+integer} [json_raw json.type k2 {.[*]}]
            assert_equal {+array} [json_raw json.type k2 .]

            r json.set k3 . {[1,1.5,"s",true,false,null,{},[],[1],{"a":1},1e5,-0,9223372036854775808]}
            assert_equal {integer number string boolean boolean null object array array object number integer number} [r json.type k3 {$[*]}]
            assert_equal {integer number string boolean boolean null object array array object number integer number integer integer} [r json.type k3 $..*]
            assert_equal object [r json.type k3 {[6]}]
        }

        # ---------------------------------------------------------------- JSON.RESP

        test "JSON.RESP legacy path (caps $caps)" {
            json_cmds_fixture
            assert_equal {"John"} [json_resp json.resp wikipedia .firstName]
            assert_equal {+true} [json_resp json.resp wikipedia .isAlive]
            assert_equal {(27)} [json_resp json.resp wikipedia .age]
            assert_equal {nil} [json_resp json.resp wikipedia .spouse]
            assert_equal {"135.17"} [json_resp json.resp wikipedia .weight]
            assert_equal {[ARR]} [json_resp json.resp wikipedia .children]
            assert_equal {[OBJ]} [json_resp json.resp wikipedia .groups]
            assert_equal {[OBJ ["street" "21 2nd Street"] ["city" "New York"] ["state" "NY"] ["zipcode" "10021-3100"]]} [json_resp json.resp wikipedia .address]
            assert_equal {[ARR [OBJ ["type" "home"] ["number" "212 555-1234"]] [OBJ ["type" "office"] ["number" "646 555-4567"]]]} [json_resp json.resp wikipedia .phoneNumbers]

            # the path defaults to the root
            set root [r json.resp wikipedia]
            assert_equal 11 [llength $root]
            assert_equal "\{" [lindex $root 0]
            assert_equal {firstName John} [lindex $root 1]
            assert_equal {age 27} [lindex $root 3]
            assert_equal {weight 135.17} [lindex $root 4]
            assert_equal [list children [list "\["]] [lindex $root 8]
            assert_equal $root [r json.resp wikipedia .]

            # null if the document key does not exist
            assert_equal nil [json_resp json.resp foo]
            assert_equal nil [json_resp json.resp foo .a]
            assert_equal nil [json_resp json.resp foo $.a]
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.resp wikipedia .foo}
            assert_error "WRONGTYPE Not a JSON document key" {r json.resp str_key}
            assert_error "WRONGTYPE Not a JSON document key" {r json.resp str_key $.a}
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.resp wikipedia .phoneNumbers extra}
            assert_error "*wrong number of arguments*" {r json.resp}
        }

        test "JSON.RESP \$ path (caps $caps)" {
            json_cmds_fixture
            assert_equal {["John"]} [json_resp json.resp wikipedia $.firstName]
            assert_equal {[+true]} [json_resp json.resp wikipedia $.isAlive]
            assert_equal {[(27)]} [json_resp json.resp wikipedia $.age]
            assert_equal {[nil]} [json_resp json.resp wikipedia $.spouse]
            assert_equal {[]} [json_resp json.resp wikipedia $.foo]
            assert_equal {["135.17"]} [json_resp json.resp wikipedia $.weight]
            assert_equal {[[ARR]]} [json_resp json.resp wikipedia $.children]
            assert_equal {["21 2nd Street" "New York" "NY" "10021-3100"]} [json_resp json.resp wikipedia $.address.*]
            assert_equal {[[OBJ ["type" "home"] ["number" "212 555-1234"]] [OBJ ["type" "office"] ["number" "646 555-4567"]]]} [json_resp json.resp wikipedia $.phoneNumbers.*]
            assert_equal [list [r json.resp wikipedia]] [r json.resp wikipedia $]
        }

        test "JSON.RESP of every kind of value (caps $caps)" {
            r flushall
            # An integer is a RESP integer, any other number a bulk string of its double, a
            # boolean a status, null a null, a string a bulk string. An array is an array that
            # starts with the status "[", followed by its elements. An object is an array that
            # starts with the status of an opening brace, followed by one [name, value] pair for
            # each member. json_resp spells the two statuses ARR and OBJ.
            r json.set t . {{"a":1,"b":2.3,"c":"foo","d":true,"e":null,"f":{},"g":[]}}
            assert_equal {[OBJ ["a" (1)] ["b" "2.3"] ["c" "foo"] ["d" +true] ["e" nil] ["f" [OBJ]] ["g" [ARR]]]} [json_resp json.resp t]
            assert_equal {[[OBJ ["a" (1)] ["b" "2.3"] ["c" "foo"] ["d" +true] ["e" nil] ["f" [OBJ]] ["g" [ARR]]]]} [json_resp json.resp t $]
            assert_equal {[(1) "2.3" "foo" +true nil [OBJ] [ARR]]} [json_resp json.resp t $.*]
            assert_equal {[]} [json_resp json.resp t $.zz]

            r json.set t . {[1,1.5,1e5,19.90,-0.0,12345678901234567890,1e-7,"s",true,false,null,[],{},[[1]],{"a":{"b":[2]}}]}
            assert_equal {[ARR (1) "1.5" "100000.0" "19.9" "-0.0" "12345678901234567000.0" "1e-7" "s" +true +false nil [ARR] [OBJ] [ARR [ARR (1)]] [OBJ ["a" [OBJ ["b" [ARR (2)]]]]]]} [json_resp json.resp t]
            assert_equal {[(1) "1.5" "100000.0" "19.9" "-0.0" "12345678901234567000.0" "1e-7" "s" +true +false nil [ARR] [OBJ] [ARR [ARR (1)]] [OBJ ["a" [OBJ ["b" [ARR (2)]]]]]]} [json_resp json.resp t {$[*]}]
            assert_equal {[OBJ ["a" [OBJ ["b" [ARR (2)]]]]]} [json_resp json.resp t {[14]}]
            assert_equal {[[ARR [ARR (1)]]]} [json_resp json.resp t {$[13]}]
            assert_equal {"100000.0"} [json_resp json.resp t {[2]}]
        }

        test "JSON.RESP number formats (caps $caps)" {
            # A number that is not an int64 integer is converted to a double and printed in the
            # module's RESP form, which is not the text JSON.GET returns.
            r flushall
            foreach {val exp} {
                0                         (0)
                -0                        (0)
                -5                        (-5)
                123456789012345678        (123456789012345678)
                9223372036854775807       (9223372036854775807)
                -9223372036854775808      (-9223372036854775808)
                9223372036854775808       {"9223372036854776000.0"}
                -9223372036854775809      {"-9223372036854776000.0"}
                123456789012345678901234567890 {"1.2345678901234568e29"}
                1234567890123456789012345 {"1.2345678901234568e24"}
                0.0                       {"0.0"}
                -0.0                      {"-0.0"}
                0.1                       {"0.1"}
                2.50                      {"2.5"}
                19.90                     {"19.9"}
                100.0                     {"100.0"}
                1.0                       {"1.0"}
                1e5                       {"100000.0"}
                1e15                      {"1000000000000000.0"}
                1e16                      {"10000000000000000.0"}
                1e17                      {"100000000000000000.0"}
                1e21                      {"1e21"}
                1.5e+21                   {"1.5e21"}
                1e-7                      {"1e-7"}
                1e-07                     {"1e-7"}
                0.000001                  {"0.000001"}
                -1e-5                     {"-0.00001"}
                0.00001234                {"0.00001234"}
                123456789.123456789123    {"123456789.12345679"}
                1.7976931348623157e308    {"1.7976931348623157e308"}
                2.2250738585072014e-308   {"2.2250738585072014e-308"}
                5e-324                    {"5e-324"}
            } {
                r json.set big . $val
                assert_equal $exp [json_resp json.resp big] "value $val"
                assert_equal "\[$exp\]" [json_resp json.resp big $] "value $val"
            }
        }

        test "JSON.RESP with a legacy path that selects several values replies with the first one (caps $caps)" {
            # The module writes one reply for each selected value here, which breaks the
            # protocol. One reply is the only valid shape.
            r flushall
            r json.set t . {{"a":1,"b":2.3,"c":"foo"}}
            assert_equal {(1)} [json_resp json.resp t .*]
            assert_equal PONG [r ping]
            r json.set t . {[["x"],{"k":true},null]}
            assert_equal {[ARR "x"]} [json_resp json.resp t {.[*]}]
            assert_equal PONG [r ping]
        }

        # ---------------------------------------------------------------- RESP3

        test "JSON string, object, CLEAR, TYPE and RESP replies in RESP3 (caps $caps)" {
            json_cmds_fixture
            r json.set k1 . {{"a":{}, "c":{"a":"a", "b":"bb"}, "d":{"a":1, "b":"b", "c":{"a":3,"b":4}}, "e":1}}
            r json.set k2 . {{"a":1,"b":2.3,"c":"foo","d":true,"e":null,"f":{},"g":[]}}
            r hello 3
            # STRLEN
            assert_equal {null} [json_raw json.strlen foo .a]
            assert_equal {null} [json_raw json.strlen foo $.a]
            assert_equal {(4)} [json_raw json.strlen wikipedia .firstName]
            assert_equal {[(4)]} [json_raw json.strlen wikipedia $.firstName]
            assert_equal {[]} [json_raw json.strlen wikipedia $.bar]
            assert_equal {[null]} [json_raw json.strlen wikipedia $.age]
            assert_equal {[null (1) null]} [json_raw json.strlen k1 $.d.*]
            # STRAPPEND
            assert_equal {[null (2) null]} [json_raw json.strappend k1 $.d.* {"a"}]
            assert_equal {(3)} [json_raw json.strappend k1 .d.* {"a"}]
            assert_equal {[]} [json_raw json.strappend k1 $.zz {"a"}]
            # OBJLEN
            assert_equal {null} [json_raw json.objlen foo .a]
            assert_equal {null} [json_raw json.objlen foo $.a]
            assert_equal {(4)} [json_raw json.objlen wikipedia .address]
            assert_equal {[(0) (2) (3) null]} [json_raw json.objlen k1 $.*]
            assert_equal {[]} [json_raw json.objlen k1 $.zz]
            # OBJKEYS
            assert_equal {null} [json_raw json.objkeys foo .a]
            assert_equal {null} [json_raw json.objkeys foo $.a]
            assert_equal {null} [json_raw json.objkeys k1 .zz]
            assert_equal {[]} [json_raw json.objkeys k1 $.zz]
            assert_equal {[]} [json_raw json.objkeys k1 .a]
            assert_equal {["a" "b"]} [json_raw json.objkeys k1 .c]
            assert_equal {[[] ["a" "b"] ["a" "b" "c"] []]} [json_raw json.objkeys k1 $.*]
            # TYPE
            assert_equal {null} [json_raw json.type foo]
            assert_equal {null} [json_raw json.type foo $.a]
            assert_equal {null} [json_raw json.type k2 .zz]
            assert_equal {[]} [json_raw json.type k2 $.zz]
            assert_equal {+integer} [json_raw json.type k2 .a]
            assert_equal {[+integer +number +string +boolean +null +object +array]} [json_raw json.type k2 $.*]
            # RESP
            assert_equal {null} [json_resp json.resp foo]
            assert_equal {null} [json_resp json.resp foo $.a]
            assert_equal {[]} [json_resp json.resp k2 $.zz]
            assert_equal {[OBJ ["a" (1)] ["b" "2.3"] ["c" "foo"] ["d" +true] ["e" null] ["f" [OBJ]] ["g" [ARR]]]} [json_resp json.resp k2]
            assert_equal {[(1) "2.3" "foo" +true null [OBJ] [ARR]]} [json_resp json.resp k2 $.*]
            assert_equal {null} [json_resp json.resp k2 .e]
            # CLEAR
            assert_equal {(4)} [json_raw json.clear k2 $.*]
            assert_equal {(0)} [json_raw json.clear k2 $.zz]
            # errors
            assert_equal {-WRONGTYPE JSON element is not a string} [json_raw json.strlen wikipedia .age]
            assert_equal {-NONEXISTENT JSON path does not exist} [json_raw json.objlen wikipedia .foo]
            assert_equal {-WRONGTYPE Not a JSON document key} [json_raw json.type str_key]
            r hello 2
            assert_equal {{"a":0,"b":0.0,"c":"","d":false,"e":null,"f":{},"g":[]}} [r json.get k2]
        }
    }
    json_set_caps {4096 128}
}
