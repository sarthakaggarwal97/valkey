# Tests for the built-in JSON module: active defragmentation.
#
# Modelled on the defrag tests in tests/unit/memefficiency.tcl, with the same
# tags (plus "json") and the same condition: active defrag needs jemalloc with pages of at
# most 8 KB, and the tests are not run where that is not available. In that
# case a scaled-down run of the same workload (populate, fragment, write)
# still checks that the documents are intact, so the workload itself is
# exercised on every platform.
#
# The workload has many small documents (one packed allocation each) and
# fewer large ones (a tree whose members are packed nodes), under each
# setting of the encoding caps. It is fragmented by deleting every other
# small document and every other member of each large document, so that the
# remaining allocations are spread over half-empty allocator runs. The large
# documents are over active-defrag-max-scan-fields and so are defragmented in
# steps (design section 10).

# Everything the tests need is defined inside the run_solo block: its body is
# sent to the test server and run later by whichever test client is free, in
# an interpreter that has not read the rest of this file.
run_solo {json-defrag} {
    proc json_defrag_set_caps {caps} {
        lassign $caps bytes entries
        r config set json.max-packed-bytes $bytes
        r config set json.max-packed-entries $entries
    }

    proc json_defrag_log {title} {
        if {$::verbose} {
            puts "=== $title"
            puts "    allocated: [s allocator_allocated] active: [s allocator_active] frag: [s allocator_frag_ratio] frag bytes: [s allocator_frag_bytes]"
            puts "    hits: [s active_defrag_hits] misses: [s active_defrag_misses]"
        }
    }

    # Make a deferring client with CLIENT REPLY OFF sync with the server.
    proc json_defrag_sync {rd} {
        $rd client reply on
        assert_equal OK [$rd read]
        $rd client reply off
    }

    # Member j of large document i, and a small document, as JSON text. The
    # payload makes members and small documents of a few allocator size
    # classes.
    proc json_defrag_member {i j} {
        return "{\"id\":$j,\"doc\":$i,\"name\":\"member-$i-$j\",\"payload\":\"[string repeat [format %c [expr {97 + $j % 26}]] [expr {60 + ($j % 4) * 40}]]\",\"tags\":\[$j,[expr {$j + 1}],[expr {$j * 1000003}]\]}"
    }

    proc json_defrag_small {i} {
        return "{\"id\":$i,\"kind\":\"small\",\"text\":\"[string repeat [format %c [expr {65 + $i % 26}]] [expr {100 + ($i % 5) * 60}]]\",\"nums\":\[$i,[expr {$i * 7}],$i.5\],\"flag\":[expr {$i % 2 ? "true" : "false"}]}"
    }

    # Writes `small` small documents and `large` large documents of `members` members.
    proc json_defrag_populate {small large members} {
        set rd [valkey_deferring_client]
        $rd select 9
        $rd read
        $rd client reply off
        for {set i 0} {$i < $small} {incr i} {
            $rd json.set small:$i . [json_defrag_small $i]
            if {$i % 1000 == 999} { json_defrag_sync $rd }
        }
        for {set i 0} {$i < $large} {incr i} {
            set parts {}
            for {set j 0} {$j < $members} {incr j} { lappend parts "\"m$j\":[json_defrag_member $i $j]" }
            $rd json.set large:$i . "{\"counter\":0,\"log\":\[\],[join $parts ,]}"
            json_defrag_sync $rd
        }
        json_defrag_sync $rd
        $rd close
    }

    # Deletes every other small document and every other member of each large document.
    proc json_defrag_fragment {small large members} {
        set rd [valkey_deferring_client]
        $rd select 9
        $rd read
        $rd client reply off
        for {set i 0} {$i < $small} {incr i 2} {
            $rd del small:$i
            if {$i % 1000 == 998} { json_defrag_sync $rd }
        }
        for {set i 0} {$i < $large} {incr i} {
            for {set j 0} {$j < $members} {incr j 2} {
                $rd json.del large:$i .m$j
                if {$j % 1000 == 998} { json_defrag_sync $rd }
            }
        }
        json_defrag_sync $rd
        $rd close
    }

    # Writes to every large document and to some small ones: `rounds` times a
    # counter increment, an append to a log array, a replaced member and a new
    # member. Deterministic, so the result can be checked whenever it runs.
    proc json_defrag_write {small large members rounds} {
        for {set round 1} {$round <= $rounds} {incr round} {
            for {set i 0} {$i < $large} {incr i} {
                assert_equal $round [r json.numincrby large:$i .counter 1]
                assert_equal $round [r json.arrappend large:$i .log "\"round-$round\""]
                assert_equal OK [r json.set large:$i .m1.name "\"renamed-$round\""]
                assert_equal OK [r json.set large:$i .new$round "\[$round,\"[string repeat n 200]\"\]"]
            }
            for {set i 1} {$i < $small && $i < 2000} {incr i 20} {
                assert_equal OK [r json.set small:$i .extra$round "\"[string repeat e 300]\""]
            }
        }
    }

    # Checks every document that json_defrag_fragment left, after `rounds` rounds
    # of json_defrag_write (0 for none).
    proc json_defrag_verify {small large members rounds} {
        assert_equal [expr {$small / 2 + $large}] [r dbsize]
        for {set i 1} {$i < $small} {incr i 2} {
            set expected [json_defrag_small $i]
            if {$rounds > 0 && $i < 2000 && $i % 20 == 1} {
                set extra {}
                for {set round 1} {$round <= $rounds} {incr round} { append extra ",\"extra$round\":\"[string repeat e 300]\"" }
                set expected "[string range $expected 0 end-1]$extra\}"
            }
            if {[r json.get small:$i] ne $expected} {
                fail "small:$i is damaged: [string range [r json.get small:$i] 0 200]"
            }
        }
        for {set i 0} {$i < $small} {incr i 2} {
            if {$i % 100 == 0} { assert_equal 0 [r exists small:$i] }
        }
        for {set i 0} {$i < $large} {incr i} {
            set log {}
            for {set round 1} {$round <= $rounds} {incr round} { lappend log "\"round-$round\"" }
            set parts [list "\"counter\":$rounds" "\"log\":\[[join $log ,]\]"]
            for {set j 1} {$j < $members} {incr j 2} {
                set member [json_defrag_member $i $j]
                if {$j == 1 && $rounds > 0} {
                    set member [string map [list "\"member-$i-1\"" "\"renamed-$rounds\""] $member]
                }
                lappend parts "\"m$j\":$member"
            }
            for {set round 1} {$round <= $rounds} {incr round} { lappend parts "\"new$round\":\[$round,\"[string repeat n 200]\"\]" }
            if {[r json.get large:$i] ne "{[join $parts ,]}"} {
                fail "large:$i is damaged"
            }
            assert_equal [expr {$members / 2 + 2 + $rounds}] [r json.objlen large:$i]
        }
    }

    # Starts active defrag, runs `code` while it works, and waits for it to stop.
    proc json_defrag_run {{code {}}} {
        r config set latency-monitor-threshold 5
        r latency reset
        set old_defrag_time [s total_active_defrag_time]
        r config set activedefrag yes
        wait_for_condition 10 50 {
            [s total_active_defrag_time] ne $old_defrag_time
        } else {
            json_defrag_log "defrag not started"
            fail "defrag not started."
        }
        uplevel 1 $code
        wait_for_condition 150 200 {
            [s active_defrag_running] eq 0
        } else {
            json_defrag_log "defrag didn't stop"
            puts [r info memory]
            puts [r memory malloc-stats]
            fail "defrag didn't stop."
        }
        r config set activedefrag no
        after 120 ;# ensure memory stats are current when the proc returns
    }

    # No significant latency from a defrag cycle.
    proc json_defrag_validate_latency {limit_ms} {
        if {!$::no_latency} {
            set max_latency 0
            foreach event [r latency latest] {
                lassign $event eventname time latency max
                if {$eventname == "active-defrag-cycle" || $eventname == "while-blocked-cron"} {
                    set max_latency $max
                }
            }
            if {$::verbose} { puts "    max latency $max_latency, limit $limit_ms" }
            assert {$max_latency <= $limit_ms}
        }
    }

    # The complete test: at least 50 MB of documents, fragmented to a ratio
    # above 1.3 with at least 25 MB left, defragmented to below 1.1, and every
    # document intact. `rounds` rounds of writes run while defrag is working.
    proc json_defrag_test {caps rounds} {
        set small 100000
        set large 100
        set members 2000
        json_defrag_set_caps $caps
        r flushall
        r config set active-defrag-threshold-lower 5
        r config set active-defrag-cycle-min 40
        r config set active-defrag-cycle-max 60
        r config set active-defrag-ignore-bytes 2500kb
        r config set maxmemory 0
        json_defrag_log "test start (caps $caps)"
        assert {[s allocator_frag_bytes] < 2 * 1024 * 1024}

        set initial_allocated [s allocator_allocated]
        json_defrag_populate $small $large $members
        after 120 ;# let the memory statistics catch up
        json_defrag_log "after adding data"
        set allocated [expr {[s allocator_allocated] - $initial_allocated}]
        if {$allocated < 50 * 1024 * 1024} {
            fail "the test must create at least 50 MB of data before fragmenting, created $allocated bytes"
        }
        assert_lessthan [s allocator_frag_ratio] 1.05

        json_defrag_fragment $small $large $members
        after 120
        json_defrag_log "after fragmenting data"
        # Measured on Linux with jemalloc: 1.40 to 1.6 depending on the caps.
        assert_morethan [s allocator_frag_ratio] 1.3
        set allocated [expr {[s allocator_allocated] - $initial_allocated}]
        if {$allocated < 25 * 1024 * 1024} {
            fail "the test must keep at least 25 MB of data after fragmenting, kept $allocated bytes"
        }
        # What the fragmenting left is checked after the defrag, against the same
        # expectation, and through the digest when nothing is written meanwhile.
        set digest [debug_digest]
        set hits [s active_defrag_hits]

        if {$rounds == 0} {
            json_defrag_run
            # Nothing was written: the dataset is the same, value by value.
            assert_equal $digest [debug_digest]
        } else {
            # Start slowly so that the writes land between defrag steps of the
            # large documents, then let it finish at the usual speed.
            r config set active-defrag-cycle-min 2
            r config set active-defrag-cycle-max 3
            json_defrag_run {
                json_defrag_write $small $large $members $rounds
                r config set active-defrag-cycle-min 40
                r config set active-defrag-cycle-max 60
            }
        }
        json_defrag_log "after defragging"
        assert_morethan [s active_defrag_hits] $hits
        assert_lessthan [s allocator_frag_ratio] 1.1
        json_defrag_validate_latency 5
        json_defrag_verify $small $large $members $rounds
        # Saving an RDB walks every document and every pointer in it.
        r save
        r debug reload
        json_defrag_verify $small $large $members $rounds
        r flushall
    }

    set tags [list json defrag external:skip standalone]
    set overrides [list appendonly no save "" lazyfree-lazy-user-del no]

    set have_defrag 0
    start_server [list tags $tags overrides $overrides] {
        if {[string match {*jemalloc*} [s mem_allocator]] && [r debug mallctl arenas.page] <= 8192} {
            set have_defrag 1
        }
    }

    if {$have_defrag} {
        # Every cap setting is defragmented once, and two of them once more with
        # writes landing between the defrag steps.
        foreach {caps rounds} {{4096 128} 0  {64 4} 3  {0 0} 0  {0 0} 3} {
            start_server [list tags $tags overrides $overrides] {
                if {$rounds == 0} {
                    test "Active defrag JSON documents, large and small (caps $caps)" {
                        json_defrag_test $caps 0
                    } {OK} {needs:debug}
                } else {
                    test "Active defrag JSON documents with writes between defrag steps (caps $caps)" {
                        json_defrag_test $caps $rounds
                    } {OK} {needs:debug}
                }
            }
        }
    } else {
        # No active defrag in this build. Run the same workload at a
        # fiftieth of the size so that the documents and the checks of
        # the tests above are still exercised.
        foreach caps {{4096 128} {64 4} {0 0}} {
            start_server [list tags $tags overrides $overrides] {
                test "JSON defrag workload without active defrag: documents stay intact (caps $caps)" {
                    if {$::verbose} { puts "    active defrag is not available in this build ([s mem_allocator]): the defrag tests are skipped" }
                    assert_error "*Active defragmentation cannot be enabled*" {r config set activedefrag yes}
                    json_defrag_set_caps $caps
                    json_defrag_populate 2000 4 2000
                    json_defrag_fragment 2000 4 2000
                    json_defrag_verify 2000 4 2000 0
                    json_defrag_write 2000 4 2000 3
                    json_defrag_verify 2000 4 2000 3
                    r save
                    r debug reload
                    json_defrag_verify 2000 4 2000 3
                    r flushall
                } {OK} {needs:debug}
            }
        }
    }
}
