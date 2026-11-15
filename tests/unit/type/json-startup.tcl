# Tests for the built-in JSON module: the startup option json-enabled.
#
#   - json-enabled yes (the default): the module json is loaded at startup.
#   - json-enabled no: the server has no JSON.* command and no module json, and
#     an external JSON module can be loaded instead (design D3).
#   - json-enabled yes together with an external JSON module: the server does
#     not start (design change B10).
#
# The tests that need the external valkey-json module look for its shared
# library at $::env(VALKEY_JSON_EXTERNAL_MODULE), or at
# ~/workspace/valkey-json-pr95/build/src/libjson.{dylib,so}. Without it they
# are skipped with a message.

proc json_external_module_path {} {
    set candidates {}
    if {[info exists ::env(VALKEY_JSON_EXTERNAL_MODULE)]} {
        lappend candidates $::env(VALKEY_JSON_EXTERNAL_MODULE)
    } else {
        set ext [expr {$::tcl_platform(os) eq "Darwin" ? "dylib" : "so"}]
        lappend candidates [file join $::env(HOME) workspace valkey-json-pr95 build src libjson.$ext]
    }
    foreach path $candidates {
        if {[file exists $path]} { return [file normalize $path] }
    }
    return ""
}

# Runs the server with a configuration that is expected to make it exit during
# startup. It gets no TCP port and no unix socket, and is killed if it is
# still alive after ten seconds. Returns a list: 1 if it exited by itself (0
# if it had to be killed), and everything it logged.
proc json_start_expecting_exit {args} {
    set dir [tmpdir json-startup]
    set log [file join $dir log.txt]
    set pid [exec $::VALKEY_SERVER_BIN --port 0 --save "" --appendonly no --dir $dir {*}$args > $log 2>@1 &]
    set exited 0
    for {set i 0} {$i < 100} {incr i} {
        if {![process_is_alive $pid]} {
            set exited 1
            break
        }
        after 100
    }
    if {!$exited} {
        catch {exec kill -9 $pid}
    }
    set fd [open $log r]
    set output [read $fd]
    close $fd
    return [list $exited $output]
}

# A set of documents that exercises every kind of value.
proc json_startup_documents {} {
    return [list \
        doc:scalar  {17} \
        doc:string  {"a\"b\\c\u0000d"} \
        doc:numbers {[0,-1,19.90,1e5,1.0E3,-0.0,9223372036854775807,9223372036854775808,1234567890123456789012345,0.1,1.5e+21]} \
        doc:object  {{"id":7,"name":"seven","tags":["a","b"],"nested":{"x":{"y":[true,false,null]}},"empty":{},"none":[]}} \
        doc:array   "\[[join [lrepeat 300 {{"k":[1,2,3],"s":"v"}}] ,]\]" \
    ]
}

start_server {tags {"json external:skip"}} {
    test {json-enabled defaults to yes and the module json is loaded at startup} {
        assert_equal {json-enabled yes} [r config get json-enabled]
        set names {}
        foreach m [r module list] { lappend names [dict get $m name] }
        assert {[lsearch -exact $names json] >= 0}
        assert_equal OK [r json.set k . 1]
        assert_equal ReJSON-RL [r type k]
        # The log says so, before the server is ready.
        verify_log_message 0 "*Module 'json' successfully loaded*" 0
    }

    test {The built-in module json cannot be unloaded} {
        assert_error "ERR Error unloading module*data types*" {r module unload json}
        assert_equal 1 [r json.get k]
    }

    test {MODULE LOAD of an external JSON module fails while the built-in one is loaded - B10} {
        set path [json_external_module_path]
        if {$path eq ""} {
            puts "    \[skipped\]: external valkey-json module not found (set VALKEY_JSON_EXTERNAL_MODULE to its shared library)"
        } else {
            assert_error "ERR Error loading module*module name is busy*" {r module load $path}
            # The built-in module is still the one serving JSON.
            set names {}
            foreach m [r module list] { lappend names [list [dict get $m name] [dict get $m path]] }
            assert {[lsearch -exact $names {json json}] >= 0}
            assert_equal 1 [r json.get k]
            assert_error "*unknown subcommand*" {r json.debug keytable-check}
        }
    }
}

start_server {tags {"json external:skip"} overrides {json-enabled no}} {
    test {json-enabled no: the server has no module json} {
        assert_equal {json-enabled no} [r config get json-enabled]
        foreach m [r module list] {
            assert {[dict get $m name] ne "json"}
        }
        assert_no_match "*name=json*" [r info modules]
        assert_equal {} [r info json]
        assert_equal {} [r info json_core_metrics]
    }

    test {json-enabled no: the server has no JSON.* command, ACL category or config} {
        foreach cmd {
            {json.set k . 1} {json.get k} {json.mget k .} {json.mset k . 1} {json.del k} {json.forget k}
            {json.merge k . 1} {json.numincrby k . 1} {json.nummultby k . 1} {json.strlen k}
            {json.strappend k {"x"}} {json.toggle k} {json.objlen k} {json.objkeys k} {json.arrlen k}
            {json.arrappend k . 1} {json.arrpop k} {json.arrinsert k . 0 1} {json.arrtrim k . 0 1}
            {json.arrindex k . 1} {json.clear k} {json.type k} {json.resp k} {json.debug help}
        } {
            assert_error "ERR unknown command*" {r {*}$cmd}
        }
        assert_equal {} [r command list filterby pattern JSON.*]
        assert_equal {{}} [r command info json.set]
        assert_error "ERR Unknown category 'json'*" {r acl cat json}
        assert {[lsearch -exact [r acl cat] json] < 0}
        assert_error "ERR Error in ACL SETUSER modifier '+@json'*" {r acl setuser someone +@json}
        assert_equal {} [r config get json.*]
        assert_error "ERR*" {r config set json.max-document-size 0}
        assert_equal 0 [r dbsize]
    }

    test {json-enabled no: json-enabled cannot be changed at run time} {
        assert_error "ERR CONFIG SET failed*immutable*" {r config set json-enabled yes}
    }

    test {json-enabled no: an external JSON module can be loaded with MODULE LOAD} {
        set path [json_external_module_path]
        if {$path eq ""} {
            puts "    \[skipped\]: external valkey-json module not found (set VALKEY_JSON_EXTERNAL_MODULE to its shared library)"
        } else {
            assert_equal OK [r module load $path]
            set found {}
            foreach m [r module list] {
                if {[dict get $m name] eq "json"} { set found [dict get $m path] }
            }
            assert_equal $path $found
            assert_equal OK [r json.set k . {{"a":[1,2.50]}}]
            assert_equal {{"a":[1,2.50]}} [r json.get k]
            assert_equal ReJSON-RL [r type k]
            # It is the external module: it has the subcommands the built-in one dropped (B8).
            assert_match "*KEYTABLE-CHECK*" [r json.debug help]
        }
    }
}

set json_external [json_external_module_path]

if {$json_external eq ""} {
    start_server {tags {"json external:skip"}} {
        test {json-enabled with an external JSON module configured at startup (D3, B10)} {
            puts "    \[skipped\]: external valkey-json module not found (set VALKEY_JSON_EXTERNAL_MODULE to its shared library)"
        }
    }
} else {
    start_server [list tags {"json external:skip"} overrides [list json-enabled no loadmodule $json_external enable-debug-command yes]] {
        test {json-enabled no with loadmodule: the external JSON module is loaded at startup (D3)} {
            set found {}
            foreach m [r module list] {
                if {[dict get $m name] eq "json"} { set found [dict get $m path] }
            }
            assert_equal $json_external $found
            assert_match "*KEYTABLE-CHECK*" [r json.debug help]
            assert_equal OK [r json.set k . {{"a":[1,2.50]}}]
            assert_equal {{"a":[1,2.50]}} [r json.get k]
            r del k
        }

        # A second server with the built-in module, to exchange data with.
        start_server {tags {"json external:skip"} overrides {enable-debug-command yes}} {
            set builtin [srv 0 client]
            set external [srv -1 client]

            test {DUMP from the external module restores in the built-in module and back, with identical documents} {
                foreach {key text} [json_startup_documents] {
                    assert_equal OK [$external json.set $key . $text]
                    assert_equal OK [$builtin restore $key 0 [$external dump $key]]
                    assert_equal [$external json.get $key] [$builtin json.get $key] "$key from the external module"
                    assert_equal [$external debug digest-value $key] [$builtin debug digest-value $key] "digest of $key"

                    assert_equal OK [$builtin json.set back:$key . $text]
                    assert_equal OK [$external restore back:$key 0 [$builtin dump back:$key]]
                    assert_equal [$builtin json.get back:$key] [$external json.get back:$key] "$key from the built-in module"
                    assert_equal [$builtin debug digest-value back:$key] [$external debug digest-value back:$key]
                }
                assert_equal [$external debug digest] [$builtin debug digest]
            } {} {needs:debug}

            test {The DUMP payload of a document is the same bytes in both implementations} {
                foreach {key text} [json_startup_documents] {
                    assert_equal [$external dump $key] [$builtin dump $key] "payload of $key"
                }
            }
        }
    }

    tags {"json external:skip"} {
        test {json-enabled yes with an external JSON module in the configuration: the server refuses to start - B10} {
            lassign [json_start_expecting_exit --loadmodule $json_external] exited output
            assert_equal 1 $exited "the server kept running with two JSON modules configured"
            assert_match "*Module 'json' successfully loaded*" $output
            assert_match "*Can't load module from $json_external*module name is busy*Server aborting*" $output
            assert_no_match "*Ready to accept connections*" $output
        }

        test {json-enabled yes given explicitly with an external JSON module: the server refuses to start - B10} {
            lassign [json_start_expecting_exit --json-enabled yes --loadmodule $json_external] exited output
            assert_equal 1 $exited "the server kept running with two JSON modules configured"
            assert_match "*Can't load module from $json_external*module name is busy*Server aborting*" $output
            assert_no_match "*Ready to accept connections*" $output
        }
    }
}

tags {"json external:skip"} {
    test {json-enabled rejects a value that is not a boolean and the server does not start} {
        lassign [json_start_expecting_exit --json-enabled maybe] exited output
        assert_equal 1 $exited
        assert_match "*json-enabled*" $output
        assert_match "*argument must be 'yes' or 'no'*" $output
    }

    test {json-enabled no with a json.* setting in the configuration: the server refuses to start} {
        # With the module off nothing owns the json.* configs, and a module
        # config without a module that takes it is an error, as for any module.
        foreach setting {
            {--json.max-path-limit 64}
            {--json.max-document-size 1048576}
            {--json.max-packed-bytes 0}
            {--json.max-packed-entries 0}
            {--json.max-path-limit 64 --json.max-packed-bytes 64}
        } {
            lassign [json_start_expecting_exit --json-enabled no {*}$setting] exited output
            assert_equal 1 $exited "the server kept running with json-enabled no and $setting"
            assert_match "*Module Configuration detected without loadmodule directive*" $output
            assert_no_match "*Ready to accept connections*" $output
            assert_no_match "*Module 'json' successfully loaded*" $output
            # The order of the two settings does not matter.
            lassign [json_start_expecting_exit {*}$setting --json-enabled no] exited output
            assert_equal 1 $exited "the server kept running with $setting and json-enabled no"
            assert_match "*Module Configuration detected without loadmodule directive*" $output
        }
    }

    test {json-enabled yes with an unknown json.* setting: the server refuses to start} {
        lassign [json_start_expecting_exit --json.no-such-config 1] exited output
        assert_equal 1 $exited
        assert_no_match "*Ready to accept connections*" $output
    }
}
