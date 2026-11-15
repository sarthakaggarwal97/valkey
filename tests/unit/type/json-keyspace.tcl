# Tests for the built-in JSON module: how a JSON document behaves as a key of
# the keyspace (TYPE, OBJECT, COPY, RENAME, MOVE, DUMP/RESTORE, expiry, lazy
# free, SCAN, WATCH, client tracking, keyspace notifications, MULTI/EXEC, Lua)
# and how the module shows up in introspection (MODULE LIST, COMMAND, ACL,
# INFO, CONFIG).
#
# The expected values are those of the external valkey-json module, taken from
# a server running it, except where the design lists a deliberate change
# (B1..B10). Such tests carry the number in their name.

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

# The 24 commands as the external module registers them:
# name arity flags firstkey lastkey keystep acl-categories key-spec-flags.
proc json_command_table {} {
    return {
        {JSON.SET       -4 {denyoom module write}      1  1 1 {@json @slow @write} {RW update}}
        {JSON.GET       -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO access}}
        {JSON.MGET      -3 {fast module readonly}      1 -2 1 {@fast @json @read}  {RO access}}
        {JSON.MSET      -4 {denyoom module write}      1 -3 3 {@json @slow @write} {RW update}}
        {JSON.DEL       -2 {fast module write}         1  1 1 {@fast @json @write} {RW delete}}
        {JSON.FORGET    -2 {fast module write}         1  1 1 {@fast @json @write} {RW delete}}
        {JSON.MERGE      4 {denyoom module write}      1  1 1 {@json @slow @write} {RW update}}
        {JSON.NUMINCRBY  4 {fast module write}         1  1 1 {@fast @json @write} {RW access update}}
        {JSON.NUMMULTBY  4 {fast module write}         1  1 1 {@fast @json @write} {RW access update}}
        {JSON.STRLEN    -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO}}
        {JSON.STRAPPEND -3 {denyoom fast module write} 1  1 1 {@fast @json @write} {RW insert}}
        {JSON.TOGGLE    -2 {denyoom fast module write} 1  1 1 {@fast @json @write} {RW access update}}
        {JSON.OBJLEN    -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO}}
        {JSON.OBJKEYS   -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO access}}
        {JSON.ARRLEN    -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO}}
        {JSON.ARRAPPEND -4 {denyoom fast module write} 1  1 1 {@fast @json @write} {RW insert}}
        {JSON.ARRPOP    -2 {fast module write}         1  1 1 {@fast @json @write} {RW access delete}}
        {JSON.ARRINSERT -5 {denyoom fast module write} 1  1 1 {@fast @json @write} {RW insert}}
        {JSON.ARRTRIM    5 {fast module write}         1  1 1 {@fast @json @write} {RW delete}}
        {JSON.ARRINDEX  -4 {fast module readonly}      1  1 1 {@fast @json @read}  {RO access}}
        {JSON.CLEAR     -2 {fast module write}         1  1 1 {@fast @json @write} {RW delete}}
        {JSON.TYPE      -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO access}}
        {JSON.RESP      -2 {fast module readonly}      1  1 1 {@fast @json @read}  {RO access}}
        {JSON.DEBUG     -2 {module movablekeys readonly} 0 0 0 {@json @read @slow} {}}
    }
}

# One invocation of every command (both path syntaxes where they differ), with
# the reply of the external module when the key does not exist. "nil" stands
# for a null reply, "E:<prefix>" for an error starting with that text. On a key
# of another type every one of them replies WRONGTYPE.
proc json_missing_key_table {} {
    return {
        {{json.get K}                 nil}
        {{json.get K $}               nil}
        {{json.get K .a}              nil}
        {{json.mget K $}              {{}}}
        {{json.mget K .}              {{}}}
        {{json.set K $.a 1}           {E:SYNTAXERR A new Valkey key's path must be root}}
        {{json.set K .a 1}            {E:SYNTAXERR A new Valkey key's path must be root}}
        {{json.mset K $.a 1}          {E:SYNTAXERR Command syntax error}}
        {{json.merge K $.a 1}         {E:SYNTAXERR A new Valkey key's path must be root}}
        {{json.del K}                 0}
        {{json.del K $.a}             0}
        {{json.forget K}              0}
        {{json.forget K .a}           0}
        {{json.numincrby K $ 1}       {E:NONEXISTENT Document key does not exist}}
        {{json.numincrby K . 1}       {E:NONEXISTENT Document key does not exist}}
        {{json.nummultby K $ 2}       {E:NONEXISTENT Document key does not exist}}
        {{json.nummultby K . 2}       {E:NONEXISTENT Document key does not exist}}
        {{json.strlen K}              nil}
        {{json.strlen K $}            nil}
        {{json.strlen K .}            nil}
        {{json.strappend K {"x"}}     {E:NONEXISTENT Document key does not exist}}
        {{json.strappend K $ {"x"}}   {E:NONEXISTENT Document key does not exist}}
        {{json.strappend K . {"x"}}   {E:NONEXISTENT Document key does not exist}}
        {{json.toggle K}              {E:NONEXISTENT Document key does not exist}}
        {{json.toggle K $}            {E:NONEXISTENT Document key does not exist}}
        {{json.toggle K .}            {E:NONEXISTENT Document key does not exist}}
        {{json.objlen K}              nil}
        {{json.objlen K $}            nil}
        {{json.objlen K .}            nil}
        {{json.objkeys K}             nil}
        {{json.objkeys K $}           nil}
        {{json.objkeys K .}           nil}
        {{json.arrlen K}              nil}
        {{json.arrlen K $}            nil}
        {{json.arrlen K .}            nil}
        {{json.arrappend K $ 1}       {E:NONEXISTENT Document key does not exist}}
        {{json.arrappend K . 1}       {E:NONEXISTENT Document key does not exist}}
        {{json.arrpop K}              {E:NONEXISTENT Document key does not exist}}
        {{json.arrpop K $}            {E:NONEXISTENT Document key does not exist}}
        {{json.arrpop K .}            {E:NONEXISTENT Document key does not exist}}
        {{json.arrinsert K $ 0 1}     {E:NONEXISTENT Document key does not exist}}
        {{json.arrinsert K . 0 1}     {E:NONEXISTENT Document key does not exist}}
        {{json.arrtrim K $ 0 1}       {E:NONEXISTENT Document key does not exist}}
        {{json.arrtrim K . 0 1}       {E:NONEXISTENT Document key does not exist}}
        {{json.arrindex K $ 1}        {E:NONEXISTENT Document key does not exist}}
        {{json.arrindex K . 1}        {E:NONEXISTENT Document key does not exist}}
        {{json.clear K}               {E:NONEXISTENT Document key does not exist}}
        {{json.clear K $}             {E:NONEXISTENT Document key does not exist}}
        {{json.clear K .}             {E:NONEXISTENT Document key does not exist}}
        {{json.type K}                nil}
        {{json.type K $}              nil}
        {{json.type K .}              nil}
        {{json.resp K}                nil}
        {{json.resp K $}              nil}
        {{json.resp K .}              nil}
        {{json.debug memory K}        nil}
        {{json.debug memory K $}      nil}
        {{json.debug memory K .}      nil}
        {{json.debug fields K}        nil}
        {{json.debug fields K $}      nil}
        {{json.debug depth K}         nil}
    }
}

# A sequence of write commands on keys k and k2{k}, with what the external module
# does for each: its reply, the keyspace events it fires, and whether it
# propagates the command (to replicas and the AOF). "nil" is a null reply,
# "E:<prefix>" an error. The sequence includes writes that match nothing, with
# both path syntaxes (design 2.4). The hash tag puts k2{k} in the slot of k, so
# that JSON.MSET may name both on a server in cluster mode.
proc json_write_effects_table {} {
    return {
        {{json.set k . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1}}}} OK {json.set:k} 1}
        {{json.set k $.a 2}                 OK                {json.set:k}       1}
        {{json.set k .a 3}                  OK                {json.set:k}       1}
        {{json.set k $.zz.yy 1}             E:NONEXISTENT     {}                 0}
        {{json.set k {$.arr[*].q} 1}        OK                {json.set:k}       1}
        {{json.set k {.arr[*].q} 1}         OK                {json.set:k}       1}
        {{json.set k . 1 NX}                nil               {}                 0}
        {{json.set k3 . 1 XX}               nil               {}                 0}
        {{json.mset k $.a 4 k2{k} . {{"z":1}}} OK             {json.mset:k json.mset:k2{k}} 1}
        {{json.mset k $.zz.yy 4 k2{k} . {{"z":2}}} E:NONEXISTENT {}              0}
        {{json.merge k $.o {{"y":2}}}       OK                {json.merge:k}     1}
        {{json.merge k $.nomatch.x 1}       E:NONEXISTENT     {}                 0}
        {{json.numincrby k $.a 1}           {[5]}             {json.numincrby:k} 1}
        {{json.numincrby k $.zz 1}          {[]}              {json.numincrby:k} 1}
        {{json.numincrby k .zz 1}           E:NONEXISTENT     {}                 0}
        {{json.numincrby k $.s 1}           {[null]}          {json.numincrby:k} 1}
        {{json.numincrby k .s 1}            E:WRONGTYPE       {}                 0}
        {{json.nummultby k $.a 2}           {[10]}            {json.nummultby:k} 1}
        {{json.nummultby k $.zz 2}          {[]}              {json.nummultby:k} 1}
        {{json.nummultby k .zz 2}           E:NONEXISTENT     {}                 0}
        {{json.strappend k $.s {"y"}}       2                 {json.strappend:k} 1}
        {{json.strappend k $.zz {"y"}}      {}                {json.strappend:k} 1}
        {{json.strappend k .zz {"y"}}       E:NONEXISTENT     {}                 0}
        {{json.strappend k $.a {"y"}}       {{}}              {json.strappend:k} 1}
        {{json.toggle k $.b}                0                 {json.toggle:k}    1}
        {{json.toggle k $.zz}               {}                {json.toggle:k}    1}
        {{json.toggle k .zz}                E:NONEXISTENT     {}                 0}
        {{json.toggle k $.a}                {{}}              {json.toggle:k}    1}
        {{json.arrappend k $.arr 4}         4                 {json.arrappend:k} 1}
        {{json.arrappend k $.zz 4}          {}                {json.arrappend:k} 1}
        {{json.arrappend k .zz 4}           E:NONEXISTENT     {}                 0}
        {{json.arrinsert k $.arr 0 0}       5                 {json.arrinsert:k} 1}
        {{json.arrinsert k $.zz 0 0}        {}                {json.arrinsert:k} 1}
        {{json.arrinsert k .zz 0 0}         E:NONEXISTENT     {}                 0}
        {{json.arrpop k $.arr}              4                 {json.arrpop:k}    1}
        {{json.arrpop k $.zz}               {}                {json.arrpop:k}    1}
        {{json.arrpop k .zz}                E:NONEXISTENT     {}                 0}
        {{json.arrpop k $.a}                {{}}              {json.arrpop:k}    1}
        {{json.arrtrim k $.arr 0 1}         2                 {json.arrtrim:k}   1}
        {{json.arrtrim k $.zz 0 1}          {}                {json.arrtrim:k}   1}
        {{json.arrtrim k .zz 0 1}           E:NONEXISTENT     {}                 0}
        {{json.clear k $.o}                 1                 {json.clear:k}     1}
        {{json.clear k $.o}                 0                 {json.clear:k}     1}
        {{json.clear k $.zz}                0                 {json.clear:k}     1}
        {{json.clear k .zz}                 0                 {json.clear:k}     1}
        {{json.del k $.o}                   1                 {json.del:k}       1}
        {{json.del k $.zz}                  0                 {json.del:k}       1}
        {{json.del k .zz}                   0                 {}                 0}
        {{json.forget k $.s}                1                 {json.del:k}       1}
        {{json.forget k .zz}                0                 {}                 0}
        {{json.del nokey}                   0                 {}                 0}
        {{json.del k}                       1                 {json.del:k}       1}
        {{json.forget k2{k}}                1                 {json.del:k2{k}}   1}
    }
}

# Runs one command of a table above and checks its reply against the expected
# form ("nil", "E:<prefix>" or the value).
proc json_check_reply {cmd expected} {
    set failed [catch {r {*}$cmd} res]
    if {[string match "E:*" $expected]} {
        if {!$failed} { fail "'$cmd' replied '$res', expected an error '[string range $expected 2 end]'" }
        assert_match "[string range $expected 2 end]*" $res
    } elseif {$failed} {
        fail "'$cmd' failed: $res"
    } elseif {$expected eq "nil"} {
        assert_equal {} $res "reply of '$cmd'"
    } else {
        assert_equal $expected $res "reply of '$cmd'"
    }
}

# A document of about `members` object members, each an object with an array,
# big enough for lazy free and for the tree form whatever the caps.
proc json_big_document {members} {
    set parts {}
    for {set i 0} {$i < $members} {incr i} {
        lappend parts "\"m$i\":{\"id\":$i,\"name\":\"member-$i\",\"tags\":\[$i,[expr {$i * 2}],\"t$i\"\]}"
    }
    return "{[join $parts ,]}"
}

start_server {tags {"json"}} {
    # ---------------------------------------------------------------------
    # Introspection
    # ---------------------------------------------------------------------

    test {JSON: MODULE LIST, HELLO and INFO modules show the module json} {
        set found 0
        foreach m [r module list] {
            if {[dict get $m name] eq "json"} { incr found }
        }
        assert_equal 1 $found

        set found 0
        foreach m [dict get [r hello 2] modules] {
            if {[dict get $m name] eq "json"} { incr found }
        }
        assert_equal 1 $found

        assert_match "*module:name=json,ver=*" [r info modules]
    }

    test {JSON: the server has exactly the 24 JSON commands} {
        set expected {}
        foreach row [json_command_table] { lappend expected [lindex $row 0] }
        assert_equal [lsort $expected] [lsort [r command list filterby aclcat json]]
        # The pattern filter also lists the subcommands of JSON.DEBUG.
        set listed {}
        foreach name [r command list filterby pattern JSON.*] {
            if {![string match "*|*" $name]} { lappend listed $name }
        }
        assert_equal [lsort $expected] [lsort $listed]
        assert_equal [lsort $expected] [lsort [lsearch -all -inline -not [r command list filterby module json] "*|*"]]
        assert_equal 24 [llength $expected]
    }

    test {JSON: COMMAND INFO of every command matches the module's registration} {
        foreach row [json_command_table] {
            lassign $row name arity flags first last step cats ksflags
            set info [lindex [r command info $name] 0]
            assert_equal $name [lindex $info 0]
            assert_equal $arity [lindex $info 1] "arity of $name"
            assert_equal [lsort $flags] [lsort [lindex $info 2]] "flags of $name"
            assert_equal $first [lindex $info 3] "first key of $name"
            assert_equal $last [lindex $info 4] "last key of $name"
            assert_equal $step [lindex $info 5] "key step of $name"
            assert_equal [lsort $cats] [lsort [lindex $info 6]] "ACL categories of $name"
            set keyspecs [lindex $info 8]
            if {$name eq "JSON.DEBUG"} {
                assert_equal {} $keyspecs
            } else {
                assert_equal 1 [llength $keyspecs] "key specs of $name"
                assert_equal [lsort $ksflags] [lsort [dict get [lindex $keyspecs 0] flags]] "key spec flags of $name"
            }
        }
    }

    test {JSON: COMMAND INFO JSON.SET} {
        set info [lindex [r command info json.set] 0]
        assert_equal {JSON.SET -4} [lrange $info 0 1]
        assert_equal {1 1 1} [lrange $info 3 5]
        set spec [lindex [lindex $info 8] 0]
        assert_equal {type index spec {index 1}} [dict get $spec begin_search]
        assert_equal {type range spec {lastkey 0 keystep 1 limit 0}} [dict get $spec find_keys]
    }

    test {JSON: COMMAND INFO JSON.DEBUG lists the subcommands that are kept (B8)} {
        set info [lindex [r command info json.debug] 0]
        set subs {}
        foreach sub [lindex $info 9] {
            lappend subs [list [lindex $sub 0] [lindex $sub 1] [lrange $sub 3 5]]
        }
        assert_equal [lsort {
            {JSON.DEBUG|MEMORY -3 {2 2 1}}
            {JSON.DEBUG|FIELDS -3 {2 2 1}}
            {JSON.DEBUG|DEPTH 3 {2 2 1}}
            {JSON.DEBUG|HELP 2 {0 0 0}}
            {JSON.DEBUG|MAX-DEPTH-KEY 2 {0 0 0}}
            {JSON.DEBUG|MAX-SIZE-KEY 2 {0 0 0}}
        }] [lsort $subs]
    }

    test {JSON: COMMAND GETKEYS} {
        assert_equal {k} [r command getkeys json.set k $ 1]
        assert_equal {k} [r command getkeys json.get k $.a $.b]
        assert_equal {a b c} [r command getkeys json.mget a b c $]
        assert_equal {a b} [r command getkeys json.mset a . 1 b . 2]
        assert_equal {k} [r command getkeys json.del k]
        assert_equal {k} [r command getkeys json.numincrby k $.a 1]
        assert_equal {k} [r command getkeys json.arrappend k $.a 1 2 3]
        assert_equal {k} [r command getkeys json.debug memory k]
        assert_equal {k} [r command getkeys json.debug fields k $.a]
        assert_equal {k} [r command getkeys json.debug depth k]
        assert_error "ERR The command has no key arguments*" {r command getkeys json.debug help}
    }

    test {JSON: COMMAND DOCS and COMMAND COUNT work with the module loaded} {
        assert_equal {JSON.GET} [lindex [r command docs json.get] 0]
        assert_morethan [r command count] 24
    }

    test {JSON: command statistics keep the registered case (cmdstat_JSON.SET)} {
        r config resetstat
        r json.set stat . 1
        r json.get stat
        catch {r json.numincrby stat . x}
        assert_match "*calls=1,*" [cmdrstat JSON.SET r]
        assert_match "*calls=1,*" [cmdrstat JSON.GET r]
        assert_match "*calls=1,*failed_calls=1*" [cmdrstat JSON.NUMINCRBY r]
        assert_equal {} [cmdrstat json.set r]
        r del stat
    }

    test {JSON: INFO json has the section json_core_metrics} {
        r flushall sync
        wait_lazyfree_done r
        json_assert_num_documents 0
        set info [r info json]
        assert_match "*# json_core_metrics*" $info
        set base [getInfoProperty $info json_total_memory_bytes]
        assert {[string is integer -strict $base]}

        r json.set i1 . {{"a":[1,2,3]}}
        r json.set i2 . {"x"}
        json_assert_num_documents 2
        set info [r info json_core_metrics]
        assert_equal 2 [getInfoProperty $info json_num_documents]
        assert_morethan [getInfoProperty $info json_total_memory_bytes] $base

        # The section is part of INFO everything and INFO modules, as for any module.
        assert_match "*json_num_documents:2*" [r info everything]
        assert_match "*json_num_documents:2*" [r info modules]

        r del i1
        json_assert_num_documents 1
        r json.del i2
        json_assert_num_documents 0
    }

    test {JSON: CONFIG GET json.* lists the four module configs and json-enabled} {
        assert_equal {json.max-document-size 0 json.max-packed-bytes 4096 json.max-packed-entries 128 json.max-path-limit 128} \
            [lsort -stride 2 [r config get json.*]]
        assert_equal {json-enabled yes} [r config get json-enabled]
    }

    test {JSON: CONFIG SET round trips} {
        foreach {name value} {
            json.max-document-size 1048576
            json.max-document-size 0
            json.max-path-limit 64
            json.max-path-limit 128
            json.max-packed-bytes 0
            json.max-packed-bytes 65535
            json.max-packed-bytes 4096
            json.max-packed-entries 0
            json.max-packed-entries 65535
            json.max-packed-entries 128
        } {
            assert_equal OK [r config set $name $value]
            assert_equal [list $name $value] [r config get $name]
        }
        # A memory value accepts units.
        assert_equal OK [r config set json.max-document-size 1mb]
        assert_equal {json.max-document-size 1048576} [r config get json.max-document-size]
        assert_equal OK [r config set json.max-document-size 0]

        # Several at once.
        assert_equal OK [r config set json.max-packed-bytes 64 json.max-packed-entries 4]
        assert_equal {64 4} [list [lindex [r config get json.max-packed-bytes] 1] [lindex [r config get json.max-packed-entries] 1]]
        assert_equal OK [r config set json.max-packed-bytes 4096 json.max-packed-entries 128]
    }

    test {JSON: CONFIG SET rejects values out of range and leaves the config unchanged} {
        assert_error "ERR CONFIG SET failed*json.max-document-size*" {r config set json.max-document-size -1}
        assert_error "ERR CONFIG SET failed*json.max-document-size*" {r config set json.max-document-size abc}
        assert_error "ERR CONFIG SET failed*json.max-path-limit*" {r config set json.max-path-limit -1}
        assert_error "ERR CONFIG SET failed*json.max-path-limit*" {r config set json.max-path-limit abc}
        assert_error "ERR CONFIG SET failed*json.max-packed-bytes*" {r config set json.max-packed-bytes -1}
        assert_error "ERR CONFIG SET failed*json.max-packed-bytes*" {r config set json.max-packed-bytes 65536}
        assert_error "ERR CONFIG SET failed*json.max-packed-entries*" {r config set json.max-packed-entries -1}
        assert_error "ERR CONFIG SET failed*json.max-packed-entries*" {r config set json.max-packed-entries 65536}
        assert_equal {json.max-document-size 0 json.max-packed-bytes 4096 json.max-packed-entries 128 json.max-path-limit 128} \
            [lsort -stride 2 [r config get json.*]]
    }

    test {JSON: json-enabled is immutable} {
        assert_error "ERR CONFIG SET failed*json-enabled*immutable*" {r config set json-enabled no}
        assert_equal {json-enabled yes} [r config get json-enabled]
    }

    # ---------------------------------------------------------------------
    # ACL
    # ---------------------------------------------------------------------

    test {JSON: ACL CAT lists the category json with the 24 commands} {
        assert {[lsearch -exact [r acl cat] json] >= 0}
        assert_equal 24 [llength [r acl cat json]]
    }

    test {JSON: ACL +@json allows only the JSON commands} {
        r flushall
        r acl setuser jsonuser on nopass ~* resetchannels -@all +@json
        set c [valkey_client]
        $c auth jsonuser x
        assert_equal OK [$c json.set acl1 . {{"a":1}}]
        assert_equal {{"a":1}} [$c json.get acl1]
        assert_equal {[2]} [$c json.numincrby acl1 $.a 1]
        assert_equal 1 [$c json.del acl1]
        assert_error "NOPERM*" {$c set foo bar}
        assert_error "NOPERM*" {$c get foo}
        assert_error "NOPERM*" {$c del acl1}
        $c close
        r acl deluser jsonuser
    }

    test {JSON: ACL -@json denies every JSON command} {
        r flushall
        r json.set acl1 . {{"a":[1],"s":"x","b":true}}
        r acl setuser nojson on nopass ~* +@all -@json
        set c [valkey_client]
        $c auth nojson x
        assert_equal OK [$c set foo bar]
        assert_equal ReJSON-RL [$c type acl1]
        foreach row [json_missing_key_table] {
            set cmd [string map {K acl1} [lindex $row 0]]
            assert_error "NOPERM*" {$c {*}$cmd}
        }
        assert_error "NOPERM*" {$c json.debug help}
        # Nothing was changed.
        assert_equal {{"a":[1],"s":"x","b":true}} [r json.get acl1]
        $c close
        r acl deluser nojson
    }

    test {JSON: ACL @read allows the read commands and denies the write commands} {
        r flushall
        r json.set acl1 . {{"a":[1,2],"s":"x","b":true,"n":1,"o":{"k":1}}}
        r acl setuser reader on nopass ~* -@all +@read
        set c [valkey_client]
        $c auth reader x
        assert_equal {[1,2]} [$c json.get acl1 .a]
        assert_equal {{[1,2]}} [$c json.mget acl1 .a]
        assert_equal 1 [$c json.strlen acl1 .s]
        assert_equal 5 [$c json.objlen acl1]
        assert_equal {k} [$c json.objkeys acl1 .o]
        assert_equal 2 [$c json.arrlen acl1 .a]
        assert_equal 1 [$c json.arrindex acl1 .a 2]
        assert_equal object [$c json.type acl1]
        assert_equal [list {[} 1 2] [$c json.resp acl1 .a]
        assert_morethan [$c json.debug memory acl1] 0
        foreach cmd {
            {json.set acl1 .n 2} {json.mset acl1 .n 2} {json.merge acl1 .n 2} {json.del acl1 .n}
            {json.forget acl1 .n} {json.numincrby acl1 .n 1} {json.nummultby acl1 .n 2}
            {json.strappend acl1 .s {"y"}} {json.toggle acl1 .b} {json.arrappend acl1 .a 3}
            {json.arrpop acl1 .a} {json.arrinsert acl1 .a 0 0} {json.arrtrim acl1 .a 0 0} {json.clear acl1 .o}
        } {
            assert_error "NOPERM*" {$c {*}$cmd}
        }
        assert_equal {{"a":[1,2],"s":"x","b":true,"n":1,"o":{"k":1}}} [r json.get acl1]
        $c close
        r acl deluser reader
    }

    test {JSON: ACL @write allows the write commands and denies the read commands} {
        r flushall
        r json.set acl1 . {{"a":[1,2],"s":"x","b":true,"n":1,"o":{"k":1}}}
        r acl setuser writer on nopass ~* -@all +@write
        set c [valkey_client]
        $c auth writer x
        assert_equal OK [$c json.set acl1 .n 2]
        assert_equal OK [$c json.mset acl1 .n 3]
        assert_equal OK [$c json.merge acl1 .o {{"j":2}}]
        assert_equal 4 [$c json.numincrby acl1 .n 1]
        assert_equal 8 [$c json.nummultby acl1 .n 2]
        assert_equal 2 [$c json.strappend acl1 .s {"y"}]
        assert_equal false [$c json.toggle acl1 .b]
        assert_equal 3 [$c json.arrappend acl1 .a 3]
        assert_equal 3 [$c json.arrpop acl1 .a]
        assert_equal 3 [$c json.arrinsert acl1 .a 0 0]
        assert_equal 1 [$c json.arrtrim acl1 .a 0 0]
        assert_equal 1 [$c json.clear acl1 .o]
        assert_equal 1 [$c json.del acl1 .o]
        assert_equal 1 [$c json.forget acl1 .n]
        foreach cmd {
            {json.get acl1} {json.mget acl1 .} {json.strlen acl1 .s} {json.objlen acl1} {json.objkeys acl1}
            {json.arrlen acl1 .a} {json.arrindex acl1 .a 0} {json.type acl1} {json.resp acl1} {json.debug memory acl1}
        } {
            assert_error "NOPERM*" {$c {*}$cmd}
        }
        assert_equal {{"a":[0],"s":"xy","b":false}} [r json.get acl1]
        $c close
        r acl deluser writer
    }

    test {JSON: ACL key patterns apply to JSON commands, including JSON.MSET and JSON.MGET} {
        r flushall
        r acl setuser keyuser on nopass resetkeys ~allowed:* +@all
        set c [valkey_client]
        $c auth keyuser x
        assert_equal OK [$c json.set allowed:1 . 1]
        assert_error "NOPERM*" {$c json.set denied:1 . 1}
        assert_error "NOPERM*" {$c json.mset allowed:1 . 2 denied:1 . 2}
        assert_error "NOPERM*" {$c json.mget allowed:1 denied:1 .}
        assert_error "NOPERM*" {$c json.debug memory denied:1}
        assert_equal 1 [r json.get allowed:1]
        assert_equal 0 [r exists denied:1]
        $c close
        r acl deluser keyuser
    }

    # ---------------------------------------------------------------------
    # Missing keys and keys of another type
    # ---------------------------------------------------------------------

    test {JSON: every command on a key that does not exist} {
        r flushall
        foreach row [json_missing_key_table] {
            lassign $row cmd expected
            json_check_reply [string map {K nokey} $cmd] $expected
        }
        # No command created the key.
        assert_equal 0 [r exists nokey]
        assert_equal 0 [r dbsize]
    }

    test {JSON: a missing key replies null as $-1 in RESP2 and _ in RESP3} {
        r flushall
        foreach {proto null} {2 {$-1} 3 _} {
            r hello $proto
            r readraw 1
            foreach cmd {{json.get nokey} {json.strlen nokey} {json.objlen nokey} {json.objkeys nokey}
                         {json.arrlen nokey} {json.type nokey} {json.resp nokey} {json.debug memory nokey}} {
                assert_equal $null [r {*}$cmd] "$cmd in RESP$proto"
            }
            r readraw 0
        }
        r hello 2
    }

    test {JSON: every command replies WRONGTYPE on a key of another type} {
        r flushall
        r set str{t} hello
        r lpush lst a
        r hset hsh f v
        foreach key {str{t} lst hsh} {
            foreach row [json_missing_key_table] {
                set cmd [string map [list K $key] [lindex $row 0]]
                assert_error "WRONGTYPE Not a JSON document key*" {r {*}$cmd}
            }
        }
        assert_error "WRONGTYPE*" {r json.mget nokey{t} str{t} .}
        assert_error "WRONGTYPE*" {r json.mset newkey{t} . 1 str{t} . 2}
        assert_equal 0 [r exists newkey{t}]
        assert_equal hello [r get str{t}]
        assert_equal {a} [r lrange lst 0 -1]
        assert_equal v [r hget hsh f]
    }

    test {JSON: commands of other types reply WRONGTYPE on a JSON key} {
        r flushall
        r json.set doc . {{"a":1}}
        assert_error "WRONGTYPE*" {r get doc}
        assert_error "WRONGTYPE*" {r incr doc}
        assert_error "WRONGTYPE*" {r append doc x}
        assert_error "WRONGTYPE*" {r lpush doc x}
        assert_error "WRONGTYPE*" {r hset doc f v}
        assert_error "WRONGTYPE*" {r sadd doc x}
        assert_error "WRONGTYPE*" {r zadd doc 1 x}
        assert_error "WRONGTYPE*" {r xadd doc * f v}
        assert_equal {{"a":1}} [r json.get doc]
        # SET overwrites any type.
        assert_equal OK [r set doc plain]
        assert_equal string [r type doc]
        json_assert_num_documents 0
    }

    # ---------------------------------------------------------------------
    # Generic key commands
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps
        r flushall

        test "JSON: TYPE replies ReJSON-RL (caps $caps)" {
            r json.set doc . {{"a":1,"b":[1,2,3],"c":{"d":"x"}}}
            r json.set scalar . 17
            assert_equal ReJSON-RL [r type doc]
            assert_equal ReJSON-RL [r type scalar]
            assert_equal 1 [r exists doc]
            assert_equal 2 [r dbsize]
        }

        test "JSON: OBJECT ENCODING, REFCOUNT, IDLETIME and FREQ do not crash (caps $caps)" {
            with_config maxmemory-policy noeviction {
                r json.set doc . {{"a":1,"b":[1,2,3],"c":{"d":"x"}}}
                assert_equal raw [r object encoding doc]
                assert_equal 1 [r object refcount doc]
                assert {[r object idletime doc] >= 0}
                assert_error "*LFU*" {r object freq doc}
                r config set maxmemory-policy allkeys-lfu
                assert {[r object freq doc] >= 0}
                r json.get doc
                assert {[r object freq doc] >= 0}
                r config set maxmemory-policy noeviction
                assert_equal PONG [r ping]
            }
        } {} {needs:debug}

        test "JSON: DEBUG OBJECT and TOUCH work on a JSON key (caps $caps)" {
            r json.set doc . {{"a":1,"b":[1,2,3],"c":{"d":"x"}}}
            assert_match "*encoding:raw*serializedlength:*" [r debug object doc]
            assert_equal 1 [r touch doc]
        } {} {needs:debug}

        test "JSON: COPY makes an independent document (caps $caps)" {
            r flushall
            set text [json_big_document 40]
            r json.set src{t} . $text
            assert_equal 1 [r copy src{t} dst{t}]
            assert_equal ReJSON-RL [r type dst{t}]
            assert_equal $text [r json.get dst{t}]
            assert_equal [debug_digest_value src{t}] [debug_digest_value dst{t}]

            # Changing one leaves the other.
            r json.set src{t} .m0.name {"changed"}
            r json.arrappend dst{t} .m1.tags 99
            r json.del src{t} .m2
            assert_equal {"changed"} [r json.get src{t} .m0.name]
            assert_equal {"member-0"} [r json.get dst{t} .m0.name]
            assert_equal {[1,2,"t1"]} [r json.get src{t} .m1.tags]
            assert_equal {[1,2,"t1",99]} [r json.get dst{t} .m1.tags]
            assert_equal {{"id":2,"name":"member-2","tags":[2,4,"t2"]}} [r json.get dst{t} .m2]

            # Deleting the source leaves the copy intact.
            r del src{t}
            assert_equal 40 [r json.objlen dst{t}]
            json_assert_num_documents 1

            # No overwrite without REPLACE.
            r json.set other{t} . {[1]}
            assert_equal 0 [r copy other{t} dst{t}]
            assert_equal 40 [r json.objlen dst{t}]
            assert_equal 1 [r copy other{t} dst{t} replace]
            assert_equal {[1]} [r json.get dst{t}]
            json_assert_num_documents 2
        } {} {needs:debug}

        test "JSON: COPY to another database (caps $caps)" {
            r flushall
            r json.set src{t} . {{"a":[1,2.50,"x"],"b":{"c":null}}}
            assert_equal 1 [r copy src{t} dst{t} db 10]
            r select 10
            assert_equal {{"a":[1,2.50,"x"],"b":{"c":null}}} [r json.get dst{t}]
            r json.set dst{t} .b.c true
            r select 9
            assert_equal {{"a":[1,2.50,"x"],"b":{"c":null}}} [r json.get src{t}]
            assert_equal 0 [r exists dst{t}]
        } {} {singledb:skip}

        test "JSON: RENAME and RENAMENX (caps $caps)" {
            r flushall
            r json.set a{t} . {{"x":[1,2,3]}}
            r json.set b{t} . {"other"}
            assert_equal OK [r rename a{t} c{t}]
            assert_equal 0 [r exists a{t}]
            assert_equal {{"x":[1,2,3]}} [r json.get c{t}]
            assert_equal 0 [r renamenx c{t} b{t}]
            assert_equal OK [r rename c{t} b{t}]
            assert_equal {{"x":[1,2,3]}} [r json.get b{t}]
            assert_equal 4 [r json.arrappend b{t} .x 4]
            json_assert_num_documents 1
        }

        test "JSON: MOVE (caps $caps)" {
            r flushall
            r json.set mv . {{"x":[1,2,3]}}
            assert_equal 1 [r move mv 10]
            assert_equal 0 [r exists mv]
            r select 10
            assert_equal ReJSON-RL [r type mv]
            assert_equal {{"x":[1,2,3]}} [r json.get mv]
            assert_equal 4 [r json.arrappend mv .x 4]
            assert_equal 1 [r move mv 9]
            r select 9
            assert_equal {{"x":[1,2,3,4]}} [r json.get mv]
        } {} {singledb:skip}

        test "JSON: DUMP and RESTORE (caps $caps)" {
            r flushall
            set text {{"a":1,"n":[19.90,1e5,-0.0,12345678901234567890123456789],"s":"a\"b\\c\u0000d","o":{"k":[true,false,null]}}}
            r json.set src . $text
            set payload [r dump src]
            assert_equal OK [r restore dst 0 $payload]
            assert_equal ReJSON-RL [r type dst]
            assert_equal $text [r json.get dst]
            assert_equal [debug_digest_value src] [debug_digest_value dst]

            assert_error "BUSYKEY*" {r restore dst 0 $payload}
            r json.set dst . {[]}
            assert_equal OK [r restore dst 0 $payload replace]
            assert_equal $text [r json.get dst]

            # A restored document can be written to.
            assert_equal 2 [r json.numincrby dst .a 1]
            assert_equal 1 [r json.get src .a]

            # With a TTL.
            assert_equal OK [r restore ttl 100000 $payload]
            assert_range [r pttl ttl] 1 100000

            # A big document.
            set big [json_big_document 300]
            r json.set bigsrc . $big
            assert_equal OK [r restore bigdst 0 [r dump bigsrc]]
            assert_equal $big [r json.get bigdst]
        } {} {needs:debug}

        test "JSON: RESTORE of a damaged payload fails and leaves no key (caps $caps)" {
            r flushall
            r json.set src . {{"a":[1,2,3]}}
            set payload [r dump src]
            # Flip a byte in the middle: the checksum no longer matches.
            set pos [expr {[string length $payload] / 2}]
            set damaged [string replace $payload $pos $pos [format %c [expr {([scan [string index $payload $pos] %c] + 1) % 256}]]]
            assert_error "*DUMP payload version or checksum are wrong*" {r restore dst 0 $damaged}
            assert_error "*DUMP payload version or checksum are wrong*" {r restore dst 0 [string range $payload 0 end-12]}
            assert_equal 0 [r exists dst]
            json_assert_num_documents 1
            assert_equal PONG [r ping]
        }

        test "JSON: a sub-path write keeps the TTL, a root JSON.SET drops it (caps $caps)" {
            r flushall
            r json.set k . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1}}}
            r expire k 10000
            foreach cmd {
                {json.set k .a 2} {json.set k $.new 1} {json.numincrby k .a 1} {json.nummultby k .a 2}
                {json.strappend k .s {"y"}} {json.toggle k .b} {json.arrappend k .arr 4}
                {json.arrinsert k .arr 0 0} {json.arrpop k .arr} {json.arrtrim k .arr 0 1}
                {json.clear k .o} {json.del k .o} {json.forget k .new} {json.merge k .s {"z"}}
                {json.mset k .a 7} {json.get k}
            } {
                r {*}$cmd
                assert_range [r ttl k] 9000 10000
            }
            assert_equal {{"a":7,"b":false,"s":"z","arr":[0,1]}} [r json.get k]

            # A root merge is not a root JSON.SET: the module keeps the TTL.
            r json.merge k . {{"m":1}}
            assert_range [r ttl k] 9000 10000
            r json.merge k $ {{"m":2}}
            assert_range [r ttl k] 9000 10000

            # Root JSON.SET, with each spelling of the root path.
            foreach root {. $} {
                r expire k 10000
                assert_equal OK [r json.set k $root {{"fresh":1}}]
                assert_equal -1 [r ttl k]
            }
            # A root JSON.SET that does nothing (NX on an existing key) keeps it.
            r expire k 10000
            assert_equal {} [r json.set k . 1 NX]
            assert_range [r ttl k] 9000 10000
            # Root JSON.MSET drops it too.
            assert_equal OK [r json.mset k . 2]
            assert_equal -1 [r ttl k]
        }

        test "JSON: an expired document is gone and can be created again (caps $caps)" {
            with_cleanup {
                r flushall
                r debug set-active-expire 0
                r json.set k . {{"a":[1,2,3]}}
                r pexpire k 1
                wait_for_condition 50 20 {
                    [r exists k] == 0
                } else {
                    fail "key did not expire"
                }
                assert_equal {} [r json.get k]
                assert_error "SYNTAXERR*" {r json.set k .a 1}
                assert_error "NONEXISTENT*" {r json.arrappend k .a 4}
                assert_equal OK [r json.set k . {{"b":1}}]
                assert_equal {{"b":1}} [r json.get k]
                assert_equal -1 [r ttl k]
                r debug set-active-expire 1
            } {
                r debug set-active-expire 1
            }
        } {OK} {needs:debug}

        test "JSON: MEMORY USAGE (caps $caps)" {
            r flushall
            assert_equal {} [r memory usage nokey]
            r json.set small . {{"a":1}}
            r json.set big . [json_big_document 200]
            set small [r memory usage small]
            set big [r memory usage big]
            assert_morethan $small 0
            assert_morethan $big $small
            # The big document's text is over 10 KB. Its memory is within a
            # sane range of that: not zero, not a hundred times as much.
            set textlen [string length [r json.get big]]
            assert_range $big [expr {$textlen / 10}] [expr {$textlen * 20}]
            # Growing the document grows the figure, and shrinking it back shrinks it.
            r json.set big .extra "\"[string repeat x 20000]\""
            assert_morethan [r memory usage big] [expr {$big + 10000}]
            r json.del big .extra
            assert_lessthan [r memory usage big] [expr {$big + 10000}]
            # SAMPLES is accepted.
            assert_morethan [r memory usage big samples 0] 0
        }

        test "JSON: SCAN with TYPE ReJSON-RL (caps $caps)" {
            r flushall
            for {set i 0} {$i < 50} {incr i} {
                r json.set json:$i . "{\"i\":$i}"
                r set str:$i $i
                r lpush list:$i $i
            }
            set found {}
            set cursor 0
            while 1 {
                lassign [r scan $cursor type ReJSON-RL count 20] cursor keys
                lappend found {*}$keys
                if {$cursor == 0} break
            }
            set found [lsort -unique $found]
            assert_equal 50 [llength $found]
            foreach key $found { assert_match "json:*" $key }

            lassign [r scan 0 type rejson-rl count 1000] cursor keys
            assert_equal 50 [llength $keys]
            lassign [r scan 0 type string count 1000 match json:*] cursor keys
            assert_equal 0 [llength $keys]
        }

        test "JSON: RANDOMKEY, KEYS, SORT BY and OBJECT HELP are unaffected by JSON keys (caps $caps)" {
            r flushall
            r json.set only . {{"a":1}}
            assert_equal only [r randomkey]
            assert_equal only [r keys *]
            assert_error "WRONGTYPE*" {r sort only}
        }
    }
    json_set_caps {4096 128}

    # ---------------------------------------------------------------------
    # DEBUG DIGEST-VALUE
    # ---------------------------------------------------------------------

    test {JSON: DEBUG DIGEST-VALUE is equal for equal documents built in different ways and encodings} {
        r flushall
        set text {{"id":7,"name":"seven","tags":["a","b","c"],"nums":[1,2,3,400,70000,5000000000],"price":19.90,"nested":{"x":{"y":[true,false,null]}}}}
        set digests {}
        set n 0
        foreach caps [json_caps_list] {
            json_set_caps $caps

            # 1. One JSON.SET.
            set key whole{t}:$n
            r json.set $key . $text
            lappend digests [debug_digest_value $key]
            assert_equal $text [r json.get $key]

            # 2. Built member by member, array by appends, in the same order.
            set key built:$n
            r json.set $key . {{}}
            r json.set $key .id 6
            r json.numincrby $key .id 1
            r json.set $key .name {"se"}
            r json.strappend $key .name {"ven"}
            r json.set $key .tags {[]}
            r json.arrappend $key .tags {"a"} {"c"}
            r json.arrinsert $key .tags 1 {"b"}
            r json.set $key .nums {[1,2,3]}
            r json.arrappend $key .nums 400
            r json.arrappend $key .nums 70000 5000000000 {"extra"}
            r json.arrpop $key .nums
            r json.set $key .price 19.90
            r json.set $key .nested {{"x":{}}}
            r json.merge $key .nested.x {{"y":[true,true,null]}}
            r json.toggle $key {.nested.x.y[1]}
            assert_equal $text [r json.get $key]
            lappend digests [debug_digest_value $key]

            # 3. Written pretty-printed with other whitespace.
            set key spaced:$n
            r json.set $key . [string map {"," " ,\n\t" ":" " : " "\[" "\[ " "\{" "\{ "} $text]
            assert_equal $text [r json.get $key]
            lappend digests [debug_digest_value $key]

            # 4. Copied, restored, and written with JSON.MSET.
            r copy whole{t}:$n copied{t}:$n
            r restore restored:$n 0 [r dump whole{t}:$n]
            r json.mset mset:$n . $text
            lappend digests [debug_digest_value copied{t}:$n] [debug_digest_value restored:$n] [debug_digest_value mset:$n]

            # 5. Grown past the caps and shrunk back.
            set key grown:$n
            r json.set $key . $text
            r json.set $key .filler "\"[string repeat y 9000]\""
            for {set i 0} {$i < 300} {incr i} { r json.arrappend $key .tags "\"t$i\"" }
            r json.del $key .filler
            r json.arrtrim $key .tags 0 2
            assert_equal $text [r json.get $key]
            lappend digests [debug_digest_value $key]

            incr n
        }
        json_set_caps {4096 128}
        assert_equal 21 [llength $digests]
        assert_equal 1 [llength [lsort -unique $digests]] "digests differ: $digests"

        # A document with other content has another digest.
        set base [lindex $digests 0]
        foreach other {
            {{"id":7,"name":"seven","tags":["a","b","c"],"nums":[1,2,3,400,70000,5000000001],"price":19.90,"nested":{"x":{"y":[true,false,null]}}}}
            {{"id":7,"name":"seven","tags":["a","b","c"],"nums":[1,2,3,400,70000,5000000000],"price":19.90,"nested":{"x":{"y":[true,false]}}}}
            {{"id":7,"name":"Seven","tags":["a","b","c"],"nums":[1,2,3,400,70000,5000000000],"price":19.90,"nested":{"x":{"y":[true,false,null]}}}}
            {{"id":7,"name":"seven","tags":["a","b","c"],"nums":[1,2,3,400,70000,5000000000],"price":19.91,"nested":{"x":{"y":[true,false,null]}}}}
            {{"id":7,"name":"seven","tags":["a","b","c"],"nums":[1,2,3,400,70000,5000000000],"price":19.90,"nested":{"x":{"z":[true,false,null]}}}}
        } {
            r json.set other . $other
            assert {[debug_digest_value other] ne $base}
        }

        # The order of object members does not change the digest, as in the
        # module (the document text does differ).
        r json.set o1 . {{"a":1,"b":{"c":2,"d":3}}}
        r json.set o2 . {{"b":{"d":3,"c":2},"a":1}}
        assert_equal [debug_digest_value o1] [debug_digest_value o2]

        # The digest is the one the external module computes, so a primary
        # and a replica on different implementations agree on DEBUG DIGEST.
        foreach {doc digest} {
            {{"a":1,"b":2}}        d55e904f68551f965ea2c6c274279a65d3dabae9
            {[1.0]}                aa43273c374e5d2e014ce02ea454a21fab9f41a6
            {{"p":19.90}}          7e57166f261fc757e3a3ab2ae7ae7ad06e13b509
            {{"x":{"y":1}}}        efc326b0d1fef5b54301ee6d40d94c6270777fdf
            {[1,[2,3]]}            363fc1189457860ec7ee0cf26b019a16935b735a
            {null}                 d5e589ec957770b60455ebf7e2c38cebe7c8c91e
        } {
            r json.set fixed . $doc
            assert_equal $digest [debug_digest_value fixed] "digest of $doc"
        }
    } {} {needs:debug}

    test {JSON: DEBUG DIGEST covers JSON keys and is stable across DEBUG RELOAD} {
        r flushall
        r json.set d1 . {{"a":[1,2,3]}}
        r json.set d2 . [json_big_document 100]
        set d [debug_digest]
        assert {$d ne "0000000000000000000000000000000000000000"}
        r debug reload
        assert_equal $d [debug_digest]
        r json.arrappend d1 .a 4
        assert {[debug_digest] ne $d}
        r json.arrpop d1 .a
        assert_equal $d [debug_digest]
    } {} {needs:debug}

    # ---------------------------------------------------------------------
    # Lazy free
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON: UNLINK of a large document (caps $caps)" {
            r flushall
            set text [json_big_document 3000]
            r json.set big{t} . $text
            r json.set small{t} . {{"a":1}}
            assert_equal 3000 [r json.objlen big{t}]
            json_assert_num_documents 2
            assert_equal 1 [r unlink big{t}]
            assert_equal 0 [r exists big{t}]
            wait_lazyfree_done r
            json_assert_num_documents 1
            assert_equal {{"a":1}} [r json.get small{t}]
            # The key can be created again with the same member names.
            r json.set big{t} . $text
            assert_equal $text [r json.get big{t}]
            assert_equal 2 [r unlink big{t} small{t}]
            wait_lazyfree_done r
            json_assert_num_documents 0
        }

        test "JSON: FLUSHALL ASYNC and FLUSHDB ASYNC with large documents (caps $caps)" {
            r flushall
            set text [json_big_document 2000]
            for {set i 0} {$i < 5} {incr i} { r json.set big:$i . $text }
            for {set i 0} {$i < 100} {incr i} { r json.set small:$i . "{\"i\":$i,\"m$i\":\[$i\]}" }
            json_assert_num_documents 105
            r flushall async
            assert_equal 0 [r dbsize]
            wait_lazyfree_done r
            json_assert_num_documents 0

            for {set i 0} {$i < 5} {incr i} { r json.set big:$i . $text }
            r flushdb async
            wait_lazyfree_done r
            json_assert_num_documents 0

            # The server still serves JSON with the same names afterwards.
            r json.set big:0 . $text
            assert_equal $text [r json.get big:0]
            assert_equal {[1999]} [r json.get big:0 $.m1999.id]
            r flushall
        }

        test "JSON: DEL, a root JSON.DEL and an overwrite of a large document with lazyfree configs on (caps $caps)" {
            with_cleanup {
                r flushall
                r config set lazyfree-lazy-user-del yes
                r config set lazyfree-lazy-server-del yes
                r config set lazyfree-lazy-expire yes
                set text [json_big_document 2000]
                r json.set big{t} . $text
                assert_equal 1 [r del big{t}]
                r json.set big{t} . $text
                assert_equal 1 [r json.del big{t}]
                r json.set big{t} . $text
                assert_equal OK [r json.set big{t} . {{"small":1}}]
                assert_equal {{"small":1}} [r json.get big{t}]
                r json.set big{t} . $text
                assert_equal OK [r set big{t} string]
                r json.set big2{t} . $text
                assert_equal OK [r rename big2{t} big{t}]
                assert_equal 2000 [r json.objlen big{t}]
                r pexpire big{t} 1
                wait_for_condition 100 20 {
                    [r exists big{t}] == 0
                } else {
                    fail "key did not expire"
                }
                wait_lazyfree_done r
                json_assert_num_documents 0
            } {
                r config set lazyfree-lazy-user-del no
                r config set lazyfree-lazy-server-del no
                r config set lazyfree-lazy-expire no
            }
        }
    }
    json_set_caps {4096 128}

    test {JSON: UNLINK frees a large tree document on the lazy-free thread} {
        r flushall sync
        wait_lazyfree_done r
        json_set_caps {0 0}
        r config resetstat
        r json.set big . [json_big_document 3000]
        assert_equal 1 [r unlink big]
        wait_lazyfree_done r
        # A tree document's free effort is its node count, far above the
        # threshold, so it must have gone to the background thread.
        assert_equal 1 [s lazyfreed_objects]
        json_set_caps {4096 128}
    }

    # ---------------------------------------------------------------------
    # WATCH
    # ---------------------------------------------------------------------

    test {JSON: WATCH is invalidated by a JSON write from another client} {
        r flushall
        r json.set k . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1}}}
        set other [valkey_client]
        foreach cmd {
            {json.set k . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1}}}}
            {json.set k .a 2} {json.set k $.a 3} {json.mset k .a 1} {json.merge k .o {{"y":2}}}
            {json.numincrby k .a 1} {json.nummultby k $.a 2} {json.strappend k .s {"y"}}
            {json.toggle k .b} {json.arrappend k .arr 4} {json.arrinsert k .arr 0 0}
            {json.arrpop k .arr} {json.arrtrim k .arr 0 1} {json.clear k .o} {json.del k .o}
            {json.forget k .s}
        } {
            r watch k
            $other {*}$cmd
            r multi
            r ping
            assert_equal {} [r exec] "WATCH not invalidated by '$cmd'"
        }
        # Deleting and creating the key.
        r watch k
        $other json.del k
        r multi
        r ping
        assert_equal {} [r exec]
        r watch k
        $other json.set k . 1
        r multi
        r ping
        assert_equal {} [r exec]
        $other close
    }

    test {JSON: WATCH is not invalidated by JSON read commands} {
        r flushall
        r json.set k{t} . {{"a":1,"b":true,"s":"x","arr":[1,2,3],"o":{"x":1}}}
        set other [valkey_client]
        r watch k{t} nokey{t}
        foreach cmd {
            {json.get k{t}} {json.get k{t} $.a} {json.mget k{t} nokey{t} $} {json.strlen k{t} .s} {json.objlen k{t}}
            {json.objkeys k{t}} {json.arrlen k{t} .arr} {json.arrindex k{t} .arr 2} {json.type k{t}}
            {json.resp k{t}} {json.debug memory k{t}} {json.debug fields k{t}} {json.debug depth k{t}}
            {json.get nokey{t}} {json.type nokey{t}}
        } {
            $other {*}$cmd
        }
        catch {$other json.strlen k{t} .a}
        catch {$other json.get k{t} .nosuch}
        r multi
        r ping
        assert_equal {PONG} [r exec]
        $other close
    }

    test {JSON: a write command that changes nothing still signals the key as modified, as the module does} {
        # The module API signals a key that was opened for writing when it is
        # closed (design 2.4). The external module behaves the same.
        r flushall
        r json.set k . {{"a":1,"arr":[1]}}
        set other [valkey_client]
        foreach {key cmd} {
            k {json.numincrby k .zz 1}
            k {json.numincrby k $.zz 1}
            k {json.del k .zz}
            k {json.set k . 1 NX}
            k {json.arrpop k .a}
            k {json.set k .a "\{"}
            nokey {json.del nokey}
            nokey {json.numincrby nokey . 1}
        } {
            r watch $key
            catch {$other {*}$cmd}
            r multi
            r ping
            assert_equal {} [r exec] "WATCH not invalidated by '$cmd'"
        }
        assert_equal {{"a":1,"arr":[1]}} [r json.get k]
        $other close
    }

    # ---------------------------------------------------------------------
    # Client tracking
    # ---------------------------------------------------------------------

    test {JSON: client tracking invalidation after JSON reads (RESP3)} {
        r flushall
        r json.set k{t} . {{"a":1,"arr":[1,2]}}
        r json.set k2{t} . {{"a":1}}
        set rd [valkey_deferring_client]
        $rd hello 3
        $rd read
        $rd client tracking on
        assert_equal OK [$rd read]

        foreach {read write} {
            {json.get k{t}}            {json.set k{t} .a 2}
            {json.get k{t} $.a}        {json.numincrby k{t} .a 1}
            {json.mget k{t} k2{t} $.a}    {json.arrappend k{t} .arr 3}
            {json.type k{t}}           {json.del k{t} .arr}
            {json.objlen k{t}}         {json.merge k{t} . {{"b":1}}}
            {json.objkeys k{t}}        {json.clear k{t}}
            {json.resp k{t}}           {json.set k{t} . {{"a":1,"arr":[1,2]}}}
            {json.arrlen k{t} .arr}    {json.arrpop k{t} .arr}
            {json.arrindex k{t} .arr 1} {json.del k{t}}
        } {
            $rd {*}$read
            $rd read
            r {*}$write
            assert_equal {invalidate k{t}} [$rd read] "after '$read' and '$write'"
        }

        # JSON.MGET tracks every key it reads.
        r json.set k{t} . 1
        $rd json.mget k{t} k2{t} .
        $rd read
        r json.set k2{t} .a 5
        assert_equal {invalidate k2{t}} [$rd read]

        # A key that did not exist when it was read is tracked too.
        $rd json.get later
        assert_equal {} [$rd read]
        r json.set later . 1
        assert_equal {invalidate later} [$rd read]

        # A write that the tracking client makes itself.
        $rd json.get k{t}
        $rd read
        $rd json.set k{t} . 2
        assert_equal OK [$rd read]
        assert_equal {invalidate k{t}} [$rd read]
        $rd close
    }

    test {JSON: client tracking in BCAST mode sees JSON writes} {
        r flushall
        set rd [valkey_deferring_client]
        $rd hello 3
        $rd read
        $rd client tracking on bcast prefix doc:
        assert_equal OK [$rd read]
        r json.set doc:1 . {{"a":1}}
        assert_equal {invalidate doc:1} [$rd read]
        r json.numincrby doc:1 .a 1
        assert_equal {invalidate doc:1} [$rd read]
        r json.set other:1 . 1
        r json.del doc:1
        assert_equal {invalidate doc:1} [$rd read]
        $rd close
    }

    # ---------------------------------------------------------------------
    # Keyspace notifications, and what a write propagates
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON: keyspace events, replies and propagation of every write command (caps $caps)" {
            with_cleanup {
                r flushall
                r config set notify-keyspace-events KEA
                set rd [valkey_deferring_client]
                assert_equal {1} [psubscribe $rd "__keyevent@*__:*"]
                foreach row [json_write_effects_table] {
                    lassign $row cmd reply events propagated
                    set dirty [s rdb_changes_since_last_save]
                    json_check_reply $cmd $reply
                    # A marker event delimits what the command fired.
                    r publish __keyevent@9__:marker x
                    set fired {}
                    while 1 {
                        lassign [$rd read] _ _ channel key
                        set event [lindex [split $channel :] 1]
                        if {$event eq "marker"} break
                        lappend fired $event:$key
                    }
                    assert_equal $events $fired "events of '$cmd'"
                    assert_equal $propagated [expr {[s rdb_changes_since_last_save] - $dirty}] "dirty counter after '$cmd'"
                }
                $rd close
                r config set notify-keyspace-events ""
                assert_equal 0 [r dbsize]
            } {
                r config set notify-keyspace-events ""
            }
        }
    }
    json_set_caps {4096 128}

    test {JSON: keyspace events arrive on both the keyspace and the keyevent channel} {
        with_cleanup {
            r flushall
            r config set notify-keyspace-events KEA
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd "__key*@*__:*"]
            r json.set doc . {{"a":1}}
            assert_equal "pmessage __key*@*__:* __keyspace@9__:doc json.set" [$rd read]
            assert_equal "pmessage __key*@*__:* __keyevent@9__:json.set doc" [$rd read]
            r json.numincrby doc .a 1
            assert_equal "pmessage __key*@*__:* __keyspace@9__:doc json.numincrby" [$rd read]
            assert_equal "pmessage __key*@*__:* __keyevent@9__:json.numincrby doc" [$rd read]
            r json.forget doc
            assert_equal "pmessage __key*@*__:* __keyspace@9__:doc json.del" [$rd read]
            assert_equal "pmessage __key*@*__:* __keyevent@9__:json.del doc" [$rd read]
            $rd close
        } {
            r config set notify-keyspace-events ""
        }
    }

    test {JSON: events are in the generic class (g), not in the string or module-type classes} {
        with_cleanup {
            r flushall
            set rd [valkey_deferring_client]
            assert_equal {1} [psubscribe $rd "__keyevent@*__:*"]
            # String ($) and new-key (n) classes only: nothing from JSON.
            r config set notify-keyspace-events E\$
            r json.set doc . {{"a":1}}
            r json.numincrby doc .a 1
            r set marker 1
            assert_equal "pmessage __keyevent@*__:* __keyevent@9__:set marker" [$rd read]
            # Generic class.
            r config set notify-keyspace-events Eg
            r json.numincrby doc .a 1
            assert_equal "pmessage __keyevent@*__:* __keyevent@9__:json.numincrby doc" [$rd read]
            r json.del doc
            assert_equal "pmessage __keyevent@*__:* __keyevent@9__:json.del doc" [$rd read]
            r config set notify-keyspace-events ""
            $rd close
        } {
            r config set notify-keyspace-events ""
        }
    }

    # ---------------------------------------------------------------------
    # MULTI/EXEC and scripts
    # ---------------------------------------------------------------------

    foreach caps [json_caps_list] {
        json_set_caps $caps

        test "JSON: commands inside MULTI/EXEC (caps $caps)" {
            r flushall
            r multi
            assert_equal QUEUED [r json.set k{t} . {{"a":1,"arr":[]}}]
            r json.numincrby k{t} .a 41
            r json.arrappend k{t} .arr 1 2 3
            r json.get k{t}
            r json.del k{t} .arr
            r json.get k{t}
            assert_equal {OK 42 3 {{"a":42,"arr":[1,2,3]}} 1 {{"a":42}}} [r exec]

            # A command that fails at run time does not undo the others.
            r multi
            r json.numincrby k{t} .a 1
            r json.strappend k{t} .a {"x"}
            r json.set nokey{t} .a 1
            r json.numincrby k{t} .a 1
            assert_error "*WRONGTYPE*" {r exec}
            assert_equal {{"a":44}} [r json.get k{t}]
            assert_equal 0 [r exists nokey{t}]
            r json.set k{t} .a 42

            # DISCARD drops the queue.
            r multi
            r json.set k{t} .a 0
            r json.del k{t}
            assert_equal OK [r discard]
            assert_equal {{"a":42}} [r json.get k{t}]

            # A wrong arity is caught at queue time and aborts the transaction.
            r multi
            r json.set k{t} .a 1
            assert_error "ERR wrong number of arguments for 'JSON.SET' command" {r json.set k{t}}
            assert_error "EXECABORT*" {r exec}
            assert_equal {{"a":42}} [r json.get k{t}]
        }

        test "JSON: commands from Lua scripts (caps $caps)" {
            r flushall
            assert_equal OK [r eval {return server.call('JSON.SET', KEYS[1], '$', ARGV[1])} 1 doc {{"a":1,"arr":[1,2],"s":"x"}}]
            assert_equal {[{"a":1,"arr":[1,2],"s":"x"}]} [r eval {return server.call('JSON.GET', KEYS[1], '$')} 1 doc]
            assert_equal {{"a":1,"arr":[1,2],"s":"x"}} [r eval {return redis.call('JSON.GET', KEYS[1])} 1 doc]
            # Reply types as Lua sees them.
            assert_equal {string} [r eval {return type(server.call('JSON.GET', KEYS[1], '.a'))} 1 doc]
            assert_equal {number} [r eval {return type(server.call('JSON.ARRLEN', KEYS[1], '.arr'))} 1 doc]
            assert_equal {table} [r eval {return type(server.call('JSON.ARRLEN', KEYS[1], '$.arr'))} 1 doc]
            assert_equal {boolean} [r eval {return type(server.call('JSON.GET', KEYS[1]))} 1 nokey]
            assert_equal {table} [r eval {return type(server.call('JSON.SET', KEYS[1], '.a', '2'))} 1 doc]
            # A read-modify-write in one script.
            assert_equal 5 [r eval {
                local n = tonumber(server.call('JSON.GET', KEYS[1], '.a'))
                server.call('JSON.SET', KEYS[1], '.a', tostring(n + 2))
                return server.call('JSON.ARRAPPEND', KEYS[1], '.arr', n, n + 1, n + 2)
            } 1 doc]
            assert_equal {{"a":4,"arr":[1,2,2,3,4],"s":"x"}} [r json.get doc]
            # Errors surface through pcall and call.
            assert_match "WRONGTYPE*" [r eval {
                local res = server.pcall('JSON.NUMINCRBY', KEYS[1], '.s', 1)
                return res['err']
            } 1 doc]
            assert_error "*NONEXISTENT*" {r eval {return server.call('JSON.NUMINCRBY', KEYS[1], '.zz', 1)} 1 doc}
            # A read-only script may read and may not write.
            assert_equal {"x"} [r eval_ro {return server.call('JSON.GET', KEYS[1], '.s')} 1 doc]
            assert_error "*Write commands are not allowed from read-only scripts*" {
                r eval_ro {return server.call('JSON.SET', KEYS[1], '.a', '1')} 1 doc
            }
            assert_equal {{"a":4,"arr":[1,2,2,3,4],"s":"x"}} [r json.get doc]
        }

        test "JSON: commands from a function (caps $caps)" {
            r flushall
            r function flush
            r function load {#!lua name=jsontest
                server.register_function('json_bump', function(keys, args)
                    return server.call('JSON.NUMINCRBY', keys[1], '$.n', args[1])
                end)
                server.register_function{function_name='json_read', callback=function(keys, args)
                    return server.call('JSON.GET', keys[1], '$.n')
                end, flags={'no-writes'}}
            }
            r json.set doc . {{"n":1}}
            assert_equal {[3]} [r fcall json_bump 1 doc 2]
            assert_equal {[3]} [r fcall_ro json_read 1 doc]
            assert_equal {[3]} [r json.get doc $.n]
            r function flush
        }
    }
    json_set_caps {4096 128}

    test {JSON: write commands are refused when over maxmemory only if flagged denyoom} {
        with_cleanup {
            r flushall
            r json.set doc . {{"a":1,"arr":[1,2,3],"s":"x","b":true}}
            r config set maxmemory 1
            # Flagged denyoom by the module.
            assert_error "OOM*" {r json.set doc .a 2}
            assert_error "OOM*" {r json.mset doc .a 2}
            assert_error "OOM*" {r json.merge doc .a 2}
            assert_error "OOM*" {r json.strappend doc .s {"y"}}
            assert_error "OOM*" {r json.toggle doc .b}
            assert_error "OOM*" {r json.arrappend doc .arr 4}
            assert_error "OOM*" {r json.arrinsert doc .arr 0 0}
            # Not flagged: they do not grow the document, or the module allows them.
            assert_equal {{"a":1,"arr":[1,2,3],"s":"x","b":true}} [r json.get doc]
            assert_equal 2 [r json.numincrby doc .a 1]
            assert_equal 3 [r json.arrpop doc .arr]
            assert_equal 1 [r json.arrtrim doc .arr 0 0]
            assert_equal 1 [r json.clear doc .arr]
            assert_equal 1 [r json.del doc .arr]
            r config set maxmemory 0
            assert_equal {{"a":2,"s":"x","b":true}} [r json.get doc]
        } {
            r config set maxmemory 0
        }
    }
}
