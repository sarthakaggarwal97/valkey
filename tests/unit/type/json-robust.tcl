# Tests for the built-in JSON module: inputs that must be rejected or survived.
#
#   - Invalid UTF-8 in JSON text (design change B1): rejected from a client,
#     accepted on a trusted path (RESTORE, RDB load).
#   - Malformed JSON text: the same verdict as the external valkey-json module
#     for every case, checked against a server running it. One deliberate
#     difference: text with a NUL byte is an error here, while the module
#     stops reading at the NUL.
#   - Very deep nesting: a client is held to json.max-path-limit, and a
#     document from any source to max(json.max-path-limit, 1024) levels. A
#     document within that bound loads from RDB and RESTORE, and every command
#     that then touches it must return without crashing or hanging. The
#     external module crashes on some of these, so there is no reference
#     value: the tests assert the stated behaviour and that the server answers
#     afterwards.
#   - Bounded work: a path whose repeated matches multiply, a recursive
#     descent of more than 4,096 levels and a formatted reply over 512 MB fail
#     with a LIMIT error, promptly and without building the result.
#   - Replies that carry client data (a key name with CR LF in it).

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

# Bytes from hex digits. The test client sends strings to the server as bytes.
proc json_hex {hex} {
    return [binary format H* $hex]
}

# Well-formed UTF-8 sequences: the first and last code point of each length,
# and the code points next to the surrogate range.
proc json_utf8_valid {} {
    return {7f c280 dfbf e0a080 efbfbf ed9fbf ee8080 f0908080 f48fbfbf c3a9 e282ac f09f9880}
}

# Ill-formed sequences: lone continuation bytes, truncated sequences, overlong
# forms, UTF-8 encoded surrogates, code points above U+10FFFF, and bytes that
# never appear in UTF-8.
proc json_utf8_invalid {} {
    return {80 bf c080 c1bf c2 c241 e08080 e09fbf e282 e28241 eda080 edbfbf f0808080 f08fbfbf f09080 f0908041
            f4908080 f5808080 f888808080 fc8480808080 fe ff c3 e2 f0 f09f98}
}

# N levels of nested arrays, the innermost empty.
proc json_deep_array {n} {
    return "[string repeat {[} $n][string repeat {]} $n]"
}

# N levels of nested objects under the member "a", the innermost value 1.
proc json_deep_object {n} {
    return "[string repeat "\{\"a\":" $n]1[string repeat "\}" $n]"
}

# Stores `text` under `key` although it is nested deeper than
# json.max-path-limit: the limit is raised for the write and put back.
proc json_set_deep {key text} {
    r config set json.max-path-limit 2000000
    assert_equal OK [r json.set $key . $text]
    r config set json.max-path-limit 128
}

# Runs a command that must fail with an error matching `pattern`, and must do
# so within `ms` milliseconds.
proc json_assert_fails_fast {pattern ms args} {
    set start [clock milliseconds]
    assert_error $pattern {r {*}$args}
    set elapsed [expr {[clock milliseconds] - $start}]
    if {$elapsed >= $ms} {
        fail "'[string range $args 0 60]' took $elapsed ms to fail, expected under $ms ms"
    }
}

start_server {tags {"json"}} {
    # ---------------------------------------------------------------------
    # B1: invalid UTF-8
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON: well-formed UTF-8 of every length is accepted and returned unchanged (caps $caps)" {
            r flushall
            foreach hex [json_utf8_valid] {
                set bytes [json_hex $hex]
                assert_equal OK [r json.set k . "\"$bytes\""]
                assert_equal "\"$bytes\"" [r json.get k]
                assert_equal [string length $bytes] [r json.strlen k]
                assert_equal OK [r json.set k . "{\"$bytes\":\[\"a${bytes}z\",1\]}"]
                assert_equal "{\"$bytes\":\[\"a${bytes}z\",1\]}" [r json.get k]
                assert_equal [list $bytes] [r json.objkeys k]
                assert_equal 0 [r json.arrindex k "\$\[\"$bytes\"\]" "\"a${bytes}z\""]
                assert_equal [expr {[string length $bytes] * 2 + 2}] [r json.strappend k "\[\"$bytes\"\]\[0\]" "\"$bytes\""]
            }
        }

        test "JSON.SET rejects ill-formed UTF-8 in a string, a member name and nested values - B1 (caps $caps)" {
            r flushall
            r json.set keep . {{"a":1}}
            foreach hex [json_utf8_invalid] {
                set bad [json_hex $hex]
                foreach text [list "\"$bad\"" "\"abc$bad\"" "\"${bad}abc\"" "{\"$bad\":1}" "{\"k\":\"$bad\"}" \
                                  "\[1,\"$bad\"\]" "{\"a\":{\"b\":\[\[\"x$bad\"\]\]}}" \
                                  "{\"pad\":\"[string repeat p 5000]\",\"k\":\"$bad\"}" \
                                  "\"[json_hex c3a9]$bad[json_hex e282ac]\""] {
                    assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.set new . $text}
                    assert_error "SYNTAXERR*" {r json.set keep . $text}
                    assert_error "SYNTAXERR*" {r json.set keep .a $text}
                    assert_error "SYNTAXERR*" {r json.set keep \$.b $text}
                }
            }
            assert_equal 0 [r exists new]
            assert_equal {{"a":1}} [r json.get keep]
        }

        test "Every command that takes JSON text rejects ill-formed UTF-8 and changes nothing - B1 (caps $caps)" {
            r flushall
            set doc {{"s":"x","arr":["a"],"o":{"k":1}}}
            r json.set k . $doc
            foreach hex {80 c080 eda080 f4908080 ff c2} {
                set bad "\"[json_hex $hex]\""
                assert_error "SYNTAXERR*" {r json.mset k .s $bad}
                assert_error "SYNTAXERR*" {r json.mset other . 1 k .s $bad}
                assert_error "SYNTAXERR*" {r json.mset k .s $bad other . 1}
                assert_error "SYNTAXERR*" {r json.merge k . "{\"s\":$bad}"}
                assert_error "SYNTAXERR*" {r json.merge k .o "{$bad:1}"}
                assert_error "SYNTAXERR*" {r json.strappend k .s $bad}
                assert_error "SYNTAXERR*" {r json.strappend k \$.s $bad}
                assert_error "SYNTAXERR*" {r json.arrappend k .arr $bad}
                assert_error "SYNTAXERR*" {r json.arrappend k .arr {"ok"} $bad}
                assert_error "SYNTAXERR*" {r json.arrappend k \$.arr $bad {"ok"}}
                assert_error "SYNTAXERR*" {r json.arrinsert k .arr 0 $bad}
                assert_error "SYNTAXERR*" {r json.arrinsert k .arr 0 {"ok"} $bad}
                assert_error "SYNTAXERR*" {r json.arrindex k .arr $bad}
            }
            assert_equal $doc [r json.get k]
            assert_equal 0 [r exists other]
        }

        test "A new member name that is not valid UTF-8 is refused when it comes from a path - B1 (caps $caps)" {
            r flushall
            r json.set u $ {{}}
            r json.set keep . {{"a":{"b":1}}}
            foreach hex {fffe 80 c080 eda080 f4908080 c3 ff} {
                set bad [json_hex $hex]
                assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.set u "\$.$bad" 1}
                foreach path [list "\$.$bad" ".$bad" "\$\[\"$bad\"\]" "\$\['$bad'\]" "\$.a.$bad" ".a.$bad" "\$.a\[\"x$bad\"\]"] {
                    assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.set keep $path 1}
                    assert_error "SYNTAXERR*" {r json.set keep $path {{"x":[1]}} NX}
                    assert_error "SYNTAXERR*" {r json.mset keep $path 1}
                    assert_error "SYNTAXERR*" {r json.mset other . 1 keep $path 1}
                    assert_error "SYNTAXERR*" {r json.merge keep $path 1}
                }
                # A read or a delete through such a path just matches nothing.
                assert_equal {[]} [r json.get keep "\$.$bad"]
                assert_equal 0 [r json.del keep "\$.$bad"]
                assert_equal {[]} [r json.numincrby keep "\$.$bad" 1]
            }
            assert_equal {{}} [r json.get u]
            assert_equal {{"a":{"b":1}}} [r json.get keep]
            assert_equal 0 [r exists other]
            # A name of well-formed multi-byte characters is accepted.
            set e [json_hex c3a9]
            assert_equal OK [r json.set keep "\$.$e" 1]
            assert_equal OK [r json.merge keep "\$.a.$e$e" 2]
            assert_equal "{\"a\":{\"b\":1,\"$e$e\":2},\"$e\":1}" [r json.get keep]
        }

        test "A member whose name is not valid UTF-8 and arrived through RESTORE can be read, updated and deleted by path - B1 (caps $caps)" {
            r flushall
            set bad [json_hex fffefffe]
            r json.set src . {{"AAAA":1,"z":2}}
            set payload [string map [list AAAA $bad] [r dump src]]
            r debug set-skip-checksum-validation 1
            assert_equal OK [r restore t 0 $payload]
            r debug set-skip-checksum-validation 0
            assert_equal "{\"$bad\":1,\"z\":2}" [r json.get t]
            assert_equal {[1]} [r json.get t "\$.$bad"]
            # The name exists, so this is an update, not a new name.
            assert_equal OK [r json.set t "\$.$bad" 5]
            assert_equal {[6]} [r json.numincrby t "\$.$bad" 1]
            assert_equal "{\"$bad\":6,\"z\":2}" [r json.get t]
            # Another invalid name is still refused.
            assert_error "SYNTAXERR*" {r json.set t "\$.[json_hex fffe]" 5}
            assert_equal 1 [r json.del t "\$.$bad"]
            assert_equal {{"z":2}} [r json.get t]
        } {} {needs:debug}

        test "JSON: \\u escapes - surrogate pairs are decoded, lone surrogates are rejected as in the module (caps $caps)" {
            r flushall
            # A pair becomes one 4-byte sequence, a BMP escape 2 or 3 bytes.
            assert_equal OK [r json.set k . {"\ud83d\ude00"}]
            assert_equal "\"[json_hex f09f9880]\"" [r json.get k]
            assert_equal OK [r json.set k . {"\u0041\u00e9\u20ac"}]
            assert_equal "\"A[json_hex c3a9e282ac]\"" [r json.get k]
            # U+0000 is kept and printed as an escape.
            assert_equal OK [r json.set k . {"a\u0000b"}]
            assert_equal {"a\u0000b"} [r json.get k]
            assert_equal 3 [r json.strlen k]
            foreach text {{"\ud800"} {"\udc00"} {"\ud83d"} {"\ud83dx"} {"\udc00\ud83d"} {"\ud83d\u0041"} {"\uZZZZ"} {"\u12"} {"\u"}} {
                assert_error "SYNTAXERR*" {r json.set bad . $text}
            }
            assert_equal 0 [r exists bad]
        }

        test "JSON: ill-formed UTF-8 arriving through RESTORE is accepted and served back, as on any trusted path - B1 (caps $caps)" {
            r flushall
            # Build the payload from the DUMP of a document of the same length.
            r json.set src . {{"name":"AAAA","list":["BBBB",1]}}
            set bad [json_hex fffe80c0]
            set payload [string map [list AAAA $bad BBBB $bad] [r dump src]]
            r debug set-skip-checksum-validation 1
            assert_equal OK [r restore k 0 $payload]
            r debug set-skip-checksum-validation 0
            set text "{\"name\":\"$bad\",\"list\":\[\"$bad\",1\]}"
            assert_equal $text [r json.get k]
            assert_equal 4 [r json.strlen k .name]
            # It survives a reload and a copy, and ordinary writes work on it.
            r debug reload
            assert_equal $text [r json.get k]
            r copy k k2
            assert_equal $text [r json.get k2]
            assert_equal 3 [r json.arrappend k .list {"ok"}]
            assert_equal 6 [r json.strappend k .name {"ok"}]
            assert_equal "{\"name\":\"${bad}ok\",\"list\":\[\"$bad\",1,\"ok\"\]}" [r json.get k]
            # The client still cannot write such bytes.
            assert_error "SYNTAXERR*" {r json.set k .name "\"$bad\""}
        } {} {needs:debug}
    }
    json_set_caps {4096 128}

    # ---------------------------------------------------------------------
    # Malformed and unusual JSON text
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON.SET rejects malformed JSON text, as the module does (caps $caps)" {
            r flushall
            r json.set keep . {{"a":1}}
            r json.set keeparr . {[1]}
            foreach text [list {[1,]} {{"a":1,}} {[,1]} {{"a"}} {{1:2}} 01 1. .5 +1 1e 1e+ --1 0x10 NaN Infinity -Infinity \
                               True nul {'a'} {"a} {"\q"} {[1 2]} {{"a":1 "b":2}} {[]]} "\{\}\}" "// c\n1" {/* */1} {[1]x} \
                               {{"a":1}{"b":2}} - {[} "\{" "\{\"a\":" {[1} tru -01 1.e5 1.5e 00 "" " " "\x0b1" "1\x0b" \
                               "\"a\x01b\"" "\"a\tb\"" "\"a\nb\"" "\x00" "\"a\x00b\"" "\x0c1" "[json_hex c2a0]1" \
                               "[json_hex efbbbf]" "{\"a\":1}[json_hex efbbbf]" "\[1,2" "\[1,2\]\]" {{"a":[}} {{"a":}}] {
                assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.set new . $text}
                assert_error "SYNTAXERR*" {r json.set keep .a $text}
                assert_error "SYNTAXERR*" {r json.arrappend keeparr . $text}
            }
            assert_equal 0 [r exists new]
            assert_equal {{"a":1}} [r json.get keep]
            assert_equal {[1]} [r json.get keeparr]
        }

        test "JSON.SET: every proper prefix of a document is rejected (caps $caps)" {
            r flushall
            set text {{"name":"x","n":[1,2.5,-3e2,true,false,null],"o":{"k":"v\"\\\u00e9","e":{}},"t":[[],[[]]]}}
            for {set i 0} {$i < [string length $text]} {incr i} {
                assert_error "SYNTAXERR*" {r json.set k . [string range $text 0 [expr {$i - 1}]]}
            }
            assert_equal 0 [r exists k]
            assert_equal OK [r json.set k . $text]
            # And the document followed by anything but whitespace.
            foreach tail {x 1 , "\]" "\}" "\"" "\\"} {
                assert_error "SYNTAXERR*" {r json.set k . "$text$tail"}
            }
            assert_equal OK [r json.set k . "$text \t\r\n"]
        }

        test "JSON.SET accepts unusual but valid JSON text, with the module's output (caps $caps)" {
            r flushall
            foreach {text printed} [list \
                -0                             0 \
                0e0                            0e0 \
                1E+2                           1E+2 \
                1e-2                           1e-2 \
                -0.0                           -0.0 \
                -0e0                           -0e0 \
                0E0                            0E0 \
                2e0                            2e0 \
                1e308                          1e308 \
                -1e308                         -1e308 \
                1e-400                         1e-400 \
                0.0000000000000000000000000000001 0.0000000000000000000000000000001 \
                123456789012345678901234567890.123456789 123456789012345678901234567890.123456789 \
                {""}                           {""} \
                {"\/"}                         {"/"} \
                {"\b\f\n\r\t"}                 {"\b\f\n\r\t"} \
                {"\"\\"}                       {"\"\\"} \
                " \t\r\n\[ 1 , 2 \] \n"        {[1,2]} \
                {{"a":1,"a":2}}                {{"a":2}} \
                {{"":1}}                       {{"":1}} \
                {[[],{},"",0,null,true,false]} {[[],{},"",0,null,true,false]} \
                "[json_hex efbbbf]{\"a\":1}"   {{"a":1}} \
                "[json_hex efbbbf]1"           1 \
            ] {
                assert_equal OK [r json.set k . $text] "JSON.SET of '$text'"
                assert_equal $printed [r json.get k] "JSON.GET after '$text'"
            }
        }
    }
    json_set_caps {4096 128}

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON text with an embedded or trailing NUL byte is an error - deliberate difference from the module (caps $caps)" {
            # The module reads JSON text as a C string: it accepts {"a":1}\0 and even
            # [1]\0garbage, storing what precedes the NUL. Here the whole argument is the
            # text and a NUL byte is not JSON, in every command that takes JSON text.
            r flushall
            set doc {{"s":"x","arr":["a"],"o":{"k":1}}}
            r json.set k . $doc
            foreach text [list "1\x00" "{\"a\":1}\x00" "\[1\]\x00garbage" "\x001" "\x00" "\[1,\x002\]" "\"a\x00b\"" \
                               "{\"a\x00\":1}" "tr\x00ue" "1\x00\x00" " \x00 1"] {
                assert_error "SYNTAXERR Failed to parse JSON string due to syntax error" {r json.set new . $text}
                assert_error "SYNTAXERR*" {r json.set k . $text}
                assert_error "SYNTAXERR*" {r json.set k .s $text}
                assert_error "SYNTAXERR*" {r json.mset k .s $text}
                assert_error "SYNTAXERR*" {r json.mset other . 1 k .s $text}
                assert_error "SYNTAXERR*" {r json.merge k .o $text}
                assert_error "SYNTAXERR*" {r json.arrappend k .arr $text}
                assert_error "SYNTAXERR*" {r json.arrappend k .arr 1 $text}
                assert_error "SYNTAXERR*" {r json.arrinsert k .arr 0 $text}
                assert_error "SYNTAXERR*" {r json.arrindex k .arr $text}
            }
            assert_error "SYNTAXERR*" {r json.strappend k .s "\"z\"\x00"}
            assert_error "SYNTAXERR*" {r json.strappend k .s "\"z\x00\""}
            assert_equal 0 [r exists new other]
            assert_equal $doc [r json.get k]
            # The escape for U+0000 inside a string is valid JSON and is kept.
            assert_equal OK [r json.set k .s {"a\u0000b"}]
            assert_equal 3 [r json.strlen k .s]
            assert_equal 4 [r json.strappend k .s {"\u0000"}]
            assert_equal {"a\u0000b\u0000"} [r json.get k .s]
            assert_equal PONG [r ping]
        }

        test "JSON text may start with one UTF-8 byte order mark, as in the module (caps $caps)" {
            r flushall
            set bom [json_hex efbbbf]
            # One leading BOM is skipped by every command that takes JSON text.
            assert_equal OK [r json.set k . "$bom{\"s\":\"x\",\"arr\":\[\],\"o\":{}}"]
            assert_equal {{"s":"x","arr":[],"o":{}}} [r json.get k]
            assert_equal OK [r json.set k .n "${bom}1"]
            assert_equal OK [r json.set k \$.t "$bom \n true"]
            assert_equal 1 [r json.arrappend k .arr "${bom}7"]
            assert_equal 3 [r json.arrappend k .arr "$bom\"a\"" "$bom\[\]"]
            assert_equal 4 [r json.arrinsert k .arr 0 "${bom}null"]
            assert_equal 1 [r json.arrindex k .arr "${bom}7"]
            assert_equal 2 [r json.arrindex k .arr "$bom\"a\""]
            assert_equal 2 [r json.strappend k .s "$bom\"y\""]
            assert_equal OK [r json.merge k .o "$bom{\"m\":1}"]
            assert_equal OK [r json.mset k .o.q "${bom}2" k2 . "$bom\[1\]"]
            assert_equal {{"s":"xy","arr":[null,7,"a",[]],"o":{"m":1,"q":2},"n":1,"t":true}} [r json.get k]
            assert_equal {[1]} [r json.get k2]
            # Only one, only at the very start, and not alone.
            foreach text [list "$bom${bom}1" "$bom" " ${bom}1" "1$bom" "\[${bom}1\]" "{\"a\":${bom}1}" "$bom $bom 1" \
                               "[json_hex efbb]1" "[json_hex fffe]1" "[json_hex feff]1"] {
                assert_error "SYNTAXERR*" {r json.set new . $text}
                assert_error "SYNTAXERR*" {r json.arrappend k .arr $text}
            }
            assert_equal 0 [r exists new]
            # Inside a string the same three bytes are ordinary content.
            assert_equal OK [r json.set k .s "\"a${bom}b\""]
            assert_equal 5 [r json.strlen k .s]
            assert_equal "\"a${bom}b\"" [r json.get k .s]
            assert_equal OK [r json.set k . "$bom\"$bom\""]
            assert_equal "\"$bom\"" [r json.get k]
        }
    }
    json_set_caps {4096 128}

    test {JSON: large inputs - a 16 MB string, a million-element array, 100,000 members} {
        r flushall
        set big [string repeat x 16000000]
        assert_equal OK [r json.set s . "\"$big\""]
        assert_equal 16000000 [r json.strlen s]
        assert_equal 16000001 [r json.strappend s {"y"}]
        assert_equal 16000003 [string length [r json.get s]]

        set elements {}
        for {set i 0} {$i < 1000000} {incr i} { lappend elements $i }
        assert_equal OK [r json.set a . "\[[join $elements ,]\]"]
        assert_equal 1000000 [r json.arrlen a]
        assert_equal 999999 [r json.get a {[-1]}]
        assert_equal 999999 [r json.arrindex a . 999999]
        assert_equal 1000001 [r json.arrappend a . {"end"}]
        assert_equal 500000 [r json.arrtrim a . 0 499999]
        assert_equal {[499999]} [r json.get a {$[-1]}]

        set members {}
        for {set i 0} {$i < 100000} {incr i} { lappend members "\"member$i\":$i" }
        assert_equal OK [r json.set o . "{[join $members ,]}"]
        assert_equal 100000 [r json.objlen o]
        assert_equal 99999 [r json.get o .member99999]
        assert_equal 1 [r json.del o .member50000]
        assert_equal {[]} [r json.get o {$.member50000}]
        assert_equal 99999 [llength [r json.objkeys o]]
        r debug reload
        assert_equal 99999 [r json.objlen o]
        assert_equal 500000 [r json.arrlen a]
        assert_equal 16000001 [r json.strlen s]
        r flushall
        json_assert_num_documents 0
    } {} {needs:debug}

    # ---------------------------------------------------------------------
    # Deep nesting
    # ---------------------------------------------------------------------

    test {JSON.SET deeper than json.max-path-limit is rejected, at the limit it is accepted} {
        r flushall
        assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
        assert_equal OK [r json.set k . [json_deep_array 128]]
        assert_error "LIMIT Document path nesting limit is exceeded" {r json.set k2 . [json_deep_array 129]}
        assert_error "LIMIT*" {r json.set k2 . [json_deep_array 5000]}
        assert_error "LIMIT*" {r json.set k2 . [json_deep_object 5000]}
        assert_error "LIMIT*" {r json.set k2 . [json_deep_array 1000000]}
        assert_equal 0 [r exists k2]
        assert_equal PONG [r ping]
    }

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON: a document 1,024 levels deep loads from RDB and RESTORE with the default limit (caps $caps)" {
            # A document may be nested max(json.max-path-limit, 1024) levels whatever
            # its source, so one that was written under a higher limit still loads.
            r flushall
            json_set_deep deep [json_deep_array 1024]
            json_set_deep deepo [json_deep_object 1024]
            assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
            r debug reload
            assert_equal ReJSON-RL [r type deep]
            assert_equal [json_deep_array 1024] [r json.get deep]
            assert_equal [json_deep_object 1024] [r json.get deepo]
            assert_equal OK [r restore deep2 0 [r dump deep]]
            assert_equal OK [r restore deepo2 0 [r dump deepo]]
            assert_equal 1 [r copy deep deep3]
            assert_equal 1 [r copy deepo deepo3]
            foreach key {deep2 deep3} { assert_equal [json_deep_array 1024] [r json.get $key] }
            foreach key {deepo2 deepo3} { assert_equal [json_deep_object 1024] [r json.get $key] }
            assert_equal [r debug digest-value deepo] [r debug digest-value deepo2]
            assert_equal [r debug digest-value deepo] [r debug digest-value deepo3]
            # A client is still held to json.max-path-limit.
            assert_error "LIMIT*" {r json.set other . [json_deep_array 1024]}
            assert_error "LIMIT*" {r json.set deep . [json_deep_array 129]}
            assert_equal [json_deep_array 1024] [r json.get deep]
            assert_equal 0 [r exists other]
        } {} {needs:debug}

        test "JSON: every read command on a document 1,024 levels deep (caps $caps)" {
            assert_equal 1023 [r json.debug depth deep]
            assert_equal 1024 [r json.debug depth deepo]
            assert_equal 1023 [r json.debug fields deep]
            assert_equal 1024 [r json.debug fields deepo]
            assert_morethan [r json.debug memory deep] 0
            assert_morethan [r memory usage deepo] 0
            assert_equal array [r json.type deep]
            assert_equal {array} [r json.type deep {$[0][0][0]}]
            assert_equal 1 [r json.arrlen deep]
            assert_equal 1 [r json.objlen deepo]
            assert_equal a [r json.objkeys deepo]
            assert_equal [json_deep_array 1021] [r json.get deep {[0][0][0]}]
            assert_equal [json_deep_object 924] [r json.get deepo [string repeat .a 100]]
            assert_equal "\[[json_deep_array 1014]\]" [r json.get deep "\$[string repeat {[0]} 10]"]
            # The innermost values, 1,023 and 1,024 steps down.
            assert_equal {[[]]} [r json.get deep "\$[string repeat {[0]} 1023]"]
            assert_equal {[]} [r json.get deep "\$[string repeat {[0]} 1024]"]
            assert_equal 1 [r json.get deepo [string repeat .a 1024]]
            assert_equal {[1]} [r json.get deepo "\$[string repeat .a 1024]"]
            assert_equal integer [r json.type deepo [string repeat .a 1024]]
            assert_equal [json_deep_array 1024] [lindex [r json.mget deep nokey .] 0]
            assert_equal -1 [r json.arrindex deep . [json_deep_array 100]]
            assert_equal -1 [r json.arrindex deep . 1]
            set pretty [r json.get deep INDENT " " NEWLINE "\n" SPACE " "]
            assert_morethan [string length $pretty] 2048
            # A recursive path visits every level.
            assert_equal [concat [lrepeat 1022 1] 0] [r json.arrlen deep {$..[0]}]
            assert_equal [concat [lrepeat 1023 object] integer] [r json.type deepo {$..a}]
            assert_equal 40 [string length [r debug digest-value deep]]
            assert_equal PONG [r ping]
        } {} {needs:debug}

        test "JSON.RESP on a document 1,024 levels deep does not crash the server (caps $caps)" {
            # The reply is 1,024 nested arrays, which the test client cannot
            # parse, so it is sent on a connection that is then closed.
            r config resetstat
            foreach key {deep deepo} {
                set rd [valkey_deferring_client]
                $rd select 9
                $rd read
                $rd json.resp $key
                $rd flush
                wait_for_condition 100 20 {
                    [string match "*calls=[expr {$key eq "deep" ? 1 : 2}],*" [cmdrstat JSON.RESP r]]
                } else {
                    fail "JSON.RESP was not executed"
                }
                $rd close
            }
            assert_equal PONG [r ping]
            assert_equal [json_deep_array 1024] [r json.get deep]
        }

        test "JSON: writes on a document 1,024 levels deep are held to json.max-path-limit (caps $caps)" {
            # Near the root: the nesting of the written value counts from its position.
            assert_equal 2 [r json.arrappend deep2 {$[0][0]} 1]
            assert_equal 3 [r json.arrinsert deep2 {[0][0]} 0 {"first"}]
            assert_equal OK [r json.set deepo2 .a.a.b {{"x":[1,2,3]}}]
            assert_equal OK [r json.set deepo2 .top 1]
            assert_equal {[1,2,3]} [r json.get deepo2 .a.a.b.x]
            assert_equal {a top} [r json.objkeys deepo2]
            assert_equal OK [r json.merge deepo2 . {{"top":null,"m":{"n":1}}}]
            assert_equal {a m} [r json.objkeys deepo2]
            assert_equal 1024 [r json.debug depth deepo2]
            # Below the limit of 128 levels a write that adds a value is refused.
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.set deep2 "\$\[0\]\[0\]\[1\][string repeat {[0]} 197]" 1}
            assert_error "LIMIT Document path nesting limit is exceeded" {r json.arrappend deep2 "\$\[0\]\[0\]\[1\][string repeat {[0]} 197]" 1}
            assert_error "LIMIT*" {r json.arrappend deep3 "\$[string repeat {[0]} 1023]" 1}
            assert_error "LIMIT*" {r json.set deepo3 "\$[string repeat .a 200].b" 1}
            # A value that would push a subtree past the limit is refused.
            assert_error "LIMIT*" {r json.set deepo2 .a.a.c [json_deep_array 127]}
            assert_equal OK [r json.set deepo2 .a.a.c [json_deep_array 125]]
            # Changing the innermost number in place adds no level and works.
            assert_equal {[2]} [r json.numincrby deepo3 "\$[string repeat .a 1024]" 1]
            assert_equal {[3]} [r json.numincrby deepo3 {$..[?(@.a==2)].a} 1]
            assert_equal [string map {":1" ":3"} [json_deep_object 1024]] [r json.get deepo3]
            assert_equal [json_deep_array 1024] [r json.get deep3]
            assert_equal PONG [r ping]
        }

        test "JSON.DEL, JSON.CLEAR, DEL and UNLINK on documents 1,024 levels deep (caps $caps)" {
            # A sub-path delete removes 1,021 levels at once.
            assert_equal 1 [r json.del deep {$[0][0][0]}]
            assert_equal {[[[]]]} [r json.get deep]
            assert_equal 1 [r json.del deepo .a.a.a]
            assert_equal {{"a":{"a":{}}}} [r json.get deepo]
            assert_equal 2 [r json.debug depth deepo]
            # The innermost value, and a value in the middle.
            assert_equal 1 [r json.del deepo3 "\$[string repeat .a 1024]"]
            assert_equal 1023 [r json.debug depth deepo3]
            assert_equal 1 [r json.clear deep3 "\$[string repeat {[0]} 1000]"]
            assert_equal 1000 [r json.debug depth deep3]
            assert_equal 1 [r json.del deep3 "\$[string repeat {[0]} 900]"]
            assert_equal 899 [r json.debug depth deep3]
            # Clearing the root container of a deep document.
            assert_equal 1 [r json.clear deep3]
            assert_equal {[]} [r json.get deep3]
            assert_equal 1 [r json.clear deepo3]
            assert_equal {{}} [r json.get deepo3]
            # Whole documents.
            foreach {key kind} {d1 array d2 object d3 array d4 object d5 object d6 object} {
                json_set_deep $key [json_deep_$kind 1024]
            }
            r debug reload
            assert_equal 1 [r json.del d1]
            assert_equal 1 [r json.forget d2 $]
            assert_equal 1 [r del d3]
            assert_equal 1 [r unlink d4]
            assert_equal OK [r json.set d5 . 1]
            assert_equal OK [r set d6 string]
            wait_lazyfree_done r
            assert_equal 0 [r exists d1 d2 d3 d4]
            assert_equal 1 [r json.get d5]
            r flushall async
            wait_lazyfree_done r
            json_assert_num_documents 0
            assert_equal PONG [r ping]
        } {} {needs:debug}
    }
    json_set_caps {4096 128}

    test {JSON: 1,025 levels are refused by RESTORE with the default limit and accepted after raising it} {
        r flushall
        json_set_deep a1024 [json_deep_array 1024]
        json_set_deep a1025 [json_deep_array 1025]
        json_set_deep o1024 [json_deep_object 1024]
        json_set_deep o1025 [json_deep_object 1025]
        foreach key {a1024 a1025 o1024 o1025} { set bound_payload($key) [r dump $key] }
        r flushall
        assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
        # Both sides of the bound of max(128, 1024) levels.
        assert_equal OK [r restore a1024 0 $bound_payload(a1024)]
        assert_equal OK [r restore o1024 0 $bound_payload(o1024)]
        assert_error "ERR Bad data format" {r restore a1025 0 $bound_payload(a1025)}
        assert_error "ERR Bad data format" {r restore o1025 0 $bound_payload(o1025)}
        assert_error "ERR Bad data format" {r restore a1024 0 $bound_payload(a1025) replace}
        assert_equal 0 [r exists a1025 o1025]
        assert_equal [json_deep_array 1024] [r json.get a1024]
        json_assert_num_documents 2
        # A limit below 1,024 does not lower the bound, a limit above it raises it.
        foreach limit {1 64 1024} {
            r config set json.max-path-limit $limit
            assert_error "ERR Bad data format" {r restore a1025 0 $bound_payload(a1025)}
            assert_equal OK [r restore a1024 0 $bound_payload(a1024) replace]
        }
        r config set json.max-path-limit 1025
        assert_equal OK [r restore a1025 0 $bound_payload(a1025)]
        assert_equal OK [r restore o1025 0 $bound_payload(o1025)]
        assert_equal [json_deep_array 1025] [r json.get a1025]
        assert_equal [json_deep_object 1025] [r json.get o1025]
        r debug reload
        assert_equal [json_deep_array 1025] [r json.get a1025]
        # With the limit lowered again the stored documents stay readable and writable.
        r config set json.max-path-limit 128
        assert_equal 1024 [r json.debug depth a1025]
        assert_equal 1025 [r json.debug depth o1025]
        assert_equal [json_deep_object 1025] [r json.get o1025]
        assert_equal {[1]} [r json.get o1025 "\$[string repeat .a 1025]"]
        assert_equal 2 [r json.arrappend a1025 {$[0]} 1]
        assert_equal 1 [r json.del a1025 {$[0][0]}]
        assert_equal {[[1]]} [r json.get a1025]
        assert_equal 1 [r del o1025]
        r flushall
    } {OK} {needs:debug}

    test {JSON: after the limit is lowered a document within 1,024 levels stays readable and reloadable} {
        r flushall
        r config set json.max-path-limit 2000
        assert_equal OK [r json.set a . [json_deep_array 1024]]
        assert_equal OK [r json.set o . [json_deep_object 1024]]
        assert_equal OK [r json.set mid . [json_deep_object 600]]
        set digest [debug_digest]
        foreach limit {1024 128 64 1 0} {
            r config set json.max-path-limit $limit
            assert_equal [json_deep_array 1024] [r json.get a]
            assert_equal [json_deep_object 1024] [r json.get o]
            r debug reload
            assert_equal $digest [debug_digest]
            assert_equal [json_deep_array 1024] [r json.get a]
            assert_equal [json_deep_object 1024] [r json.get o]
            assert_equal [json_deep_object 600] [r json.get mid]
            assert_equal OK [r restore copy 0 [r dump o] replace]
            assert_equal 1 [r del copy]
        }
        r config set json.max-path-limit 128
        r flushall
    } {OK} {needs:debug}
}

# DEBUG RELOAD of a document that is deeper than the bound fails, and a failed
# reload leaves the dataset empty, so this has a server of its own.
start_server {tags {"json"}} {
    test {JSON: DEBUG RELOAD fails when a stored document is deeper than max(json.max-path-limit, 1024)} {
        r config set json.max-path-limit 2000
        assert_equal OK [r json.set deep . [json_deep_array 1025]]
        assert_equal OK [r json.set other . {{"a":1}}]
        r debug reload
        assert_equal [json_deep_array 1025] [r json.get deep]
        r config set json.max-path-limit 128
        assert_equal [json_deep_array 1025] [r json.get deep]
        assert_error "*Error trying to load the RDB dump*" {r debug reload}
        assert_equal PONG [r ping]
        # The server keeps working after the failed load.
        r flushall
        assert_equal OK [r json.set again . {[1,2,3]}]
        r debug reload
        assert_equal {[1,2,3]} [r json.get again]
    } {} {needs:debug}
}

start_server {tags {"json"}} {
    # ---------------------------------------------------------------------
    # 5,000 levels, with json.max-path-limit raised to 6,000
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON: a document 5,000 levels deep loads once json.max-path-limit is at least 5,000 (caps $caps)" {
            r flushall
            r config set json.max-path-limit 6000
            assert_equal OK [r json.set deep . [json_deep_array 5000]]
            assert_equal OK [r json.set deepo . [json_deep_object 5000]]
            set digest [debug_digest]
            r debug reload
            assert_equal $digest [debug_digest]
            assert_equal [json_deep_array 5000] [r json.get deep]
            assert_equal [json_deep_object 5000] [r json.get deepo]
            assert_equal OK [r restore deep2 0 [r dump deep]]
            assert_equal OK [r restore deepo2 0 [r dump deepo]]
            assert_equal 1 [r copy deep deep3]
            assert_equal 1 [r copy deepo deepo3]
            foreach key {deep2 deep3} { assert_equal [json_deep_array 5000] [r json.get $key] }
            foreach key {deepo2 deepo3} { assert_equal [json_deep_object 5000] [r json.get $key] }
            assert_equal [r debug digest-value deepo] [r debug digest-value deepo2]
            # Beyond the raised limit a client is refused, and so is a payload.
            assert_error "LIMIT*" {r json.set other . [json_deep_array 6001]}
            assert_equal OK [r json.set other . [json_deep_array 6000]]
            set payload [r dump other]
            r config set json.max-path-limit 5999
            assert_error "ERR Bad data format" {r restore other2 0 $payload}
            r config set json.max-path-limit 6000
            assert_equal OK [r restore other2 0 $payload]
            assert_equal 2 [r del other other2]
        } {} {needs:debug}

        test "JSON: reads on a document 5,000 levels deep (caps $caps)" {
            assert_equal 4999 [r json.debug depth deep]
            assert_equal 5000 [r json.debug depth deepo]
            assert_equal 4999 [r json.debug fields deep]
            assert_equal 5000 [r json.debug fields deepo]
            assert_morethan [r json.debug memory deep] 0
            assert_morethan [r memory usage deepo] 0
            assert_equal array [r json.type deep]
            assert_equal {array} [r json.type deep {$[0][0][0]}]
            assert_equal 1 [r json.arrlen deep]
            assert_equal 1 [r json.objlen deepo]
            assert_equal a [r json.objkeys deepo]
            assert_equal [json_deep_array 4997] [r json.get deep {[0][0][0]}]
            assert_equal [json_deep_object 4900] [r json.get deepo [string repeat .a 100]]
            assert_equal "\[[json_deep_array 4990]\]" [r json.get deep "\$[string repeat {[0]} 10]"]
            assert_equal {[1]} [r json.get deepo "\$[string repeat .a 5000]"]
            assert_equal [json_deep_array 5000] [lindex [r json.mget deep nokey .] 0]
            assert_equal -1 [r json.arrindex deep . [json_deep_array 100]]
            set pretty [r json.get deep INDENT " " NEWLINE "\n" SPACE " "]
            assert_morethan [string length $pretty] 10000
            assert_equal 40 [string length [r debug digest-value deep]]
            assert_equal PONG [r ping]
        } {} {needs:debug}

        test "JSON: a recursive descent more than 4,096 levels below its start fails fast (caps $caps)" {
            # Without a bound, `$..*` on such a document builds a result of gigabytes.
            set peak [s used_memory_peak]
            foreach {cmd key path} {
                json.get deep {$..*}        json.get deepo {$..*}        json.get deepo {$..a}
                json.get deep {$..[0]}      json.arrlen deep {$..[0]}    json.type deepo {$..a}
                json.objkeys deepo {$..a}   json.resp deep {$..*}        json.debug deep {$..*}
            } {
                if {$cmd eq "json.debug"} {
                    json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 json.debug memory $key $path
                } else {
                    json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 $cmd $key $path
                }
            }
            # Write commands too, and they change nothing.
            json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 json.del deep3 {$..[0]}
            json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 json.set deepo3 {$..a} 2
            json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 json.numincrby deepo3 {$..a} 1
            json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 json.clear deep3 {$..*}
            assert_equal [json_deep_array 5000] [r json.get deep3]
            assert_equal [json_deep_object 5000] [r json.get deepo3]
            assert_lessthan [expr {[s used_memory_peak] - $peak}] [expr {64 * 1024 * 1024}]
            # A descent that starts further down is within the bound.
            assert_equal 903 [llength [r json.arrlen deep "\$[string repeat {[0]} 4096]..\[0\]"]]
            assert_equal [concat [lrepeat 99 object] integer] [r json.type deepo "\$[string repeat .a 4900]..a"]
            assert_equal PONG [r ping]
        }

        test "JSON: the bound of a recursive descent is 4,096 levels below its start (caps $caps)" {
            foreach {key text} [list a4097 [json_deep_array 4097] a4098 [json_deep_array 4098] \
                                     o4097 [json_deep_object 4097] o4098 [json_deep_object 4098]] {
                assert_equal OK [r json.set $key . $text]
            }
            assert_equal 4096 [llength [r json.arrlen a4097 {$..[0]}]]
            assert_error "LIMIT Parser recursion depth is exceeded" {r json.arrlen a4098 {$..[0]}}
            assert_equal 4096 [llength [r json.arrlen a4098 {$[0]..[0]}]]
            assert_equal 4097 [llength [r json.type o4097 {$..a}]]
            assert_error "LIMIT Parser recursion depth is exceeded" {r json.type o4098 {$..a}}
            assert_equal 4097 [llength [r json.type o4098 {$.a..a}]]
            assert_equal 4 [r del a4097 a4098 o4097 o4098]
        }

        test "JSON.RESP on a document 5,000 levels deep does not crash the server (caps $caps)" {
            r config resetstat
            foreach key {deep deepo} {
                set rd [valkey_deferring_client]
                $rd select 9
                $rd read
                $rd json.resp $key
                $rd flush
                wait_for_condition 100 20 {
                    [string match "*calls=[expr {$key eq "deep" ? 1 : 2}],*" [cmdrstat JSON.RESP r]]
                } else {
                    fail "JSON.RESP was not executed"
                }
                $rd close
            }
            assert_equal PONG [r ping]
            assert_equal [json_deep_array 5000] [r json.get deep]
        }

        test "JSON: writes and deletes on documents 5,000 levels deep (caps $caps)" {
            assert_equal 2 [r json.arrappend deep2 {$[0][0]} 1]
            assert_equal OK [r json.set deepo2 .a.a.b {{"x":[1,2,3]}}]
            assert_equal {[2]} [r json.numincrby deepo3 "\$[string repeat .a 5000]" 1]
            assert_equal [string map {":1" ":2"} [json_deep_object 5000]] [r json.get deepo3]
            assert_equal 2 [r json.arrappend deep3 "\$[string repeat {[0]} 4998]" {"innermost"}]
            assert_equal {[[],"innermost"]} [r json.get deep3 "[string repeat {[0]} 4998]"]
            # A sub-path delete removes 4,997 levels at once.
            assert_equal 1 [r json.del deep {$[0][0][0]}]
            assert_equal {[[[]]]} [r json.get deep]
            assert_equal 1 [r json.del deepo .a.a.a]
            assert_equal {{"a":{"a":{}}}} [r json.get deepo]
            assert_equal 1 [r json.clear deep3]
            assert_equal {[]} [r json.get deep3]
            assert_equal 1 [r json.clear deepo3]
            assert_equal {{}} [r json.get deepo3]
            # Whole documents: delete, unlink, overwrite, expire, evict, flush.
            foreach {key kind} {d1 array d2 object d3 array d4 object d5 object d6 object d7 array d8 object} {
                assert_equal OK [r json.set $key . [json_deep_$kind 5000]]
            }
            r debug reload
            assert_equal 1 [r json.del d1]
            assert_equal 1 [r json.forget d2 $]
            assert_equal 1 [r del d3]
            assert_equal 1 [r unlink d4]
            assert_equal OK [r json.set d5 . 1]
            assert_equal OK [r set d6 string]
            r pexpire d7 1
            wait_for_condition 100 20 {
                [r exists d7] == 0
            } else {
                fail "key did not expire"
            }
            assert_equal 8 [r del d5 d6 deep deepo deep2 deepo2 deep3 deepo3]
            r config set maxmemory-policy allkeys-random
            r config set maxmemory 1
            catch {r set trigger 1}
            r config set maxmemory 0
            r config set maxmemory-policy noeviction
            assert_equal 0 [r exists d8]
            r flushall async
            wait_lazyfree_done r
            json_assert_num_documents 0
            assert_equal PONG [r ping]
        } {} {needs:debug}
    }
    json_set_caps {4096 128}

    test {JSON: mixed nesting 100,000 levels deep survives load, reads, writes and free once the limit allows it} {
        r flushall
        r config set json.max-path-limit 2000000
        set n 100000
        set text "[string repeat "\{\"a\":\[" [expr {$n / 2}]]null[string repeat "\]\}" [expr {$n / 2}]]"
        assert_equal OK [r json.set deep . $text]
        r debug reload
        assert_equal $n [r json.debug depth deep]
        assert_equal [string length $text] [string length [r json.get deep]]
        assert_equal OK [r restore deep2 0 [r dump deep]]
        assert_equal 1 [r copy deep deep3]
        assert_equal [r debug digest-value deep] [r debug digest-value deep2]
        assert_morethan [r memory usage deep] 0
        assert_equal object [r json.type deep {.a[0].a[0]}]
        json_assert_fails_fast "LIMIT Parser recursion depth is exceeded" 2000 json.get deep {$..*}
        assert_equal OK [r json.set deep {.a[0].b} 1]
        assert_equal 1 [r json.del deep {$.a[0].a}]
        assert_equal {{"a":[{"b":1}]}} [r json.get deep]
        assert_equal 1 [r json.clear deep2]
        assert_equal 1 [r unlink deep3]
        assert_equal 1 [r del deep2]
        wait_lazyfree_done r
        r config set json.max-path-limit 128
        assert_equal PONG [r ping]
    } {} {needs:debug}

    # ---------------------------------------------------------------------
    # Bounded path evaluation
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON path: repeated matches are returned as before while they are few (caps $caps)" {
            r flushall
            r json.set u . [json_deep_object 30]
            r json.set w . [json_deep_array 30]
            set a29 [json_deep_object 29]
            set a28 [json_deep_object 28]
            assert_equal "\[$a29,$a29\]" [r json.get u {$['a','a']}]
            assert_equal "\[$a28,$a28,$a28,$a28\]" [r json.get u {$['a','a']['a','a']}]
            assert_equal {object object} [r json.type u {$['a','a']}]
            assert_equal {1 1 1 1 1 1 1 1} [r json.arrlen w {$[0,0][0,0][0,0]}]
            assert_equal 1024 [llength [r json.arrlen w "\$[string repeat {[0,0]} 10]"]]
            assert_equal 1024 [llength [r json.objlen u "\$[string repeat {['a','a']} 10]"]]
        }

        test "JSON path: a query whose repeated matches multiply fails fast with a LIMIT error (caps $caps)" {
            set limit "*LIMIT Total number of repeated matches in the query result exceeds the limit*"
            set peak [s used_memory_peak]
            # 2^22 and 2^24 matches if they were collected.
            json_assert_fails_fast $limit 2000 json.get u "\$[string repeat {['a','a']} 22]"
            json_assert_fails_fast $limit 2000 json.arrlen w "\$[string repeat {[0,0]} 24]"
            foreach n {16 20 22 24 26 29} {
                json_assert_fails_fast $limit 2000 json.get u "\$[string repeat {['a','a']} $n]"
                json_assert_fails_fast $limit 2000 json.type u "\$[string repeat {['a','a']} $n]"
                json_assert_fails_fast $limit 2000 json.objlen u "\$[string repeat {['a','a']} $n]"
                json_assert_fails_fast $limit 2000 json.resp u "\$[string repeat {['a','a']} $n]"
                json_assert_fails_fast $limit 2000 json.arrlen w "\$[string repeat {[0,0]} $n]"
                json_assert_fails_fast $limit 2000 json.arrindex w "\$[string repeat {[0,0]} $n]" 1
                json_assert_fails_fast $limit 2000 json.get u "\$[string repeat {['a','a','a']} $n]"
            }
            json_assert_fails_fast $limit 2000 json.mget u w "\$[string repeat {['a','a']} 22]"
            assert_lessthan [expr {[s used_memory_peak] - $peak}] [expr {64 * 1024 * 1024}]
            assert_equal [json_deep_object 30] [r json.get u]
            assert_equal PONG [r ping]
        }

        test "JSON path: a write through repeated matches succeeds and changes each value once (caps $caps)" {
            # A write drops a value that is matched twice, so nothing multiplies.
            set t0 [clock milliseconds]
            assert_equal OK [r json.set u "\$[string repeat {['a','a']} 26]" 7]
            assert_lessthan [expr {[clock milliseconds] - $t0}] 2000
            assert_equal "[string repeat "\{\"a\":" 26]7[string repeat "\}" 26]" [r json.get u]
            assert_equal {[8]} [r json.numincrby u "\$[string repeat {['a','a']} 26]" 1]
            assert_equal {[8]} [r json.get u "\$[string repeat .a 26]"]
            assert_equal 1 [r json.del w "\$[string repeat {[0,0]} 24]"]
            assert_equal [json_deep_array 24] [r json.get w]
            assert_equal 1 [r json.arrappend w "\$[string repeat {[0,0]} 23]" 5]
            assert_equal 1 [r json.clear w "\$[string repeat {[0,0]} 23]"]
            assert_equal [json_deep_array 24] [r json.get w]
            assert_equal 1 [r json.del u "\$[string repeat {['a','a']} 26]"]
            assert_equal "[string repeat "\{\"a\":" 25]{}[string repeat "\}" 25]" [r json.get u]
        }
    }
    json_set_caps {4096 128}

    # ---------------------------------------------------------------------
    # Replies that carry client data
    # ---------------------------------------------------------------------

    test {JSON.DEBUG MAX-SIZE-KEY and MAX-DEPTH-KEY reply a key name with CR LF in it as one bulk string} {
        r flushall
        set evil "evil\r\n+INJECTED\r\n:1"
        assert_equal OK [r json.set $evil . {{"a":{"b":{"c":[1,2,3]}}}}]
        assert_equal OK [r json.set plain . 1]
        set memory [r json.debug memory $evil]
        assert_equal [list $memory $evil] [r json.debug max-size-key]
        assert_equal PONG [r ping]
        assert_equal [list 4 $evil] [r json.debug max-depth-key]
        assert_equal PONG [r ping]
        # The reply is an array of an integer and a bulk string of the key's length.
        r readraw 1
        assert_equal {*2} [r json.debug max-depth-key]
        assert_equal {:4} [r read]
        assert_equal "\$[string length $evil]" [r read]
        assert_equal "evil" [r read]
        assert_equal "+INJECTED" [r read]
        assert_equal ":1" [r read]
        r readraw 0
        assert_equal PONG [r ping]
        # Other bytes a simple string could not carry.
        foreach name [list "nul\x00byte" "new\nline" "-ERR fake" "\r\n\r\n" [json_hex fffe] " "] {
            r flushall
            r json.set $name . {[[1]]}
            assert_equal [list 2 $name] [r json.debug max-depth-key]
            assert_equal $name [lindex [r json.debug max-size-key] 1]
            assert_equal PONG [r ping]
        }
        r flushall
    } {OK}

    test {JSON.GET: formatting arguments that would make a reply over 512 MB are refused before the reply is built} {
        r flushall
        set mb [string repeat " " 1048576]
        set items {}
        for {set i 0} {$i < 600} {incr i} { lappend items $i }
        r json.set arr . "\[[join $items ,]\]"
        set members {}
        for {set i 0} {$i < 600} {incr i} { lappend members "\"k$i\":$i" }
        r json.set obj . "{[join $members ,]}"
        r json.set small . {{"a":[1,2,3],"b":{"c":[true,null]}}}
        set limit "LIMIT Formatted JSON reply size limit is exceeded"
        set peak [s used_memory_peak]
        # 600 elements with a megabyte before, after or inside each: over 600 MB.
        json_assert_fails_fast $limit 2000 json.get arr INDENT $mb
        json_assert_fails_fast $limit 2000 json.get arr NEWLINE $mb
        json_assert_fails_fast $limit 2000 json.get arr . NEWLINE $mb
        json_assert_fails_fast $limit 2000 json.get arr $ NEWLINE $mb
        json_assert_fails_fast $limit 2000 json.get obj SPACE $mb
        json_assert_fails_fast $limit 2000 json.get obj INDENT $mb NEWLINE $mb SPACE $mb
        json_assert_fails_fast $limit 2000 json.get obj . INDENT $mb
        json_assert_fails_fast $limit 2000 json.get arr {$[0:590]} NEWLINE $mb
        json_assert_fails_fast $limit 2000 json.get arr . {$[0]} INDENT $mb
        # The server did not build the reply first.
        assert_lessthan [expr {[s used_memory_peak] - $peak}] [expr {128 * 1024 * 1024}]
        assert_equal PONG [r ping]
        # Large formatting arguments whose reply stays below the bound still work.
        set reply [r json.get small INDENT $mb]
        assert_equal [expr {20 * 1048576 + [string length [r json.get small INDENT "" NEWLINE "" SPACE ""]]}] [string length $reply]
        assert_equal 3145763 [string length [r json.get small SPACE $mb]]
        assert_equal [expr {8 * 1048576 + 23}] [string length [r json.get obj {$.k1} {$.k2} INDENT $mb]]
        r flushall
    } {OK}
}
