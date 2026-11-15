# JSON (built-in module): paths. Member names with quotes and escapes, indexes, slices, unions,
# wildcards, recursive descent, filters, result order and de-duplication, the insert, update
# and delete modes, the path limits, and the deliberate changes B4, B5 and B6.
#
# Ported from the valkey-json module's tst/integration/test_json_basic.py. Expected values are
# the module's (cases that are not in that file were checked against the module), except where
# VALKEY_JSON_DESIGN.md section 2.3 lists a deliberate change. Those tests carry the change
# number in their name.

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

# {"a":{"a":...1...}} with $depth levels of "a".
proc json_path_nested {depth} {
    return "[string repeat "{\"a\":" $depth]1[string repeat "}" $depth]"
}

# The test client writes strings to the socket byte for byte, so text outside ASCII has to be
# converted to UTF-8 first (the built-in module rejects JSON text that is not UTF-8, B1).
proc json_path_utf8 {str} {
    return [encoding convertto utf-8 $str]
}

set ::json_path_wikipedia {{"firstName":"John","lastName":"Smith","age":27,"weight":135.17,"isAlive":true,"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[{"type":"home","number":"212 555-1234"},{"type":"office","number":"646 555-4567"}],"children":[],"spouse":null,"groups":{}}}

set ::json_path_organism {{"heavy_animal":200,"plants":[{"name":"Cactus","height":120,"weight":90},{"name":"Ghost Plant","height":"Unknown","weight":"Unknown"},{"name":"Redwood","height":4200,"weight":50000}],"animals":[{"name":"Platypus","length":24,"weight":5},{"fish":[{"name":"Bass","length":34,"weight":5},{"name":"Swordfish","length":177,"weight":200}]},{"mammals":[{"name":"Platypus","length":24,"weight":5},{"name":"Horse","height":68,"weight":660},{"primates":[{"name":"Monkey","height":18,"weight":30},{"name":"Baboon","height":26,"weight":50},{"apes":[{"name":"Chimpanzee","height":66,"weight":130},{"name":"Gorilla","height":66,"weight":400},{"name":"Orangutan","height":70,"weight":300}]}]}]}]}}

set ::json_path_store {
{
  "store": {
    "books": [
      {"category": "reference", "author": "Nigel Rees", "title": "Sayings of the Century", "price": 8.95},
      {"category": "fiction", "author": "Evelyn Waugh", "title": "Sword of Honour", "price": 12.99,
       "movies": [{"title": "Sword of Honour", "realisator": {"first_name": "Bill", "last_name": "Anderson"}}]},
      {"category": "fiction", "author": "Herman Melville", "title": "Moby Dick", "isbn": "0-553-21311-3", "price": 9},
      {"category": "fiction", "author": "J. R. R. Tolkien", "title": "The Lord of the Rings", "isbn": "0-395-19395-8", "price": 22.99}
    ],
    "bicycle": {"color": "red", "price": 19.95}
  }
}
}

set ::json_path_store8 {
{
  "store": {
    "books": [
      {"category": "reference", "author": "Nigel Rees", "title": "Sayings of the Century", "price": 8.95, "in-stock": true},
      {"category": "fiction", "author": "Evelyn Waugh", "title": "Sword of Honour", "price": 12.99, "in-stock": true,
       "movies": [{"title": "Sword of Honour - movie", "realisator": {"first_name": "Bill", "last_name": "Anderson"}}]},
      {"category": "fiction", "author": "Herman Melville", "title": "Moby Dick", "isbn": "0-553-21311-3", "price": 9, "in-stock": false},
      {"category": "fiction", "author": "J. R. R. Tolkien", "title": "The Lord of the Rings", "isbn": "0-115-03266-2", "price": 22.99, "in-stock": true},
      {"category": "reference", "author": "William Jr. Strunk", "title": "The Elements of Style", "price": 6.99, "in-stock": false},
      {"category": "fiction", "author": "Leo Tolstoy", "title": "Anna Karenina", "price": 22.99, "in-stock": true},
      {"category": "reference", "author": "Sarah Janssen", "title": "The World Almanac and Book of Facts 2021", "isbn": "0-925-23305-2", "price": 10.69, "in-stock": false},
      {"category": "reference", "author": "Kate L. Turabian", "title": "Manual for Writers of Research Papers", "isbn": "0-675-16695-1", "price": 8.59, "in-stock": true}
    ],
    "bicycle": {"color": "red", "price": 19.64, "in-stock": true}
  }
}
}

start_server {tags {"json"}} {
    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall
        r config set json.max-path-limit 128
        r config set json.max-document-size 0

        # ---------------------------------------------------------------------------------
        # Member names
        # ---------------------------------------------------------------------------------

        test "JSON path: member names that contain escaped characters (caps $caps)" {
            assert_equal OK [r json.set k1 . {{"a\\a":1, "b\tb":2, "c\nc":3, "d\rd":4, "e\be":5, "f\"f":6, "":7, "'":8}}]
            assert_equal OK [r json.set k2 . {{"key\u0000":"value\u0000", "key\u001F":"value\u001F"}}]
            foreach {key path exp} {
                k1 {$["a\\a"]} {[1]}
                k1 {$['a\a']} {[1]}
                k1 {$["b\tb"]} {[2]}
                k1 {$['b\tb']} {[2]}
                k1 {$["c\nc"]} {[3]}
                k1 {$['c\nc']} {[3]}
                k1 {$["d\rd"]} {[4]}
                k1 {$['d\rd']} {[4]}
                k1 {$["e\be"]} {[5]}
                k1 {$['e\be']} {[5]}
                k1 {$["f\"f"]} {[6]}
                k1 {$['f"f']} {[6]}
                k1 {$[""]} {[7]}
                k1 {$['']} {[7]}
                k1 {$["'"]} {[8]}
                k1 {$['\'']} {[8]}
                k2 {$["key\u0000"]} {["value\u0000"]}
                k2 {$["key\u001F"]} {["value\u001F"]}
                k1 {$["a\u005ca"]} {[1]}
                k1 {$["e\u0008e"]} {[5]}
                k1 {$["\/"]} {[]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
            # The same names through a legacy path.
            assert_equal 2 [r json.get k1 {.["b\tb"]}]
            assert_equal 3 [r json.get k1 {["c\nc"]}]
            assert_equal {{"a\\a":1,"b\tb":2,"c\nc":3,"d\rd":4,"e\be":5,"f\"f":6,"":7,"'":8}} [r json.get k1]
        }

        test "JSON path: writing and deleting members whose names need escapes (caps $caps)" {
            assert_equal OK [r json.set k1 {$["g\"g"]} 9]
            assert_equal OK [r json.set k1 {$['h\'h']} 10]
            assert_equal OK [r json.set k1 {$["i\ti"]} 11]
            assert_equal OK [r json.set k1 {$["j\u0041j"]} 12]
            assert_equal OK [r json.set k1 {$["l\\l"]} 13]
            assert_equal {[9,10,12]} [r json.get k1 {$["g\"g","h'h","jAj"]}]
            assert_equal 1 [r json.del k1 {$["i\ti"]}]
            assert_equal 1 [r json.del k1 {$['f"f']}]
            assert_equal {[14]} [r json.numincrby k1 {$["l\\l"]} 1]
            assert_equal {{"a\\a":1,"b\tb":2,"c\nc":3,"d\rd":4,"e\be":5,"":7,"'":8,"g\"g":9,"h'h":10,"jAj":12,"l\\l":14}} [r json.get k1]
        }

        test "JSON path: escaped quotes in member names are serialised escaped (caps $caps)" {
            assert_equal OK [r json.set k1 . {{"\"a":1, "\"b":2}}]
            assert_equal {{"\"a":1,"\"b":2}} [r json.get k1 .]
            assert_equal {{"\"a": 1,"\"b": 2}} [r json.get k1 space { } .]
        }

        test "JSON path: member names outside ASCII, raw and as \\u escapes (caps $caps)" {
            set e [json_path_utf8 "\u00e9"]
            set j [json_path_utf8 "\u65e5\u672c"]
            assert_equal OK [r json.set o . "{\"$e\":3,\"${e}2\":8,\"$j\":{\"$e\":\"$j\"},\"k\":\"\\u00e9\"}"]
            assert_equal {[3]} [r json.get o "\$\[\"$e\"\]"]
            assert_equal {[3]} [r json.get o "\$\['$e'\]"]
            assert_equal {[3]} [r json.get o "\$.$e"]
            assert_equal {[8]} [r json.get o "\$.${e}2"]
            assert_equal 3 [r json.get o ".$e"]
            assert_equal "\[\"$j\"\]" [r json.get o "\$.$j.$e"]
            assert_equal "\[\"$j\"\]" [r json.get o "\$\[\"$j\"\]\[\"$e\"\]"]
            # A \u escape in a double-quoted name selects the same member.
            assert_equal {[3]} [r json.get o {$["\u00e9"]}]
            assert_equal "\[\"$j\"\]" [r json.get o {$["\u65e5\u672c"]["\u00e9"]}]
            # ...but not in a single-quoted name.
            assert_equal {[]} [r json.get o {$['\u00E9']}]
            # A string value written with an escape is stored and returned as UTF-8.
            assert_equal "\[\"$e\"\]" [r json.get o {$.k}]
            assert_equal "\[\"$j\"\]" [r json.get o "\$..\[?(@.$e==\"$j\")\].$e"]
            assert_equal OK [r json.set o "\$.$j.n$e" 1]
            assert_equal "\[{\"$e\":\"$j\",\"n$e\":1}\]" [r json.get o "\$.$j"]
            assert_equal 1 [r json.del o "\$.$e"]
            assert_equal [list [list "${e}2" $j k]] [r json.objkeys o {$}]
            r del o
        }

        test "JSON path: unquoted member names may hold any symbol except a terminator (caps $caps)" {
            assert_equal OK [r json.set k1 . {{
                "%x22key%x22":1, "+2":2, "-3":3, "/4":4,
                ")5":5, "6)":6, "(7":7, "8(":8,
                "]9":9, "10]":10, "[11":11, "12[":12,
                ">13":13, "14>":14, "<15":15, "16<":16,
                "=17":17, "18=":18, "!19":19, "20!":20,
                "21.21":21
            }}]
            foreach {path exp} {
                {$.%x22key%x22} {[1]}
                {$.+2} {[2]}
                {$.-3} {[3]}
                {$./4} {[4]}
                {$.6)} {[]}
                {$.8(} {[]}
                {$.10]} {[]}
                {$["6)"]} {[6]}
                {$["8("]} {[8]}
                {$["10]"]} {[10]}
                {$["12["]} {[12]}
                {$["14>"]} {[14]}
                {$["16<"]} {[16]}
                {$["18="]} {[18]}
                {$["20!"]} {[20]}
                {$["21.21"]} {[21]}
                {$.12[} {[]}
                {$.14>} {[]}
                {$.16<} {[]}
                {$.18=} {[]}
                {$.20!} {[]}
                {$.21.21} {[]}
            } {
                assert_equal $exp [r json.get k1 $path] "path $path"
            }
            # Unquoted object member cannot start with a character that is a member name terminator.
            foreach path {{$.)5} {$.]7} {$.]9} {$.>13} {$.<15} {$.=17} {$.!19}} {
                assert_error "SYNTAXERR Invalid object member name" {r json.get k1 $path}
            }
            assert_error "SYNTAXERR Invalid JSON path" {r json.get k1 {$.[11}}
        }

        test "JSON path: a number as a member name, and \$ without a dot (caps $caps)" {
            r json.set k1 . {{"0":0,"1":true,"2":"string","3":[0.0,1.1,2.0],"4":{},"5":{"0":1,"1":"string"}}}
            foreach {path exp} {
                {$.0} {[0]}
                {$.3} {[[0.0,1.1,2.0]]}
                {$.5.1} {["string"]}
                {$5.0} {[1]}
                {$0} {[0]}
                {$["5"]["0"]} {[1]}
                {$.3[1]} {[1.1]}
                {$a.b} {[]}
            } {
                assert_equal $exp [r json.get k1 $path] "path $path"
            }
        }

        test "JSON path: root forms and malformed ends (caps $caps)" {
            r json.set o . {{"x":{"y":[10,20]},"a b":4,"a.b":5}}
            assert_equal {[{"x":{"y":[10,20]},"a b":4,"a.b":5}]} [r json.get o {$}]
            assert_equal {{"x":{"y":[10,20]},"a b":4,"a.b":5}} [r json.get o .]
            assert_equal {{"x":{"y":[10,20]},"a b":4,"a.b":5}} [r json.get o {}]
            assert_equal {{"x":{"y":[10,20]},"a b":4,"a.b":5}} [r json.get o]
            foreach {path exp} {
                {$["a b"]} {[4]}
                {$.a b} {[]}
                {$["a.b"]} {[5]}
                {$.x.y[1]} {[20]}
                {$["x"]["y"][1]} {[20]}
                {$.x["y"][-1]} {[20]}
                {.x.y[1]} 20
                {["x"]["y"][1]} 20
                {x.y[1]} 20
                {$$} {[]}
                {$.x..} {[{"y":[10,20]},[10,20]]}
                {$...x} {[]}
                {$["x]} {[]}
            } {
                assert_equal $exp [r json.get o $path] "path $path"
            }
            assert_error "SYNTAXERR Invalid object member name" {r json.get o {$.}}
            assert_error "SYNTAXERR Invalid object member name" {r json.get o {$.x.}}
            assert_error "SYNTAXERR Expression token cannot be empty" {r json.get o {$[}}
            assert_error "SYNTAXERR Invalid JSON path" {r json.get o {$["x"}}
        }

        # ---------------------------------------------------------------------------------
        # Indexes, slices, unions, wildcards
        # ---------------------------------------------------------------------------------

        test "JSON path: array indexes, negative and out of range (caps $caps)" {
            r json.set a . {[0,1,2,3,4,5,6,7,8,9]}
            foreach {path exp} {
                {$[-1]} {[9]}
                {$[-10]} {[0]}
                {$[-11]} {[]}
                {$[10]} {[]}
                {$[ 1 ]} {[1]}
                {$[01]} {[1]}
                {$[+1]} {[1]}
                {$[99999999999999999999]} {[]}
                {$[4294967296]} {[]}
                {$[4294967297]} {[]}
                {$[-4294967295]} {[]}
                {.[-1]} 9
                {.[0]} 0
            } {
                assert_equal $exp [r json.get a $path] "path $path"
            }
            # A legacy path that names an element outside the array is an error.
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.get a {.[10]}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.get a {.[-11]}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.get a {.[4294967297]}}
            assert_error "SYNTAXERR Invalid JSON path" {r json.get a {$[1.5]}}
        }

        test "JSON path: an array index that is not a number is a syntax error (caps $caps)" {
            r json.set wikipedia . $::json_path_wikipedia
            foreach path {{.phoneNumbers[]} {.phoneNumbers[x]} {$.phoneNumbers[]} {$.phoneNumbers[x]} {$.phoneNumbers[ ]} {$.phoneNumbers[-]}} {
                assert_error "SYNTAXERR Array index is not a number" {r json.get wikipedia $path}
            }
            assert_error "SYNTAXERR Invalid JSON path" {r json.get wikipedia {$.phoneNumbers[1x]}}
        }

        test "JSON path: slices with and without a step (caps $caps)" {
            r json.set a . {[0,1,2,3,4,5,6,7,8,9]}
            foreach {path exp} {
                {$[0:3]} {[0,1,2]}
                {$[:3]} {[0,1,2]}
                {$[7:]} {[7,8,9]}
                {$[:]} {[0,1,2,3,4,5,6,7,8,9]}
                {$[-3:]} {[7,8,9]}
                {$[:-7]} {[0,1,2]}
                {$[ 1 : 3 ]} {[1,2]}
                {$[0:10:3]} {[0,3,6,9]}
                {$[1:8:2]} {[1,3,5,7]}
                {$[::2]} {[0,2,4,6,8]}
                {$[5:100]} {[5,6,7,8,9]}
                {$[-100:2]} {[0,1]}
                {$[3:1]} {[]}
                {$[4294967296:]} {[]}
                {$[:4294967298]} {[0,1,2,3,4,5,6,7,8,9]}
                {$[8:2:-2]} {[8,6,4]}
                {$[-1:-5:-1]} {[9,8,7,6]}
                {$[::-1]} {[]}
                {$[0:6:2147483647]} {[0]}
                {$[0:6:9223372036854775807]} {[0]}
                {.[0:2]} 0
                {.[7:]} 7
            } {
                assert_equal $exp [r json.get a $path] "path $path"
            }
            assert_error "SYNTAXERR Step in the slice cannot be zero" {r json.get a {$[0:4:0]}}
            assert_error "SYNTAXERR Step in the slice cannot be zero" {r json.del a {$[0:4:0]}}
            # A slice cannot be combined with other selectors in a union.
            assert_error "SYNTAXERR Invalid JSON path" {r json.get a {$[1,1:3,-1]}}
            assert_error "SYNTAXERR Invalid JSON path" {r json.get a {$[0:2,5]}}
            assert_equal {[0,1,2,3,4,5,6,7,8,9]} [r json.get a]
        }

        test "JSON path: slices select elements for writes (caps $caps)" {
            r json.set a . {[0,1,2,3,4,5,6,7,8,9]}
            assert_equal OK [r json.set a {$[1:8:3]} {"s"}]
            assert_equal {[0,"s",2,3,"s",5,6,"s",8,9]} [r json.get a]
            assert_equal {[18,19]} [r json.numincrby a {$[-2:]} 10]
            assert_equal 3 [r json.del a {$[1:8:3]}]
            assert_equal {[0,2,3,5,6,18,19]} [r json.get a]
            assert_equal 2 [r json.del a {$[:2]}]
            assert_equal {[3,5,6,18,19]} [r json.get a]
            assert_equal 0 [r json.del a {$[3:1]}]
            assert_equal 5 [r json.del a {$[:]}]
            assert_equal {[]} [r json.get a]
        }

        test "JSON path: slice arithmetic is 64-bit - a step of 2^31 or more selects the first element only (caps $caps)" {
            # The module's loop counter is an int, so these steps wrap or make it spin for
            # ever. Here the step is taken as written: after the first element the next
            # one is already outside the slice.
            r json.set a . {[0,1,2,3,4,5]}
            foreach {path exp} {
                {$[0:4:2147483648]}             {[0]}
                {$[0:4:4294967296]}             {[0]}
                {$[0:4:8589934592]}             {[0]}
                {$[0:4:12884901888]}            {[0]}
                {$[0:4:9223372036854775807]}    {[0]}
                {$[0:4:9223372036854775808]}    {[0]}
                {$[0:4:18446744073709551616]}   {[0]}
                {$[0:4:18446744073709551617]}   {[0]}
                {$[2:6:4294967296]}             {[2]}
                {$[5:0:-2147483648]}            {[5]}
                {$[5:0:-4294967296]}            {[5]}
                {$[5:0:-9223372036854775808]}   {[5]}
                {$[5:0:-18446744073709551616]}  {[5]}
                {$[0:4:2147483647]}             {[0]}
                {$[0:4:2147483649]}             {[0]}
                {$[0:4:4294967295]}             {[0]}
                {$[0:4:6442450945]}             {[0]}
                {$[5:0:-2147483649]}            {[5]}
                {$[5:0:-4294967295]}            {[5]}
                {$[0:4:3]}                      {[0,3]}
            } {
                assert_equal $exp [r json.get a $path] "JSON.GET $path"
                assert_equal PONG [r ping]
            }
            # A step above 2^32 whose low 32 bits are a positive int does not hang the module:
            # it uses those 32 bits. That result is reproduced (checked against the module).
            foreach {path exp} {
                {$[0:6:4294967297]}             {[0,1,2,3,4,5]}
                {$[0:6:4294967298]}             {[0,2,4]}
                {$[0:6:4294967299]}             {[0,3]}
                {$[0:6:8589934593]}             {[0,1,2,3,4,5]}
                {$[5:0:-4294967297]}            {[5,4,3,2,1]}
            } {
                assert_equal $exp [r json.get a $path] "JSON.GET $path"
            }
            assert_error "SYNTAXERR Step in the slice cannot be zero" {r json.get a {$[0:4:0]}}
            assert_equal {} [r json.get nokey {$[0:4:4294967296]}]
            # Slice bounds beyond 2^64 are clamped like any other bound out of range.
            foreach {path exp} {
                {$[18446744073709551616:]}                          {[]}
                {$[:18446744073709551616]}                          {[0,1,2,3,4,5]}
                {$[18446744073709551617:18446744073709551619]}      {[]}
                {$[-18446744073709551617:2]}                        {[0,1]}
                {$[0:18446744073709551618:2]}                       {[0,2,4]}
            } {
                assert_equal $exp [r json.get a $path] "JSON.GET $path"
            }
        }

        test "JSON path: an index at or beyond 2^64 is out of range, it does not wrap (caps $caps)" {
            # The module reduces such an index modulo 2^64, so there 2^64 names element 0 and
            # 2^64 + 1 names element 1. Here it is an index past the end, like 10.
            r json.set a . {[0,1,2,3,4,5]}
            foreach index {18446744073709551616 18446744073709551617 18446744073709551621 99999999999999999999
                           -18446744073709551616 -18446744073709551615 9223372036854775807 9223372036854775808
                           -9223372036854775808 -9223372036854775809} {
                assert_equal {[]} [r json.get a "\$\[$index\]"] "index $index"
                assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.get a ".\[$index\]"}
                assert_equal {} [r json.type a "\$\[$index\]"]
                assert_error "OUTOFBOUNDARIES*" {r json.set a "\$\[$index\]" 7}
                assert_error "OUTOFBOUNDARIES*" {r json.set a ".\[$index\]" 7}
                assert_error "OUTOFBOUNDARIES*" {r json.numincrby a "\$\[$index\]" 1}
                assert_error "OUTOFBOUNDARIES*" {r json.del a "\$\[$index\]"}
                assert_error "OUTOFBOUNDARIES*" {r json.del a ".\[$index\]"}
            }
            # In a union the out-of-range index selects nothing and the others still do.
            assert_equal {[1]} [r json.get a {$[10,18446744073709551616,1]}]
            assert_equal {[1]} [r json.get a {$[1,18446744073709551617]}]
            assert_equal {[0,1,2,3,4,5]} [r json.get a]
        }

        test "JSON path: JSON.NUMINCRBY through a slice step of 2^32 changes and replies the first element only (caps $caps)" {
            r json.set a . {[0,1,2,3,4,5]}
            assert_equal {[10]} [r json.numincrby a {$[0:4:4294967296]} 10]
            assert_equal {[10,1,2,3,4,5]} [r json.get a]
            assert_equal {[15]} [r json.nummultby a {$[5:0:-4294967296]} 3]
            assert_equal OK [r json.set a {$[1:6:8589934592]} {"set"}]
            assert_equal {[10,"set",2,3,4,15]} [r json.get a]
            r del a
        }

        test "JSON path: unions of indexes and of member names (caps $caps)" {
            r json.set a . {[0,1,2,3,4,5,6,7,8,9]}
            r json.set o . {{"x":{"y":[10,20]},"a b":4,"n":null}}
            r json.set wikipedia . $::json_path_wikipedia
            r json.set organism . $::json_path_organism
            foreach {key path exp} {
                a {$[2,0]} {[2,0]}
                a {$[0,0,1]} {[0,0,1]}
                a {$[9,-1,20,-20]} {[9,9]}
                a {$[0,4294967297]} {[0]}
                a {.[2,0]} 2
                o {$["x","a b"]} {[{"y":[10,20]},4]}
                o {$["x","nope","a b","x"]} {[{"y":[10,20]},4,{"y":[10,20]}]}
                o {$.x.y[0,0]} {[10,10]}
                wikipedia {$.["firstName","lastName"]} {["John","Smith"]}
                wikipedia {$.[ "firstName", "lastName" ]} {["John","Smith"]}
                wikipedia {$.address.[ "street", "city", "state", "zipcode" ]} {["21 2nd Street","New York","NY","10021-3100"]}
                organism {$.animals["2","2"].mammals..weight} {[]}
                organism {$.animals["2","Junk"]} {[]}
                organism {$.animals["Junk","Junk"]} {[]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
        }

        test "JSON path: wildcards on arrays and objects (caps $caps)" {
            foreach {key path exp} {
                a {$[*]} {[0,1,2,3,4,5,6,7,8,9]}
                a {$.*} {[0,1,2,3,4,5,6,7,8,9]}
                a {$[ * ]} {[0,1,2,3,4,5,6,7,8,9]}
                a {.[*]} 0
                a {.*} 0
                a {$.*.*} {[]}
                o {$.*} {[{"y":[10,20]},4,null]}
                o {$[*]} {[{"y":[10,20]},4,null]}
                o {$.x.*} {[[10,20]]}
                o {$.x.*.*} {[10,20]}
                o {$.*.*} {[[10,20]]}
                o {$.*.y[1]} {[20]}
                o {.*} {{"y":[10,20]}}
                wikipedia {$.phoneNumbers[*].type} {["home","office"]}
                wikipedia {$.address.*} {["21 2nd Street","New York","NY","10021-3100"]}
                wikipedia {$.children[*]} {[]}
                wikipedia {$.groups.*} {[]}
                wikipedia {$.age.*} {[]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
            assert_error "NONEXISTENT JSON path does not exist" {r json.get wikipedia {.children[*]}}
        }

        test "JSON path: result order is selector order; reads keep duplicates except after a recursive descent (caps $caps)" {
            assert_equal {[2,0]} [r json.get a {$[2,0]}]
            assert_equal {[10,10]} [r json.get o {$.x.y[0,0]}]
            assert_equal {[10]} [r json.get o {$..y[0,0]}]
            assert_equal {[{"y":[10,20]}]} [r json.get o {$..["x","x"]}]
            assert_equal {integer integer} [r json.type a {$[3,3]}]
            assert_equal {integer} [r json.type a {$..[3,3]}]
            # Several paths in one JSON.GET are evaluated one by one.
            assert_equal {{"$[2,0]":[2,0],"$[0,0]":[0,0]}} [r json.get a {$[2,0]} {$[0,0]}]
            r del a o
        }

        # ---------------------------------------------------------------------------------
        # Filters
        # ---------------------------------------------------------------------------------

        test "JSON path: filter expressions (caps $caps)" {
            assert_equal OK [r json.set k1 . $::json_path_store]
            assert_equal OK [r json.set k2 . {[1,2,3,4,5]}]
            assert_equal OK [r json.set k3 . {[true,false,true,false,null,1,2,3,4]}]
            assert_equal OK [r json.set k4 . {{"books": [{"price":5,"sold":true,"in-stock":true,"title":"foo"}, {"price":15,"sold":false,"title":"abc"}]}}]
            assert_equal OK [r json.set k5 . {[1,2,3,4,5,6,7,8,9]}]
            foreach {key path exp} {
                k1 {$.store.books[?(@.isbn)].price} {[9,22.99]}
                k1 {$.store.books[?( @.isbn )].price} {[9,22.99]}
                k1 {$.store.books[?(@["isbn"])]["price"]} {[9,22.99]}
                k1 {$.store.books[?(@[ "isbn" ])][ "price" ]} {[9,22.99]}
                k1 {$.store.books[?(@['isbn'])]['price']} {[9,22.99]}
                k1 {$.store.books[?(@.category == "reference")].price} {[8.95]}
                k1 {$.store.books[?(@.["category"] == "fiction")].price} {[12.99,9,22.99]}
                k1 {$.store.books[?(@.price<1.0E1)].price} {[8.95,9]}
                k1 {$.store.books[?(@["price"]<1.0E1)]["price"]} {[8.95,9]}
                k1 {$.store.books[?(@.["price"]<1.0E1)]["price"]} {[8.95,9]}
                k1 {$.store.books[?(@['price']<1.0E1)]['price']} {[8.95,9]}
                k1 {$.store.books[?(@.price>-1.23e1&&@.price<1.0E1)].price} {[8.95,9]}
                k1 {$.store.books[?(@["price"]>-1.23e1&&@["price"]<1.0E1)]["price"]} {[8.95,9]}
                k1 {$.store.books[?(@.["price"]>-1.23e1&&@.["price"]<1.0E1)].["price"]} {[8.95,9]}
                k1 {$.store.books[?(@["price"] > -1.23e1 && @["price"] < 1.0E1)]["price"]} {[8.95,9]}
                k1 {$.store.books[?(@.price==22.99)].title} {["The Lord of the Rings"]}
                k1 {$.store.books[?(@["price"]==22.99)].title} {["The Lord of the Rings"]}
                k1 {$.store.books[?(@.price<10.0&&@.isbn)].price} {[9]}
                k1 {$.store.books[?(@.price<9||@.price>20)].price} {[8.95,22.99]}
                k1 {$.store.books[?(@.price<9||@.price>10&&@.isbn)].price} {[8.95,22.99]}
                k1 {$.store.books[?((@.price<9||@.price>10)&&@.isbn)].price} {[22.99]}
                k1 {$.store.books[?((@.price < 9 || @.price>10) && @.isbn)].price} {[22.99]}
                k1 {$.store.books[?((@["price"]<9||@["price"]>10)&&@["isbn"])]["price"]} {[22.99]}
                k1 {$.store.books[?((@["price"] < 9 || @["price"] > 10) && @["isbn"])]["price"]} {[22.99]}
                k2 {$.*.[?(@>2)]} {[3,4,5]}
                k2 {$.*.[?(@ > 2)]} {[3,4,5]}
                k2 {$.*[?(@>2)]} {[3,4,5]}
                k2 {$[*][?(@>2)]} {[3,4,5]}
                k2 {$[ * ][?( @ > 2 )]} {[3,4,5]}
                k2 {$.*[?(@ == 3)]} {[3]}
                k2 {$.*[?(@ != 3)]} {[1,2,4,5]}
                k3 {$.*.[?(@==true)]} {[true,true]}
                k3 {$[*][?(@==true)]} {[true,true]}
                k3 {$[*][?(@ == true)]} {[true,true]}
                k3 {$.*.[?(@>1)]} {[2,3,4]}
                k3 {$.*.[?( @ > 1 ) ]} {[2,3,4]}
                k3 {$[*][?(@>1)]} {[2,3,4]}
                k3 {$[ * ][?( @ > 1 ) ]} {[2,3,4]}
                k4 {$.books[?(@.price>1&&@.price<20&&@.in-stock)]} {[{"price":5,"sold":true,"in-stock":true,"title":"foo"}]}
                k4 {$.books[?(@['price']>1&&@.price<20&&@["in-stock"])]} {[{"price":5,"sold":true,"in-stock":true,"title":"foo"}]}
                k4 {$.books[?((@.price>1&&@.price<20)&&(@.sold==false))]} {[{"price":15,"sold":false,"title":"abc"}]}
                k4 {$["books"][?((@["price"]>1&&@["price"]<20)&&(@["sold"]==false))]} {[{"price":15,"sold":false,"title":"abc"}]}
                k5 {$.*[?(@ > 7 || @ < 3)]} {[8,9,1,2]}
                k5 {$.*[?(@ < 3 || @ > 7)]} {[1,2,8,9]}
                k5 {$.*[?(@ > 3 && @ < 7)]} {[4,5,6]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
        }

        test "JSON path: every filter operator on numbers, strings, booleans and null (caps $caps)" {
            r json.set f . {{"n":[10,20,20.5,-3,0],"s":["apple","banana","cherry",""],"b":[true,false,null,1,"true"],"o":[{"v":1,"t":"x"},{"v":2,"t":"y","e":null},{"v":3},{"t":"z","flag":false}]}}
            foreach {path exp} {
                {$.n[?(@>10)]} {[20,20.5]}
                {$.n[?(@>=10)]} {[10,20,20.5]}
                {$.n[?(@<10)]} {[-3,0]}
                {$.n[?(@<=10)]} {[10,-3,0]}
                {$.n[?(@==20)]} {[20]}
                {$.n[?(@!=20)]} {[10,20.5,-3,0]}
                {$.n[?(@==20.5)]} {[20.5]}
                {$.n[?(@==-3)]} {[-3]}
                {$.n[?(@>-4&&@<1)]} {[-3,0]}
                {$.n[?(@==10||@==0)]} {[10,0]}
                {$.n[?(@>5&&@<15||@==0)]} {[10,0]}
                {$.n[?(@>5&&(@<15||@==20))]} {[10,20]}
                {$.n[?(10<@)]} {[20,20.5]}
                {$.n[?(@=="10")]} {[]}
                {$.s[?(@=="banana")]} {["banana"]}
                {$.s[?(@=='banana')]} {["banana"]}
                {$.s[?(@!="banana")]} {["apple","cherry",""]}
                {$.s[?(@<"banana")]} {["apple",""]}
                {$.s[?(@<="banana")]} {["apple","banana",""]}
                {$.s[?(@>"banana")]} {["cherry"]}
                {$.s[?(@>="banana")]} {["banana","cherry"]}
                {$.s[?(@=="")]} {[""]}
                {$.s[?(@==1)]} {[]}
                {$.b[?(@==true)]} {[true]}
                {$.b[?(@==false)]} {[false]}
                {$.b[?(@!=true)]} {[false]}
                {$.b[?(@==null)]} {[null]}
                {$.b[?(@=="true")]} {["true"]}
                {$.b[?(@==1)]} {[1]}
                {$.o[?(@.v)].v} {[1,2,3]}
                {$.o[?(@.e)].v} {[2]}
                {$.o[?(@.flag)].t} {["z"]}
                {$.o[?(@.v>1)].v} {[2,3]}
                {$.o[?(@.v>1&&@.t)].t} {["y"]}
                {$.o[?(@.v==1||@.t=="z")].t} {["x","z"]}
                {$.o[?(@.t!="x")].t} {["y","z"]}
                {$.o[?(@.e==null)].v} {[2]}
                {$.o[?(@.flag==false)].t} {["z"]}
                {$.o[?(@.nosuch)]} {[]}
                {$.o[?(@.v>100)]} {[]}
                {$..[?(@.v==3)]} {[{"v":3}]}
                {$..[?(@>20)]} {[20.5]}
            } {
                assert_equal $exp [r json.get f $path] "path $path"
            }
            # There is no negation operator, and an expression needs both operands.
            assert_error "SYNTAXERR Invalid number" {r json.get f {$.n[?(!(@==20))]}}
            assert_error "SYNTAXERR Invalid number" {r json.get f {$.n[?()]}}
            assert_error "SYNTAXERR Invalid number" {r json.get f {$.n[?(@ == )]}}
            assert_error "SYNTAXERR Invalid JSON path" {r json.get f {$.n[?(@>1]}}
            assert_error "SYNTAXERR Invalid JSON path" {r json.get f {$.o[?(@[1]==20)]}}
            r del f
        }

        test "JSON path: writes through a filter (caps $caps)" {
            assert_equal OK [r json.set k1 {$.store.books[?(@.price<10.0)].price} 10.01]
            foreach {key path exp} {
                k1 {$.store.books[?(@.price<10.0)]} {[]}
                k1 {$.store.books[?(@.price<=10.02)].price} {[10.01,10.01]}
                k1 {$.store.books[?(@.price <= 10.02)].price} {[10.01,10.01]}
                k1 {$.store.books[?(@.price==10.01)].title} {["Sayings of the Century","Moby Dick"]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
            foreach path {
                {$.books[?((@.price>1&&@.price<20)&&(@.sold==false))].price}
                {$.books[?((@.price > 1 && @.price < 20) && (@.sold == false))].price}
                {$["books"][?((@["price"]>1&&@["price"]<20)&&(@["sold"]==false))]["price"]}
                {$["books"][?((@["price"] > 1 && @["price"] < 20) && (@["sold"] == false))]["price"]}
            } {
                assert_equal OK [r json.set k4 $path 13.13]
                assert_equal {[13.13]} [r json.get k4 $path]
            }
            assert_equal {{"books":[{"price":5,"sold":true,"in-stock":true,"title":"foo"},{"price":13.13,"sold":false,"title":"abc"}]}} [r json.get k4]
        }

        test "JSON path: delete through a filter (caps $caps)" {
            assert_equal OK [r json.set k1 . $::json_path_store]
            assert_equal OK [r json.set k2 . $::json_path_store]
            foreach {key path exp} {
                k1 {$.store.books[?(@.["category"] == "fiction")].price} 3
                k2 {$.store.books[?((@["price"] < 9 || @["price"] > 10) && @["isbn"])]["price"]} 1
            } {
                assert_equal $exp [r json.del $key $path]
                assert_equal {[]} [r json.get $key $path]
            }
            assert_equal {[8.95,19.95]} [r json.get k1 {$..price}]
            assert_equal {[8.95,12.99,9,19.95]} [r json.get k2 {$..price}]
            # Deleting the filtered elements themselves.
            assert_equal 3 [r json.del k1 {$.store.books[?(@.category=="fiction")]}]
            assert_equal {["Sayings of the Century"]} [r json.get k1 {$.store.books[*].title}]
        }

        test "JSON path: a filter on an object, and string comparison (caps $caps)" {
            assert_equal OK [r json.set k1 . {{
                "key for key"     : "key inside here",
                "an object"       : {"weight" : 300, "a value" : 300, "my key" : "key inside here"},
                "another object"  : {"weight" : 400, "a value" : 400, "my key" : "key inside there"},
                "objects": [
                    {"weight" : 100, "a value" : 100, "my key" : "key inside here"},
                    {"weight" : 200, "a value" : 200, "my key" : "key inside there"},
                    {"weight" : 300, "a value" : 300, "my key" : "key inside here"},
                    {"weight" : 400, "a value" : 400, "my key" : "key inside there"}
                ]
            }}]
            foreach {path exp} {
                {$["an object"].[?(@.weight > 200)].["a value"]} {[300]}
                {$["an object"].[?(@.weight == 300)].["a value"]} {[300]}
                {$["an object"].[?(@.weight > 300)].["a value"]} {[]}
                {$["another object"].[?(@["a value"] > 200)].weight} {[400]}
                {$["another object"].[?(@.["a value"] > 200)].weight} {[400]}
                {$["another object"].[?(@.["my key"] == "key inside there")].weight} {[400]}
                {$["objects"].[?(@.weight > 200)].["a value"]} {[300,400]}
                {$["objects"].[?(@.["my key"] == "key inside there")].weight} {[200,400]}
                {$["objects"].[?(@.["my key"] != "key inside there")].weight} {[100,300]}
                {$["objects"].[?(@.["my key"] == "key inside here")].weight} {[100,300]}
                {$["objects"].[?(@.["my key"] != "key inside here")].weight} {[200,400]}
                {$["objects"].[?(@.["my key"] <= "key inside here")].weight} {[100,300]}
                {$["objects"].[?(@.["my key"] >= "key inside here")].weight} {[100,200,300,400]}
                {$["objects"].[?(@.["my key"] < "key inside herf")].weight} {[100,300]}
                {$["objects"].[?(@.["my key"] > "key insidd here")].weight} {[100,200,300,400]}
                {$["key for key"].[?(@.["a value"] > 200)]} {[]}
            } {
                assert_equal $exp [r json.get k1 $path] "path $path"
            }
        }

        test "JSON path: filters after a recursive descent return each value once (caps $caps)" {
            r json.set organism . $::json_path_organism
            assert_equal {["Redwood","Horse"]} [r json.get organism {$..[?(@.weight>400)].name}]
            assert_equal {["Chimpanzee"]} [r json.get organism {$..[?(@.weight>=60&&@.name=="Chimpanzee")].name}]
        }

        # ---------------------------------------------------------------------------------
        # Recursive descent
        # ---------------------------------------------------------------------------------

        test "JSON path: recursive descent reads (caps $caps)" {
            foreach {key val} {
                k1 {{"a":{"a":1}}}
                k2 {{"a":{"a":{"a":{"a":1}}}}}
                k3 {{"x": {}, "y": {"a":"a"}, "z": {"a":"", "b":"b"}}}
                k4 {{"a":{"b":{"z":{"y":1}}, "c":{"z":{"y":2}}, "z":{"y":3}}}}
                k5 {{"a":1, "b": {"e":[0,1,2]}, "c":{"e":[10,11,12]}}}
                k6 {{"a":[1], "b": {"a": [2,3]}, "c": {"a": [4,5,6]}}}
            } {
                assert_equal OK [r json.set $key . $val]
            }
            foreach {key path exp} {
                k1 {$..a} {[{"a":1},1]}
                k2 {$..a} {[{"a":{"a":{"a":1}}},{"a":{"a":1}},{"a":1},1]}
                k2 {$..a..a} {[{"a":{"a":1}},{"a":1},1]}
                k2 {$..a..a..a} {[{"a":1},1]}
                k2 {$..a..a..a..a} {[1]}
                k2 {$..a..a..a..a..a} {[]}
                k3 {$..a} {["a",""]}
                k4 {$.a..z.y} {[3,1,2]}
                k4 {$.a..z.*} {[3,1,2]}
                k4 {$.a.*..y} {[1,2,3]}
                k5 {$..e.[*]} {[0,1,2,10,11,12]}
                k5 {$..e[1]} {[1,11]}
                k5 {$..e.[1]} {[1,11]}
                k5 {$..e[0:2]} {[0,1,10,11]}
                k5 {$..["e"][1]} {[1,11]}
                k5 {$..["e"][0:2]} {[0,1,10,11]}
                k5 {$..[ "e" ][ 0 : 2 ]} {[0,1,10,11]}
                k5 {$..*} {[1,{"e":[0,1,2]},{"e":[10,11,12]},[0,1,2],0,1,2,[10,11,12],10,11,12]}
                k5 {$..[0]} {[0,10]}
                k5 {$..e..[2]} {[2,12]}
                k1 {..a} {{"a":1}}
                k5 {..e} {[0,1,2]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
        }

        test "JSON path: recursive descent writes (caps $caps)" {
            # recursive ARRAPPEND
            assert_equal {2 3 4} [r json.arrappend k6 {$..a} 0]
            assert_equal {{"a":[1,0],"b":{"a":[2,3,0]},"c":{"a":[4,5,6,0]}}} [r json.get k6]
            # recursive delete
            assert_equal 2 [r json.del k3 {$..a}]
            assert_equal {{"x":{},"y":{},"z":{"b":"b"}}} [r json.get k3]
            # Nested matches: all four are counted, and the outermost removes the rest.
            assert_equal 4 [r json.del k2 {$..a}]
            assert_equal {{}} [r json.get k2]
        }

        test "JSON path: recursive insert, update and delete of members (caps $caps)" {
            assert_equal OK [r json.set store . $::json_path_store8]
            assert_equal OK [r json.set store2 . {{"store": {"title": "foo", "bicycle": {"title": "foo2", "color": "red", "price": 19.64, "in-stock": true}}}}]
            # recursive delete
            assert_equal 2 [r json.del store2 {$..title}]
            assert_equal {[]} [r json.get store2 {$..title}]
            assert_equal {{"store":{"bicycle":{"color":"red","price":19.64,"in-stock":true}}}} [r json.get store2]
            # recursive insert, update and delete
            assert_equal {["Sayings of the Century","Sword of Honour","Sword of Honour - movie","Moby Dick","The Lord of the Rings","The Elements of Style","Anna Karenina","The World Almanac and Book of Facts 2021","Manual for Writers of Research Papers"]} [r json.get store {$..title}]
            assert_equal OK [r json.set store {$..title} {"foo"}]
            assert_equal {["foo","foo","foo","foo","foo","foo","foo","foo","foo"]} [r json.get store {$..title}]
            foreach {path exp} {
                {$.title} {[]}
                {$.store.title} {[]}
                {$.store.bicycle.title} {[]}
                {$.store.books[1].title} {["foo"]}
                {$.store.books[1].movies[0].title} {["foo"]}
                {$.store.books[1].movies[0].realisator.title} {[]}
            } {
                assert_equal $exp [r json.get store $path] "path $path"
            }
            assert_equal 9 [r json.del store {$..title}]
            assert_equal {[]} [r json.get store {$..title}]
            assert_equal {[8.95,12.99,9,22.99,6.99,22.99,10.69,8.59,19.64]} [r json.get store {$..price}]
            r del store store2
        }

        test "JSON path: recursive insert, update and delete of array elements (caps $caps)" {
            set doc {{"input": {"a": 1, "b": {"e": [0,1,2]}, "c": {"e": [10,11,12]}}}}
            assert_equal OK [r json.set input . $doc]
            assert_equal OK [r json.set input2 . $doc]
            # recursive delete
            assert_equal 2 [r json.del input2 {$..e}]
            assert_equal {[]} [r json.get input2 {$..e}]
            assert_equal {{"input":{"a":1,"b":{},"c":{}}}} [r json.get input2]
            # recursive insert, update and delete
            assert_equal {[0,1,2,10,11,12]} [r json.get input {$..e[*]}]
            assert_equal OK [r json.set input {$..e[*]} 4]
            assert_equal {[4,4,4,4,4,4]} [r json.get input {$..e[*]}]
            assert_equal {[4,4,4]} [r json.get input {$.input.b.e[*]}]
            foreach {path exp} {
                {$.e} {[]}
                {$.input.e} {[]}
                {$.input.a.e} {[]}
                {$.input.b.e[*]} {[4,4,4]}
                {$.input.c.e[*]} {[4,4,4]}
            } {
                assert_equal $exp [r json.get input $path] "path $path"
            }
            assert_equal 2 [r json.del input {$..e}]
            assert_equal 0 [r json.del input {$..e}]
            assert_equal {[]} [r json.get input {$..e}]
            r del input input2
        }

        # ---------------------------------------------------------------------------------
        # Write modes
        # ---------------------------------------------------------------------------------

        test "JSON path: write commands apply once to a value matched several times (caps $caps)" {
            foreach {key val} {
                k1 {[0,1,2,3,4,5,6,7,8,9]}
                k2 {[0,1,2,3,4,5,6,7,8,9]}
                k3 {[0,1,2,3,4,5,6,7,8,9]}
                k4 {["0","1","2","3","4","5","6","7","8","9"]}
                k5 {[[0],[1],[2],[3],[4],[5],[6],[7],[8],[9]]}
                k6 {[true,true,true,true,true]}
                k7 {[[0],[1],[2],[3],[4],[5],[6],[7],[8],[9]]}
                k8 {[[0],[1],[2],[3],[4],[5],[6],[7],[8],[9]]}
                k9 {[[0],[1],[2],[3],[4],[5],[6],[7],[8],[9]]}
                k10 {[[0,1,2,3,4],[0,1,2,3,4],[0,1,2,3,4]]}
                k11 {[0,1,2,3,4,5,6,7,8,9]}
                k12 {[0,1,2,3,4,5,6,7,8,9]}
            } {
                assert_equal OK [r json.set $key . $val]
            }
            # NUMINCRBY
            assert_equal {[10]} [r json.numincrby k1 {$[0,0,0,0,0]} 10]
            assert_equal {[10,1,2,3,4,5,6,7,8,9]} [r json.get k1]
            # NUMMULTBY
            assert_equal {[18]} [r json.nummultby k2 {$[9,9,9,9,9]} 2]
            assert_equal {[0,1,2,3,4,5,6,7,8,18]} [r json.get k2]
            # CLEAR
            assert_equal 1 [r json.clear k5 {$[0,0,0,0,0]}]
            assert_equal {[[],[1],[2],[3],[4],[5],[6],[7],[8],[9]]} [r json.get k5]
            # TOGGLE
            assert_equal {0} [r json.toggle k6 {$[0,0,0,0]}]
            assert_equal {[false,true,true,true,true]} [r json.get k6]
            # STRAPPEND
            assert_equal {4} [r json.strappend k4 {$[0,0,0,0,0]} {"foo"}]
            assert_equal {["0foo","1","2","3","4","5","6","7","8","9"]} [r json.get k4]
            # ARRAPPEND
            assert_equal {3} [r json.arrappend k7 {$[0,0,0,0,0]} 8 9]
            assert_equal {[[0,8,9],[1],[2],[3],[4],[5],[6],[7],[8],[9]]} [r json.get k7]
            # ARRINSERT
            assert_equal {2} [r json.arrinsert k8 {$[0,0,0,0,0]} 0 9]
            assert_equal {[[9,0],[1],[2],[3],[4],[5],[6],[7],[8],[9]]} [r json.get k8]
            # ARRPOP
            assert_equal {0} [r json.arrpop k9 {$[0,0,0,0,0]}]
            assert_equal {[[],[1],[2],[3],[4],[5],[6],[7],[8],[9]]} [r json.get k9]
            # ARRTRIM
            assert_equal {2} [r json.arrtrim k10 {$[0,0,0,0,0]} 0 1]
            assert_equal {[[0,1],[0,1,2,3,4],[0,1,2,3,4]]} [r json.get k10]
            # DEL
            assert_equal 1 [r json.del k3 {$[0,0,0,0,0]}]
            assert_equal {[1,2,3,4,5,6,7,8,9]} [r json.get k3]
            # DEL: delete arbitrary elements with duplicates
            assert_equal 5 [r json.del k12 {$[1,4,7,0,0,3,3]}]
            assert_equal {[2,5,6,8,9]} [r json.get k12]
            # SET
            assert_equal OK [r json.set k11 {$[0,0,0,0,0]} 9]
            assert_equal {[9,1,2,3,4,5,6,7,8,9]} [r json.get k11]
            # MERGE
            assert_equal OK [r json.merge k11 {$[1,1,-9]} {{"m":1}}]
            assert_equal {[9,{"m":1},2,3,4,5,6,7,8,9]} [r json.get k11]
            r del k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11 k12
        }

        test "JSON path: insert, update and delete modes (caps $caps)" {
            assert_equal OK [r json.set organism . $::json_path_organism]
            assert_equal OK [r json.set organism2 . $::json_path_organism]
            set path {$.animals[*].mammals[*].primates[*].apes[?(@.weight<400)].weight}
            # delete
            assert_equal {[130,300]} [r json.get organism $path]
            assert_equal 2 [r json.del organism $path]
            assert_equal {[]} [r json.get organism $path]
            # insert: a path with a filter matches nothing to insert into
            assert_equal OK [r json.set organism $path 25]
            assert_equal {[]} [r json.get organism $path]
            # update
            assert_equal {[130,300]} [r json.get organism2 $path]
            assert_equal OK [r json.set organism2 $path 25]
            assert_equal {[25,25]} [r json.get organism2 $path]
            assert_equal 2 [r json.del organism2 $path]
            assert_equal {[]} [r json.get organism2 $path]
            assert_equal [r json.get organism] [r json.get organism2]
        }

        test "JSON path: a member is added only as the last step, parents are not created, arrays do not grow (caps $caps)" {
            r json.set w . {{"a":{"b":1},"arr":[1,2],"n":5}}
            assert_equal OK [r json.set w {$.a.c} 2]
            assert_equal OK [r json.set w .a.d 3]
            assert_equal OK [r json.set w {$["a"]["e"]} 4]
            assert_equal {{"a":{"b":1,"c":2,"d":3,"e":4},"arr":[1,2],"n":5}} [r json.get w]
            assert_error "NONEXISTENT JSON path does not exist" {r json.set w {$.x.y} 1}
            assert_error "NONEXISTENT JSON path does not exist" {r json.set w .x.y 1}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.set w {$.arr[2]} 1}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.set w {.arr[5]} 1}
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.set w {$.n.x} 1}
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.set w {$.arr.x} 1}
            # A wildcard or union updates the members that exist and inserts into the other
            # objects it reaches. (With exactly one update and at least one insert the module
            # inserts the old value instead of the new one, design 2.5. That is not reproduced.)
            r json.set w . {{"a":{"k":1},"b":{},"c":[],"d":1}}
            assert_equal OK [r json.set w {$.*.k} 9]
            assert_equal {{"a":{"k":9},"b":{"k":9},"c":[],"d":1}} [r json.get w]
            assert_equal OK [r json.set w {$["a","b"].z} 0]
            assert_equal {{"a":{"k":9,"z":0},"b":{"k":9,"z":0},"c":[],"d":1}} [r json.get w]
            r del w
        }

        test "JSON path: malformed paths (caps $caps)" {
            assert_equal OK [r json.set k1 . {{"store":{"book":[{"price":5,"sold":true,"in-stock":true,"title":"foo","author":"me","isbn":"978-3-16-148410-0"}]}}}]
            assert_equal OK [r json.set k2 . {[1,2,3,4,5,6,7,8,9,10]}]
            foreach {key path exp} {
                k1 {$[0:2]$[0:1]$[0:2]$[0:2]$[0<2065>:2]$[0:2]} {[]}
                k1 {$[0,1]} {[]}
                k2 {$.["a"].["b"].["c"]} {[]}
                k1 {$a.b.c.d} {[]}
                k2 {$a$b$c$d} {[]}
            } {
                assert_equal $exp [r json.get $key $path] "path $path"
            }
            foreach path {
                {.[0:2].[0:1].[0:2].[0:2].[0<2065>:2].[0:2]}
                {.[0:2]$[0:1]$[0:2]$[0:2]$[0<2065>:2]$[0:2]}
            } {
                assert_error "WRONGTYPE JSON element is not an array" {r json.get k1 $path}
            }
            foreach path {{&&$.store..price} {!$.store..price} {=$.store..price} {=.store..price} {||.store..price}} {
                assert_error "SYNTAXERR Invalid object member name" {r json.get k1 $path}
                assert_error "SYNTAXERR Invalid object member name" {r json.set k1 $path 0}
                assert_error "SYNTAXERR Invalid object member name" {r json.del k1 $path}
                assert_error "SYNTAXERR Invalid object member name" {r json.numincrby k1 $path 1}
            }
            assert_equal {{"store":{"book":[{"price":5,"sold":true,"in-stock":true,"title":"foo","author":"me","isbn":"978-3-16-148410-0"}]}}} [r json.get k1]
        }

        # ---------------------------------------------------------------------------------
        # Deliberate changes: B4, B5, B6
        # ---------------------------------------------------------------------------------

        test "JSON.SET nested matches are resolved against the document before the command (B4) (caps $caps)" {
            # The module applies ancestors first and resolves the path again for each match,
            # giving {"a":{"a":{"a":2}}}.
            r json.set k1 . {{"a":{"a":1}}}
            assert_equal OK [r json.set k1 {$..a} {{"a":2}}]
            assert_equal {{"a":{"a":2}}} [r json.get k1]
            # Applying it again gives the same document: the outermost match wins.
            assert_equal OK [r json.set k1 {$..a} {{"a":2}}]
            assert_equal {{"a":{"a":2}}} [r json.get k1]
            r json.set k1 . {{"a":1,"b":{"a":2}}}
            assert_equal OK [r json.set k1 {$..a} {{"a":7}}]
            assert_equal {{"a":{"a":7},"b":{"a":{"a":7}}}} [r json.get k1]
            assert_equal OK [r json.set k1 {$..a} {{"a":8}}]
            assert_equal {{"a":{"a":8},"b":{"a":{"a":8}}}} [r json.get k1]
            # Matches that do not nest are all written.
            r json.set k1 . {{"a":{"b":{"a":1}},"c":{"a":2}}}
            assert_equal OK [r json.set k1 {$..a} 9]
            assert_equal {{"a":9,"c":{"a":9}}} [r json.get k1]
            r json.set k1 . {{"a":{"b":{"a":1}},"c":{"a":2}}}
            assert_equal OK [r json.set k1 ..a 9]
            assert_equal {{"a":9,"c":{"a":9}}} [r json.get k1]
            r json.set k1 . {{"t":[1,2,{"t":[3,4]}]}}
            assert_equal OK [r json.set k1 {$..t[*]} 0]
            assert_equal {{"t":[0,0,0]}} [r json.get k1]
            r json.set k1 . {[[1,[2]],[3]]}
            assert_equal OK [r json.set k1 {$..[0]} 0]
            assert_equal {[0,[0]]} [r json.get k1]
        }

        test "JSON.SET \$..a with a nested value does not deepen the document on each call (B4) (caps $caps)" {
            # The module's test_v2_path_limit_recursive_descent relies on each call adding one
            # level. With B4 the outermost match is replaced and the depth stays at 2.
            r config set json.max-path-limit 10
            assert_equal OK [r json.set k1 {$} {{"a":0}}]
            for {set j 0} {$j < 12} {incr j} {
                assert_equal OK [r json.set k1 {$..a} {{"a":0}}]
            }
            assert_equal 2 [r json.debug depth k1]
            assert_equal {{"a":{"a":0}}} [r json.get k1]
            r config set json.max-path-limit 128
        }

        test "JSON.DEL, JSON.CLEAR with nested matches: every match is counted, the outermost wins (B4) (caps $caps)" {
            r json.set k1 . {{"a":{"a":{"a":5}}}}
            assert_equal 3 [r json.del k1 {$..a}]
            assert_equal {{}} [r json.get k1]
            r json.set k1 . {{"a":{"a":{"a":5}}}}
            assert_equal 3 [r json.del k1 ..a]
            assert_equal {{}} [r json.get k1]
            r json.set k1 . {{"a":[1,{"a":[2,3]}]}}
            assert_equal 2 [r json.del k1 {$..a}]
            assert_equal {{}} [r json.get k1]
            r json.set k1 . {{"a":{"b":{"a":1}},"c":{"a":2}}}
            assert_equal 3 [r json.del k1 ..a]
            assert_equal {{"c":{}}} [r json.get k1]
            r json.set k1 . {[[1,2],[3,4]]}
            assert_equal 3 [r json.del k1 {$..[0]}]
            assert_equal {[[4]]} [r json.get k1]
            # CLEAR: the module counts 1 here, having cleared the outer value first. The
            # reply is computed from the document before the command (design Q1).
            r json.set k1 . {{"a":{"a":{"b":1},"c":[1,2]}}}
            assert_equal 2 [r json.clear k1 {$..a}]
            assert_equal {{"a":{}}} [r json.get k1]
            r json.set k1 . {{"a":[{"a":[2,3]},1]}}
            assert_equal 2 [r json.clear k1 {$..a}]
            assert_equal {{"a":[]}} [r json.get k1]
        }

        test "JSON array commands with nested matches work on the document before the command (B4) (caps $caps)" {
            # The module reads freed memory in these cases (design 2.5), so there is no module
            # value to compare with. Commands that only add apply to every match. A match
            # inside a removed value has its effect dropped but still gets its reply.
            r json.set k1 . {{"a":[1,{"a":[2,3]}]}}
            assert_equal {3 3} [r json.arrappend k1 {$..a} 0]
            assert_equal {{"a":[1,{"a":[2,3,0]},0]}} [r json.get k1]
            r json.set k1 . {{"a":[1,{"a":[2,3]}]}}
            assert_equal {3 3} [r json.arrinsert k1 {$..a} 0 9]
            assert_equal {{"a":[9,1,{"a":[9,2,3]}]}} [r json.get k1]
            r json.set k1 . {{"a":[1,{"a":[2,3]}]}}
            assert_equal {{{"a":[2,3]}} 3} [r json.arrpop k1 {$..a}]
            assert_equal {{"a":[1]}} [r json.get k1]
            r json.set k1 . {{"a":[{"a":[2,3]},1]}}
            assert_equal {{{"a":[2,3]}} 2} [r json.arrpop k1 {$..a} 0]
            assert_equal {{"a":[1]}} [r json.get k1]
            r json.set k1 . {{"a":[1,{"a":[2,3]}]}}
            assert_equal {1 1} [r json.arrtrim k1 {$..a} 0 0]
            assert_equal {{"a":[1]}} [r json.get k1]
            r json.set k1 . {{"a":[{"a":[2,3]},1]}}
            assert_equal {1 1} [r json.arrtrim k1 {$..a} 0 0]
            assert_equal {{"a":[{"a":[2]}]}} [r json.get k1]
            r del k1
        }

        test "JSON.SET a new member name is literal: \$.b~1c creates b~1c (B5) (caps $caps)" {
            # The module's test_jsonpath_member_name_containing_slash, where test 3 pins that
            # $.b~1c writes the member b/c. Here it creates a member called b~1c.
            r json.set k1 . {{"a": 4.5, "b/c": 85}}
            # Test 1: json.get with member name containing slash
            assert_equal {[85]} [r json.get k1 {$.b/c}]
            # Test 2: json.set with member name containing slash
            r json.set k1 {$.b/c} 0
            assert_equal {{"a":4.5,"b/c":0}} [r json.get k1]
            # Test 3: a name with ~1 is a different member
            r json.set k1 {$.b~1c} 2
            assert_equal {{"a":4.5,"b/c":0,"b~1c":2}} [r json.get k1]
            assert_equal {[2]} [r json.get k1 {$.b~1c}]
            assert_equal 1 [r json.del k1 {$.b~1c}]
            r json.set k1 {$.b/c} 2
            assert_equal {{"a":4.5,"b/c":2}} [r json.get k1]
            # Test 4: json.numincrby with member name containing slash
            assert_equal {[3]} [r json.numincrby k1 {$.b/c} 1]
            assert_equal {{"a":4.5,"b/c":3}} [r json.get k1]
            # Test 5: json.nummultby with member name containing slash
            assert_equal {[6]} [r json.nummultby k1 {$.b/c} 2]
            assert_equal {{"a":4.5,"b/c":6}} [r json.get k1]
            # Test 6: json.get with member name containing slash
            assert_equal {[6]} [r json.get k1 {$.b/c}]
            # Test 7: json.set with recursive search
            r json.set k1 {$..b/c} 7
            assert_equal {{"a":4.5,"b/c":7}} [r json.get k1]
            # Test 8: json.numincrby with recursive search
            assert_equal {[9]} [r json.numincrby k1 {$..b/c} 2]
            assert_equal {{"a":4.5,"b/c":9}} [r json.get k1]
        }

        test "JSON member names containing / and ~ can be created, read, updated and deleted (B5) (caps $caps)" {
            # In the module a new name containing / creates nested objects, ~0 and ~1 are
            # unescaped, and an existing member whose name contains ~ cannot be set or deleted.
            r json.set b . {{"a":4.5,"b/c":85,"t~x":1}}
            assert_equal OK [r json.set b {$.b~1c} 2]
            assert_equal OK [r json.set b {$.b~0c} 3]
            assert_equal OK [r json.set b {$.x/y} 4]
            assert_equal OK [r json.set b {$["p/q"]} 5]
            assert_equal OK [r json.set b {.r/s} 6]
            assert_equal OK [r json.set b {$.t~x} 7]
            assert_equal {{"a":4.5,"b/c":85,"t~x":7,"b~1c":2,"b~0c":3,"x/y":4,"p/q":5,"r/s":6}} [r json.get b]
            assert_equal {[2]} [r json.get b {$.b~1c}]
            assert_equal {[2]} [r json.get b {$["b~1c"]}]
            assert_equal {[4]} [r json.get b {$.x/y}]
            assert_equal {[7]} [r json.get b {$.t~x}]
            assert_equal 6 [r json.get b .r/s]
            assert_equal {[8]} [r json.numincrby b {$.t~x} 1]
            assert_equal 1 [r json.del b {$.t~x}]
            assert_equal 1 [r json.del b {$.b~1c}]
            assert_equal 1 [r json.del b {$.x/y}]
            assert_equal 1 [r json.del b {$.b/c}]
            assert_equal {{"a":4.5,"b~0c":3,"p/q":5,"r/s":6}} [r json.get b]
            assert_equal OK [r json.mset b {$.u/v} 1 b {$.u~0v} 2]
            assert_equal {{"a":4.5,"b~0c":3,"p/q":5,"r/s":6,"u/v":1,"u~0v":2}} [r json.get b]

            r json.set b . {{"o":{}}}
            assert_equal OK [r json.set b {$.o.a/b/c} 1]
            assert_equal OK [r json.set b {$.o["~"]} 2]
            assert_equal OK [r json.set b {$.o["/"]} 3]
            assert_equal OK [r json.set b {$.o["~01"]} 4]
            assert_equal {{"o":{"a/b/c":1,"~":2,"/":3,"~01":4}}} [r json.get b]
            assert_equal {[4]} [r json.get b {$.o["~01"]}]
            assert_equal 1 [r json.del b {$.o["~"]}]
            assert_equal 1 [r json.del b {$.o["/"]}]
            assert_equal {{"o":{"a/b/c":1,"~01":4}}} [r json.get b]
            r del b
        }

        test "JSON.NUMINCRBY, JSON.NUMMULTBY failing on one of several matches change nothing (B6) (caps $caps)" {
            # The module stops at the failing match and keeps the earlier changes: it leaves
            # [1e+308,1.7e308,3] after the first command below.
            r json.set n . {[1,1.7e308,3]}
            assert_error "OVERFLOW Addition would overflow" {r json.numincrby n {$[*]} 1e308}
            assert_equal {[1,1.7e308,3]} [r json.get n]
            assert_error "OVERFLOW Addition would overflow" {r json.numincrby n {.[*]} 1e308}
            assert_equal {[1,1.7e308,3]} [r json.get n]
            assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby n {$[*]} 10}
            assert_equal {[1,1.7e308,3]} [r json.get n]
            assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby n {.[*]} 10}
            assert_equal {[1,1.7e308,3]} [r json.get n]
            # The failing match last, first, and under a recursive descent.
            r json.set n . {{"a":1,"b":{"a":2.5,"c":{"a":-1.7e308}},"d":[{"a":4}]}}
            assert_error "OVERFLOW Addition would overflow" {r json.numincrby n {$..a} -1e308}
            assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby n {$..a} 1e10}
            assert_equal {{"a":1,"b":{"a":2.5,"c":{"a":-1.7e308}},"d":[{"a":4}]}} [r json.get n]
            # The same command succeeds once no match overflows.
            assert_equal {[2,3.5,-1.7e+308,5]} [r json.numincrby n {$..a} 1]
            r del n
        }

        test "JSON.ARRINSERT failing on one of several matches changes nothing (B6) (caps $caps)" {
            # The module inserts into the first array and then fails on the second.
            r json.set ai . {[[1,2,3],[1]]}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert ai {$[*]} 2 9}
            assert_equal {[[1,2,3],[1]]} [r json.get ai]
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert ai {.[*]} 2 9}
            assert_equal {[[1,2,3],[1]]} [r json.get ai]
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.arrinsert ai {$[*]} -3 9}
            assert_equal {[[1,2,3],[1]]} [r json.get ai]
            assert_equal {4 2} [r json.arrinsert ai {$[*]} 1 9]
            assert_equal {[[1,9,2,3],[1,9]]} [r json.get ai]
            r del ai
        }

        test "JSON multi-match writes refused by a limit change nothing (B6) (caps $caps)" {
            # Nesting limit: the value fits under the first match and not under the second.
            r config set json.max-path-limit 3
            r json.set lim . {[0,[0]]}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set lim {$..[0]} {[[1]]}}
            assert_equal {[0,[0]]} [r json.get lim]
            r config set json.max-path-limit 128
            # Size limit: the sum over all matches is what counts.
            r json.set lim . {{"a":["x"],"b":["y"],"c":["z"],"s":["p","q"]}}
            r config set json.max-document-size [expr {[r json.debug memory lim] + 4096}]
            set big "\"[string repeat w 2000]\""
            assert_error "LIMIT Document size limit is exceeded" {r json.arrappend lim {$.*} $big}
            assert_error "LIMIT Document size limit is exceeded" {r json.strappend lim {$.*[*]} $big}
            assert_error "LIMIT Document size limit is exceeded" {r json.set lim {$.*[0]} $big}
            assert_error "LIMIT Document size limit is exceeded" {r json.arrinsert lim {$.*} 0 $big}
            assert_error "LIMIT Document size limit is exceeded" {r json.merge lim {$.*} "{\"m\":$big}"}
            assert_equal {{"a":["x"],"b":["y"],"c":["z"],"s":["p","q"]}} [r json.get lim]
            # One match alone fits.
            assert_equal 2 [r json.arrappend lim {$.a} $big]
            r config set json.max-document-size 0
            r del lim
        }

        # ---------------------------------------------------------------------------------
        # Limits
        # ---------------------------------------------------------------------------------

        test "json.max-path-limit: a value inserted by a sub-path write counts from its position (caps $caps)" {
            r config set json.max-path-limit 3
            assert_equal OK [r json.set k1 {$} {{"a":{"a":{"a":0}}}}]
            assert_equal 3 [r json.debug depth k1]
            # insert a nesting object
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set k1 {$.a.a.b} {{"b":0}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set k1 {$.a.a.a} {[1]}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set k1 . {{"a":{"a":{"a":{"a":0}}}}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set k1 . {[[[[1]]]]}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.mset k1 {$.a.a.b} {{"b":0}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.mset fresh . {[[[[1]]]]}}
            assert_equal {{"a":{"a":{"a":0}}}} [r json.get k1]
            assert_equal 0 [r exists fresh]
            # increase limit by 1
            r config set json.max-path-limit 4
            assert_equal OK [r json.set k1 {$.a.a.b} {{"b":0}}]
            assert_equal {{"a":{"a":{"a":0,"b":{"b":0}}}}} [r json.get k1]
            assert_equal 4 [r json.debug depth k1]
            r config set json.max-path-limit 128
        }

        test "json.max-path-limit: reached step by step through a recursive path (caps $caps)" {
            # Each call replaces the innermost member, adding one level, until the limit.
            r config set json.max-path-limit 10
            assert_equal OK [r json.set k1 {$} {{"a":0}}]
            for {set j 0} {$j < 9} {incr j} {
                assert_equal OK [r json.set k1 {$..[?(@.a==0)].a} {{"a":0}}]
            }
            assert_equal 10 [r json.debug depth k1]
            set before [r json.get k1]
            # one more increase should exceed the path limit
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set k1 {$..[?(@.a==0)].a} {{"a":0}}}
            assert_equal $before [r json.get k1]
            r config set json.max-path-limit 128
            assert_equal OK [r json.set k1 {$..[?(@.a==0)].a} {{"a":0}}]
            assert_equal 11 [r json.debug depth k1]
        }

        test "json.max-path-limit CONFIG SET, GET and the limit of 0 (caps $caps)" {
            assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
            r config set json.max-path-limit 0
            assert_equal {json.max-path-limit 0} [r config get json.max-path-limit]
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set z . {[1]}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set z . {{}}}
            assert_equal OK [r json.set z . 1]
            assert_error "ERR CONFIG SET failed*" {r config set json.max-path-limit -1}
            assert_error "ERR CONFIG SET failed*" {r config set json.max-path-limit abc}
            r config set json.max-path-limit 300
            assert_equal OK [r json.set z . [json_path_nested 250]]
            assert_equal 250 [r json.debug depth z]
            # Reads are not limited by the document's depth, in either syntax.
            assert_equal {[1]} [r json.get z "\$[string repeat .a 250]"]
            assert_equal 1 [r json.get z "[string repeat .a 250]"]
            assert_equal {[]} [r json.get z "\$[string repeat .a 251]"]
            assert_equal {integer} [r json.type z "\$[string repeat .a 250]"]
            # Lowering the limit keeps the stored document readable and editable in place.
            r config set json.max-path-limit 128
            assert_equal 250 [r json.debug depth z]
            assert_equal {[2]} [r json.numincrby z "\$[string repeat .a 250]" 1]
            assert_equal 1 [r json.del z "\$[string repeat .a 200]"]
            assert_equal 199 [r json.debug depth z]
            r del z
        }

        test "JSON path limit: expression nesting (caps $caps)" {
            r json.set s . {[1,2,3]}
            foreach n {1 10 65} {
                assert_equal {[2,3]} [r json.get s "\$\[?([string repeat ( $n]@>1[string repeat ) $n])\]"] "$n parentheses"
            }
            foreach n {66 100 201 5000} {
                set path "\$\[?([string repeat ( $n]@>1[string repeat ) $n])\]"
                assert_error "LIMIT Parser recursion depth is exceeded" {r json.get s $path}
                assert_error "LIMIT Parser recursion depth is exceeded" {r json.del s $path}
                assert_error "LIMIT Parser recursion depth is exceeded" {r json.set s $path 0}
                assert_error "LIMIT Parser recursion depth is exceeded" {r json.numincrby s $path 1}
            }
            assert_error "LIMIT Parser recursion depth is exceeded" {
                r json.get s ".\[?([string repeat ( 66]@>1[string repeat ) 66])\]"
            }
            assert_equal {[1,2,3]} [r json.get s]
        }

        test "JSON path limit: recursive descent tokens (caps $caps)" {
            # The count is of tokens evaluated, so it depends on how many values the earlier
            # descents reach: two descents pass on a document 19 levels deep and not on 20.
            r json.set e . [json_path_nested 19]
            assert_equal 18 [llength [r json.arrlen e {$..a..a}]]
            assert_equal 19 [llength [r json.type e {$..a}]]
            r json.set e . [json_path_nested 20]
            assert_equal 20 [llength [r json.type e {$..a}]]
            foreach path {{$..a..a} {$..a..a..a} {$..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a..a}} {
                assert_error "LIMIT Total number of recursive descent tokens in the query string exceeds the limit" {r json.get e $path}
                assert_error "LIMIT Total number of recursive descent tokens in the query string exceeds the limit" {r json.arrlen e $path}
                assert_error "LIMIT Total number of recursive descent tokens in the query string exceeds the limit" {r json.set e $path 1}
                assert_error "LIMIT Total number of recursive descent tokens in the query string exceeds the limit" {r json.del e $path}
                assert_error "LIMIT Total number of recursive descent tokens in the query string exceeds the limit" {r json.numincrby e $path 1}
            }
            assert_error "LIMIT Total number of recursive descent tokens in the query string exceeds the limit" {r json.get e ..a..a}
            assert_equal [json_path_nested 20] [r json.get e]
            # Where the first descent reaches nothing, the later ones are never evaluated.
            assert_equal {[]} [r json.get s "\$[string repeat ..a 40]"]
            assert_equal 0 [r json.del s "\$[string repeat ..a 40]"]
            assert_error "NONEXISTENT JSON path does not exist" {r json.get s "[string repeat ..a 40]"}
            r del e
        }

        test "JSON path limit: query string size (caps $caps)" {
            # 128 KB, counting the whole path.
            assert_equal {[]} [r json.get s "\$.[string repeat a 131070]"]
            assert_error "NONEXISTENT JSON path does not exist" {r json.get s ".[string repeat a 131071]"}
            foreach path [list "\$.[string repeat a 131071]" ".[string repeat a 131072]" "\$\[\"[string repeat a 131072]\"\]"] {
                assert_error "LIMIT Query string size limit is exceeded" {r json.get s $path}
                assert_error "LIMIT Query string size limit is exceeded" {r json.set s $path 1}
                assert_error "LIMIT Query string size limit is exceeded" {r json.del s $path}
                assert_error "LIMIT Query string size limit is exceeded" {r json.type s $path}
                assert_error "LIMIT Query string size limit is exceeded" {r json.get s {$[0]} $path}
            }
            assert_equal {[1,2,3]} [r json.get s]
            r del s
        }
    }
}

# JSON.DEL through a slice with a step of 2^32, on a server of its own for each cap setting. A
# server that mishandles the step can stay busy for minutes (the module does), so the reply is
# awaited for a bounded time on a second connection, and a server that has not replied is
# stopped.
foreach caps [json_caps_list] {
    start_server {tags {"json"}} {
        json_set_caps $caps

        test "JSON path: JSON.DEL through a slice step of 2^32 replies promptly and removes the first element only (caps $caps)" {
            r json.set a . {[0,1,2,3,4,5]}
            set rd [valkey_deferring_client]
            $rd json.del a {$[0:4:4294967296]}
            set fd [$rd channel]
            fconfigure $fd -blocking 0
            set reply {}
            wait_for_condition 50 100 {
                [string match "*\r\n" [append reply [read $fd]]]
            } else {
                catch {exec kill -KILL [srv 0 pid]}
                fail "JSON.DEL a '\$\[0:4:4294967296\]' did not reply within 5 seconds"
            }
            $rd close
            assert_equal ":1\r\n" $reply
            assert_equal {[1,2,3,4,5]} [r json.get a]
            assert_equal 1 [r json.del a {$[4:0:-4294967296]}]
            assert_equal {[1,2,3,4]} [r json.get a]
            assert_equal 1 [r json.clear a {$[0:4:18446744073709551616]}]
            assert_equal {[0,2,3,4]} [r json.get a]
        }
    }
}
