# JSON (built-in module): JSON.MERGE.
#
# Ported from the valkey-json module's tst/integration/test_json_basic.py
# (test_json_merge_command_*), with further cases whose expected values were taken from the
# module. Tests that assert a deliberate change of VALKEY_JSON_DESIGN.md section 2.3 carry its
# number in their name.

proc json_caps_list {} { return {{4096 128} {64 4} {0 0}} }
proc json_set_caps {caps} {
    lassign $caps bytes entries
    r config set json.max-packed-bytes $bytes
    r config set json.max-packed-entries $entries
}

start_server {tags {"json"}} {
    test {JSON.MERGE wrong number of arguments} {
        assert_error "ERR wrong number of arguments for 'JSON.MERGE' command" {r json.merge}
        assert_error "ERR wrong number of arguments for 'JSON.MERGE' command" {r json.merge k}
        assert_error "ERR wrong number of arguments for 'JSON.MERGE' command" {r json.merge k .}
        assert_error "ERR wrong number of arguments for 'JSON.MERGE' command" {r json.merge k . 1 2}
        assert_equal 0 [r exists k]
    }

    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        test "JSON.MERGE basic: creates the key, then updates and adds members (caps $caps)" {
            assert_equal OK [r json.merge merge_test . {{"a":1,"b":2}}]
            assert_equal {{"a":1,"b":2}} [r json.get merge_test]
            assert_equal ReJSON-RL [r type merge_test]
            assert_equal OK [r json.merge merge_test . {{"b":3,"c":4}}]
            assert_equal {{"a":1,"b":3,"c":4}} [r json.get merge_test]
        }

        test "JSON.MERGE nested objects are merged member by member (caps $caps)" {
            assert_equal OK [r json.merge merge_nested . {{"user":{"name":"John","age":30},"city":"NYC"}}]
            assert_equal OK [r json.merge merge_nested . {{"user":{"age":31,"email":"john@example.com"},"country":"USA"}}]
            assert_equal {"John"} [r json.get merge_nested .user.name]
            assert_equal 31 [r json.get merge_nested .user.age]
            assert_equal {"john@example.com"} [r json.get merge_nested .user.email]
            assert_equal {"NYC"} [r json.get merge_nested .city]
            assert_equal {"USA"} [r json.get merge_nested .country]
            assert_equal {{"user":{"name":"John","age":31,"email":"john@example.com"},"city":"NYC","country":"USA"}} [r json.get merge_nested]
        }

        test "JSON.MERGE at a sub-path (caps $caps)" {
            assert_equal OK [r json.set merge_path . {{"a":{"x":1,"y":2},"b":10}}]
            assert_equal OK [r json.merge merge_path .a {{"y":3,"z":4}}]
            assert_equal {{"x":1,"y":3,"z":4}} [r json.get merge_path .a]
            assert_equal 10 [r json.get merge_path .b]
            assert_equal OK [r json.merge merge_path {$.a} {{"x":null,"w":[1]}}]
            assert_equal {{"a":{"y":3,"z":4,"w":[1]},"b":10}} [r json.get merge_path]
        }

        test "JSON.MERGE creates a new key from a root path, legacy and \$ (caps $caps)" {
            assert_equal OK [r json.merge merge_new_key . {{"a":1,"b":2}}]
            assert_equal {{"a":1,"b":2}} [r json.get merge_new_key]
            # On a new key the value is stored as given: nulls are not stripped.
            assert_equal OK [r json.merge merge_new_key2 {$} {{"a":null,"b":1}}]
            assert_equal {{"a":null,"b":1}} [r json.get merge_new_key2]
            assert_equal OK [r json.merge merge_new_key3 . null]
            assert_equal 1 [r exists merge_new_key3]
            assert_equal null [r json.get merge_new_key3]
            assert_equal OK [r json.merge merge_new_key4 . {[null]}]
            assert_equal {[null]} [r json.get merge_new_key4]
        }

        test "JSON.MERGE replaces a value that is not an object (caps $caps)" {
            assert_equal OK [r json.set merge_replace . {{"a":1,"b":"string","c":true}}]
            assert_equal OK [r json.merge merge_replace .b {{"nested":"object"}}]
            assert_equal {{"nested":"object"}} [r json.get merge_replace .b]
            assert_equal OK [r json.merge merge_replace .c {{"another":"object"}}]
            assert_equal {{"another":"object"}} [r json.get merge_replace .c]
            assert_equal {{"a":1,"b":{"nested":"object"},"c":{"another":"object"}}} [r json.get merge_replace]
        }

        test "JSON.MERGE deep nesting (caps $caps)" {
            assert_equal OK [r json.merge merge_deep . {{"level1":{"level2":{"level3":{"a":1}}}}}]
            assert_equal OK [r json.merge merge_deep . {{"level1":{"level2":{"level3":{"b":2}}}}}]
            assert_equal {{"a":1,"b":2}} [r json.get merge_deep .level1.level2.level3]
        }

        test "JSON.MERGE at the root of an existing document (caps $caps)" {
            assert_equal OK [r json.set merge_root . {{"a":1,"b":2}}]
            assert_equal OK [r json.merge merge_root . {{"b":3,"c":4}}]
            assert_equal {{"a":1,"b":3,"c":4}} [r json.get merge_root]
            assert_equal OK [r json.merge merge_root {$} {{"a":null,"d":{"e":null}}}]
            assert_equal {{"b":3,"c":4,"d":{}}} [r json.get merge_root]
        }

        test "JSON.MERGE error conditions (caps $caps)" {
            assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.merge merge_error .invalid.path {{"a":1}}}
            assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.merge merge_error . {invalid json}}
            assert_equal 0 [r exists merge_error]
            r set string_key not_json
            assert_error "WRONGTYPE Not a JSON document key" {r json.merge string_key . {{"a":1}}}
            assert_error "WRONGTYPE Not a JSON document key" {r json.merge string_key .a {{"a":1}}}
            assert_equal not_json [r get string_key]

            r json.set merge_error . {{"a":1,"arr":[1,2,3]}}
            assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.merge merge_error . {{"x":1e5,}}}
            assert_error "SYNTAXERR Expression token cannot be empty" {r json.merge merge_error {$.arr[} 1}
            # Missing parents are not created, by either syntax.
            assert_error "NONEXISTENT*" {r json.merge merge_error {$.nope.deeper} 1}
            assert_error "NONEXISTENT*" {r json.merge merge_error .nope.deeper 1}
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.merge merge_error {$.a.deeper} 1}
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.merge merge_error .a.deeper 1}
            assert_error "ERROR Cannot insert a member into a non-object value" {r json.merge merge_error {$.arr.x} 1}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.merge merge_error {$.arr[10]} 1}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.merge merge_error {.arr[10]} 1}
            assert_equal {{"a":1,"arr":[1,2,3]}} [r json.get merge_error]
            r del merge_error string_key
        }

        test "JSON.MERGE replaces an array with the patch (caps $caps)" {
            assert_equal OK [r json.set merge_array . {{"arr":[1,2,3]}}]
            assert_equal OK [r json.merge merge_array .arr {{"merged":"object"}}]
            assert_equal {{"merged":"object"}} [r json.get merge_array .arr]
            # An array in the patch replaces the target as it is, nulls included.
            assert_equal OK [r json.merge merge_array .arr {[null,{"a":null}]}]
            assert_equal {{"arr":[null,{"a":null}]}} [r json.get merge_array]
            # Array elements can be targets.
            assert_equal OK [r json.merge merge_array {$.arr[1]} {{"a":null,"b":1}}]
            assert_equal {{"arr":[null,{"b":1}]}} [r json.get merge_array]
        }

        test "JSON.MERGE adds a new child when only the last path element is missing (caps $caps)" {
            assert_equal OK [r json.set merge_new_child . {{"a":{"b":{"c":1}}}}]
            # Path .a.b exists, but .a.b.d doesn't - should add 'd' as new child
            assert_equal OK [r json.merge merge_new_child .a.b.d {{"new":"value"}}]
            assert_equal {{"new":"value"}} [r json.get merge_new_child .a.b.d]
            assert_equal 1 [r json.get merge_new_child .a.b.c]
            assert_equal OK [r json.merge merge_new_child .a.b.d {{"another":"field"}}]
            assert_equal {{"new":"value","another":"field"}} [r json.get merge_new_child .a.b.d]
            # A new child is stored as given, nulls included.
            assert_equal OK [r json.merge merge_new_child {$.a.e} {{"x":null,"y":{"z":null,"w":1}}}]
            assert_equal {{"a":{"b":{"c":1,"d":{"new":"value","another":"field"}},"e":{"x":null,"y":{"z":null,"w":1}}}}} [r json.get merge_new_child]
            assert_equal OK [r json.merge merge_new_child {$.a.f} null]
            assert_equal {[null]} [r json.get merge_new_child {$.a.f}]
        }

        test "JSON.MERGE a null in the patch deletes the member (caps $caps)" {
            # Test 1: Delete key at root level
            assert_equal OK [r json.set merge_null . {{"a":1,"b":2,"c":3}}]
            assert_equal OK [r json.merge merge_null . {{"b":null}}]
            assert_equal {{"a":1,"c":3}} [r json.get merge_null]
            # Test 2: Delete nested key
            assert_equal OK [r json.set merge_null . {{"user":{"name":"John","age":30,"email":"test@example.com"}}}]
            assert_equal OK [r json.merge merge_null . {{"user":{"age":null}}}]
            assert_equal {{"name":"John","email":"test@example.com"}} [r json.get merge_null .user]
            # Test 3: Null on non-existing key should not add it
            assert_equal OK [r json.set merge_null . {{"a":1}}]
            assert_equal OK [r json.merge merge_null . {{"b":null,"c":3}}]
            assert_equal {{"a":1,"c":3}} [r json.get merge_null]
            # Nulls nested under a member that does not exist yet are dropped too...
            assert_equal OK [r json.merge merge_null . {{"n":{"x":{"y":null}}}}]
            assert_equal {{"a":1,"c":3,"n":{"x":{}}}} [r json.get merge_null]
            # ...but an object that replaces a value of another type is stored as given.
            assert_equal OK [r json.merge merge_null . {{"a":{"b":null}}}]
            assert_equal {{"a":{"b":null},"c":3,"n":{"x":{}}}} [r json.get merge_null]
            assert_equal OK [r json.merge merge_null . {{"a":null,"c":null,"n":null}}]
            assert_equal {{}} [r json.get merge_null]
        }

        test "JSON.MERGE a null patch at a path sets the value to null (caps $caps)" {
            assert_equal OK [r json.set merge_null . {{"a":{"x":1,"y":2},"b":10}}]
            assert_equal OK [r json.merge merge_null .a null]
            assert_equal {{"a":null,"b":10}} [r json.get merge_null]
            assert_equal OK [r json.merge merge_null {$.b} null]
            assert_equal {{"a":null,"b":null}} [r json.get merge_null]
            assert_equal OK [r json.merge merge_null {$.nope} null]
            assert_equal {{"a":null,"b":null,"nope":null}} [r json.get merge_null]
            assert_equal OK [r json.merge merge_null . null]
            assert_equal 1 [r exists merge_null]
            assert_equal null [r json.get merge_null]
        }

        test "JSON.MERGE value rules: scalars and arrays replace, objects merge (caps $caps)" {
            assert_equal OK [r json.set merge_values . {{"str":"hello","num":42,"arr":[1,2,3],"obj":{"x":1}}}]
            assert_equal OK [r json.merge merge_values . {{"str":"world","num":99,"arr":[4,5],"obj":{"y":2},"new":"added"}}]
            assert_equal {"world"} [r json.get merge_values .str]
            assert_equal 99 [r json.get merge_values .num]
            assert_equal {[4,5]} [r json.get merge_values .arr]
            assert_equal {{"x":1,"y":2}} [r json.get merge_values .obj]
            assert_equal {"added"} [r json.get merge_values .new]
            assert_equal {{"str":"world","num":99,"arr":[4,5],"obj":{"x":1,"y":2},"new":"added"}} [r json.get merge_values]
        }

        test "JSON.MERGE a patch that is not an object replaces the root (caps $caps)" {
            r json.set merge_values . {{"a":1}}
            assert_equal OK [r json.merge merge_values . 5]
            assert_equal 5 [r json.get merge_values]
            assert_equal OK [r json.merge merge_values . {{"a":{"b":{"c":null}}}}]
            assert_equal {{"a":{"b":{}}}} [r json.get merge_values]
            assert_equal OK [r json.merge merge_values . {"str"}]
            assert_equal {"str"} [r json.get merge_values]
            assert_equal OK [r json.merge merge_values . {[1,null]}]
            assert_equal {[1,null]} [r json.get merge_values]
            assert_equal OK [r json.merge merge_values {$[1]} {{"a":null}}]
            assert_equal {[1,{}]} [r json.get merge_values]
            assert_equal OK [r json.merge merge_values {$[*]} 7]
            assert_equal {[7,7]} [r json.get merge_values]
            assert_equal OK [r json.merge merge_values {$} {{}}]
            assert_equal {{}} [r json.get merge_values]
        }

        test "JSON.MERGE keeps number text exactly (caps $caps)" {
            r json.set merge_values . {{"keep":19.90}}
            assert_equal OK [r json.merge merge_values . {{"x":1e5,"y":19.90,"z":-0,"big":123456789012345678901234567890,"f":1.0}}]
            assert_equal {{"keep":19.90,"x":1e5,"y":19.90,"z":0,"big":123456789012345678901234567890,"f":1.0}} [r json.get merge_values]
            assert_equal {number number number integer number number} [r json.type merge_values {$.*}]
        }

        test "JSON.MERGE with wildcards and several targets (caps $caps)" {
            assert_equal OK [r json.set merge_wildcards . {{"users":[{"name":"Alice","age":25,"meta":{"active":true}},{"name":"Bob","age":30,"meta":{"active":false}}],"items":[{"id":1,"price":10},{"id":2,"price":20}]}}]
            assert_equal OK [r json.merge merge_wildcards {$..meta} {{"active":true,"updated":true}}]
            assert_equal {{"active":true,"updated":true}} [r json.get merge_wildcards {.users[0].meta}]
            assert_equal {{"active":true,"updated":true}} [r json.get merge_wildcards {.users[1].meta}]
            assert_equal OK [r json.merge merge_wildcards {$..age} 31]
            assert_equal 31 [r json.get merge_wildcards {.users[0].age}]
            assert_equal 31 [r json.get merge_wildcards {.users[1].age}]
            assert_equal OK [r json.merge merge_wildcards {$..price} 15]
            assert_equal 15 [r json.get merge_wildcards {.items[0].price}]
            assert_equal 15 [r json.get merge_wildcards {.items[1].price}]
            assert_equal {{"users":[{"name":"Alice","age":31,"meta":{"active":true,"updated":true}},{"name":"Bob","age":31,"meta":{"active":true,"updated":true}}],"items":[{"id":1,"price":15},{"id":2,"price":15}]}} [r json.get merge_wildcards]
            # A $ path that matches nothing is not an error and changes nothing.
            assert_equal OK [r json.merge merge_wildcards {$..nosuch.member} 1]
            assert_equal OK [r json.merge merge_wildcards {$.users[*].meta.a.b} 1]
            assert_equal {[]} [r json.get merge_wildcards {$..nosuch}]
        }

        test "JSON.MERGE with a legacy wildcard path updates every match (caps $caps)" {
            assert_equal OK [r json.set merge_mixed . {{"users":[{"id":1,"name":"Alice","profile":{"email":"alice@example.com"}},{"id":2,"name":"Bob","profile":{"email":"bob@example.com"}}]}}]
            assert_equal OK [r json.merge merge_mixed {.users[*].profile} {{"email":"updated@example.com","phone":"123-456-7890"}}]
            assert_equal {{"email":"updated@example.com","phone":"123-456-7890"}} [r json.get merge_mixed {.users[0].profile}]
            assert_equal {{"email":"updated@example.com","phone":"123-456-7890"}} [r json.get merge_mixed {.users[1].profile}]
            assert_equal {"Bob"} [r json.get merge_mixed {.users[1].name}]
            assert_equal 2 [r json.get merge_mixed {.users[1].id}]

            assert_equal OK [r json.set merge_mixed . {{"user1":{"name":"Alice","profile":{"email":"alice@example.com"}},"user2":{"name":"Bob"}}}]
            assert_equal OK [r json.merge merge_mixed .user1.profile {{"email":"updated1@example.com","phone":"111-111-1111"}}]
            assert_equal OK [r json.merge merge_mixed .user2.profile {{"email":"new@example.com","phone":"222-222-2222"}}]
            assert_equal {{"email":"updated1@example.com","phone":"111-111-1111"}} [r json.get merge_mixed .user1.profile]
            assert_equal {{"email":"new@example.com","phone":"222-222-2222"}} [r json.get merge_mixed .user2.profile]
        }

        test "JSON.MERGE updates existing members and inserts missing ones in one command (caps $caps)" {
            assert_equal OK [r json.set merge_updates_inserts . {{"users":[{"id":1,"profile":{"email":"a@x.com"}},{"id":2}]}}]
            assert_equal OK [r json.merge merge_updates_inserts {.users[*].profile} {{"phone":"123"}}]
            assert_equal {{"email":"a@x.com","phone":"123"}} [r json.get merge_updates_inserts {.users[0].profile}]
            assert_equal {{"phone":"123"}} [r json.get merge_updates_inserts {.users[1].profile}]
            assert_equal 2 [r json.get merge_updates_inserts {.users[1].id}]
            # The same with a $ path and a recursive descent.
            assert_equal OK [r json.set merge_updates_inserts . {{"users":[{"id":1,"profile":{"email":"a@x.com"}},{"id":2}]}}]
            assert_equal OK [r json.merge merge_updates_inserts {$.users[*].profile} {{"phone":"123","email":null}}]
            assert_equal {{"users":[{"id":1,"profile":{"phone":"123"}},{"id":2,"profile":{"phone":"123","email":null}}]}} [r json.get merge_updates_inserts]
        }

        test "JSON.MERGE a path that is not the root on a new key is an error (caps $caps)" {
            assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.merge merge_new_key_error .user {{"name":"John"}}}
            assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.merge merge_new_key_error2 .a.b {{"x":1}}}
            assert_error "SYNTAXERR A new Valkey key's path must be root" {r json.merge merge_new_key_error3 {$..field} {{"value":1}}}
            assert_equal 0 [r exists merge_new_key_error merge_new_key_error2 merge_new_key_error3]
            assert_equal OK [r json.merge merge_new_key_error . {{"a":1}}]
            assert_equal OK [r json.merge merge_new_key_error .user {{"name":"John"}}]
            assert_equal {{"a":1,"user":{"name":"John"}}} [r json.get merge_new_key_error]
        }

        test "JSON.MERGE with an empty object changes nothing (caps $caps)" {
            set original_doc {{"a":1,"b":{"x":2,"y":3},"c":[4,5,6],"d":"string"}}
            assert_equal OK [r json.set merge_empty_noop . $original_doc]
            assert_equal OK [r json.merge merge_empty_noop . {{}}]
            assert_equal $original_doc [r json.get merge_empty_noop]
            assert_equal 1 [r json.get merge_empty_noop .a]
            assert_equal 2 [r json.get merge_empty_noop .b.x]
            assert_equal 3 [r json.get merge_empty_noop .b.y]
            assert_equal {[4,5,6]} [r json.get merge_empty_noop .c]
            assert_equal {"string"} [r json.get merge_empty_noop .d]
            assert_equal OK [r json.merge merge_empty_noop .b {{}}]
            assert_equal 2 [r json.get merge_empty_noop .b.x]
            assert_equal 3 [r json.get merge_empty_noop .b.y]
            assert_equal $original_doc [r json.get merge_empty_noop]
        }

        test "JSON.MERGE nested matches: the outermost match wins (B4) (caps $caps)" {
            # The module applies the patch to both matches, reading the inner one after the
            # outer one changed (design 2.5). Here every match is resolved against the document
            # as it was before the command, and a match inside a merged value is dropped.
            r json.set m1 . {{"a":{"a":{"b":1}}}}
            assert_equal OK [r json.merge m1 {$..a} {{"x":1}}]
            assert_equal {{"a":{"a":{"b":1},"x":1}}} [r json.get m1]
            r json.set m1 . {{"a":{"a":{"a":5}}}}
            assert_equal OK [r json.merge m1 {$..a} {{"a":null,"z":1}}]
            assert_equal {{"a":{"z":1}}} [r json.get m1]
            r json.set m1 . {{"a":{"a":{"a":5}}}}
            assert_equal OK [r json.merge m1 {$..a} 7]
            assert_equal {{"a":7}} [r json.get m1]
            # Matches that do not nest are all applied.
            r json.set m1 . {{"a":{"b":1},"c":{"a":{"b":2}}}}
            assert_equal OK [r json.merge m1 {$..a} {{"b":null,"d":3}}]
            assert_equal {{"a":{"d":3},"c":{"a":{"d":3}}}} [r json.get m1]
            r del m1
        }

        test "JSON.MERGE a new member name in the path is literal (B5) (caps $caps)" {
            # The module passes the name through a JSON Pointer, so "m/n" creates nested
            # objects and "~1" becomes "/".
            r json.set b . {{"a":1}}
            assert_equal OK [r json.merge b {$.m/n} {{"z":1}}]
            assert_equal OK [r json.merge b {$.m~1n} {{"z":2}}]
            assert_equal OK [r json.merge b {.p~0q} 3]
            assert_equal {{"a":1,"m/n":{"z":1},"m~1n":{"z":2},"p~0q":3}} [r json.get b]
            assert_equal OK [r json.merge b {$.m/n} {{"z":null,"y":4}}]
            assert_equal {[{"y":4}]} [r json.get b {$.m/n}]
            assert_equal OK [r json.merge b . {{"m/n":null,"m~1n":null,"p~0q":null,"s/t":{"u~v":1}}}]
            assert_equal {{"a":1,"s/t":{"u~v":1}}} [r json.get b]
            r del b
        }

        test "JSON.MERGE honours json.max-path-limit and leaves the document unchanged (caps $caps)" {
            r config set json.max-path-limit 3
            r json.set p . {{"a":{"a":{"a":0}}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.merge p {$.a.a} {{"m":{"n":1}}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.merge p {$.a.a} {{"m":{}}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.merge p . {{"a":{"a":{"a":{"a":1}}}}}}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.merge fresh . {[[[[1]]]]}}
            assert_equal 0 [r exists fresh]
            assert_equal {{"a":{"a":{"a":0}}}} [r json.get p]
            assert_equal OK [r json.merge p {$.a.a} {{"m":1}}]
            assert_equal {{"a":{"a":{"a":0,"m":1}}}} [r json.get p]
            r config set json.max-path-limit 128
            assert_equal OK [r json.merge p {$.a.a} {{"m":{"n":1}}}]
            assert_equal {{"a":{"a":{"a":0,"m":{"n":1}}}}} [r json.get p]
            r del p
        }

        test "JSON.MERGE many members across the packed and tree forms (caps $caps)" {
            # Grow one object member by member, then shrink it again with nulls, comparing
            # the whole document after each group.
            r json.set big . {{}}
            set expected {}
            for {set j 0} {$j < 200} {incr j} {
                assert_equal OK [r json.merge big . "{\"k$j\":{\"v\":$j,\"s\":\"[string repeat x [expr {$j % 40}]]\"}}"]
                lappend expected "\"k$j\":{\"v\":$j,\"s\":\"[string repeat x [expr {$j % 40}]]\"}"
            }
            assert_equal "{[join $expected ,]}" [r json.get big]
            assert_equal 200 [r json.objlen big]
            # One patch that updates half of the members and deletes the other half.
            set patch {}
            set expected {}
            for {set j 0} {$j < 200} {incr j} {
                if {$j % 2} {
                    lappend patch "\"k$j\":null"
                } else {
                    lappend patch "\"k$j\":{\"v\":null,\"w\":\[$j\]}"
                    lappend expected "\"k$j\":{\"s\":\"[string repeat x [expr {$j % 40}]]\",\"w\":\[$j\]}"
                }
            }
            assert_equal OK [r json.merge big . "{[join $patch ,]}"]
            assert_equal "{[join $expected ,]}" [r json.get big]
            assert_equal 100 [r json.objlen big]
            # Delete everything.
            set patch {}
            for {set j 0} {$j < 200} {incr j 2} { lappend patch "\"k$j\":null" }
            assert_equal OK [r json.merge big . "{[join $patch ,]}"]
            assert_equal {{}} [r json.get big]
            r del big
        }

        test "JSON.MERGE reply with RESP3 (caps $caps)" {
            r hello 3
            assert_equal OK [r json.merge r3 . {{"a":1}}]
            assert_equal OK [r json.merge r3 {$.b} {{"c":null,"d":2}}]
            assert_equal {{"a":1,"b":{"c":null,"d":2}}} [r json.get r3]
            assert_equal OK [r json.merge r3 . {{"b":{"c":null}}}]
            assert_equal {{"a":1,"b":{"d":2}}} [r json.get r3]
            r hello 2
            r del r3
        }
    }
}
