# Tests for the built-in JSON module: JSON.SET, JSON.GET, JSON.MGET, JSON.DEL and
# JSON.FORGET with both path syntaxes (legacy ".a.b" and JSONPath "$.a.b").
#
# The expected values are those of the external valkey-json module (its
# tst/integration/test_json_basic.py), except where the design lists a
# deliberate change (B1..B10) or a module defect that is not reproduced
# (design 2.5). Such tests carry the number in their name.
#
# The main tests run three times, once per encoding cap setting: default caps
# (mostly packed), small caps (tree with packed children) and zero caps (all
# tree).

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

# The "wikipedia" sample document of the module's test suite, pretty printed
# with INDENT "  ", SPACE " " and NEWLINE "\n".
proc json_wikipedia_pretty {} {
    return [join {
        "{"
        "  \"firstName\": \"John\","
        "  \"lastName\": \"Smith\","
        "  \"age\": 27,"
        "  \"weight\": 135.17,"
        "  \"isAlive\": true,"
        "  \"address\": {"
        "    \"street\": \"21 2nd Street\","
        "    \"city\": \"New York\","
        "    \"state\": \"NY\","
        "    \"zipcode\": \"10021-3100\""
        "  },"
        "  \"phoneNumbers\": ["
        "    {"
        "      \"type\": \"home\","
        "      \"number\": \"212 555-1234\""
        "    },"
        "    {"
        "      \"type\": \"office\","
        "      \"number\": \"646 555-4567\""
        "    }"
        "  ],"
        "  \"children\": [],"
        "  \"spouse\": null,"
        "  \"groups\": {}"
        "}"
    } "\n"]
}

proc json_wikipedia_compact {} {
    return {{"firstName":"John","lastName":"Smith","age":27,"weight":135.17,"isAlive":true,"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[{"type":"home","number":"212 555-1234"},{"type":"office","number":"646 555-4567"}],"children":[],"spouse":null,"groups":{}}}
}

# Fresh keyspace with the sample document and one plain string key.
proc json_setup_wikipedia {} {
    r flushall
    assert_equal OK [r json.set wikipedia . [json_wikipedia_pretty]]
    r set str_key {{"firstName":"John","lastName":"Smith"}}
}

# cases is a flat list of {path expected}.
proc json_assert_get {key cases} {
    foreach {path exp} $cases {
        assert_equal $exp [r json.get $key $path] "JSON.GET $key $path"
    }
}

# Every path must fail with an error matching pattern.
proc json_assert_get_error {key pattern paths} {
    foreach path $paths {
        assert_error $pattern {r json.get $key $path}
    }
}

# The raw protocol line of a null reply for the given protocol version.
proc json_raw_null {proto} {
    if {$proto == 3 || $::force_resp3} { return _ }
    return {$-1}
}

start_server {tags {"json"}} {

foreach caps [json_caps_list] {
    json_set_caps $caps

    # ------------------------------------------------------------------
    # Sanity
    # ------------------------------------------------------------------

    tags {"json cluster:skip"} {
    test "JSON sanity: SET/GET/MSET/MGET/DEL with legacy and JSONPath syntax (caps $caps)" {
        r flushall
        assert_equal OK [r json.set k1 . {{"a":"1", "b":"2", "c":"3"}}]
        assert_equal OK [r json.set k2 . {[1,2,3,4,5]}]
        assert_equal OK [r json.mset k3 . {[1,2,3,4,5]}]
        assert_equal OK [r json.mset k2 . {[5,4,3,2,1]} k1 . {{"a":"1", "b":"2", "c":"3"}}]
        assert_equal {{{"a":"1","b":"2","c":"3"}} {[5,4,3,2,1]} {[1,2,3,4,5]}} [r json.mget k1 k2 k3 .]
        assert_equal OK [r json.set k2 . {[1,2,3,4,5]}]
        assert_equal {{"a":"1","b":"2","c":"3"}} [r json.get k1]
        json_assert_get k1 {
            {.}    {{"a":"1","b":"2","c":"3"}}
            {.a}   {"1"}
        }
        json_assert_get k2 {
            {.}    {[1,2,3,4,5]}
            {[-1]} {5}
        }

        # pretty print
        foreach {key fmt path exp} {
            k1 SPACE  {.}    {{"a": "1","b": "2","c": "3"}}
            k1 INDENT {.}    {{ "a":"1", "b":"2", "c":"3"}}
            k2 INDENT {.}    {[ 1, 2, 3, 4, 5]}
            k2 INDENT {[-2]} {4}
        } {
            assert_equal $exp [r json.get $key $fmt { } $path] "JSON.GET $key $fmt ' ' $path"
        }

        assert_equal {{[{"a":"1","b":"2","c":"3"}]} {[[1,2,3,4,5]]}} [r json.mget k1 k2 {$}]
        json_assert_get k1 {
            {$}       {[{"a":"1","b":"2","c":"3"}]}
            {$.*}     {["1","2","3"]}
        }
        json_assert_get k2 {
            {$}       {[[1,2,3,4,5]]}
            {$.[*]}   {[1,2,3,4,5]}
            {$.[-1]}  {[5]}
            {$.[0:3]} {[1,2,3]}
        }

        # pretty print
        foreach {key fmt path exp} {
            k1 SPACE  {$}       {[{"a": "1","b": "2","c": "3"}]}
            k1 INDENT {$}       {[ {  "a":"1",  "b":"2",  "c":"3" }]}
            k1 INDENT {$.*}     {[ "1", "2", "3"]}
            k2 INDENT {$}       {[ [  1,  2,  3,  4,  5 ]]}
            k2 INDENT {$.[0:3]} {[ 1, 2, 3]}
        } {
            assert_equal $exp [r json.get $key $fmt { } $path] "JSON.GET $key $fmt ' ' $path"
        }

        assert_equal 1 [r json.del k1]
        assert_equal 1 [r json.del k2]
        assert_equal OK [r json.set k1 {$} {{"a":"1", "b":"2", "c":"3"}}]
        assert_equal OK [r json.set k2 {$} {[1,2,3,4,5]}]
        foreach {key val_before del_path del_ret val_after} {
            k1 {[{"a":"1","b":"2","c":"3"}]} {$.*}   3 {[{}]}
            k2 {[[1,2,3,4,5]]}               {$.[*]} 5 {[[]]}
        } {
            assert_equal $val_before [r json.get $key {$}]
            assert_equal $del_ret [r json.del $key $del_path]
            assert_equal $val_after [r json.get $key {$}]
        }
    }
    }

    # ------------------------------------------------------------------
    # JSON.SET
    # ------------------------------------------------------------------

    test "JSON.SET rejects an invalid JSON string (caps $caps)" {
        json_setup_wikipedia
        foreach input {foo {{"a"}} {[a]} {} { } {[1,2] x} {[1,2} {{"a":1,}} {[1,]} {'a'} {tru} {01} {1.} {.5} {+1} {"abc}} {
            assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {
                r json.set wikipedia .firstName $input
            }
            assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {
                r json.set newkey . $input
            }
        }
        # nothing was changed or created
        assert_equal {"John"} [r json.get wikipedia .firstName]
        assert_equal [json_wikipedia_compact] [r json.get wikipedia]
        assert_equal 0 [r exists newkey]
    }

    test "JSON.SET supports all datatypes (caps $caps)" {
        json_setup_wikipedia
        foreach {path value} {
            {.address.city}           {"Boston"}
            {.age}                    {30}
            {.foo}                    {[1,2,3]}
            {.phoneNumbers[0].number} {"1234567"}
            {.foo}                    {{"a":"b"}}
            {.lastName}               {null}
            {.isAlive}                {false}
        } {
            assert_equal OK [r json.set wikipedia $path $value]
            assert_equal $value [r json.get wikipedia $path]
        }
        assert_equal {{"firstName":"John","lastName":null,"age":30,"weight":135.17,"isAlive":false,"address":{"street":"21 2nd Street","city":"Boston","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[{"type":"home","number":"1234567"},{"type":"office","number":"646 555-4567"}],"children":[],"spouse":null,"groups":{},"foo":{"a":"b"}}} [r json.get wikipedia]

        # the same with JSONPath syntax
        json_setup_wikipedia
        foreach {path value} {
            {$.address.city}           {"Boston"}
            {$.age}                    {30}
            {$.foo}                    {[1,2,3]}
            {$.phoneNumbers[0].number} {"1234567"}
            {$.foo}                    {{"a":"b"}}
            {$.lastName}               {null}
            {$.isAlive}                {false}
        } {
            assert_equal OK [r json.set wikipedia $path $value]
            assert_equal "\[$value\]" [r json.get wikipedia $path]
        }
        assert_equal {[{"firstName":"John","lastName":null,"age":30,"weight":135.17,"isAlive":false,"address":{"street":"21 2nd Street","city":"Boston","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[{"type":"home","number":"1234567"},{"type":"office","number":"646 555-4567"}],"children":[],"spouse":null,"groups":{},"foo":{"a":"b"}}]} [r json.get wikipedia {$}]
    }

    test "JSON.SET root document (caps $caps)" {
        r flushall
        # path to the root document is represented as '.'
        foreach {key value} {
            k1 {"Boston"}
            k2 {123}
            k3 {["Seattle","Boston"]}
            k3 {[1,2,3]}
            k4 {{"a":"b"}}
            k4 {{}}
            k5 {null}
            k6 {false}
        } {
            assert_equal OK [r json.set $key . $value]
            assert_equal $value [r json.get $key]
        }
        # ... or as '$', and replacing the root may change its type
        foreach {key value} {
            k1 {{"a":"b"}}
            k2 {"Boston"}
            k3 {true}
            k4 {[]}
            k5 {[1.5,2.5]}
            k6 {-12}
            k7 {{"a":[1,{"b":null}]}}
        } {
            assert_equal OK [r json.set $key {$} $value]
            assert_equal $value [r json.get $key]
            assert_equal "\[$value\]" [r json.get $key {$}]
        }
        # an empty path is the root as well
        assert_equal OK [r json.set k7 {} {[1]}]
        assert_equal {[1]} [r json.get k7 {}]
        # insignificant whitespace is dropped
        assert_equal OK [r json.set k8 . "  \[1 , 2,\t\n {\"a\" : 1 } \]  "]
        assert_equal {[1,2,{"a":1}]} [r json.get k8]
    }

    test "JSON.SET NX and XX options (caps $caps)" {
        json_setup_wikipedia
        foreach {path value cond exp_set exp_get} {
            {.address.city} {"Boston"} NX {}  {"New York"}
            {.address.city} {"Boston"} XX OK  {"Boston"}
            {.foo}          {"bar"}    NX OK  {"bar"}
            {.firstName}    {"bar"}    NX {}  {"John"}
        } {
            assert_equal $exp_set [r json.set wikipedia $path $value $cond]
            assert_equal $exp_get [r json.get wikipedia $path]
        }
        # ported as written in the module's test: the path is ."value"
        assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.set k1 {."value"} badword}
        # an unknown option
        assert_error "SYNTAXERR Command syntax error" {r json.set k1 . {"value"} badword}
        assert_equal 0 [r exists k1]

        # the options are case-insensitive
        assert_equal {} [r json.set wikipedia . 1 nx]
        assert_equal OK [r json.set wikipedia .age 28 xx]
        assert_equal 28 [r json.get wikipedia .age]

        # the same with JSONPath syntax
        json_setup_wikipedia
        foreach {path value cond exp_set exp_get} {
            {$.address.city} {"Boston"} NX {}  {["New York"]}
            {$.address.city} {"Boston"} XX OK  {["Boston"]}
            {$.foo}          {"bar"}    XX {}  {[]}
            {$.foo}          {"bar"}    NX OK  {["bar"]}
            {$.firstName}    {"bar"}    NX {}  {["John"]}
        } {
            assert_equal $exp_set [r json.set wikipedia $path $value $cond]
            assert_equal $exp_get [r json.get wikipedia $path]
        }

        # a new key
        assert_equal {} [r json.set k2 {$} 1 XX]
        assert_equal 0 [r exists k2]
        assert_equal OK [r json.set k2 {$} 1 NX]
        assert_equal {} [r json.set k2 {$} 2 NX]
        assert_equal OK [r json.set k2 {$} 3 XX]
        assert_equal 3 [r json.get k2]
    }

    test "JSON.SET NX and XX with a wildcard that both updates and inserts (caps $caps)" {
        r flushall
        r json.set k . {{"a":{"x":1},"b":{},"c":3}}
        # some matches exist: NX does nothing, XX updates the existing ones and inserts the others
        assert_equal {} [r json.set k {$.*.x} 7 NX]
        assert_equal {{"a":{"x":1},"b":{},"c":3}} [r json.get k]
        r json.set k . {{"a":{"x":1},"b":{"x":2},"c":3}}
        assert_equal OK [r json.set k {$.*.x} 8 XX]
        assert_equal {{"a":{"x":8},"b":{"x":8},"c":3}} [r json.get k]
        # one update and one insert: neither NX nor XX applies
        r json.set k . {{"a":{"x":1},"b":{}}}
        assert_equal {} [r json.set k {$.*.x} 7 NX]
        assert_equal {} [r json.set k {$.*.x} 8 XX]
        assert_equal {{"a":{"x":1},"b":{}}} [r json.get k]
        # a wildcard over an empty container matches nothing: OK whatever the option
        r json.set k . {{"a":[],"b":{}}}
        foreach path {{$.a[*]} {.a[*]} {$.b.*} {.b.*}} {
            assert_equal OK [r json.set k $path 1 NX]
            assert_equal OK [r json.set k $path 1 XX]
        }
        assert_equal {{"a":[],"b":{}}} [r json.get k]
        # but a missing parent is an error with either syntax
        foreach path {{$.zz[*]} {.zz[*]} {$.zz.*} {.zz.*}} {
            assert_error "NONEXISTENT JSON path does not exist" {r json.set k $path 1}
        }
    }

    test "JSON.SET with one update and one insert sets both (design 2.5, module defect not reproduced) (caps $caps)" {
        r flushall
        # The external module moves the new value into the update and then
        # inserts what is left, giving {"a":{"x":5},"b":{"x":1}}.
        r json.set k . {{"a":{"x":1},"b":{}}}
        assert_equal OK [r json.set k {$.*.x} 5]
        assert_equal {{"a":{"x":5},"b":{"x":5}}} [r json.get k]

        r json.set k . {{"a":{"x":1},"b":{},"c":3}}
        assert_equal OK [r json.set k {$.*.x} {{"y":[1,2]}}]
        assert_equal {{"a":{"x":{"y":[1,2]}},"b":{"x":{"y":[1,2]}},"c":3}} [r json.get k]

        # two updates and one insert already worked in the module
        r json.set k . {{"a":{"x":1},"b":{},"c":{"x":2}}}
        assert_equal OK [r json.set k {$.*.x} 5]
        assert_equal {{"a":{"x":5},"b":{"x":5},"c":{"x":5}}} [r json.get k]

        # legacy path
        r json.set k . {{"a":{"x":1},"b":{},"c":3}}
        assert_equal OK [r json.set k {.*.x} 6]
        assert_equal {{"a":{"x":6},"b":{"x":6},"c":3}} [r json.get k]
    }

    test "JSON.SET error conditions (caps $caps)" {
        json_setup_wikipedia
        # A new key's path must be root
        assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.set foo .bar {"bar"}}
        assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.set foo {$.bar} {"bar"}}
        assert_equal 0 [r exists foo]

        # XX sets the value only if the path exists. If it does not, the reply is null, not an error.
        assert_equal {} [r json.set k1 . {"some value"} XX]
        assert_equal 0 [r exists k1]
        assert_equal {} [r json.set wikipedia .foo {"bar"} XX]
        assert_error "NONEXISTENT JSON path does not exist" {r json.get wikipedia .foo}

        # NX sets the value only if the path does not exist. If it does, the reply is null, not an error.
        assert_equal {} [r json.set wikipedia . {"some new value"} NX]
        assert_equal {} [r json.set wikipedia .firstName {"Tom"} NX]
        assert_equal {"John"} [r json.get wikipedia .firstName]

        # syntax error: wrong option
        assert_error "SYNTAXERR Command syntax error" {r json.set wikipedia . {"some new value"} NXXX}
        assert_error "SYNTAXERR Command syntax error" {r json.set wikipedia . {"some new value"} XXXX}
        assert_error "*wrong number of arguments for 'JSON.SET' command" {r json.set wikipedia . {"bar"} a b}
        assert_error "*wrong number of arguments for 'JSON.SET' command" {r json.set wikipedia .}
        assert_error "*wrong number of arguments for 'JSON.SET' command" {r json.set wikipedia}
        assert_error "*wrong number of arguments for 'JSON.SET' command" {r json.set}

        # a key that does not hold a JSON document
        assert_error "WRONGTYPE Not a JSON document key" {r json.set str_key . 1}
        assert_error "WRONGTYPE Not a JSON document key" {r json.set str_key .a 1}
        assert_error "WRONGTYPE Not a JSON document key" {r json.set str_key {$.a} 1 NX}
        assert_equal {{"firstName":"John","lastName":"Smith"}} [r get str_key]

        # an invalid path
        assert_error "SYNTAXERR*" {r json.set wikipedia {$[} 1}
        assert_error "SYNTAXERR*" {r json.set wikipedia {.address[} 1}

        # nothing above changed the document
        assert_equal [json_wikipedia_compact] [r json.get wikipedia]
    }

    test "JSON.SET ancestor keys are not overridden (caps $caps)" {
        json_setup_wikipedia
        assert_error "ERROR Cannot insert a member into a non-object value" {
            r json.set wikipedia .firstName.a {"some new value"}
        }
        assert_error "ERROR Cannot insert a member into a non-object value" {
            r json.set wikipedia .age.a 1
        }
        assert_error "NONEXISTENT JSON path does not exist" {
            r json.set wikipedia .address.a.b {"some new value"}
        }
        assert_error "WRONGTYPE JSON element is not an array" {
            r json.set wikipedia {.address[0]} {"some new value"}
        }
        # the same errors with JSONPath syntax
        assert_error "ERROR Cannot insert a member into a non-object value" {
            r json.set wikipedia {$.firstName.a} {"some new value"}
        }
        assert_error "ERROR Cannot insert a member into a non-object value" {
            r json.set wikipedia {$.age.a} 1
        }
        assert_error "NONEXISTENT JSON path does not exist" {
            r json.set wikipedia {$.address.a.b} {"some new value"}
        }
        assert_error "WRONGTYPE JSON element is not an array" {
            r json.set wikipedia {$.address[0]} {"some new value"}
        }
        assert_equal [json_wikipedia_compact] [r json.get wikipedia]
    }

    test "JSON.SET rejects an out of boundary array index (caps $caps)" {
        json_setup_wikipedia
        foreach path {
            {phoneNumbers[9]} {phoneNumbers[2]} {phoneNumbers[-3]}
            {$.phoneNumbers[9]} {$.phoneNumbers[2]} {$.phoneNumbers[-3]}
        } {
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {
                r json.set wikipedia $path {"123"}
            }
        }
        assert_equal [json_wikipedia_compact] [r json.get wikipedia]
    }

    test "JSON.SET inserts a value only as the last step of the path (caps $caps)" {
        json_setup_wikipedia
        # insert is allowed if and only if the parent node is the last child in the path.
        foreach {path new_val} {
            {["address"]["z"]} {"z"}
            {.address.z2}      {"z2"}
        } {
            assert_equal OK [r json.set wikipedia $path $new_val]
            assert_equal $new_val [r json.get wikipedia $path]
        }
        assert_equal {{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100","z":"z","z2":"z2"}} [r json.get wikipedia .address]

        # if the parent node is not the last child in the path, insertion is not allowed.
        foreach {path new_val} {
            {["address"]["foo"]["z"]} {"z"}
            {.address.foo.z}          {"z"}
            {$.address.foo.z}         {"z"}
        } {
            assert_error "NONEXISTENT JSON path does not exist" {
                r json.set wikipedia $path $new_val
            }
        }
        assert_equal {{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100","z":"z","z2":"z2"}} [r json.get wikipedia .address]

        # a new member goes last, into the root and into an empty object
        assert_equal OK [r json.set wikipedia {$.groups.g1} {[1]}]
        assert_equal OK [r json.set wikipedia {$.newRoot} {{}}]
        assert_equal OK [r json.set wikipedia {.newRoot.child} null]
        assert_equal {[{"g1":[1]}]} [r json.get wikipedia {$.groups}]
        assert_equal {[{"child":null}]} [r json.get wikipedia {$.newRoot}]
        assert_equal {firstName lastName age weight isAlive address phoneNumbers children spouse groups newRoot} [r json.objkeys wikipedia]
    }

    test "JSON.SET with a negative array index (caps $caps)" {
        json_setup_wikipedia
        assert_equal OK [r json.set wikipedia {.phoneNumbers[-1].number} {"1-2-3"}]
        assert_equal {"1-2-3"} [r json.get wikipedia {.phoneNumbers[-1].number}]
        assert_equal {["212 555-1234","1-2-3"]} [r json.get wikipedia {$.phoneNumbers[*].number}]
        assert_equal OK [r json.set wikipedia {$.phoneNumbers[-2].number} {"4-5-6"}]
        assert_equal {["4-5-6","1-2-3"]} [r json.get wikipedia {$.phoneNumbers[*].number}]
        # replace a whole element
        assert_equal OK [r json.set wikipedia {$.phoneNumbers[-1]} 7]
        assert_equal {[[{"type":"home","number":"4-5-6"},7]]} [r json.get wikipedia {$.phoneNumbers}]
    }

    test "JSON.SET with legacy and JSONPath wildcards (caps $caps)" {
        json_setup_wikipedia
        foreach key {wikipedia2 wikipedia3 wikipedia4} {
            r json.set $key . [json_wikipedia_compact]
        }
        foreach {key path new_val exp path2 exp2} {
            wikipedia  {$.address.*}              {"1"} {["1","1","1","1"]} {$.address.*}              {["1","1","1","1"]}
            wikipedia2 {.address.*}               {"1"} {"1"}               {$.address.*}              {["1","1","1","1"]}
            wikipedia3 {$.phoneNumbers[*].number} {"1"} {["1","1"]}         {$.phoneNumbers[*].number} {["1","1"]}
            wikipedia4 {.phoneNumbers[*].number}  {"1"} {"1"}               {$.phoneNumbers[*].number} {["1","1"]}
        } {
            assert_equal OK [r json.set $key $path $new_val]
            assert_equal $exp [r json.get $key $path]
            # verify all values
            assert_equal $exp2 [r json.get $key $path2]
        }
        assert_equal {{"firstName":"John","lastName":"Smith","age":27,"weight":135.17,"isAlive":true,"address":{"street":"1","city":"1","state":"1","zipcode":"1"},"phoneNumbers":[{"type":"home","number":"212 555-1234"},{"type":"office","number":"646 555-4567"}],"children":[],"spouse":null,"groups":{}}} [r json.get wikipedia2]
        assert_equal {{"firstName":"John","lastName":"Smith","age":27,"weight":135.17,"isAlive":true,"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},"phoneNumbers":[{"type":"home","number":"1"},{"type":"office","number":"1"}],"children":[],"spouse":null,"groups":{}}} [r json.get wikipedia3]

        r json.set k1 . {{"a":[], "b":[1], "c":[1,2,3]}}
        r json.set k2 . {{"a":{}, "b":{"a":1}, "c":{"a":1, "b":2, "c":3}}}

        # NOTE: The expected results below account for the outcome of previous commands.
        # A wildcard over an empty container matches nothing: JSON.SET replies
        # OK and changes nothing with either syntax, and a legacy JSON.GET of
        # that path is an error.
        foreach {key path new_val exp exp2} {
            k1 {$.a[*]} 1 {[]}      {{"a":[],"b":[1],"c":[1,2,3]}}
            k1 {$.b[*]} 2 {[2]}     {{"a":[],"b":[2],"c":[1,2,3]}}
            k1 {$.c[*]} 4 {[4,4,4]} {{"a":[],"b":[2],"c":[4,4,4]}}
            k1 {.a[*]}  1 -         -
            k1 {.b[*]}  3 {3}       {{"a":[],"b":[3],"c":[4,4,4]}}
            k1 {.c[*]}  5 {5}       {{"a":[],"b":[3],"c":[5,5,5]}}
            k2 {$.a.*}  1 {[]}      {{"a":{},"b":{"a":1},"c":{"a":1,"b":2,"c":3}}}
            k2 {$.b.*}  2 {[2]}     {{"a":{},"b":{"a":2},"c":{"a":1,"b":2,"c":3}}}
            k2 {$.c.*}  4 {[4,4,4]} {{"a":{},"b":{"a":2},"c":{"a":4,"b":4,"c":4}}}
            k2 {.a.*}   1 -         -
            k2 {.b.*}   3 {3}       {{"a":{},"b":{"a":3},"c":{"a":4,"b":4,"c":4}}}
            k2 {.c.*}   5 {5}       {{"a":{},"b":{"a":3},"c":{"a":5,"b":5,"c":5}}}
        } {
            if {$exp eq "-"} {
                set before [r json.get $key .]
                assert_equal OK [r json.set $key $path $new_val]
                assert_error "NONEXISTENT JSON path does not exist" {r json.get $key $path}
                assert_equal $before [r json.get $key .]
            } else {
                assert_equal OK [r json.set $key $path $new_val]
                assert_equal $exp [r json.get $key $path]
                # verify entire key
                assert_equal $exp2 [r json.get $key .]
            }
        }
    }

    test "JSON.SET with a path that selects nothing (caps $caps)" {
        r flushall
        r json.set k . {{"a":[],"b":{}}}
        # a slice, a union or a recursive descent that selects nothing: OK, no change
        foreach path {{$.a[0:5]} {$.b["x","y"]} {$..zz} {$.a[*]} {$.b.*}} {
            assert_equal OK [r json.set k $path 1]
        }
        assert_equal {{"a":[],"b":{}}} [r json.get k]
        # a missing parent or an index out of bounds is an error
        assert_error "NONEXISTENT JSON path does not exist" {r json.set k {$.zz.yy} 1}
        assert_error "NONEXISTENT JSON path does not exist" {r json.set k {$.zz[0]} 1}
        assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.set k {$.a[0]} 1}
        assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.set k {.a[0]} 1}
        assert_equal {{"a":[],"b":{}}} [r json.get k]
    }

    test "JSON.SET resolves nested matches against the document before the command (B4) (caps $caps)" {
        r flushall
        # The external module applies the outer match and then re-resolves the
        # path, giving {"a":{"a":{"a":2}}}.
        r json.set k . {{"a":{"a":1}}}
        assert_equal OK [r json.set k {$..a} {{"a":2}}]
        assert_equal {{"a":{"a":2}}} [r json.get k]

        r json.set k . {{"a":{"a":{"a":1},"b":2},"c":{"a":3}}}
        assert_equal OK [r json.set k {$..a} {"x"}]
        assert_equal {{"a":"x","c":{"a":"x"}}} [r json.get k]
    }

    test "JSON.SET replaces values of every type inside arrays and objects (caps $caps)" {
        r flushall
        # an array of integers, then elements of other types written into it
        r json.set k . {[1,2,3,4,5,6,7,8]}
        assert_equal OK [r json.set k {$[0]} 1.5]
        assert_equal {[1.5,2,3,4,5,6,7,8]} [r json.get k]
        assert_equal OK [r json.set k {$[1]} {"two"}]
        assert_equal OK [r json.set k {.[2]} {{"three":3}}]
        assert_equal OK [r json.set k {$[3]} {[4,[4]]}]
        assert_equal OK [r json.set k {$[4]} null]
        assert_equal OK [r json.set k {$[-1]} true]
        assert_equal {[1.5,"two",{"three":3},[4,[4]],null,6,7,true]} [r json.get k]
        assert_equal OK [r json.set k {$[*]} 0]
        assert_equal {[0,0,0,0,0,0,0,0]} [r json.get k]
        assert_equal OK [r json.set k {$[1:7:2]} {"x"}]
        assert_equal {[0,"x",0,"x",0,"x",0,0]} [r json.get k]
        assert_equal OK [r json.set k {$[0,-1]} {[]}]
        assert_equal {[[],"x",0,"x",0,"x",0,[]]} [r json.get k]

        # an array of doubles
        r json.set k . {[0.5,1.25,2.125]}
        assert_equal OK [r json.set k {$[1]} 7]
        assert_equal {[0.5,7,2.125]} [r json.get k]
        assert_equal OK [r json.set k {$[2]} {"s"}]
        assert_equal {[0.5,7,"s"]} [r json.get k]

        # a nested container shrinks to a scalar and grows back
        r json.set k . {{"a":{"b":{"c":[1,2,3]}},"z":"end"}}
        assert_equal OK [r json.set k {$.a.b} 1]
        assert_equal {{"a":{"b":1},"z":"end"}} [r json.get k]
        assert_equal OK [r json.set k {.a.b} {{"c":{"d":{"e":[{"f":"a longer string value than before"}]}}}}]
        assert_equal {{"a":{"b":{"c":{"d":{"e":[{"f":"a longer string value than before"}]}}}},"z":"end"}} [r json.get k]
        assert_equal OK [r json.set k {$.a.b.c.d.e[0].f} {""}]
        assert_equal {{"a":{"b":{"c":{"d":{"e":[{"f":""}]}}}},"z":"end"}} [r json.get k]
        assert_equal {["end"]} [r json.get k {$.z}]
    }

    test "JSON.SET on a document larger than the default caps (caps $caps)" {
        r flushall
        # 300 elements and 300 members: over the default 128 entries per packed node
        set arr {}
        set obj {}
        for {set i 0} {$i < 300} {incr i} {
            lappend arr $i
            lappend obj "\"m$i\":$i"
        }
        assert_equal OK [r json.set k . "{\"arr\":\[[join $arr ,]\],\"obj\":{[join $obj ,]}}"]
        assert_equal {[0,299,150]} [r json.get k {$.arr[0,-1,150]}]
        assert_equal {[0,299,150]} [r json.get k {$.obj["m0","m299","m150"]}]
        assert_equal OK [r json.set k {$.arr[150]} {"x"}]
        assert_equal OK [r json.set k {$.obj.m150} {"y"}]
        assert_equal OK [r json.set k {$.obj.m300} {{"new":true}}]
        assert_equal {[149,"x",151]} [r json.get k {$.arr[149:152]}]
        assert_equal {"y"} [r json.get k .obj.m150]
        assert_equal {{"new":true}} [r json.get k .obj.m300]
        set arr [lreplace $arr 150 150 {"x"}]
        set obj [lreplace $obj 150 150 {"m150":"y"}]
        lappend obj {"m300":{"new":true}}
        assert_equal "{\"arr\":\[[join $arr ,]\],\"obj\":{[join $obj ,]}}" [r json.get k]
        # every element through a wildcard
        assert_equal OK [r json.set k {$.arr[*]} null]
        assert_equal "\[\[[join [lrepeat 300 null] ,]\]\]" [r json.get k {$.arr}]
        assert_equal 300 [r json.del k {$.arr[*]}]
        assert_equal 301 [r json.del k {$.obj.*}]
        assert_equal {{"arr":[],"obj":{}}} [r json.get k]
    }

    # ------------------------------------------------------------------
    # JSON.GET
    # ------------------------------------------------------------------

    test "JSON.GET supports all datatypes (caps $caps)" {
        json_setup_wikipedia
        json_assert_get wikipedia {
            {.firstName}    {"John"}
            {.address.city} {"New York"}
            {.spouse}       {null}
            {.children}     {[]}
            {.groups}       {{}}
            {.isAlive}      {true}
            {.age}          {27}
            {["weight"]}    {135.17}
        }
        json_assert_get wikipedia {
            {$.firstName}    {["John"]}
            {$.address.city} {["New York"]}
            {$.spouse}       {[null]}
            {$.children}     {[[]]}
            {$.groups}       {[{}]}
            {$.isAlive}      {[true]}
            {$.age}          {[27]}
            {$["weight"]}    {[135.17]}
        }
    }

    test "JSON.GET path syntax for object keys (caps $caps)" {
        json_setup_wikipedia
        json_assert_get wikipedia {
            {["firstName"]}         {"John"}
            {address['city']}       {"New York"}
            {['address']['city']}   {"New York"}
            {["address"]["city"]}   {"New York"}
            {["address"]['city']}   {"New York"}
            {["isAlive"]}           {true}
            {['age']}               {27}
            {["weight"]}            {135.17}
        }

        # invalid json path
        foreach {path err} {
            {[firstName"]}              {SYNTAXERR Array index is not a number}
            {address["city'}            {SYNTAXERR Failed to parse JSON string due to syntax error}
            {["address'][[[["city"]}    {SYNTAXERR Invalid JSON path}
            {[[["address"]]]["city"]}   {SYNTAXERR Array index is not a number}
            {"["address"]['city']}      {SYNTAXERR Invalid object member name}
            {"['address"]["city"]}      {SYNTAXERR Invalid object member name}
            {[""""address]["city"]}     {SYNTAXERR Invalid JSON path}
            {[address""""]}             {SYNTAXERR Array index is not a number}
            {['address']]]['city']}     {SYNTAXERR Invalid object member name}
            {["address"]'['"city"]}     {SYNTAXERR Invalid object member name}
        } {
            assert_error $err {r json.get wikipedia $path}
        }
    }

    test "JSON.GET floating point values keep their text (caps $caps)" {
        json_setup_wikipedia
        foreach value {0 0.1 0.3 1.23456789 -0.1 -0.3 -1.23456789} {
            assert_equal OK [r json.set wikipedia .foo $value]
            assert_equal $value [r json.get wikipedia .foo]
        }

        # max double and min double: floating points are returned exactly as is
        assert_equal OK [r json.set wikipedia .foo 1.7976e+308]
        assert_equal OK [r json.set wikipedia .bar -1.7976e+308]
        assert_equal 1.7976e+308 [r json.get wikipedia .foo]
        assert_equal -1.7976e+308 [r json.get wikipedia .bar]

        # 1.234567890123456789 exceeds the precision of a double but persists regardless.
        assert_equal OK [r json.set wikipedia .foo 1.234567890123456789]
        assert_equal 1.234567890123456789 [r json.get wikipedia .foo]

        # trailing zeros are not removed.
        assert_equal OK [r json.set wikipedia .foo 0.3000000]
        assert_equal 0.3000000 [r json.get wikipedia .foo]
        assert_equal {[0.3000000,-1.7976e+308]} [r json.get wikipedia {$["foo","bar"]}]
    }

    test "JSON.GET with multiple paths (caps $caps)" {
        json_setup_wikipedia
        assert_equal {{".firstName":"John",".lastName":"Smith"}} [r json.get wikipedia .firstName .lastName]
        assert_error "NONEXISTENT JSON path does not exist" {
            r json.get wikipedia .firstName .lastName .foo .bar
        }
        # a path named twice appears twice
        assert_equal {{".age":27,".age":27}} [r json.get wikipedia .age .age]
        assert_equal {{"$.age":[27],"$.age":[27],"$.groups":[{}]}} [r json.get wikipedia {$.age} {$.age} {$.groups}]
        r json.set one . 1
        assert_equal {{".":1,".":1}} [r json.get one . .]
    }

    test "JSON.GET returns compact JSON by default (caps $caps)" {
        json_setup_wikipedia
        # default format is compact JSON string - no indent, no space and no newline
        assert_equal [json_wikipedia_compact] [r json.get wikipedia]
        assert_equal [json_wikipedia_compact] [r json.get wikipedia .]
        assert_equal "\[[json_wikipedia_compact]\]" [r json.get wikipedia {$}]
        # NOESCAPE is accepted and ignored
        assert_equal [json_wikipedia_compact] [r json.get wikipedia NOESCAPE]
        assert_equal [json_wikipedia_compact] [r json.get wikipedia noescape NOESCAPE]
        assert_equal {[]} [r json.get wikipedia NOESCAPE .children NOESCAPE]
    }

    test "JSON.GET with INDENT, SPACE and NEWLINE (caps $caps)" {
        json_setup_wikipedia
        set exp [json_wikipedia_pretty]

        # get the root document with custom indent/space/newline
        assert_equal $exp [r json.get wikipedia INDENT {  } SPACE { } NEWLINE "\n"]

        # NOESCAPE is ignored wherever it appears
        assert_equal $exp [r json.get wikipedia INDENT {  } SPACE { } NEWLINE "\n" NOESCAPE]
        assert_equal $exp [r json.get wikipedia NOESCAPE INDENT {  } SPACE { } NEWLINE "\n"]
        assert_equal $exp [r json.get wikipedia INDENT {  } NOESCAPE SPACE { } NEWLINE "\n"]

        # the option names are case-insensitive
        assert_equal $exp [r json.get wikipedia indent {  } Space { } newLine "\n"]

        # get a sub-document with custom indent/space/newline
        set exp_json "{\n\t\"street\": \"21 2nd Street\",\n\t\"city\": \"New York\",\n\t\"state\": \"NY\",\n\t\"zipcode\": \"10021-3100\"\n}"
        assert_equal $exp_json [r json.get wikipedia INDENT "\t" SPACE { } NEWLINE "\n" .address]

        # INDENT: =*=*, SPACE: --, NEWLINE: \r\n
        set exp_json "{\r\n=*=*\"street\":--\"21 2nd Street\",\r\n=*=*\"city\":--\"New York\",\r\n=*=*\"state\":--\"NY\",\r\n=*=*\"zipcode\":--\"10021-3100\"\r\n}"
        assert_equal $exp_json [r json.get wikipedia INDENT =*=* SPACE -- NEWLINE "\r\n" .address]

        # path args do not need to be positioned at the end
        assert_equal $exp [r json.get wikipedia . INDENT {  } SPACE { } NEWLINE "\n"]
        assert_equal {"John"} [r json.get wikipedia INDENT {  } .firstName SPACE { } NEWLINE "\n"]

        set exp_json "{\n  \".firstName\": \"John\",\n  \".lastName\": \"Smith\"\n}"
        assert_equal $exp_json [r json.get wikipedia INDENT {  } SPACE { } .firstName .lastName NEWLINE "\n"]
        set exp_json "{\n\t\"street\": \"21 2nd Street\",\n\t\"city\": \"New York\",\n\t\"state\": \"NY\",\n\t\"zipcode\": \"10021-3100\"\n}"
        assert_equal $exp_json [r json.get wikipedia INDENT "\t" .address SPACE { } NEWLINE "\n"]
        set exp_json "\[\n\t{\n\t\t\"street\": \"21 2nd Street\",\n\t\t\"city\": \"New York\",\n\t\t\"state\": \"NY\",\n\t\t\"zipcode\": \"10021-3100\"\n\t}\n\]"
        assert_equal $exp_json [r json.get wikipedia INDENT "\t" {$.address} SPACE { } NEWLINE "\n"]

        # path args can have formatting in between them
        set exp_json "{\n  \".firstName\": \"John\",\n  \".lastName\": \"Smith\"\n}"
        assert_equal $exp_json [r json.get wikipedia INDENT {  } .firstName SPACE { } .lastName NEWLINE "\n"]
        assert_equal $exp_json [r json.get wikipedia .firstName INDENT {  } SPACE { } NEWLINE "\n" .lastName]
    }

    test "JSON.GET formatting of empty and nested containers (caps $caps)" {
        r flushall
        r json.set e . {{"a":[],"b":{},"c":[{}],"d":{"e":[]}}}
        assert_equal "{\n  \"a\": \[\],\n  \"b\": {},\n  \"c\": \[\n    {}\n  \],\n  \"d\": {\n    \"e\": \[\]\n  }\n}" \
            [r json.get e INDENT {  } NEWLINE "\n" SPACE { }]
        assert_equal "\[\n  \[\]\n\]" [r json.get e INDENT {  } NEWLINE "\n" SPACE { } {$..e}]
        assert_equal "\[\]" [r json.get e INDENT {  } NEWLINE "\n" SPACE { } {$.zz}]
        assert_equal "{\n  \".a\": \[\],\n  \".b\": {}\n}" [r json.get e INDENT {  } NEWLINE "\n" SPACE { } .a .b]
        assert_equal {{+-"a":_[],+-"b":_{},+-"c":_[+--{}+-],+-"d":_{+--"e":_[]+-}+}} [r json.get e indent - newline + space _]
        # each option alone
        assert_equal {{z"a":[],z"b":{},z"c":[z{}z],z"d":{z"e":[]z}z}} [r json.get e NEWLINE z]
        assert_equal {{"a":y[],"b":y{},"c":y[{}],"d":y{"e":y[]}}} [r json.get e SPACE y]
        assert_equal {{x"a":[],x"b":{},x"c":[xx{}x],x"d":{xx"e":[]x}}} [r json.get e INDENT x]
        # scalars are not affected
        r json.set s . {"str"}
        assert_equal {"str"} [r json.get s INDENT {  } NEWLINE "\n" SPACE { }]
        assert_equal "\[\n  \"str\"\n\]" [r json.get s INDENT {  } NEWLINE "\n" SPACE { } {$}]
    }

    test "JSON.GET formatting error conditions (caps $caps)" {
        json_setup_wikipedia
        # NEWLINE is the last arg
        assert_error "SYNTAXERR Command syntax error" {r json.get wikipedia INDENT {  } SPACE { } NEWLINE}
        # SPACE is the last arg
        assert_error "SYNTAXERR Command syntax error" {r json.get wikipedia INDENT {  } SPACE}
        # INDENT is the last arg
        assert_error "SYNTAXERR Command syntax error" {r json.get wikipedia NEWLINE "\n" SPACE { } INDENT}
        assert_error "SYNTAXERR Command syntax error" {r json.get wikipedia INDENT}
        # the arguments are checked even when the key does not exist
        assert_error "SYNTAXERR Command syntax error" {r json.get nokey INDENT}
    }

    test "JSON.GET error conditions (caps $caps)" {
        json_setup_wikipedia
        # if the document key does not exist, the command returns null without an error.
        assert_equal {} [r json.get foo .firstName]
        assert_equal {} [r json.get foo {$.firstName}]
        assert_equal {} [r json.get foo]
        assert_equal {} [r json.get foo .a .b]
        # ... even if the path is invalid
        assert_equal {} [r json.get foo {$[}]

        # if the key is not a document key, the command fails.
        assert_error "WRONGTYPE Not a JSON document key" {r json.get str_key}
        assert_error "WRONGTYPE Not a JSON document key" {r json.get str_key .firstName}
        assert_error "WRONGTYPE Not a JSON document key" {r json.get str_key {$.firstName}}

        # if a legacy path does not exist, the command fails; a JSONPath gives an empty array.
        assert_error "NONEXISTENT JSON path does not exist" {r json.get wikipedia .foo}
        assert_error "NONEXISTENT JSON path does not exist" {r json.get wikipedia .address.foo.bar}
        assert_equal {[]} [r json.get wikipedia {$.foo}]
        assert_equal {[]} [r json.get wikipedia {$.address.foo.bar}]

        # an invalid path
        assert_error "SYNTAXERR Expression token cannot be empty" {r json.get wikipedia {$[}}

        # Wrong number of arguments
        assert_error "*wrong number of arguments for 'JSON.GET' command" {r json.get}
    }

    test "JSON.GET and JSON.SET with numbers as member names (caps $caps)" {
        r flushall
        foreach key {k1 k2 k3 k4 k5} {
            r json.set $key . {{"1":1, "2":{"3":4}}}
        }

        # test GET
        json_assert_get k1 {
            {["2"]["3"]} {4}
            {['2']['3']} {4}
            {.1}         {1}
            {.2.3}       {4}
            {$.2.3}      {[4]}
        }

        # test SET
        foreach {key path val exp_new_val} {
            k1 {["2"]["3"]} 5 {{"1":1,"2":{"3":5}}}
            k2 {['2']['3']} 5 {{"1":1,"2":{"3":5}}}
            k3 {.2.3}       5 {{"1":1,"2":{"3":5}}}
            k4 {.1}         2 {{"1":2,"2":{"3":4}}}
            k5 {$.2.*}      5 {{"1":1,"2":{"3":5}}}
        } {
            assert_equal OK [r json.set $key $path $val]
            assert_equal $exp_new_val [r json.get $key .]
        }
    }

    test "JSON.GET with legacy and JSONPath wildcards (caps $caps)" {
        # A JSONPath starts with the dollar sign and always returns an array of
        # values. Any other path is a legacy path: it returns a single value,
        # the first one selected, or a NONEXISTENT error if nothing is selected.
        json_setup_wikipedia
        json_assert_get wikipedia {
            {$.address.*}                          {["21 2nd Street","New York","NY","10021-3100"]}
            {$.['address'].*}                      {["21 2nd Street","New York","NY","10021-3100"]}
            {.address.*}                           {"21 2nd Street"}
            {.["address"].*}                       {"21 2nd Street"}
            {$.phoneNumbers.*.type}                {["home","office"]}
            {$.phoneNumbers[*].type}               {["home","office"]}
            {$.["phoneNumbers"][*].["type"]}       {["home","office"]}
            {.phoneNumbers.*.type}                 {"home"}
            {.phoneNumbers[*].type}                {"home"}
            {.["phoneNumbers"][*].["type"]}        {"home"}
            {$.[ 'address' ].*}                    {["21 2nd Street","New York","NY","10021-3100"]}
            {.[ "address" ].*}                     {"21 2nd Street"}
            {$.[ "phoneNumbers" ][ * ].[ "type" ]} {["home","office"]}
        }

        r json.set k1 . {{"a":[], "b":[1], "c":[1,2]}}
        r json.set k2 . {{"a":{}, "b": {"a": 1}, "c": {"a": 1, "b": 2}}}
        r json.set k3 . {{"a":[[[1,2],[3,4],[5,6]],[[7,8],[9,10],[11,12]]]}}
        r json.set k4 . {{"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h:":1}}}}}}}}}

        # JSONPath always returns an array of values. Test multiple wildcards.
        foreach {key path exp} {
            k1 {$.a[*]}            {[]}
            k1 {$.b[*]}            {[1]}
            k1 {$.c[*]}            {[1,2]}
            k2 {$.a.*}             {[]}
            k2 {$.b.*}             {[1]}
            k2 {$.c.*}             {[1,2]}
            k3 {$.a[*]}            {[[[1,2],[3,4],[5,6]],[[7,8],[9,10],[11,12]]]}
            k3 {$.a[*][*]}         {[[1,2],[3,4],[5,6],[7,8],[9,10],[11,12]]}
            k3 {$.a[*][*][1]}      {[2,4,6,8,10,12]}
            k4 {$.a.*.*}           {[{"d":{"e":{"f":{"g":{"h:":1}}}}}]}
            k4 {$.a.*.*.*}         {[{"e":{"f":{"g":{"h:":1}}}}]}
            k4 {$.a.*.c.*}         {[{"e":{"f":{"g":{"h:":1}}}}]}
            k4 {$.a.*.c.*.e.*}     {[{"g":{"h:":1}}]}
            k4 {$.a.*.c.*.e.*.g.*} {[1]}
        } {
            assert_equal $exp [r json.get $key $path] "JSON.GET $key $path"
        }

        # Legacy path always returns a single value, which is the first value.
        foreach {key path exp} {
            k1 {.b[*]}            {1}
            k1 {.c[*]}            {1}
            k2 {.b.*}             {1}
            k2 {.c.*}             {1}
            k3 {.a[*]}            {[[1,2],[3,4],[5,6]]}
            k3 {.a[*][*]}         {[1,2]}
            k3 {.a[*][*][1]}      {2}
            k4 {.a.*.*}           {{"d":{"e":{"f":{"g":{"h:":1}}}}}}
            k4 {.a.*.*.*}         {{"e":{"f":{"g":{"h:":1}}}}}
            k4 {.a.*.c.*}         {{"e":{"f":{"g":{"h:":1}}}}}
            k4 {.a.*.c.*.e.*}     {{"g":{"h:":1}}}
            k4 {.a.*.c.*.e.*.g.*} {1}
        } {
            assert_equal $exp [r json.get $key $path] "JSON.GET $key $path"
        }

        # Legacy path returns non-existent error if no value is selected.
        assert_error "NONEXISTENT JSON path does not exist" {r json.get k1 {.a[*]}}
        assert_error "NONEXISTENT JSON path does not exist" {r json.get k2 {.a.*}}
    }

    test "JSON.GET negative array index with a legacy path (caps $caps)" {
        json_setup_wikipedia
        json_assert_get wikipedia {
            {.phoneNumbers[-2].type}            {"home"}
            {.phoneNumbers[-1].type}            {"office"}
            {.phoneNumbers[ -1 ].type}          {"office"}
            {.["phoneNumbers"][-2]["type"]}     {"home"}
            {.["phoneNumbers"][-1]["type"]}     {"office"}
            {.["phoneNumbers"][ -1]["type" ]}   {"office"}
            {.["phoneNumbers"][-1 ][ "type"]}   {"office"}
            {.["phoneNumbers"][ -1 ][ "type" ]} {"office"}
        }

        # index out of bounds
        json_assert_get_error wikipedia "OUTOFBOUNDARIES Array index is out of bounds" {
            {.phoneNumbers[2].type}
            {.phoneNumbers[10].type}
            {.phoneNumbers[-3].type}
        }

        # an index on a non-array value
        json_assert_get_error wikipedia "WRONGTYPE JSON element is not an array" {
            {.firstName[-1]}
            {.age[-1]}
            {.weight[-1]}
            {.address[-1]}
            {.phoneNumbers[0][-1]}
        }
    }

    test "JSON.GET negative array index with a JSONPath (caps $caps)" {
        json_setup_wikipedia
        # JSONPath always returns an array of values
        json_assert_get wikipedia {
            {$.phoneNumbers[-2].type}              {["home"]}
            {$.phoneNumbers[ -2 ].type}            {["home"]}
            {$.phoneNumbers[-1].type}              {["office"]}
            {$.phoneNumbers[ -1].type}             {["office"]}
            {$.phoneNumbers[-1 ].type}             {["office"]}
            {$.["phoneNumbers"][-2]["type"]}       {["home"]}
            {$.["phoneNumbers"][-1]["type"]}       {["office"]}
            {$.phoneNumbers[2].type}               {[]}
            {$.phoneNumbers[10].type}              {[]}
            {$.firstName[-1]}                      {[]}
            {$.age[-1]}                            {[]}
            {$.weight[-1]}                         {[]}
            {$.weight[    -1 ]}                    {[]}
            {$.phoneNumbers[0][-1]}                {[]}
            {$.phoneNumbers[ 0][ -1 ]}             {[]}
            {$.phoneNumbers[ 0 ][ -1 ]}            {[]}
            {$.[ "phoneNumbers" ][ -1 ][ "type" ]} {["office"]}
            {$.phoneNumbers[-3].type}              {[]}
            {$.phoneNumbers[ -3 ].type}            {[]}
        }
    }

    test "JSON.GET array slices with a JSONPath (caps $caps)" {
        r flushall
        r json.set k1 . {[0,1,2,3,4,5,6,7,8,9]}
        json_assert_get k1 {
            {$[0:3]}         {[0,1,2]}
            {$[ 0 : 3 ]}     {[0,1,2]}
            {$[0:+3]}        {[0,1,2]}
            {$[ 0 : +3 ]}    {[0,1,2]}
            {$[0:-1]}        {[0,1,2,3,4,5,6,7,8]}
            {$[ 0 : -1 ]}    {[0,1,2,3,4,5,6,7,8]}
            {$[2:-2]}        {[2,3,4,5,6,7]}
            {$[ 2 : -2 ]}    {[2,3,4,5,6,7]}
            {$[+2:-2]}       {[2,3,4,5,6,7]}
            {$[1:1]}         {[]}
            {$[1:2]}         {[1]}
            {$[+1:+2]}       {[1]}
            {$[1:3]}         {[1,2]}
            {$[1:0]}         {[]}
            {$[5:]}          {[5,6,7,8,9]}
            {$[ 5 :  ]}      {[5,6,7,8,9]}
            {$[:3]}          {[0,1,2]}
            {$[  :  3]}      {[0,1,2]}
            {$[:+3]}         {[0,1,2]}
            {$[: +3]}        {[0,1,2]}
            {$[:6:2]}        {[0,2,4]}
            {$[ : 6 : 2]}    {[0,2,4]}
            {$[:]}           {[0,1,2,3,4,5,6,7,8,9]}
            {$[ : ]}         {[0,1,2,3,4,5,6,7,8,9]}
            {$[::]}          {[0,1,2,3,4,5,6,7,8,9]}
            {$[ : : ]}       {[0,1,2,3,4,5,6,7,8,9]}
            {$[::2]}         {[0,2,4,6,8]}
            {$[ : : 2 ]}     {[0,2,4,6,8]}
            {$[3::2]}        {[3,5,7,9]}
            {$[3 :: 2]}      {[3,5,7,9]}
            {$[0::1]}        {[0,1,2,3,4,5,6,7,8,9]}
            {$[0:8:2]}       {[0,2,4,6]}
            {$[ 0 : 8 : 2 ]} {[0,2,4,6]}
            {$[0:+8:+2]}     {[0,2,4,6]}
            {$[0 : +8 : +2]} {[0,2,4,6]}
            {$[6:0:-1]}      {[6,5,4,3,2,1]}
            {$[6::-1]}       {[]}
            {$[6:0:-2]}      {[6,4,2]}
            {$[6::-2]}       {[]}
            {$[8:0:-2]}      {[8,6,4,2]}
        }
    }

    test "JSON.GET array unions with a JSONPath (caps $caps)" {
        r flushall
        r json.set k1 . {[0,1,2,3,4,5,6,7,8,9]}
        json_assert_get k1 {
            {$[0,1,2]}                      {[0,1,2]}
            {$[0, 1, 2]}                    {[0,1,2]}
            {$[0, 1, 2  ]}                  {[0,1,2]}
            {$[ 0, 1, 2 ]}                  {[0,1,2]}
            {$[ 0,1, 2 ]}                   {[0,1,2]}
            {$[0,1]}                        {[0,1]}
            {$[-1,-2]}                      {[9,8]}
            {$[-10,-5,-6]}                  {[0,5,4]}
            {$[0,1,5,0,1,2]}                {[0,1,5,0,1,2]}
            {$[0, 1,5,0, 1,2]}              {[0,1,5,0,1,2]}
            {$[ 0, 1, 5, 0, 1, 2 ]}         {[0,1,5,0,1,2]}
            {$[ -10 , -5 , -6 ]}            {[0,5,4]}
            {$[-10,-9,-8,0,1,2,-1000,1000]} {[0,1,2,0,1,2]}
        }

        r json.set k2 . {[{"name":"name0","id":0},{"name":"name1","id":1},{"name":"name2","id":2}]}
        json_assert_get k2 {
            {$[0,2].name} {["name0","name2"]}
            {$[2,0].name} {["name2","name0"]}
            {$[1,1].id}   {[1,1]}
        }

        # mixing unions and slices is not supported, nor are extraneous commas
        json_assert_get_error k1 "SYNTAXERR Invalid JSON path" {
            {$[0,1,2:4]}
            {$[0:2,3,4]}
            {$[0,,4]}
            {$[0,,,4]}
            {$[,4]}
            {$[4,]}
            {$[,4,]}
            {$[,,4,,]}
            {$[,]}
            {$[,0,4]}
            {$[,0,4,]}
            {$[0,4,]}
        }
    }

    test "JSON.GET with multiple paths, legacy, JSONPath or mixed (caps $caps)" {
        r flushall
        r json.set k1 . {{"a":[], "b":[1], "c":[1,2]}}
        r json.set k2 . {{"a":{}, "b": {"a": 1}, "c": {"a": 1, "b": 2}}}

        # if all paths are legacy paths, the result conforms to the legacy path version:
        # the 1st path returns 1 value, the 2nd path returns the first one of 2 values.
        assert_equal {{".b[*]":1,".c[*]":1}} [r json.get k1 {.b[*]} {.c[*]}]
        assert_equal {{".b.*":1,".c.*":1}} [r json.get k2 {.b.*} {.c.*}]

        # all legacy: if one path returns no value, the command fails with NONEXISTENT.
        foreach {key path1 path2} {
            k1 {.a[*]}  {.b[*]}
            k2 {.a.*}   {.b.*}
            k2 {.foo.*} {.c.*}
        } {
            assert_error "NONEXISTENT JSON path does not exist" {r json.get $key $path1 $path2}
        }

        # if at least one path is a JSONPath, the result conforms to the JSONPath version
        foreach {key path1 path2 path3 exp} {
            k1 {$.a[*]} {$.b[*]} {$.c[*]} {{"$.a[*]":[],"$.b[*]":[1],"$.c[*]":[1,2]}}
            k1 {$.a[*]} {.b[*]}  {.c[*]}  {{"$.a[*]":[],".b[*]":[1],".c[*]":[1,2]}}
            k1 {.a[*]}  {$.b[*]} {.c[*]}  {{".a[*]":[],"$.b[*]":[1],".c[*]":[1,2]}}
            k2 {.a.*}   {.b.*}   {$.c.*}  {{".a.*":[],".b.*":[1],"$.c.*":[1,2]}}
            k2 {$.a.*}  {$.b.*}  {.c.*}   {{"$.a.*":[],"$.b.*":[1],".c.*":[1,2]}}
            k2 {.a.*}   {$.b.*}  {$.c.*}  {{".a.*":[],"$.b.*":[1],"$.c.*":[1,2]}}
            k2 {.foo.*} {$.b.*}  {$.c.*}  {{".foo.*":[],"$.b.*":[1],"$.c.*":[1,2]}}
        } {
            assert_equal $exp [r json.get $key $path1 $path2 $path3]
        }
    }

    # ------------------------------------------------------------------
    # JSON.MGET
    # ------------------------------------------------------------------

    tags {"json cluster:skip"} {
    test "JSON.MGET (caps $caps)" {
        json_setup_wikipedia
        assert_equal OK [r json.set k1 . {{"foo":"bar1"}}]
        assert_equal OK [r json.set k2 . {{"foo":"bar2"}}]
        assert_equal OK [r json.set k3 . {{"foo":"bar3"}}]
        assert_equal {{"bar1"} {"bar2"} {"bar3"}} [r json.mget k1 k2 k3 .foo]
        # the JSON path does not exist
        assert_equal {{} {} {}} [r json.mget k1 k2 k3 .bar]
        assert_equal {{[]} {[]} {[]}} [r json.mget k1 k2 k3 {$.bar}]
        # the key does not exist
        assert_equal {{} {}} [r json.mget baz foo .]
        assert_equal {{} {"bar2"}} [r json.mget baz k2 .foo]
        assert_equal {{} {["bar2"]}} [r json.mget baz k2 {$.foo}]
        # a single key
        assert_equal {{{"foo":"bar1"}}} [r json.mget k1 .]
        # the same key twice
        assert_equal {{"bar1"} {"bar1"}} [r json.mget k1 k1 .foo]

        # Wrong number of arguments
        assert_error "*wrong number of arguments for 'JSON.MGET' command" {r json.mget}
        assert_error "*wrong number of arguments for 'JSON.MGET' command" {r json.mget k1}

        assert_equal OK [r json.set k4 . {[2,5,{"level0":[null,true,{"level0_1":[3,false]}],"level1":{"level1_0":33}}]}]
        assert_equal OK [r json.set k5 . {[4,5,{"level0":[null,false,{"level0_1":[null,false]}],"level1":{"level1_0":[12,13]}}]}]
        assert_equal OK [r json.set k6 . {[2,5,{"level0":[true,20,{"level0_1":[3,true]}],"level1":{"level1_0":33}}]}]
        foreach {path exp} {
            {$..level0_1}              {{[[3,false]]} {[[null,false]]} {[[3,true]]}}
            {$.[2].level1}             {{[{"level1_0":33}]} {[{"level1_0":[12,13]}]} {[{"level1_0":33}]}}
            {$.[2].level1.level1_0[2]} {{[]} {[]} {[]}}
            {$.[2].level1.level1_0[1]} {{[]} {[13]} {[]}}
        } {
            assert_equal $exp [r json.mget k4 k5 k6 $path] "JSON.MGET k4 k5 k6 $path"
        }
    }

    test "JSON.MGET with a non-JSON key, a failing path and an invalid path (caps $caps)" {
        json_setup_wikipedia
        r json.set k1 . {[0,1,2]}
        r json.set k2 . {{"a":1}}
        # A key of another type fails the whole command, as in the module.
        assert_error "WRONGTYPE Not a JSON document key" {r json.mget k1 str_key nokey {$}}
        assert_error "WRONGTYPE Not a JSON document key" {r json.mget k1 str_key nokey .}
        assert_error "WRONGTYPE Not a JSON document key" {r json.mget str_key .}
        # A legacy path that does not exist in one document gives null for that
        # document, but other legacy path errors fail the command, as in the module.
        assert_equal {{} 1 {}} [r json.mget k1 k2 nokey .a]
        assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.mget k1 nokey {.[77]}}
        assert_equal {{[]} {}} [r json.mget k1 nokey {$[77]}]
        # An invalid JSONPath gives null for every key, as in the module.
        assert_equal {{} {}} [r json.mget k1 nokey {$[0}]
        assert_equal {{} {}} [r json.mget nokey1 nokey2 {$[}]
    }
    }

    # ------------------------------------------------------------------
    # JSON.DEL and JSON.FORGET
    # ------------------------------------------------------------------

    foreach cmd {json.del json.forget} {
        set CMD [string toupper $cmd]
        test "$CMD (caps $caps)" {
            json_setup_wikipedia
            # delete an element
            foreach path {.spouse .phoneNumbers} {
                assert_equal 1 [r $cmd wikipedia $path]
                assert_error "NONEXISTENT JSON path does not exist" {r json.get wikipedia $path}
            }
            assert_equal {{"firstName":"John","lastName":"Smith","age":27,"weight":135.17,"isAlive":true,"address":{"street":"21 2nd Street","city":"New York","state":"NY","zipcode":"10021-3100"},"children":[],"groups":{}}} [r json.get wikipedia]

            # the path does not exist
            assert_equal 0 [r $cmd wikipedia .foo]
            assert_equal 0 [r $cmd wikipedia {$.foo}]
            assert_equal 0 [r $cmd wikipedia .address.foo.bar]
            assert_equal 0 [r $cmd wikipedia {$.address.foo.bar}]

            # delete a doc: path not provided
            assert_equal 1 [r $cmd wikipedia]
            assert_equal 0 [r exists wikipedia]

            # delete a doc: path arg provided
            r json.set k1 . 1
            assert_equal 1 [r $cmd k1 .]
            assert_equal 0 [r exists k1]
            r json.set k1 . {{"a":1}}
            assert_equal 1 [r $cmd k1 {$}]
            assert_equal 0 [r exists k1]

            # return is 0 if the document key does not exist
            assert_equal 0 [r $cmd foo .firstName]
            assert_equal 0 [r $cmd foo {$.firstName}]
            assert_equal 0 [r $cmd foo]
            assert_equal 0 [r $cmd wikipedia .foo]

            # a key that does not hold a JSON document
            assert_error "WRONGTYPE Not a JSON document key" {r $cmd str_key}
            assert_error "WRONGTYPE Not a JSON document key" {r $cmd str_key .a}
            assert_error "WRONGTYPE Not a JSON document key" {r $cmd str_key {$.a}}
            assert_equal 1 [r exists str_key]

            # Wrong number of arguments
            r json.set wikipedia . [json_wikipedia_compact]
            assert_error "*wrong number of arguments for '$CMD' command" {r $cmd wikipedia .children extra}
            assert_error "*wrong number of arguments for '$CMD' command" {r $cmd}

            # path errors
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r $cmd wikipedia {.phoneNumbers[9]}}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r $cmd wikipedia {$.phoneNumbers[9]}}
            assert_error "SYNTAXERR Expression token cannot be empty" {r $cmd wikipedia {$[}}
            assert_error "ERROR Cannot insert a member into a non-object value" {r $cmd wikipedia .firstName.a}
            assert_equal [json_wikipedia_compact] [r json.get wikipedia]
        }
    }

    test "JSON.DEL with JSONPath wildcards, slices, unions and recursive descent (caps $caps)" {
        r flushall
        r json.set k1 . {{"x": {}, "y": {"a":"a"}, "z": {"a":"", "b":"b"}}}
        foreach key {k2 k3 k4 k5} {
            r json.set $key . {[0,1,2,3,4,5,6,7,8,9]}
        }

        # NOTE: The expected values below account for the outcome of previous commands.
        foreach {key path exp_ret exp_val} {
            k1 {$.z.*}   2 {{"x":{},"y":{"a":"a"},"z":{}}}
            k1 {$.*}     3 {{}}
            k2 {$.[3:6]} 3 {[0,1,2,6,7,8,9]}
            k2 {$.*}     7 {[]}
        } {
            assert_equal $exp_ret [r json.del $key $path]
            assert_equal $exp_val [r json.get $key]
        }

        # delete whole doc
        foreach key {k1 k2} {
            assert_equal 1 [r json.del $key {$}]
            assert_equal 0 [r exists $key]
        }

        # delete with wildcard, slice, and union
        assert_equal 10 [r json.del k3 {$[*]}]
        assert_equal {[]} [r json.get k3]
        assert_equal 4 [r json.del k4 {$[3:7]}]
        assert_equal {[0,1,2,7,8,9]} [r json.get k4]
        assert_equal 4 [r json.del k5 {$[1,5,7,8]}]
        assert_equal {[0,2,3,4,6,9]} [r json.get k5]

        assert_equal OK [r json.set k1 . {[2,5,{"level0":[null,true,{"level0_1":[3,false]}],"level1":{"level1_0":33}}]}]
        foreach {path exp_ret exp_val} {
            {$[2].level0..level0_1[1]} 1 {[2,5,{"level0":[null,true,{"level0_1":[3]}],"level1":{"level1_0":33}}]}
            {$[2].level0..level0_1}    1 {[2,5,{"level0":[null,true,{}],"level1":{"level1_0":33}}]}
            {$..level1.level1_0}       1 {[2,5,{"level0":[null,true,{}],"level1":{}}]}
            {$..level1}                1 {[2,5,{"level0":[null,true,{}]}]}
            {.*}                       3 {[]}
        } {
            assert_equal $exp_ret [r json.del k1 $path]
            assert_equal $exp_val [r json.get k1]
            if {$path ne {.*}} {
                assert_equal {[]} [r json.get k1 $path]
            } else {
                assert_error "NONEXISTENT JSON path does not exist" {r json.get k1 $path}
            }
        }
    }

    test "JSON.DEL counts a value matched twice once, and nested matches each (caps $caps)" {
        r flushall
        r json.set d . {[0,1,2,3,4,5]}
        # index 1 is named three times (1, 1 and -5)
        assert_equal 2 [r json.del d {$[1,1,2,-5]}]
        assert_equal {[0,3,4,5]} [r json.get d]
        # reads keep duplicates
        assert_equal {[3,3,0]} [r json.get d {$[1,1,0]}]
        assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.del d {$[5]}}
        assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.del d {.[5]}}
        assert_equal 4 [r json.del d {$[0:100]}]
        assert_equal {[]} [r json.get d]

        # matches nested inside each other are all counted, as in the module
        r json.set d . {{"a":{"a":{"a":1}},"b":[1,2,3]}}
        assert_equal 3 [r json.del d {$..a}]
        assert_equal {{"b":[1,2,3]}} [r json.get d]

        # legacy path: the first step that matches nothing gives 0
        r json.set d . {{"a":1,"b":{"c":[1,2]}}}
        assert_equal 1 [r json.del d .b.c]
        assert_equal 0 [r json.del d .b.zz]
        assert_equal 0 [r json.del d {$.b.zz.yy}]
        assert_equal 0 [r json.del d .b.zz.yy]
        assert_equal {{"a":1,"b":{}}} [r json.get d]
        # legacy wildcard deletes every match
        assert_equal 2 [r json.del d {.*}]
        assert_equal {{}} [r json.get d]
        # deleting the last member does not delete the key
        assert_equal 1 [r exists d]
        assert_equal ReJSON-RL [r type d]
    }

    # ------------------------------------------------------------------
    # Unicode
    # ------------------------------------------------------------------

    test "JSON unicode strings are supported (caps $caps)" {
        r flushall
        foreach str [list "\"Eat, drink, \u611b\"" "\"hyv\u00e4\u00e4-\u00e9l\u00e8ve\""] {
            set utf8 [encoding convertto utf-8 $str]
            assert_equal OK [r json.set k1 . $utf8]
            assert_equal $utf8 [r json.get k1 NOESCAPE .]
            assert_equal $utf8 [r json.get k1]
            assert_equal "\[$utf8\]" [r json.get k1 {$}]
            assert_equal 1 [r json.del k1]
            assert_equal 0 [r exists k1]
        }
        # \u escapes, including a surrogate pair, are stored as UTF-8; control
        # characters, the quote and the backslash stay escaped on output
        assert_equal OK [r json.set k1 . {"\u00e9l\u00e8ve \ud83d\ude00 \n\t\"\\ \/ \u0001"}]
        # The expected bytes are spelled out: U+00E9, U+00E8 and U+1F600 as UTF-8. A Tcl
        # string cannot hold a character beyond the BMP in every Tcl version.
        set e_acute [binary format H* c3a9]
        set e_grave [binary format H* c3a8]
        set grin [binary format H* f09f9880]
        assert_equal "\"${e_acute}l${e_grave}ve $grin \\n\\t\\\"\\\\ / \\u0001\"" [r json.get k1]
        assert_equal [r json.get k1] [r json.get k1 NOESCAPE]
    }

    test "JSON.SET rejects invalid UTF-8 (B1) (caps $caps)" {
        r flushall
        r json.set k1 . {"valid"}
        # The external module stores these bytes and later emits invalid JSON.
        foreach bad [list "\"\xff\xfe\"" "\"abc\xc3\"" "\"\xed\xa0\x80\"" "{\"k\xc0\xaf\":1}"] {
            assert_error "SYNTAXERR*" {r json.set k1 . $bad}
            assert_error "SYNTAXERR*" {r json.set k2 . $bad}
        }
        assert_equal {"valid"} [r json.get k1]
        assert_equal 0 [r exists k2]
    }

    test "JSON non-ASCII member names in paths (caps $caps)" {
        r flushall
        set love [encoding convertto utf-8 "\u611b"]
        r json.set k1 . "{\"a\": {\"$love\": \"love\", \"b\": \"b\"}}"
        r json.set k2 . "{\"$love\": \[1,2,3\]}"
        assert_equal {"love"} [r json.get k1 ".a.$love"]
        assert_equal "{\".a.$love\":\"love\",\".a.b\":\"b\"}" [r json.get k1 ".a.$love" .a.b]
        assert_equal 2 [r json.get k2 ".$love\[1\]"]
        assert_equal {[1,2,3]} [r json.get k2 "\$.$love\[*\]"]
        assert_equal {["love"]} [r json.get k1 "\$.a\[\"$love\"\]"]
        # write and delete through such a path
        assert_equal OK [r json.set k1 "\$.a.$love" {"amour"}]
        assert_equal OK [r json.set k1 ".$love$love" 1]
        assert_equal "{\"a\":{\"$love\":\"amour\",\"b\":\"b\"},\"$love$love\":1}" [r json.get k1]
        assert_equal 1 [r json.del k1 ".a.$love"]
        assert_equal 1 [r json.del k1 "\$.$love$love"]
        assert_equal {{"a":{"b":"b"}}} [r json.get k1]
    }
}

    # ------------------------------------------------------------------
    # Tests that do not depend on the encoding
    # ------------------------------------------------------------------
    json_set_caps {4096 128}

    test "JSON commands declare their keys (COMMAND GETKEYS)" {
        # These commands take a single key
        foreach cmd {DEL GET SET TYPE NUMINCRBY NUMMULTBY TOGGLE STRAPPEND STRLEN
                     ARRAPPEND ARRINDEX ARRLEN ARRPOP CLEAR OBJKEYS OBJLEN FORGET RESP} {
            if {$cmd in {SET NUMMULTBY NUMINCRBY ARRAPPEND ARRINDEX STRAPPEND RESP}} {
                # Dummy value in command
                assert_equal {k1} [r command getkeys JSON.$cmd k1 . 5]
            } else {
                assert_equal {k1} [r command getkeys JSON.$cmd k1]
            }
        }
        # ARRINSERT requires an index, ARRTRIM a start and an end
        assert_equal {k1} [r command getkeys JSON.ARRINSERT k1 . 0 5]
        assert_equal {k1} [r command getkeys JSON.ARRTRIM k1 . 0 5]
        assert_equal {k1} [r command getkeys JSON.MERGE k1 . 5]

        foreach sub {MEMORY DEPTH FIELDS} {
            assert_equal {k1} [r command getkeys JSON.DEBUG $sub k1]
        }

        # JSON.MGET and JSON.MSET are the multi-key commands
        assert_equal {k1 k2 k3} [r command getkeys JSON.MGET k1 k2 k3 .]
        assert_equal {k1} [r command getkeys JSON.MGET k1 .]
        assert_equal {k1 k2 k3} [r command getkeys JSON.MSET k1 . 1 k2 . 2 k3 . 3]
    }

    tags {"json cluster:skip"} {
    test "JSON.SET, JSON.GET, JSON.MGET, JSON.DEL and JSON.FORGET reply shapes in RESP2 and RESP3" {
        r flushall
        r json.set k1 . {{"a":1,"b":"two"}}
        foreach proto {2 3} {
            r hello $proto
            set null [json_raw_null $proto]

            # parsed replies are the same in both protocols
            assert_equal {{"a":1,"b":"two"}} [r json.get k1]
            assert_equal {[1]} [r json.get k1 {$.a}]
            assert_equal {{"$.a":[1],".b":["two"]}} [r json.get k1 {$.a} .b]
            assert_equal {1 {}} [r json.mget k1 nokey .a]
            assert_equal {{["two"]} {}} [r json.mget k1 nokey {$.b}]

            r readraw 1
            # nulls: a missing key, a failed NX or XX
            assert_equal $null [r json.get nokey]
            assert_equal $null [r json.get nokey {$.a}]
            assert_equal $null [r json.set k1 . 1 NX]
            assert_equal $null [r json.set nokey . 1 XX]
            assert_equal $null [r json.set k1 {$.zz} 1 XX]
            # a simple string for a successful SET
            assert_equal {+OK} [r json.set k1 {$.c} true]
            # a bulk string for GET, also with a JSONPath and with several paths
            assert_equal {$1} [r json.get k1 .a]
            assert_equal 1 [r read]
            assert_equal {$2} [r json.get k1 {$.zz}]
            assert_equal {[]} [r read]
            assert_equal {$23} [r json.get k1 .a {$.c}]
            assert_equal {{".a":[1],"$.c":[true]}} [r read]
            # an array of bulk strings and nulls for MGET
            assert_equal {*3} [r json.mget k1 nokey k1 .b]
            assert_equal {$5} [r read]
            assert_equal {"two"} [r read]
            assert_equal $null [r read]
            assert_equal {$5} [r read]
            assert_equal {"two"} [r read]
            # integers for DEL and FORGET
            assert_equal {:0} [r json.del nokey]
            assert_equal {:0} [r json.forget k1 {$.zz}]
            assert_equal {:1} [r json.del k1 .c]
            assert_equal {:1} [r json.forget k1 {$.a}]
            # errors
            assert_equal {-NONEXISTENT JSON path does not exist} [r json.get k1 .a]
            r readraw 0

            assert_equal OK [r json.set k1 .a 1]
            assert_equal {{"b":"two","a":1}} [r json.get k1]
            r json.set k1 . {{"a":1,"b":"two"}}
        }
        r hello 2
    }
    }
}
