# JSON numbers: number text, JSON.TYPE integer vs number, JSON.TOGGLE, JSON.NUMINCRBY and
# JSON.NUMMULTBY.
#
# Ported from the valkey-json module's tst/integration/test_json_basic.py (number scanner,
# toggle, numincrby, nummultby and the document-memory tests), with the module's expected values
# except where the built-in module deliberately differs. Those tests carry the number of the
# change in their name:
#   B2  a number too large for a double is rejected from an ordinary client
#   B3  computed numbers print in shortest form, a whole double result keeps its fraction and
#       its type, an integer result is exact
#   B6  a multi-match write that fails on one match changes nothing

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
proc json_num_fixture {} {
    r flushall
    r json.set wikipedia . $::json_wikipedia
    r set str_key {{"firstName":"John","lastName":"Smith"}}
}

# A random normal double (no inf, nan or subnormal), as Tcl's shortest round-trip text.
proc json_random_double {} {
    while 1 {
        set hi [expr {int(rand() * 4294967296)}]
        set lo [expr {int(rand() * 4294967296)}]
        set e [expr {($hi >> 20) & 0x7ff}]
        if {$e == 0 || $e == 0x7ff} continue
        binary scan [binary format II $hi $lo] Q d
        return $d
    }
}

# 1 if a number text with one significant digit fewer than `txt` reads back as the double
# `val`, that is, if `txt` is not the shortest text of `val`. The candidates are `val` rounded
# to that many digits and its two neighbours one unit in the last place away. Nothing here
# depends on how Tcl prints a double.
proc json_shorter_text_exists {txt val} {
    set n [string length [json_sig_digits $txt]]
    if {$n < 2} { return 0 }
    if {![regexp {^(-?)([0-9])\.?([0-9]*)e([-+][0-9]+)$} [format %.[expr {$n - 2}]e $val] -> sign first rest exp]} {
        error "unexpected format of $val"
    }
    set mant "$first$rest"
    scan $exp %d exp10
    set shift [expr {$exp10 - ($n - 2)}]
    foreach m [list [expr {$mant - 1}] $mant [expr {$mant + 1}]] {
        set candidate "$sign${m}e$shift"
        if {[expr {double($candidate) == $val}]} { return 1 }
    }
    return 0
}

# The significant digits of a number text, without sign, point, exponent or padding zeros.
proc json_sig_digits {txt} {
    if {![regexp {^-?([0-9]+(?:\.[0-9]+)?)(?:[eE][-+]?[0-9]+)?$} $txt -> mant]} {
        error "not a number text: $txt"
    }
    set mant [string map {. {}} $mant]
    return [string trimright [string trimleft $mant 0] 0]
}

# A random valid JSON number token: any sign, 1 to 26 integer digits, an optional fraction of up
# to 22 digits and an optional exponent in any spelling. Always within the range of a double.
proc json_random_digits {n} {
    set s ""
    for {set i 0} {$i < $n} {incr i} { append s [expr {int(rand() * 10)}] }
    return $s
}
proc json_random_number_token {} {
    set s ""
    if {int(rand() * 3) == 0} { append s - }
    if {int(rand() * 4) == 0} {
        append s 0
    } else {
        append s [expr {1 + int(rand() * 9)}][json_random_digits [expr {int(rand() * [lindex {1 3 6 10 19 26} [expr {int(rand() * 6)}]])}]]
    }
    set style [expr {int(rand() * 6)}]
    if {$style >= 2} {
        append s .[json_random_digits [expr {1 + int(rand() * [lindex {1 2 3 6 9 17 22} [expr {int(rand() * 7)}]])}]]
    }
    if {$style == 1 || $style == 5} {
        append s [lindex {e E} [expr {int(rand() * 2)}]][lindex {{} + -} [expr {int(rand() * 3)}]]
        if {int(rand() * 3) == 0} { append s 0 }
        append s [expr {int(rand() * [lindex {3 10 30 250} [expr {int(rand() * 4)}]])}]
    }
    return $s
}

# A random integer of any magnitude within int64.
proc json_random_int64 {} {
    set bits [expr {1 + int(rand() * 63)}]
    set v [expr {int(rand() * (1 << $bits))}]
    if {$bits > 52} {
        set v [expr {($v << 10) | int(rand() * 1024)}]
        if {$v > 9223372036854775807} { set v 9223372036854775807 }
    }
    if {int(rand() * 2)} { set v [expr {-$v}] }
    return $v
}

start_server {tags {"json"}} {
    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        # ---------------------------------------------------------------- number text and type

        test "JSON number scanner: int64 range is integer, beyond it is number (caps $caps)" {
            foreach {v type} {
                9223372036854775807   integer
                -9223372036854775807  integer
                -9223372036854775808  integer
                9223372036854775808   number
                -9223372036854775809  number
            } {
                r json.set k1 . $v
                assert_equal $type [r json.type k1 .] "value $v"
                assert_equal $v [r json.get k1 .] "value $v"
            }
        }

        test "JSON number text is kept exactly at the root, in an object and in an array (caps $caps)" {
            foreach v {
                0 1 -1 7 -128 32767 65536 2147483648 123456789012345678
                9223372036854775807 -9223372036854775808
                9223372036854775808 -9223372036854775809 18446744073709551615 18446744073709551616
                1234567890123456789012345 -1234567890123456789012345
                0.0 -0.0 0.1 98.6 1.0 135.17 100000.0 0.000001 0.30000000000000004
                19.90 0.10 1.50 2.5000 123456789.123456789123 3.14159265358979323846
                1e5 1E5 1e+5 1E+5 1.0E3 1e-07 1e-7 1E-2 1.5e+21 1e21 1e22 2e30 2E-30
                0e0 0E+0 -0e-0 0.0e0
                5e-324 4.9406564584124654e-324 2.2250738585072014e-308
                1.7976931348623157e308 1.7976931348623157e+308 -1.7976931348623157E308 1.79e308
            } {
                r json.set k . $v
                assert_equal $v [r json.get k] "root $v"
                assert_equal "\[$v\]" [r json.get k $] "root $v"
                assert_equal $v [r json.get k INDENT "\t" NEWLINE "\n" SPACE " " .] "root $v"

                r json.set k . "{\"a\":$v,\"b\":{\"c\":$v}}"
                assert_equal "{\"a\":$v,\"b\":{\"c\":$v}}" [r json.get k] "object $v"
                assert_equal $v [r json.get k .b.c] "object $v"
                assert_equal "\[$v\]" [r json.get k $.a] "object $v"

                r json.set k . "\[$v, \"s\", $v,$v \]"
                assert_equal "\[$v,\"s\",$v,$v\]" [r json.get k] "array $v"
                assert_equal $v [r json.get k {[2]}] "array $v"

                # an array of numbers only (a typed array, where the engine has one)
                r json.set k . "\[$v,$v,$v\]"
                assert_equal "\[$v,$v,$v\]" [r json.get k] "number array $v"
                assert_equal "\[$v\]" [r json.get k {$[1]}] "number array $v"

                # written through a sub-path
                r json.set k . {{"a":null,"b":[null]}}
                r json.set k .a $v
                r json.set k {$.b[0]} $v
                r json.arrappend k .b $v
                assert_equal "{\"a\":$v,\"b\":\[$v,$v\]}" [r json.get k] "sub-path $v"
            }
        }

        test "JSON -0 in integer syntax is integer 0, as in the module (caps $caps)" {
            r json.set k . -0
            assert_equal 0 [r json.get k]
            assert_equal integer [r json.type k]
            r json.set k . {{"a":-0,"b":[-0,1,-0]}}
            assert_equal {{"a":0,"b":[0,1,0]}} [r json.get k]
            assert_equal {integer integer integer} [r json.type k {$.b[*]}]
            # with a fraction or an exponent it is a double and keeps its text
            foreach v {-0.0 -0e0 -0.00 -0E-5} {
                r json.set k . $v
                assert_equal $v [r json.get k]
                assert_equal number [r json.type k]
            }
        }

        test "JSON.TYPE is integer for integer syntax within int64 and number otherwise (caps $caps)" {
            foreach {v type} {
                0                           integer
                -0                          integer
                1                           integer
                -1                          integer
                123456789012345678          integer
                9223372036854775807         integer
                -9223372036854775808        integer
                9223372036854775808         number
                -9223372036854775809        number
                18446744073709551616        number
                1234567890123456789012345   number
                0.0                         number
                -0.0                        number
                1.0                         number
                0.1                         number
                19.90                       number
                1e5                         number
                1E5                         number
                1e0                         number
                1.0E3                       number
                1e-07                       number
                5e-324                      number
                1.7976931348623157e308      number
            } {
                r json.set k . $v
                assert_equal $type [r json.type k] "root $v"
                assert_equal $type [r json.type k $] "root $v"
                r json.set k . "{\"a\":$v,\"b\":\[1,$v\],\"c\":\[$v,$v\]}"
                assert_equal $type [r json.type k .a] "member $v"
                assert_equal [list $type] [r json.type k {$.b[1]}] "element $v"
                assert_equal [list $type $type] [r json.type k {$.c[*]}] "number array $v"
            }
        }

        test "JSON arrays of numbers keep every element's text (caps $caps)" {
            foreach doc {
                {[1,2,3]}
                {[-1,0,255,65536,-2147483649,9223372036854775807]}
                {[1.5,2.5,-0.25]}
                {[1.50,2.25,3.00]}
                {[0.100000,0.250000,7.000000,-0.000001]}
                {[1,2.5,3]}
                {[1,2.50,3]}
                {[1.5,2.50,3.125]}
                {[1e5,2,3.0]}
                {[1.0,2.0,3.0]}
                {[0.1,1e-7,1.5e+21,5e-324]}
                {[9223372036854775808,1,18446744073709551616]}
                {[1234567890123456789012345,0.5]}
                {[0,-0.0,0.0]}
                {[[1.50,2.25],[1,2],[1e5],[]]}
                {{"v":[0.123456,0.654321,1.000000],"w":[19.90,20.00]}}
            } {
                r json.set k . $doc
                assert_equal $doc [r json.get k]
                assert_equal "\[$doc\]" [r json.get k $]
            }

            # longer than the default packed caps
            foreach {fmt n} {%d 300 %.6f 300 %.2f 2000 %.17g 300 %e 300} {
                set items {}
                for {set i 0} {$i < $n} {incr i} { lappend items [format $fmt [expr {$i * 7 - 300}]] }
                if {$fmt ne "%d"} {
                    set items {}
                    for {set i 0} {$i < $n} {incr i} { lappend items [format $fmt [expr {($i * 7 - 300) / 13.0}]] }
                }
                set doc "\[[join $items ,]\]"
                r json.set k . $doc
                assert_equal $doc [r json.get k] "format $fmt"
                assert_equal [lindex $items end] [r json.get k {[-1]}] "format $fmt"
                assert_equal $n [r json.arrlen k] "format $fmt"
            }
        }

        test "JSON random number tokens of every style keep their text in arrays (caps $caps)" {
            # Arrays of unrelated tokens, and arrays whose decimals all have the same number of
            # fraction digits (one scale, design 5.4), with an integer mixed in.
            r flushall
            expr {srand(777)}
            for {set i 0} {$i < 500} {incr i} {
                set toks {}
                set count [expr {1 + int(rand() * 8)}]
                if {$i % 2} {
                    set k [expr {1 + int(rand() * 8)}]
                    for {set j 0} {$j < $count} {incr j} {
                        lappend toks "[lindex {{} {} -} [expr {int(rand() * 3)}]][expr {int(rand() * 100000)}].[json_random_digits $k]"
                    }
                    if {int(rand() * 4) == 0} { lappend toks [expr {int(rand() * 1000)}] }
                } else {
                    for {set j 0} {$j < $count} {incr j} {
                        set t [json_random_number_token]
                        # -0 alone is the one token that does not keep its text
                        if {$t eq "-0"} { set t 0 }
                        lappend toks $t
                    }
                }
                set doc "\[[join $toks ,]\]"
                assert_equal OK [r json.set k . $doc]
                assert_equal $doc [r json.get k]
                # append one more token, read it back, pop it
                set t [json_random_number_token]
                if {$t eq "-0"} { set t 0 }
                assert_equal [expr {[llength $toks] + 1}] [r json.arrappend k . $t]
                assert_equal $t [r json.get k {[-1]}]
                assert_equal $t [r json.arrpop k]
                assert_equal $doc [r json.get k]
                # the identity operation keeps the value of any element
                set idx [expr {int(rand() * [llength $toks])}]
                set t [lindex $toks $idx]
                set res [string range [r json.numincrby k "\$\[$idx\]" 0] 1 end-1]
                if {[string is wideinteger -strict $t]} {
                    assert_equal $t $res
                } else {
                    assert {double($res) == double($t)}
                }
            }
        }

        test "JSON double consistency: random doubles keep their text, plain and pretty-printed (caps $caps)" {
            # Port of test_json_double_consistency. The module keeps a double as its source
            # text, and so does the built-in module (design D2).
            json_num_fixture
            expr {srand(1234)}
            for {set i 0} {$i < 2000} {incr i} {
                set val [json_random_double]
                assert_equal OK [r json.set wikipedia .foo $val]
                set v0 [r json.get wikipedia .foo]
                set vp [r json.get wikipedia INDENT "\t" .foo]
                assert_equal $val $v0
                assert_equal $v0 $vp
            }
        }

        test "JSON.NUMINCRBY 0 and NUMMULTBY 1 on random doubles keep the value and print its shortest digits - B3 (caps $caps)" {
            # The expected digits are not taken from Tcl, whose conversion of a double to text
            # differs between Tcl versions. The reply must read back as the same double, have
            # at most 17 significant digits, and no text with one digit fewer may read back
            # as that double.
            r flushall
            expr {srand(4321)}
            for {set i 0} {$i < 1000} {incr i} {
                set val [json_random_double]
                foreach {cmd operand} {json.numincrby 0 json.nummultby 1} {
                    r json.set k . $val
                    set res [r $cmd k . $operand]
                    assert {$res == $val}
                    assert_lessthan_equal [string length [json_sig_digits $res]] 17
                    assert_equal 0 [json_shorter_text_exists $res $val] "$cmd on $val replied $res, which is not its shortest text"
                    assert_equal $res [r json.get k] "$cmd on $val"
                    assert_equal number [r json.type k]
                }
            }
        }

        test "JSON.NUMINCRBY 0 and NUMMULTBY 1 print the canonical text of a double, from a fixed table - B3 (caps $caps)" {
            # Each input is a double written with 17 significant digits. The expected text is
            # its shortest round-trip digits in the canonical form of design 5.4: ".0" on a
            # whole value below 1e21, plain decimal from 1e-6, exponent form elsewhere. The
            # table was computed outside Tcl (Python's repr gives the shortest digits).
            r flushall
            foreach {input expected} {
                3.0000000000000004e-01     0.30000000000000004
                3.3333333333333331e-01     0.3333333333333333
                6.6666666666666663e-01     0.6666666666666666
                1.0000000000000000e+21     1e+21
                9.9999999999999995e-08     1e-7
                1.2345678901234568e+20     123456789012345680000.0
                4.9406564584124654e-324    5e-324
                1.7976931348623157e+308    1.7976931348623157e+308
                2.2250738585072014e-308    2.2250738585072014e-308
                4.3499999999999996e+00     4.35
                9.9999999999999995e-07     0.000001
                1.2345678900000000e+05     123456.789
                1.0000000000000000e+16     10000000000000000.0
                1.5000000000000000e+21     1.5e+21
                9.9999999999999987e+20     999999999999999900000.0
                1.2340000000000000e-06     0.000001234
                1.4874529493275051e+154    1.487452949327505e+154
                9.0071992547409920e+15     9007199254740992.0
                1.0000000000000000e+22     1e+22
                9.9999999999999992e+22     1e+23
                4.9999999999999998e-07     5e-7
                1.0000000000000000e+02     100.0
                5.0000000000000000e-01     0.5
                2.5000000000000001e-05     0.000025
                1.0000000000000002e+00     1.0000000000000002
                4.9406564584124654e-324    5e-324
                1.2345678912345678e+08     123456789.12345678
                1.0000000000000001e-01     0.1
                1.0000000000000000e+100    1e+100
                7.2057594037927952e+16     72057594037927950.0
                1.2786223913500820e-11     1.278622391350082e-11
                -1.8852257052241999e-201   -1.8852257052242e-201
                1.8495478477068476e+289    1.8495478477068476e+289
                -1.4788019135862963e+18    -1478801913586296300.0
                1.3056383685098237e+305    1.3056383685098237e+305
                4.1718831205389047e+231    4.171883120538905e+231
                -9.4823930634916865e+116   -9.482393063491686e+116
                1.2809890279307679e+222    1.280989027930768e+222
                -1.2765049184880330e-12    -1.276504918488033e-12
                5.8461476920047315e-276    5.8461476920047315e-276
                5.8056848214973560e+210    5.805684821497356e+210
                -2.1077957057380303e-168   -2.1077957057380303e-168
                -2.9200027332747816e+25    -2.9200027332747816e+25
                2.0918418470616806e+159    2.0918418470616806e+159
                -6.2460380361735023e-67    -6.246038036173502e-67
                5.4451140806643961e-37     5.445114080664396e-37
                1.4908300809895834e+253    1.4908300809895834e+253
                -3.4187785053545495e-118   -3.4187785053545495e-118
                -1.0183259594603819e-280   -1.0183259594603819e-280
                1.9365388359613437e-48     1.9365388359613437e-48
                1.1410569973172104e+145    1.1410569973172104e+145
                -3.5832130199436562e+162   -3.5832130199436562e+162
                2.7633651739172474e-70     2.7633651739172474e-70
                -1.2852146246136004e+274   -1.2852146246136004e+274
                8.7163049983658503e+50     8.71630499836585e+50
                -5.3147156478195631e+264   -5.314715647819563e+264
                1.1845104377311450e-154    1.184510437731145e-154
                4.0098923482054111e-81     4.009892348205411e-81
                -3.9421803694997105e-230   -3.9421803694997105e-230
                -1.4986450388393537e+193   -1.4986450388393537e+193
                -3.0000000000000004e-01    -0.30000000000000004
                -3.3333333333333331e-01    -0.3333333333333333
                -6.6666666666666663e-01    -0.6666666666666666
                -1.0000000000000000e+21    -1e+21
                -9.9999999999999995e-08    -1e-7
                -1.2345678901234568e+20    -123456789012345680000.0
                -4.9406564584124654e-324   -5e-324
                -1.7976931348623157e+308   -1.7976931348623157e+308
                -2.2250738585072014e-308   -2.2250738585072014e-308
                -4.3499999999999996e+00    -4.35
                -9.9999999999999995e-07    -0.000001
                -1.2345678900000000e+05    -123456.789
            } {
                foreach {cmd operand} {json.numincrby 0 json.nummultby 1} {
                    assert_equal OK [r json.set k . $input]
                    assert_equal $input [r json.get k]
                    assert_equal $expected [r $cmd k . $operand] "$cmd on $input"
                    assert_equal $expected [r json.get k] "$cmd on $input"
                    assert_equal number [r json.type k]
                    assert_equal "\[$expected\]" [r $cmd k $ $operand] "$cmd on $expected"
                    assert_equal $expected [r json.get k]
                }
            }
        }

        # ---------------------------------------------------------------- B2

        test "JSON.SET rejects a number too large for a double - B2 (caps $caps)" {
            r flushall
            foreach v {1.8e308 -1.8e308 1e309 1e400 -1e999 1.7976931348623159e308 17976931348623159e292
                       {[1.8e308]} {[1,2,1.8e308]} {[1.5,-1e400]} {{"a":1.8e308}} {{"a":{"b":[0,{"c":-1e309}]}}}} {
                assert_error "SYNTAXERR*" {r json.set k $ $v}
                assert_error "SYNTAXERR*" {r json.set k . $v}
                assert_equal 0 [r exists k]
            }

            # nothing is changed in an existing document either
            r json.set k . {{"a":1,"b":[1,2]}}
            assert_error "SYNTAXERR*" {r json.set k . 1.8e308}
            assert_error "SYNTAXERR*" {r json.set k $.a 1.8e308}
            assert_error "SYNTAXERR*" {r json.set k $.new {[1e309]}}
            assert_error "SYNTAXERR*" {r json.arrappend k .b 1 1e309}
            assert_error "SYNTAXERR*" {r json.arrinsert k .b 0 -1e309}
            assert_error "SYNTAXERR*" {r json.arrindex k .b 1e309}
            assert_equal {{"a":1,"b":[1,2]}} [r json.get k]

            # the largest doubles are still accepted
            foreach v {1.7976931348623157e308 -1.7976931348623157e308 17976931348623157e292 1.79e308} {
                assert_equal OK [r json.set k $ $v]
                assert_equal $v [r json.get k]
            }
            assert_equal PONG [r ping]
        }

        test "JSON.NUMINCRBY and NUMMULTBY reject an operand that is not a finite number - B2 (caps $caps)" {
            r json.set k . 1
            foreach v {1.8e308 -1.8e308 1e309 1e400 nan inf -inf Infinity NaN} {
                assert_error "WRONGTYPE Value is not a number" {r json.numincrby k . $v}
                assert_error "WRONGTYPE Value is not a number" {r json.nummultby k $ $v}
            }
            assert_equal 1 [r json.get k]
        }

        test "JSON.SET k \$ 1.8e308 then JSON.NUMINCRBY k \$ 1 does not crash the server - B2 (caps $caps)" {
            # This pair crashes the external module (valkey-json issue 105).
            r flushall
            assert_error "SYNTAXERR*" {r json.set k $ 1.8e308}
            assert_error "NONEXISTENT*" {r json.numincrby k $ 1}
            assert_equal PONG [r ping]
            assert_equal 0 [r exists k]
        }

        test "JSON number out of double range arriving through RESTORE is kept as text and arithmetic on it is an error - B2 (caps $caps)" {
            # A trusted path may not reject anything (design 5.4, Range). Build the payload from
            # the DUMP of a number of the same length.
            r flushall
            r json.set src $ 1.7e308
            set payload [string map {1.7e308 1.8e308} [r dump src]]
            r debug set-skip-checksum-validation 1
            set failed [catch {r restore k 0 $payload} res]
            r debug set-skip-checksum-validation 0
            assert_equal {0 OK} [list $failed $res]
            assert_equal 1.8e308 [r json.get k]
            assert_equal {[1.8e308]} [r json.get k $]
            assert_equal number [r json.type k]
            assert_error "OVERFLOW*" {r json.numincrby k $ 1}
            assert_error "OVERFLOW*" {r json.numincrby k . 1}
            assert_error "OVERFLOW*" {r json.numincrby k . -1e308}
            assert_error "OVERFLOW*" {r json.nummultby k $ 1}
            assert_error "OVERFLOW*" {r json.nummultby k . 0.5}
            assert_equal 1.8e308 [r json.get k]
            # reading it in other ways must not crash
            r json.resp k
            r json.debug memory k
            r debug digest-value k
            r dump k
            assert_equal PONG [r ping]
            assert_equal 1 [r json.clear k]
            assert_equal number [r json.type k]
            assert_equal 1 [r del k]
        } {} {needs:debug}

        test "JSON.RESP of the smallest subnormal does not hang the server (caps $caps)" {
            # [5e-324] followed by JSON.RESP hangs the external module.
            r flushall
            assert_equal OK [r json.set k $ {[5e-324]}]
            assert_equal {{[} 5e-324} [r json.resp k]
            assert_equal {{{[} 5e-324}} [r json.resp k $]
            assert_equal {[5e-324]} [r json.get k]
            assert_equal {number} [r json.type k {$[0]}]
            r json.set k $ {{"a":5e-324,"b":[4.9406564584124654e-324,-5e-324,2.2250738585072014e-308]}}
            assert_equal 5e-324 [r json.resp k .a]
            assert_equal 4 [llength [r json.resp k .b]]
            assert_equal {{"a":5e-324,"b":[4.9406564584124654e-324,-5e-324,2.2250738585072014e-308]}} [r json.get k]
            assert_equal PONG [r ping]
        }

        # ---------------------------------------------------------------- JSON.TOGGLE

        test "JSON.TOGGLE legacy path (caps $caps)" {
            json_num_fixture
            assert_equal OK [r json.set wikipedia .foobool false]
            assert_equal false [r json.get wikipedia .foobool]
            assert_equal true [r json.toggle wikipedia .foobool]
            assert_equal true [r json.get wikipedia .foobool]
            assert_equal false [r json.toggle wikipedia .foobool]
            assert_equal false [r json.get wikipedia .foobool]
            assert_equal {"false"} [json_raw json.toggle wikipedia .isAlive]
            assert_equal {"true"} [json_raw json.toggle wikipedia .isAlive]

            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.toggle wikipedia .foobool extra}
            assert_error "*wrong number of arguments*" {r json.toggle}

            # wrong types
            assert_equal OK [r json.set wikipedia .foonum 55]
            assert_error "WRONGTYPE JSON element is not a bool" {r json.toggle wikipedia .foonum}
            assert_equal OK [r json.set wikipedia .foostr {"ok"}]
            assert_error "WRONGTYPE JSON element is not a bool" {r json.toggle wikipedia .foostr}
            foreach path {.address .children .spouse .weight .} {
                assert_error "WRONGTYPE JSON element is not a bool" {r json.toggle wikipedia $path}
            }
            # the path defaults to the root
            assert_error "WRONGTYPE JSON element is not a bool" {r json.toggle wikipedia}

            # missing path, missing key, key of another type
            assert_error "NONEXISTENT JSON path does not exist" {r json.toggle wikipedia .nope}
            assert_error "NONEXISTENT Document key does not exist" {r json.toggle nokey .a}
            assert_error "NONEXISTENT Document key does not exist" {r json.toggle nokey $.a}
            assert_error "NONEXISTENT Document key does not exist" {r json.toggle nokey}
            assert_error "WRONGTYPE Not a JSON document key" {r json.toggle str_key .a}
            assert_error "WRONGTYPE Not a JSON document key" {r json.toggle str_key $.a}

            # a boolean root
            r json.set b . true
            assert_equal false [r json.toggle b]
            assert_equal true [r json.toggle b .]
            assert_equal {0} [r json.toggle b $]
            assert_equal false [r json.get b]
        }

        test "JSON.TOGGLE \$ path and wildcards (caps $caps)" {
            r flushall
            assert_equal OK [r json.set k1 . {{"a":true, "b":false, "c":1, "d":null, "e":"foo", "f":[], "g":{}}}]
            assert_equal OK [r json.set k2 . {[true, false, 1, null, "foo", [], {}]}]
            foreach {key path exp exp_new_val} {
                k1 $.*   {0 1 {} {} {} {} {}}  {{"a":false,"b":true,"c":1,"d":null,"e":"foo","f":[],"g":{}}}
                k1 $.*   {1 0 {} {} {} {} {}}  {{"a":true,"b":false,"c":1,"d":null,"e":"foo","f":[],"g":{}}}
                k2 $[*]  {0 1 {} {} {} {} {}}  {[false,true,1,null,"foo",[],{}]}
                k2 $[*]  {1 0 {} {} {} {} {}}  {[true,false,1,null,"foo",[],{}]}
            } {
                assert_equal $exp [r json.toggle $key $path]
                assert_equal $exp_new_val [r json.get $key .]
            }
            # reply types: integers, and a null for a value that is not a boolean
            assert_equal {[(0) (1) nil nil nil nil nil]} [json_raw json.toggle k1 $.*]
            assert_equal {[(1) (0) nil nil nil nil nil]} [json_raw json.toggle k1 $.*]
            assert_equal {[]} [json_raw json.toggle k1 $.nope]
            assert_equal {[nil]} [json_raw json.toggle k1 $.c]
            assert_equal {[nil]} [json_raw json.toggle k1 $]

            # a legacy wildcard toggles every boolean and replies with the first new value
            assert_equal {"false"} [json_raw json.toggle k1 .*]
            assert_equal {{"a":false,"b":true,"c":1,"d":null,"e":"foo","f":[],"g":{}}} [r json.get k1]
            assert_error "WRONGTYPE JSON element is not a bool" {r json.toggle k1 .f}
            assert_equal {{"a":false,"b":true,"c":1,"d":null,"e":"foo","f":[],"g":{}}} [r json.get k1]

            # recursive descent, nested booleans
            r json.set k3 . {{"a":true,"b":{"a":false,"c":[true,{"a":true}]}}}
            assert_equal {0 1 0} [r json.toggle k3 $..a]
            assert_equal {{"a":false,"b":{"a":true,"c":[true,{"a":false}]}}} [r json.get k3]
        }

        test "JSON.TOGGLE RESP3 (caps $caps)" {
            r flushall
            r json.set k1 . {{"a":true, "b":false, "c":1, "d":null}}
            r hello 3
            assert_equal {[(0) (1) null null]} [json_raw json.toggle k1 $.*]
            assert_equal {[]} [json_raw json.toggle k1 $.nope]
            assert_equal {"true"} [json_raw json.toggle k1 .a]
            assert_equal {-NONEXISTENT Document key does not exist} [json_raw json.toggle nokey .a]
            r hello 2
            assert_equal {{"a":true,"b":true,"c":1,"d":null}} [r json.get k1]
        }

        # ---------------------------------------------------------------- NUMINCRBY / NUMMULTBY

        test "JSON.NUMINCRBY legacy path - B3 (caps $caps)" {
            json_num_fixture
            assert_equal 28 [r json.numincrby wikipedia .age 1]
            assert_equal 38 [r json.numincrby wikipedia .age 10]
            assert_equal 33 [r json.numincrby wikipedia .age -5]
            assert_equal integer [r json.type wikipedia .age]

            assert_equal OK [r json.set wikipedia .foo 1]
            assert_equal 1.5 [r json.numincrby wikipedia .foo 0.5]
            # B3: a whole double result keeps its fraction (the module replies 2)
            assert_equal 2.0 [r json.numincrby wikipedia .foo 0.5]
            assert_equal 2.0 [r json.get wikipedia .foo]
            assert_equal number [r json.type wikipedia .foo]
            assert_equal 135.67 [r json.numincrby wikipedia .weight 0.5]

            # error condition: document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.numincrby foo .age 2}
            assert_error "NONEXISTENT Document key does not exist" {r json.numincrby foo $.age 2}
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.numincrby wikipedia .bar 2}
            # error condition: not a JSON key
            assert_error "WRONGTYPE Not a JSON document key" {r json.numincrby str_key .age 2}
            assert_error "WRONGTYPE Not a JSON document key" {r json.numincrby str_key $.age 2}
            # error condition: the value is not a number
            foreach path {.firstName .isAlive .address .children .spouse .} {
                assert_error "WRONGTYPE JSON element is not a number" {r json.numincrby wikipedia $path 2}
            }
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.numincrby wikipedia .age 1 extra}
            assert_error "*wrong number of arguments*" {r json.numincrby wikipedia .age}
            assert_error "*wrong number of arguments*" {r json.numincrby wikipedia}
            assert_equal 33 [r json.get wikipedia .age]
        }

        test "JSON.NUMMULTBY legacy path - B3 (caps $caps)" {
            json_num_fixture
            assert_equal 270 [r json.nummultby wikipedia .age 10]
            assert_equal 2700 [r json.nummultby wikipedia .age 10]
            # B3: a double result stays a double (the module replies 27 and turns it into an integer)
            assert_equal 27.0 [r json.nummultby wikipedia .age 0.01]
            assert_equal number [r json.type wikipedia .age]

            assert_equal OK [r json.set wikipedia .foo 1]
            assert_equal 0.5 [r json.nummultby wikipedia .foo 0.5]
            assert_equal 0.25 [r json.nummultby wikipedia .foo 0.5]
            assert_equal 1.0 [r json.nummultby wikipedia .foo 4]
            assert_equal 1.0 [r json.get wikipedia .foo]
            assert_equal number [r json.type wikipedia .foo]

            # error condition: document key does not exist
            assert_error "NONEXISTENT Document key does not exist" {r json.nummultby foo .age 2}
            assert_error "NONEXISTENT Document key does not exist" {r json.nummultby foo $.age 2}
            # error condition: path does not exist
            assert_error "NONEXISTENT JSON path does not exist" {r json.nummultby wikipedia .bar 2}
            # error condition: not a JSON key
            assert_error "WRONGTYPE Not a JSON document key" {r json.nummultby str_key .age 2}
            assert_error "WRONGTYPE Not a JSON document key" {r json.nummultby str_key $.age 2}
            # error condition: the value is not a number
            foreach path {.firstName .isAlive .address .children .spouse .} {
                assert_error "WRONGTYPE JSON element is not a number" {r json.nummultby wikipedia $path 2}
            }
            # wrong number of arguments
            assert_error "*wrong number of arguments*" {r json.nummultby wikipedia .age 2 extra}
            assert_error "*wrong number of arguments*" {r json.nummultby wikipedia .age}
            assert_error "*wrong number of arguments*" {r json.nummultby wikipedia}
        }

        test "JSON.NUMMULTBY 1 and NUMINCRBY 0 print the canonical shortest form - B3 (caps $caps)" {
            # Port of test_json_simple_operations_remove_double_styling. The module prints
            # %.17g ("2.0000000000000002e-30") and drops ".0" ("200000"); B3 prints the
            # shortest digits and keeps a whole double a double.
            json_num_fixture
            foreach {initial unstyled} {
                2.50000   2.5
                2e30      2e+30
                2E+30     2e+30
                2E30      2e+30
                2E-30     2e-30
                2e5       200000.0
                -2.50000  -2.5
                -2e30     -2e+30
                -2E+30    -2e+30
                -2E30     -2e+30
                -2E-30    -2e-30
                -2e5      -200000.0
                19.90     19.9
                1e-07     1e-7
                1.0E3     1000.0
                0.1       0.1
                1e21      1e+21
                123456789012345680000  123456789012345680000.0
                0.000001  0.000001
                0.0000001 1e-7
                1234567890123456789012345  1.2345678901234568e+24
                9223372036854775808        9223372036854776000.0
            } {
                foreach {cmd operand} {json.nummultby 1 json.numincrby 0} {
                    assert_equal OK [r json.set wikipedia .foo $initial]
                    assert_equal $initial [r json.get wikipedia .foo]
                    assert_equal $unstyled [r $cmd wikipedia .foo $operand] "$cmd on $initial"
                    assert_equal $unstyled [r json.get wikipedia .foo] "$cmd on $initial"
                    assert_equal number [r json.type wikipedia .foo]
                }
            }
        }

        test "JSON.NUMINCRBY and NUMMULTBY reject an operand that is not a JSON number (caps $caps)" {
            # Port of test_json_double_operations_wrongtype.
            json_num_fixture
            foreach {cmd field arg} {
                json.nummultby .age       {"2.0"}
                json.nummultby .age       2.
                json.numincrby .age       2.0.
                json.nummultby .age       -2.
                json.numincrby .age       -2.0.
                json.nummultby .age       +2.
                json.numincrby .age       +2.0.
                json.numincrby .age       .2.0
                json.numincrby .age       a2.0
                json.numincrby .age       -a2.0
                json.numincrby .age       +a2.0
                json.numincrby .age       e2.0
                json.numincrby .age       -e2.0
                json.numincrby .age       +e2.0
                json.numincrby .age       e+2.0
                json.numincrby .age       -e-2.0
                json.numincrby .age       +E+2.0
                json.numincrby .age       2.0e
                json.numincrby .age       2.0eq
                json.numincrby .age       2.0e3q
                json.numincrby .age       2.0e+
                json.numincrby .age       2.0e+a
                json.numincrby .age       2.0e+41a
                json.numincrby .firstName 2
                json.numincrby .age       +1
                json.numincrby .age       01
                json.numincrby .age       0x10
                json.numincrby .age       {}
                json.numincrby .age       abc
                json.numincrby .age       true
                json.numincrby .age       null
                json.numincrby .age       {[1]}
                json.nummultby .age       {{"a":1}}
                json.nummultby .age       {1 2}
            } {
                assert_error "WRONGTYPE*" {r $cmd wikipedia $field $arg}
            }
            assert_equal 27 [r json.get wikipedia .age]
            assert_equal integer [r json.type wikipedia .age]
        }

        test "JSON.NUMINCRBY near the int64 and double boundaries - B3 (caps $caps)" {
            # Port of test_json_numincrby_large_numbers.
            json_num_fixture
            foreach val {9223372036854775807 -9223372036854775808} {
                assert_equal OK [r json.set wikipedia .foo $val]
                assert_equal $val [r json.get wikipedia .foo]
                assert_equal $val [r json.get wikipedia SPACE " " NEWLINE "\n" .foo]
            }

            # The module replies -9.2233720368547758e+18 or the exact integer depending on the
            # build; B3 makes an integer result exact and a double result shortest.
            foreach {val incr exp} {
                -9223372036854775808  0   -9223372036854775808
                -9223372036854775808  1   -9223372036854775807
                -9223372036854775808  2   -9223372036854775806
                9223372036854775807   0   9223372036854775807
                9223372036854775807   -1  9223372036854775806
                9223372036854775807   -2  9223372036854775805
                1.79e+308             0   1.79e+308
                1.79e+308             1   1.79e+308
                1.79e+308             -1  1.79e+308
                -1.79e+308            0   -1.79e+308
                -1.79e+308            1   -1.79e+308
                -1.79e+308            -1  -1.79e+308
                1.79e308              0   1.79e+308
                -1.79e308             1   -1.79e+308
            } {
                assert_equal OK [r json.set wikipedia .foo $val]
                assert_equal $val [r json.get wikipedia .foo]
                assert_equal $val [r json.get wikipedia INDENT "\t" .foo]
                assert_equal $exp [r json.numincrby wikipedia .foo $incr] "$val + $incr"
                assert_equal $exp [r json.get wikipedia .foo]
                assert_equal $exp [r json.get wikipedia INDENT "\t" .foo]
            }

            # Past int64 the result is a double. It is still a valid number.
            foreach {val incr exp} {
                9223372036854775807   1   9223372036854776000.0
                9223372036854775807   2   9223372036854776000.0
                -9223372036854775808  -1  -9223372036854776000.0
                -9223372036854775808  -2  -9223372036854776000.0
                5                     9223372036854775807  9223372036854776000.0
                5                     9223372036854775808  9223372036854776000.0
                9223372036854775808   -1  9223372036854776000.0
            } {
                assert_equal OK [r json.set wikipedia .foo $val]
                assert_equal $exp [r json.numincrby wikipedia .foo $incr] "$val + $incr"
                assert_equal $exp [r json.get wikipedia .foo]
                assert_equal number [r json.type wikipedia .foo]
            }
        }

        test "JSON.NUMMULTBY near the int64 and double boundaries - B3 (caps $caps)" {
            # Port of test_json_nummultby_large_numbers.
            json_num_fixture

            # multiplication overflows a double
            foreach {val mult} {
                9223372036854775807  1.79e+308
                9223372036854775807  -1.79e+308
                1.79e+308            1.79e+308
                1.79e308             1.79e+308
                1.03e+300            1.03e+300
                1.79e+308            -1.79e+308
                -1.03e+300           1.03e+300
            } {
                assert_equal OK [r json.set wikipedia .foo $val]
                assert_equal $val [r json.get wikipedia .foo]
                assert_equal $val [r json.get wikipedia SPACE " " NEWLINE "\n" .foo]
                assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby wikipedia .foo $mult}
                assert_equal $val [r json.get wikipedia .foo]
            }

            # the result is within the range of a double
            foreach {val mult exp} {
                9223372036854775807  0                     0
                9223372036854775807  2                     18446744073709552000.0
                9223372036854775807  9223372036854775807   8.507059173023462e+37
                9223372036854775807  -9223372036854775807  -8.507059173023462e+37
                1.79e308             0                     0.0
                1.79e308             1                     1.79e+308
                -1.79e308            1                     -1.79e+308
                1.79e+308            1                     1.79e+308
                9223372036854775807  1                     9223372036854775807
                -9223372036854775808 1                     -9223372036854775808
                9223372036854775807  -1                    -9223372036854775807
                -9223372036854775808 -1                    9223372036854776000.0
                4611686018427387904  2                     9223372036854776000.0
                -4611686018427387904 2                     -9223372036854775808
                3037000500           3037000500            9223372037000250000.0
                3037000499           3037000499            9223372030926249001
            } {
                assert_equal OK [r json.set wikipedia .foo $val]
                assert_equal $val [r json.get wikipedia .foo]
                assert_equal $val [r json.get wikipedia INDENT "\t" .foo]
                assert_equal $exp [r json.nummultby wikipedia .foo $mult] "$val * $mult"
                assert_equal $exp [r json.get wikipedia .foo]
                assert_equal $exp [r json.get wikipedia INDENT "\t" .foo]
            }
        }

        test "JSON.NUMINCRBY and NUMMULTBY results: shortest form, stable type, exact integers - B3 (caps $caps)" {
            r flushall
            # value, command, operand, reply (and stored text), type of the result
            foreach {val cmd operand exp type} {
                1                  json.numincrby 1.1    2.1                  number
                1.1                json.numincrby 2.2    3.3000000000000003   number
                0.1                json.numincrby 0.2    0.30000000000000004  number
                0.1                json.nummultby 3      0.30000000000000004  number
                1.5                json.numincrby 1.5    3.0                  number
                1.5                json.nummultby 2      3.0                  number
                1.5                json.numincrby -1.5   0.0                  number
                3                  json.nummultby 0.5    1.5                  number
                2                  json.nummultby 0.5    1.0                  number
                1                  json.numincrby 1.0    2.0                  number
                1                  json.numincrby 1E2    101.0                number
                5                  json.nummultby 1.0    5.0                  number
                2                  json.numincrby -0     2                    integer
                2                  json.numincrby -0.0   2.0                  number
                3                  json.numincrby -3     0                    integer
                0                  json.nummultby 5      0                    integer
                0.0                json.nummultby 5      0.0                  number
                -0.0               json.numincrby 0      0.0                  number
                -0.0               json.nummultby 1      -0.0                 number
                7                  json.nummultby -3     -21                  integer
                9007199254740993   json.numincrby 1      9007199254740994     integer
                9007199254740993   json.nummultby 3      27021597764222979    integer
                -9007199254740993  json.numincrby -2     -9007199254740995    integer
                1e5                json.numincrby 1      100001.0             number
                19.90              json.numincrby 0.1    20.0                 number
                1.0E3              json.numincrby 1      1001.0               number
                1e-07              json.nummultby 10     0.000001             number
                123456789.123456789123  json.numincrby 0 123456789.12345679   number
                1234567890123456789012345 json.numincrby 1 1.2345678901234568e+24 number
                5                  json.numincrby 12345678901234567890123 1.2345678901234568e+22 number
                1e300              json.nummultby 1e-300 1.0                  number
                1e21               json.nummultby 0.1    100000000000000000000.0 number
                1e21               json.nummultby 10     1e+22                number
            } {
                r json.set k . $val
                assert_equal $exp [r $cmd k . $operand] "$val $cmd $operand"
                assert_equal $exp [r json.get k] "$val $cmd $operand"
                assert_equal $type [r json.type k] "$val $cmd $operand"

                # the same inside an object and through a $ path
                r json.set k . "{\"n\":$val,\"s\":\"x\"}"
                assert_equal "\[$exp\]" [r $cmd k $.n $operand] "$val $cmd $operand"
                assert_equal "{\"n\":$exp,\"s\":\"x\"}" [r json.get k] "$val $cmd $operand"
                assert_equal [list $type] [r json.type k $.n] "$val $cmd $operand"
            }
        }

        test "JSON.NUMINCRBY and NUMMULTBY of two random integers are exact within int64 and a double beyond - B3 (caps $caps)" {
            r flushall
            expr {srand(99)}
            for {set i 0} {$i < 500} {incr i} {
                set a [json_random_int64]
                set b [json_random_int64]
                foreach {cmd op} {json.numincrby + json.nummultby *} {
                    r json.set k . $a
                    # Tcl integers do not overflow
                    set exact [expr "$a $op $b"]
                    set res [r $cmd k . $b]
                    if {$exact >= -9223372036854775808 && $exact <= 9223372036854775807} {
                        assert_equal $exact $res "$a $op $b"
                        assert_equal integer [r json.type k]
                    } else {
                        # a whole double: digits and ".0" below 1e21, exponent form from there
                        assert_match {*[.e]*} $res
                        assert_lessthan [expr {abs(($res - $exact) / double($exact))}] 1e-15
                        assert_lessthan_equal [string length [json_sig_digits $res]] 17
                        assert_equal number [r json.type k]
                    }
                    assert_equal $res [r json.get k]
                }
            }
        }

        test "JSON.NUMINCRBY and NUMMULTBY on arrays of numbers - B3 (caps $caps)" {
            r flushall
            # an integer array takes a double
            r json.set k . {[1,2,3]}
            assert_equal {[3.0]} [r json.nummultby k {$[1]} 1.5]
            assert_equal {[1,3.0,3]} [r json.get k]
            assert_equal {integer number integer} [r json.type k {$[*]}]
            assert_equal {[9223372036854776000.0]} [r json.numincrby k {$[0]} 9223372036854775807]
            assert_equal {[9223372036854776000.0,3.0,3]} [r json.get k]
            assert_equal {[4]} [r json.numincrby k {$[2]} 1]
            assert_equal {[9223372036854776000.0,3.0,4]} [r json.get k]

            r json.set k . {[1,2,3]}
            assert_equal {[2,3,4]} [r json.numincrby k {$[*]} 1]
            assert_equal {[4,6,8]} [r json.nummultby k {$[*]} 2]
            assert_equal {[4,6,8]} [r json.get k]
            assert_equal {integer integer integer} [r json.type k {$[*]}]
            assert_equal {[4.5,6.5,8.5]} [r json.numincrby k {$[*]} 0.5]
            assert_equal {[4.5,6.5,8.5]} [r json.get k]

            # a double array
            r json.set k . {[1.5,2.5]}
            assert_equal {[2.0,3.0]} [r json.numincrby k {$[*]} 0.5]
            assert_equal {[2.0,3.0]} [r json.get k]
            assert_equal {number number} [r json.type k {$[*]}]
            assert_equal {[1e+300]} [r json.numincrby k {$[0]} 1e300]
            assert_equal {[1e+300,3.0]} [r json.get k]

            # An array of decimals with two fraction digits. A computed result is stored
            # with its canonical text, the one the reply has, whatever the scale of its
            # neighbours (design 5.4). Elements that were not computed keep their text.
            r json.set k . {[1.50,2.25,3.00]}
            assert_equal {[1.75]} [r json.numincrby k {$[0]} 0.25]
            assert_equal {[1.75,2.25,3.00]} [r json.get k]
            assert_equal {[2.375]} [r json.numincrby k {$[1]} 0.125]
            assert_equal {[1.75,2.375,3.00]} [r json.get k]
            assert_equal {[4.0]} [r json.numincrby k {$[2]} 1]
            assert_equal {[1.75,2.375,4.0]} [r json.get k]
            assert_equal {number number number} [r json.type k {$[*]}]

            r json.set k . {[1.50,2.25,3.00]}
            assert_equal {[3.0,4.5,6.0]} [r json.nummultby k {$[*]} 2]
            assert_equal {[3.0,4.5,6.0]} [r json.get k]
            assert_equal {[0.30000000000000004,0.45,0.6000000000000001]} [r json.nummultby k {$[*]} 0.1]
            assert_equal {[0.30000000000000004,0.45,0.6000000000000001]} [r json.get k]

            r json.set k . {[0.100000,0.250000,7.000000]}
            assert_equal {[0.100001,0.250001,7.000001]} [r json.numincrby k {$[*]} 0.000001]
            assert_equal {[0.100001,0.250001,7.000001]} [r json.get k]
            r json.set k . {[0.500000,0.250000,7.000000]}
            assert_equal {[0.625,0.375,7.125]} [r json.numincrby k {$[*]} 0.125]
            assert_equal {[0.625,0.375,7.125]} [r json.get k]
            assert_equal {[1.625,1.375,8.125]} [r json.numincrby k {$[*]} 1]
            assert_equal {[1.625,1.375,8.125]} [r json.get k]
            assert_equal {[1.6250001,1.3750001,8.1250001]} [r json.numincrby k {$[*]} 0.0000001]
            assert_equal {[1.6250001,1.3750001,8.1250001]} [r json.get k]

            # the stored value is the computed double in every case
            r json.set k . {[1.50,2.25,3.00]}
            r json.numincrby k {$[2]} 1
            assert_equal {[5.0]} [r json.numincrby k {$[2]} 1]
            assert_equal {2} [r json.arrindex k $ 5.0]
        }

        test "JSON.NUMINCRBY and NUMMULTBY overflow is an error and changes nothing (caps $caps)" {
            r flushall
            foreach {val cmd operand err} {
                1e308                   json.nummultby 10       "OVERFLOW Multiplication would overflow"
                1e308                   json.numincrby 1e308    "OVERFLOW Addition would overflow"
                -1e308                  json.numincrby -1e308   "OVERFLOW Addition would overflow"
                -1e308                  json.nummultby 10       "OVERFLOW Multiplication would overflow"
                1.7976931348623157e308  json.numincrby 1e292    "OVERFLOW Addition would overflow"
                1.7976931348623157e308  json.nummultby 1.0000000000000002 "OVERFLOW Multiplication would overflow"
                1.7976931348623157e308  json.nummultby 2        "OVERFLOW Multiplication would overflow"
                9223372036854775807     json.nummultby 1e300    "OVERFLOW Multiplication would overflow"
            } {
                r json.set k . $val
                assert_error $err {r $cmd k . $operand}
                assert_error $err {r $cmd k $ $operand}
                assert_equal $val [r json.get k]
            }
            # the largest double can still be used
            r json.set k . 1.7976931348623157e308
            assert_equal {[1.7976931348623157e+308]} [r json.numincrby k $ 1]
            assert_equal 0.0 [r json.nummultby k . 0]
        }

        test "JSON.NUMINCRBY and NUMMULTBY that overflow on one of several matches change nothing - B6 (caps $caps)" {
            # The module stops at the failing match and leaves the earlier ones changed.
            r flushall
            r json.set k . {[1,1e308,3]}
            assert_error "OVERFLOW Addition would overflow" {r json.numincrby k {$[*]} 1e308}
            assert_equal {[1,1e308,3]} [r json.get k]
            assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby k {$[*]} 10}
            assert_equal {[1,1e308,3]} [r json.get k]
            assert_error "OVERFLOW Addition would overflow" {r json.numincrby k {.[*]} 1e308}
            assert_equal {[1,1e308,3]} [r json.get k]
            assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby k {.[*]} 10}
            assert_equal {[1,1e308,3]} [r json.get k]

            r json.set k . {{"a":2,"b":"x","c":{"d":[5,-1e308]},"e":7}}
            assert_error "OVERFLOW Multiplication would overflow" {r json.nummultby k $..* 1e10}
            assert_error "OVERFLOW Addition would overflow" {r json.numincrby k $..* -1e308}
            assert_equal {{"a":2,"b":"x","c":{"d":[5,-1e308]},"e":7}} [r json.get k]
            # without the failing match the same write applies to all of them
            assert_equal {[3.0,null,null,10.5,null,7.5,-1.5e+308]} [r json.nummultby k $..* 1.5]
            assert_equal {{"a":3.0,"b":"x","c":{"d":[7.5,-1.5e+308]},"e":10.5}} [r json.get k]
        }

        test "JSON.NUMINCRBY \$ path and wildcards (caps $caps)" {
            # Port of test_json_numincrby_jsonpath_and_wildcard. A value that is not a number
            # replies null in its position.
            r flushall
            r json.set k1 . {{"a":[], "b":[1], "c":[1,2], "d":[1,2,3]}}
            r json.set k2 . {{"a":{}, "b":{"a":1}, "c":{"a":1, "b":2}, "d":{"a":1, "b":2, "c":3}}}
            r json.set k3 . {{"a":{"a":"a"}, "b":{"a":"a", "b":1}, "c":{"a":"a", "b":"b"}, "d":{"a":1, "b":"b", "c":3}}}
            foreach {key path exp exp_get} {
                k1 $.a.*   {[]}           {[]}
                k1 $.b.*   {[2]}          {[2]}
                k1 $.b[*]  {[3]}          {[3]}
                k1 $.d.*   {[2,3,4]}      {[2,3,4]}
                k1 $.d[*]  {[3,4,5]}      {[3,4,5]}
                k2 $.a.*   {[]}           {[]}
                k2 $.b.*   {[2]}          {[2]}
                k2 $.d.*   {[2,3,4]}      {[2,3,4]}
                k3 $.a.*   {[null]}       {["a"]}
                k3 $.b.*   {[null,2]}     {["a",2]}
                k3 $.c.*   {[null,null]}  {["a","b"]}
                k3 $.d.*   {[2,null,4]}   {[2,"b",4]}
            } {
                assert_equal $exp [r json.numincrby $key $path 1] "$key $path"
                assert_equal $exp_get [r json.get $key $path] "$key $path"
            }
            assert_equal {{"a":[],"b":[3],"c":[1,2],"d":[3,4,5]}} [r json.get k1]
            assert_equal {{"a":{},"b":{"a":2},"c":{"a":1,"b":2},"d":{"a":2,"b":3,"c":4}}} [r json.get k2]
            assert_equal {{"a":{"a":"a"},"b":{"a":"a","b":2},"c":{"a":"a","b":"b"},"d":{"a":2,"b":"b","c":4}}} [r json.get k3]

            # more shapes: no match, the root, recursive descent, a $ reply is one bulk string
            assert_equal {"[]"} [json_raw json.numincrby k1 $.zz 2]
            assert_equal {"[null]"} [json_raw json.numincrby k1 $ 2]
            assert_equal {"[null]"} [json_raw json.numincrby k1 $.a 2]
            assert_equal {"[5,6,7]"} [json_raw json.numincrby k1 {$.d[*]} 2]
            assert_equal {"7"} [json_raw json.numincrby k1 {.d[1]} 1]
            r json.set k4 . {{"a":1,"b":[1,2.5],"c":{"d":3}}}
            assert_equal {[2,null,null,2,3.5,4]} [r json.numincrby k4 $..* 1]
            assert_equal {{"a":2,"b":[2,3.5],"c":{"d":4}}} [r json.get k4]
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.numincrby k4 {$.b[5]} 2}
            assert_error "OUTOFBOUNDARIES Array index is out of bounds" {r json.numincrby k4 {.b[5]} 2}
        }

        test "JSON.NUMINCRBY legacy path and wildcards (caps $caps)" {
            # Port of test_json_numincrby_legacy_path_and_wildcard.
            r flushall
            r json.set k1 . {{"a":[], "b":[1], "c":[1,2], "d":[1,2,3]}}
            r json.set k2 . {{"a":{}, "b":{"a":1}, "c":{"a":1, "b":2}, "d":{"a":1, "b":2, "c":3}}}
            r json.set k3 . {{"a":{"a":"a"}, "b":{"a":"a", "b":1}, "c":{"a":"a", "b":"b"}, "d":{"a":1, "b":"b", "c":3}}}

            # NONEXISTENT if no value is selected
            foreach key {k1 k2} {
                assert_error "NONEXISTENT*" {r json.numincrby $key .a.* 1}
                assert_error "NONEXISTENT*" {r json.get $key .a.*}
            }
            # WRONGTYPE if no number is selected
            assert_error "WRONGTYPE JSON element is not a number" {r json.numincrby k3 .a.* 1}
            assert_error "WRONGTYPE JSON element is not a number" {r json.numincrby k3 .c.* 1}

            # A single value is returned, the last one updated, while JSON.GET returns the first
            # value selected.
            foreach {key path exp exp_get} {
                k1 .b.*   2  2
                k1 .b[*]  3  3
                k1 .d.*   4  2
                k1 .d[*]  5  3
                k2 .b.*   2  2
                k2 .d.*   4  2
                k3 .b.*   2  {"a"}
                k3 .d.*   4  2
            } {
                assert_equal $exp [r json.numincrby $key $path 1] "$key $path"
                assert_equal $exp_get [r json.get $key $path] "$key $path"
            }
            assert_equal {{"a":[],"b":[3],"c":[1,2],"d":[3,4,5]}} [r json.get k1]
            assert_equal {{"a":{},"b":{"a":2},"c":{"a":1,"b":2},"d":{"a":2,"b":3,"c":4}}} [r json.get k2]
            assert_equal {{"a":{"a":"a"},"b":{"a":"a","b":2},"c":{"a":"a","b":"b"},"d":{"a":2,"b":"b","c":4}}} [r json.get k3]
        }

        test "JSON.NUMMULTBY \$ path and wildcards (caps $caps)" {
            # Port of test_json_nummultby_jsonpath_and_wildcard.
            r flushall
            r json.set k1 . {{"a":[], "b":[1], "c":[1,2], "d":[1,2,3]}}
            r json.set k2 . {{"a":{}, "b":{"a":1}, "c":{"a":1, "b":2}, "d":{"a":1, "b":2, "c":3}}}
            r json.set k3 . {{"a":{"a":"a"}, "b":{"a":"a", "b":1}, "c":{"a":"a", "b":"b"}, "d":{"a":1, "b":"b", "c":3}}}
            foreach {key path exp exp_get} {
                k1 $.a.*   {[]}           {[]}
                k1 $.b.*   {[2]}          {[2]}
                k1 $.b[*]  {[4]}          {[4]}
                k1 $.d.*   {[2,4,6]}      {[2,4,6]}
                k1 $.d[*]  {[4,8,12]}     {[4,8,12]}
                k2 $.a.*   {[]}           {[]}
                k2 $.b.*   {[2]}          {[2]}
                k2 $.d.*   {[2,4,6]}      {[2,4,6]}
                k3 $.a.*   {[null]}       {["a"]}
                k3 $.b.*   {[null,2]}     {["a",2]}
                k3 $.c.*   {[null,null]}  {["a","b"]}
                k3 $.d.*   {[2,null,6]}   {[2,"b",6]}
            } {
                assert_equal $exp [r json.nummultby $key $path 2] "$key $path"
                assert_equal $exp_get [r json.get $key $path] "$key $path"
            }
            assert_equal {{"a":[],"b":[4],"c":[1,2],"d":[4,8,12]}} [r json.get k1]
            assert_equal {"[]"} [json_raw json.nummultby k1 $.zz 2]
            assert_equal {"[null]"} [json_raw json.nummultby k1 $ 2]
            assert_equal {"[8,16,24]"} [json_raw json.nummultby k1 {$.d[*]} 2]
            assert_equal {"32"} [json_raw json.nummultby k1 {.d[1]} 2]
        }

        test "JSON.NUMMULTBY legacy path and wildcards (caps $caps)" {
            # Port of test_json_nummultby_legacy_path_and_wildcard.
            r flushall
            r json.set k1 . {{"a":[], "b":[1], "c":[1,2], "d":[1,2,3]}}
            r json.set k2 . {{"a":{}, "b":{"a":1}, "c":{"a":1, "b":2}, "d":{"a":1, "b":2, "c":3}}}
            r json.set k3 . {{"a":{"a":"a"}, "b":{"a":"a", "b":1}, "c":{"a":"a", "b":"b"}, "d":{"a":1, "b":"b", "c":3}}}

            foreach key {k1 k2} {
                assert_error "NONEXISTENT*" {r json.nummultby $key .a.* 2}
                assert_error "NONEXISTENT*" {r json.get $key .a.*}
            }
            assert_error "WRONGTYPE JSON element is not a number" {r json.nummultby k3 .a.* 2}
            assert_error "WRONGTYPE JSON element is not a number" {r json.nummultby k3 .c.* 2}

            foreach {key path exp exp_get} {
                k1 .b.*   2   2
                k1 .b[*]  4   4
                k1 .d.*   6   2
                k1 .d[*]  12  4
                k2 .b.*   2   2
                k2 .d.*   6   2
                k3 .b.*   2   {"a"}
                k3 .d.*   6   2
            } {
                assert_equal $exp [r json.nummultby $key $path 2] "$key $path"
                assert_equal $exp_get [r json.get $key $path] "$key $path"
            }
            assert_equal {{"a":[],"b":[4],"c":[1,2],"d":[4,8,12]}} [r json.get k1]
            assert_equal {{"a":{},"b":{"a":2},"c":{"a":1,"b":2},"d":{"a":2,"b":4,"c":6}}} [r json.get k2]
            assert_equal {{"a":{"a":"a"},"b":{"a":"a","b":2},"c":{"a":"a","b":"b"},"d":{"a":2,"b":"b","c":6}}} [r json.get k3]
        }

        test "JSON.NUMINCRBY and NUMMULTBY RESP3 replies are bulk strings (caps $caps)" {
            r flushall
            r json.set k . {{"a":1,"b":2.5,"c":"x"}}
            r hello 3
            assert_equal {"[2,3.5,null]"} [json_raw json.numincrby k $.* 1]
            assert_equal {"[4,7.0,null]"} [json_raw json.nummultby k $.* 2]
            assert_equal {"5"} [json_raw json.numincrby k .a 1]
            assert_equal {"3.5"} [json_raw json.nummultby k .b 0.5]
            assert_equal {"[]"} [json_raw json.numincrby k $.zz 1]
            assert_equal {-NONEXISTENT JSON path does not exist} [json_raw json.numincrby k .zz 1]
            assert_equal {-WRONGTYPE JSON element is not a number} [json_raw json.nummultby k .c 1]
            r hello 2
            assert_equal {{"a":5,"b":3.5,"c":"x"}} [r json.get k]
        }

        test "JSON.NUMINCRBY, NUMMULTBY and TOGGLE on containers larger than the packed caps (caps $caps)" {
            r flushall
            set ints {}
            set bools {}
            set members {}
            for {set i 0} {$i < 300} {incr i} {
                lappend ints $i
                lappend bools [expr {$i % 2 ? "true" : "false"}]
                lappend members "\"m$i\":$i"
            }
            r json.set k . "{\"ints\":\[[join $ints ,]\],\"bools\":\[[join $bools ,]\],\"obj\":{[join $members ,]}}"

            # every integer outgrows its stored width
            set big {}
            set bigmembers {}
            foreach i $ints {
                lappend big [expr {$i + 5000000000}]
                lappend bigmembers "\"m$i\":[expr {$i + 5000000000}]"
            }
            assert_equal "\[[join $big ,]\]" [r json.numincrby k {$.ints[*]} 5000000000]
            assert_equal "\[[join $big ,]\]" [r json.get k .ints]
            assert_equal "\[[join $big ,]\]" [r json.numincrby k $.obj.* 5000000000]
            assert_equal "{[join $bigmembers ,]}" [r json.get k .obj]

            # every integer becomes a double
            set halves {}
            set halfmembers {}
            foreach i $ints {
                # Half of an integer, as text built from integers: "<n>.0" or "<n>.5".
                set sum [expr {$i + 5000000000}]
                set h "[expr {$sum / 2}].[expr {$sum % 2 ? 5 : 0}]"
                lappend halves $h
                lappend halfmembers "\"m$i\":$h"
            }
            assert_equal "\[[join $halves ,]\]" [r json.nummultby k {$.ints[*]} 0.5]
            assert_equal "\[[join $halves ,]\]" [r json.get k .ints]
            assert_equal "\[[join $halves ,]\]" [r json.nummultby k $.obj.* 0.5]
            assert_equal "{[join $halfmembers ,]}" [r json.get k .obj]
            assert_equal [lrepeat 300 number] [r json.type k {$.ints[*]}]

            # every boolean flips
            set flipped {}
            set replies {}
            foreach b $bools {
                lappend flipped [expr {$b eq "true" ? "false" : "true"}]
                lappend replies [expr {$b eq "true" ? 0 : 1}]
            }
            assert_equal $replies [r json.toggle k {$.bools[*]}]
            assert_equal "\[[join $flipped ,]\]" [r json.get k .bools]

            # one value in the middle and one at the end
            assert_equal 2500000075.25 [r json.numincrby k {.ints[150]} 0.25]
            lset halves 150 2500000075.25
            assert_equal true [r json.toggle k {.bools[299]}]
            lset flipped 299 true
            assert_equal 7.0 [r json.numincrby k .obj.m299 -2500000142.5]
            lset halfmembers 299 "\"m299\":7.0"
            assert_equal "{\"ints\":\[[join $halves ,]\],\"bools\":\[[join $flipped ,]\],\"obj\":{[join $halfmembers ,]}}" [r json.get k]
            assert_equal [r json.debug memory k] [r json.debug memory k .]
        }

        # ---------------------------------------------------------------- document memory

        test "JSON document size stays correct while NUMINCRBY flips a value between integer and double (caps $caps)" {
            # Port of test_doc_mem_size_with_numincrby. The absolute sizes are those of the new
            # representation; what is checked is that the tracked size and the size computed by
            # walking the document agree and do not drift.
            r flushall
            assert_equal OK [r json.set k1 . {{"a":1955439684,"b":5398,"c":[],"d":{"e":0,"f":2.7233281135559082,"g":1732953600,"h":false,"i":true}}}]
            set sizes {}
            for {set i 0} {$i < 1000} {incr i} {
                assert_equal 7.055081799084579 [r json.numincrby k1 .d.e 7.055081799084578998899889890890]
                assert_equal OK [r json.set k1 .d.e 0]
                set tracked [r json.debug memory k1]
                assert_lessthan $tracked 1000
                assert_equal $tracked [r json.debug memory k1 .]
                lappend sizes $tracked
            }
            assert_equal [lindex $sizes 1] [lindex $sizes end]
            assert_equal {{"a":1955439684,"b":5398,"c":[],"d":{"e":0,"f":2.7233281135559082,"g":1732953600,"h":false,"i":true}}} [r json.get k1]
        }

        test "JSON document size reported for every key agrees with a walk of the document (caps $caps)" {
            # Port of test_verify_doc_mem_size, on generated documents instead of the module's
            # data files.
            r flushall
            r json.set wikipedia . $::json_wikipedia
            r json.set scalar . 12345
            r json.set text . {"some text"}
            r json.set empty . {{}}
            set items {}
            for {set i 0} {$i < 500} {incr i} {
                lappend items "{\"id\":$i,\"name\":\"item-$i\",\"price\":[format %.2f [expr {$i * 1.25}]],\"tags\":\[\"a\",\"b\",$i\],\"ok\":true,\"none\":null}"
            }
            r json.set records . "{\"items\":\[[join $items ,]\]}"
            set nums {}
            for {set i 0} {$i < 1536} {incr i} { lappend nums [format %.6f [expr {sin($i)}]] }
            r json.set embedding . "\[[join $nums ,]\]"
            # some writes, so the tracked size has been updated incrementally
            r json.numincrby records {$.items[*].id} 1
            r json.arrappend records {$.items[*].tags} {"new"} 1.5
            r json.del records {$.items[?(@.id>400)]}
            r json.strappend wikipedia .firstName {"athan"}
            r json.arrtrim embedding . 100 1000

            set count 0
            set cursor 0
            while 1 {
                lassign [r scan $cursor type ReJSON-RL] cursor keys
                foreach key $keys {
                    set tracked [r json.debug memory $key]
                    assert_equal $tracked [r json.debug memory $key .] "key $key"
                    assert_morethan $tracked 0
                    assert_lessthan $tracked [expr {64 * 1024 * 1024}]
                    incr count
                }
                if {$cursor == 0} break
            }
            assert_equal 6 $count
        }
    }
    json_set_caps {4096 128}
}
