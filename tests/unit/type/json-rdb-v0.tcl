# Tests for the built-in JSON module: loading RDB encoding version 0, the
# external module's older form, written one node at a time. This server only
# reads it (jsonTypeRdbLoad, v0LoadAsText in src/modules/json/json_module.c).
#
# The payloads are built here byte by byte and loaded with RESTORE. The CRC64
# of the DUMP footer is not computed: each RESTORE runs with DEBUG
# SET-SKIP-CHECKSUM-VALIDATION on, as tests/integration/corrupt-dump.tcl does,
# and turns it off again.
#
# A DUMP payload of a module value is:
#   RDB_TYPE_MODULE_2 (7), the module type id as an RDB length, the value,
#   RDB_MODULE_OPCODE_EOF (0), the RDB version (2 bytes, little endian) and
#   the CRC64 (8 bytes).
# The type id is the type name "ReJSON-RL", 6 bits a character, followed by
# the encoding version in the low 10 bits. Each item of the value is an
# opcode (an RDB length) and its data: an unsigned (opcode 2) or a signed
# integer (the module API writes a signed one as an unsigned, opcode 2 too)
# as an RDB length, a double (opcode 4) as 8 bytes little endian, a string
# (opcode 5) as an RDB length and the bytes.
#
# A version 0 node is its code (an unsigned), then: nothing for null, a string,
# a double, a signed integer, the string "0" or "1" for a boolean, or a count
# (an unsigned) and that many nodes for an array, or that many members for an
# object, each member being PAIR (an unsigned), the name (a string) and a node.

set ::json_v0_codes {null 0x01 str 0x02 dbl 0x04 int 0x08 bool 0x10 obj 0x20 arr 0x40 pair 0x80}

proc json_v0_code {name} {
    return [expr {[dict get $::json_v0_codes $name]}]
}

# The bytes of an RDB length (rdbSaveLen): 6, 14, 32 or 64 bits. A negative
# value is written as its 64-bit two's complement, as a signed module value is.
proc json_v0_len {n} {
    if {$n >= 0 && $n < 64} { return [binary format c $n] }
    if {$n >= 0 && $n < 16384} { return [binary format S [expr {0x4000 | $n}]] }
    if {$n >= 0 && $n <= 0xffffffff} { return "[binary format c 0x80][binary format I $n]" }
    if {$n >= 2**63} { set n [expr {$n - 2**64}] }
    return "[binary format c 0x81][binary format W $n]"
}

# Module value items.
proc json_v0_uint {n} { return "[json_v0_len 2][json_v0_len $n]" }
proc json_v0_str {s} { return "[json_v0_len 5][json_v0_len [string length $s]]$s" }
proc json_v0_dbl {d} { return "[json_v0_len 4][binary format q $d]" }

# The bytes of a node given as a Tcl list: {null}, {str S}, {dbl D}, {int N},
# {bool S} (S the raw text, "0" or "1" when well formed), {arr {NODE ...}},
# {obj {NAME NODE ...}}.
proc json_v0_node {node} {
    lassign $node kind v
    set out [json_v0_uint [json_v0_code $kind]]
    switch $kind {
        null {}
        str - bool { append out [json_v0_str $v] }
        dbl { append out [json_v0_dbl $v] }
        int { append out [json_v0_uint $v] }
        arr {
            append out [json_v0_uint [llength $v]]
            foreach e $v { append out [json_v0_node $e] }
        }
        obj {
            append out [json_v0_uint [expr {[llength $v] / 2}]]
            foreach {name e} $v { append out [json_v0_uint [json_v0_code pair]] [json_v0_str $name] [json_v0_node $e] }
        }
    }
    return $out
}

# The module type id of ReJSON-RL with encoding version `encver`.
proc json_v0_type_id {encver} {
    set cset ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_
    set id 0
    foreach c [split ReJSON-RL ""] { set id [expr {($id << 6) | [string first $c $cset]}] }
    return [expr {($id << 10) | $encver}]
}

# A DUMP payload around the module value `body`, with a CRC64 of zeros.
proc json_v0_payload {body {encver 0}} {
    return "[binary format c 7][json_v0_len [json_v0_type_id $encver]]$body[json_v0_len 0]$::json_v0_rdbver[string repeat \x00 8]"
}

# RESTOREs `key` from the module value `body`, with the checksum check off for
# this command only.
proc json_v0_restore {key body {encver 0}} {
    r debug set-skip-checksum-validation 1
    set failed [catch {r restore $key 0 [json_v0_payload $body $encver]} res]
    r debug set-skip-checksum-validation 0
    if {$failed} { error $res }
    return $res
}

# N levels of nested arrays, the innermost empty: the text of json_deep_array.
proc json_v0_deep_array {n} {
    return "[string repeat "[json_v0_uint [json_v0_code arr]][json_v0_uint 1]" [expr {$n - 1}]][json_v0_uint [json_v0_code arr]][json_v0_uint 0]"
}

# N levels of nested objects under the member "a", the innermost value 1.
proc json_v0_deep_object {n} {
    set level "[json_v0_uint [json_v0_code obj]][json_v0_uint 1][json_v0_uint [json_v0_code pair]][json_v0_str a]"
    return "[string repeat $level $n][json_v0_node {int 1}]"
}

proc json_v0_num_documents {} {
    return [getInfoProperty [r info json] json_num_documents]
}

# A version 0 payload that the server must refuse: RESTORE fails with an
# error, creates no key and leaves the server serving.
proc json_v0_assert_refused {body what {encver 0}} {
    set docs [json_v0_num_documents]
    assert_error "*Bad data format*" {json_v0_restore bad $body $encver} $what
    assert_equal 0 [r exists bad] $what
    assert_equal PONG [r ping]
    assert_equal $docs [json_v0_num_documents] $what
}

start_server {tags {"json needs:debug"}} {
    r json.set v3 . {{"a":[1,2]}}
    set ::json_v0_rdbver [string range [r dump v3] end-9 end-8]

    test {JSON RDB v0: the payload framing of these tests matches a DUMP of version 3} {
        # Version 3 is the document as one string. A payload built here with the
        # same text is the DUMP up to the checksum, so the framing is right.
        set dump [r dump v3]
        assert_equal [string range $dump 0 end-8] [string range [json_v0_payload [json_v0_str {{"a":[1,2]}}] 3] 0 end-8]
        assert_equal OK [json_v0_restore v3copy [json_v0_str {{"a":[1,2]}}] 3]
        assert_equal {{"a":[1,2]}} [r json.get v3copy]
        r del v3
        r del v3copy
    }

    test {JSON RDB v0: every node type at the root} {
        r flushall
        foreach {node text type} {
            {null}            null    null
            {str hello}       {"hello"} string
            {str {}}          {""}    string
            {int 0}           0       integer
            {int 42}          42      integer
            {int -42}         -42     integer
            {bool 1}          true    boolean
            {bool 0}          false   boolean
            {dbl 1.5}         1.5     number
            {arr {}}          {[]}    array
            {obj {}}          {{}}    object
        } {
            assert_equal OK [json_v0_restore k [json_v0_node $node]] "restore of $node"
            assert_equal $text [r json.get k] "JSON.GET after $node"
            assert_equal $type [r json.type k] "JSON.TYPE after $node"
            r del k
        }
    }

    test {JSON RDB v0: nested objects and arrays with every node type, and empty containers} {
        r flushall
        set node {obj {
            n {null}
            s {str "text"}
            i {int 7}
            neg {int -9000000000}
            t {bool 1}
            f {bool 0}
            d {dbl 2.25}
            empty_arr {arr {}}
            empty_obj {obj {}}
            arr {arr {{int 1} {str x} {null} {bool 1} {dbl -0.5} {arr {}} {obj {}} {arr {{arr {{int 2}}}}} {obj {k {arr {{int 3}}}}}}}
            obj {obj {a {obj {b {obj {c {arr {{null} {obj {d {str deep}}}}}}}}}}}
        }}
        set text {{"n":null,"s":"text","i":7,"neg":-9000000000,"t":true,"f":false,"d":2.25,"empty_arr":[],"empty_obj":{},"arr":[1,"x",null,true,-0.5,[],{},[[2]],{"k":[3]}],"obj":{"a":{"b":{"c":[null,{"d":"deep"}]}}}}}
        assert_equal OK [json_v0_restore k [json_v0_node $node]]
        assert_equal $text [r json.get k]
        assert_equal {object} [r json.type k]
        assert_equal {integer} [r json.type k .neg]
        assert_equal {number} [r json.type k .d]
        assert_equal {[[[2]]]} [r json.get k {$.arr[7]}]
        assert_equal {["deep"]} [r json.get k {$.obj.a.b.c[1].d}]
        assert_equal 6 [r json.debug depth k]
        # The document is a document like any other: it takes writes, and a DUMP
        # of it is version 3 and loads again.
        assert_equal {[8]} [r json.numincrby k $.i 1]
        assert_equal 10 [r json.arrappend k .arr 10]
        assert_equal OK [r restore k2 0 [r dump k]]
        assert_equal [r json.get k] [r json.get k2]
        assert_equal [r debug digest-value k] [r debug digest-value k2]
        r debug reload
        assert_equal [r json.get k] [r json.get k2]
        r del k
        r del k2
    }

    test {JSON RDB v0: integers at the ends of the 64-bit range} {
        r flushall
        foreach v {1 63 64 16383 16384 4294967295 4294967296 9223372036854775807 -1 -9223372036854775808} {
            assert_equal OK [json_v0_restore k [json_v0_node [list arr [list [list int $v]]]]] "restore of $v"
            assert_equal "\[$v\]" [r json.get k]
            assert_equal integer [r json.type k {[0]}]
            r del k
        }
    }

    test {JSON RDB v0: strings and member names are escaped as JSON text} {
        r flushall
        set e [encoding convertto utf-8 "é€"]
        set node [list obj [list "q\"b\\s" [list str "line\nbreak\ttab\x01\x1f"] "" [list str ""] $e [list str "x${e}y"] "nul\x00" [list str "a\x00b"]]]
        assert_equal OK [json_v0_restore k [json_v0_node $node]]
        assert_equal "{\"q\\\"b\\\\s\":\"line\\nbreak\\ttab\\u0001\\u001F\",\"\":\"\",\"$e\":\"x${e}y\",\"nul\\u0000\":\"a\\u0000b\"}" [r json.get k]
        assert_equal 4 [r json.objlen k]
        assert_equal "\"x${e}y\"" [r json.get k "\[\"$e\"\]"]
        r del k
    }

    test {JSON RDB v0: a double becomes the text of %.17g, so a whole one loads as an integer} {
        r flushall
        # The loader prints a version 0 double as the external module does and
        # parses the text. A double with a fraction keeps it, with up to 17
        # significant digits; a whole one has no fraction digit and is an integer
        # from then on.
        foreach {d text type} {
            1.5                      1.5                      number
            -2.25                    -2.25                    number
            0.1                      0.10000000000000001      number
            1e300                    1.0000000000000001e+300  number
            1.25e-7                  1.2499999999999999e-07   number
            3.0                      3                        integer
            -4.0                     -4                       integer
            0.0                      0                        integer
            1e20                     1e+20                    number
        } {
            assert_equal OK [json_v0_restore k [json_v0_node [list arr [list [list dbl $d]]]]] "restore of $d"
            assert_equal "\[$text\]" [r json.get k] "JSON.GET after $d"
            assert_equal $type [r json.type k {[0]}] "JSON.TYPE after $d"
            r del k
        }
    }

    test {JSON RDB v0: large counts} {
        r flushall
        # An array of 100,000 integers, an object of 20,000 members, a 1 MB string.
        set n 100000
        set body "[json_v0_uint [json_v0_code arr]][json_v0_uint $n]"
        set elements {}
        for {set i 0} {$i < $n} {incr i} {
            append body [json_v0_node [list int [expr {$i * 3 - 1000}]]]
            lappend elements [expr {$i * 3 - 1000}]
        }
        assert_equal OK [json_v0_restore big $body]
        assert_equal $n [r json.arrlen big]
        assert_equal "\[[join $elements ,]\]" [r json.get big]

        set m 20000
        set body "[json_v0_uint [json_v0_code obj]][json_v0_uint $m]"
        for {set i 0} {$i < $m} {incr i} {
            append body [json_v0_uint [json_v0_code pair]] [json_v0_str "m$i"] [json_v0_node [list arr [list [list int $i] {bool 1}]]]
        }
        assert_equal OK [json_v0_restore bigobj $body]
        assert_equal $m [r json.objlen bigobj]
        assert_equal {[19999,true]} [r json.get bigobj .m19999]
        assert_equal {[0,true]} [r json.get bigobj .m0]

        set s [string repeat abcdefgh 131072]
        assert_equal OK [json_v0_restore bigstr [json_v0_node [list str $s]]]
        assert_equal [string length $s] [r json.strlen bigstr]
        r del big
        r del bigobj
        r del bigstr
    }

    test {JSON RDB v0: nesting up to the load bound loads, one level more is refused} {
        r flushall
        # The bound is max(json.max-path-limit, 1024) levels, as for version 3.
        assert_equal {json.max-path-limit 128} [r config get json.max-path-limit]
        assert_equal OK [json_v0_restore a1024 [json_v0_deep_array 1024]]
        assert_equal OK [json_v0_restore o1024 [json_v0_deep_object 1024]]
        assert_equal "[string repeat {[} 1024][string repeat {]} 1024]" [r json.get a1024]
        assert_equal "[string repeat "\{\"a\":" 1024]1[string repeat "\}" 1024]" [r json.get o1024]
        assert_equal 1024 [r json.debug depth o1024]
        json_v0_assert_refused [json_v0_deep_array 1025] "arrays 1,025 deep"
        json_v0_assert_refused [json_v0_deep_object 1025] "objects 1,025 deep"
        # Far beyond the bound: refused without a crash or a long stall.
        json_v0_assert_refused [json_v0_deep_array 100000] "arrays 100,000 deep"
        json_v0_assert_refused [json_v0_deep_object 100000] "objects 100,000 deep"
        r del a1024
        r del o1024
    }

    test {JSON RDB v0: a higher json.max-path-limit raises the load bound} {
        r flushall
        with_config json.max-path-limit 2000 {
            assert_equal OK [json_v0_restore a2000 [json_v0_deep_array 2000]]
            assert_equal OK [json_v0_restore o2000 [json_v0_deep_object 2000]]
            assert_equal 2000 [r json.debug depth o2000]
            json_v0_assert_refused [json_v0_deep_array 2001] "arrays 2,001 deep"
            json_v0_assert_refused [json_v0_deep_object 2001] "objects 2,001 deep"
            r del a2000
            r del o2000
        }
    }

    test {JSON RDB v0: malformed payloads are refused} {
        r flushall
        set good [json_v0_node {obj {a {arr {{int 1} {str x} {bool 1} {null} {dbl 1.5}}} b {obj {c {int 2}}}}}]
        assert_equal OK [json_v0_restore k $good]
        assert_equal {{"a":[1,"x",true,null,1.5],"b":{"c":2}}} [r json.get k]
        r del k

        # Every proper prefix of a good value.
        for {set i 0} {$i < [string length $good]} {incr i} {
            json_v0_assert_refused [string range $good 0 [expr {$i - 1}]] "prefix of $i bytes"
        }
        # A value followed by more data before the EOF opcode.
        json_v0_assert_refused "$good[json_v0_node {null}]" "trailing node"

        set obj [json_v0_uint [json_v0_code obj]]
        set arr [json_v0_uint [json_v0_code arr]]
        set pair [json_v0_uint [json_v0_code pair]]
        # Wrong PAIR code, a missing PAIR code, and a PAIR code that is not an unsigned.
        foreach code {0 1 0x40 0x81 0xff 0x180} {
            json_v0_assert_refused "$obj[json_v0_uint 1][json_v0_uint $code][json_v0_str a][json_v0_node {null}]" "PAIR code $code"
        }
        json_v0_assert_refused "$obj[json_v0_uint 1][json_v0_str a][json_v0_node {null}]" "no PAIR code"
        json_v0_assert_refused "$obj[json_v0_uint 1][json_v0_str \x80][json_v0_str a][json_v0_node {null}]" "PAIR code as a string"
        # A PAIR code where a node is expected, and a member name that is not a string.
        json_v0_assert_refused "$arr[json_v0_uint 1]$pair[json_v0_str a][json_v0_node {null}]" "PAIR code in an array"
        json_v0_assert_refused "$obj[json_v0_uint 1]$pair[json_v0_uint 7][json_v0_node {null}]" "member name as an unsigned"

        # Booleans other than "0" and "1", and a boolean that is not a string.
        foreach b [list 2 {} 01 10 t true false 0.0 "1\x00" "\x01"] {
            json_v0_assert_refused [json_v0_node [list bool $b]] "boolean '$b'"
            json_v0_assert_refused [json_v0_node [list arr [list {int 1} [list bool $b]]]] "boolean '$b' in an array"
        }
        json_v0_assert_refused "[json_v0_uint [json_v0_code bool]][json_v0_uint 1]" "boolean as an unsigned"

        # Unknown node codes, among them combinations of two codes.
        foreach code {0 3 5 6 0x0c 0x11 0x30 0x60 0x80 0x81 0xff 0x100 4294967296 18446744073709551615} {
            json_v0_assert_refused [json_v0_uint $code] "node code $code"
            json_v0_assert_refused "$arr[json_v0_uint 2][json_v0_node {int 1}][json_v0_uint $code]" "node code $code in an array"
        }
        # A node code that is not an unsigned, and node data of the wrong kind.
        json_v0_assert_refused [json_v0_str x] "node code as a string"
        json_v0_assert_refused "[json_v0_uint [json_v0_code str]][json_v0_uint 5]" "string as an unsigned"
        json_v0_assert_refused "[json_v0_uint [json_v0_code dbl]][json_v0_uint 5]" "double as an unsigned"
        json_v0_assert_refused "[json_v0_uint [json_v0_code int]][json_v0_dbl 5]" "integer as a double"
        json_v0_assert_refused "[json_v0_uint [json_v0_code int]][json_v0_len 1][json_v0_len 5]" "integer with the SINT opcode"
        json_v0_assert_refused "$arr[json_v0_str 1][json_v0_node {null}]" "count as a string"
        # A double that is not a JSON number.
        foreach d {NaN Inf -Inf} {
            json_v0_assert_refused [json_v0_node [list dbl $d]] "double $d"
        }

        # Counts larger than the data, up to the largest unsigned.
        foreach count {2 3 1000 4294967296 4611686018427387904 18446744073709551615} {
            json_v0_assert_refused "$arr[json_v0_uint $count][json_v0_node {int 1}]" "array count $count for 1 element"
            json_v0_assert_refused "$obj[json_v0_uint $count]$pair[json_v0_str a][json_v0_node {int 1}]" "object count $count for 1 member"
        }
        # Nothing at all, and an empty string as the value.
        json_v0_assert_refused "" "no value"
        json_v0_assert_refused [json_v0_str ""] "an empty string"

        # Encoding versions this server does not read.
        foreach encver {1 2 4 1023} {
            json_v0_assert_refused [json_v0_str {{"a":1}}] "encoding version $encver" $encver
        }
        assert_equal PONG [r ping]
    }
}
