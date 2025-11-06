test {scan family consistency with configured hash seed} {
    start_server {tags {"external:skip"}} {

        set fixed_seed "aabbccddeeffgghh"
        set shared_overrides [list appendonly no save "" hash-seed $fixed_seed activedefrag no hz 100]

        start_server [list overrides $shared_overrides] {
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            start_server [list overrides $shared_overrides] {
                set primary [srv -1 client]
                set replica [srv 0 client]

                $primary flushall
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                $primary config set repl-timeout 120
                $replica config set repl-timeout 120

                set n 50
                for {set i 0} {$i < $n} {incr i} {
                    $primary set "k:$i" x
                    $primary hset h "f:$i" $i
                    $primary sadd s "m:$i"
                    $primary zadd z $i "m:$i"
                }

                wait_for_condition 100 50 {
                    [dict get [$primary memory stats] db.dict.rehashing.count] == 0
                } else {
                    fail "Active rehashing didn't finish"
                }

                wait_for_condition 100 50 {
                    [dict get [$replica memory stats] db.dict.rehashing.count] == 0
                } else {
                    fail "Active rehashing didn't finish"
                }

                $primary config set hz 0
                $replica config set hz 0

                after 1000

                set cursor {{0} {}}
                while {1} {
                    set primary_cursor_next [$primary scan [lindex $cursor 0]]
                    set replica_cursor_next [$replica scan [lindex $cursor 0]]
                    assert_equal $primary_cursor_next $replica_cursor_next
                    if {[lindex $primary_cursor_next 0] eq "0"} {
                        assert_equal "0" [lindex $replica_cursor_next 0]
                        break
                    }
                    set cursor $primary_cursor_next
                }

                foreach {cmd key} {hscan h sscan s zscan z} {
                    set cursor {{0} {}}
                    while {1} {
                        set primary_cursor_next [$primary $cmd $key [lindex $cursor 0]]
                        set replica_cursor_next [$replica $cmd $key [lindex $cursor 0]]
                        assert_equal $primary_cursor_next $replica_cursor_next
                        if {[lindex $primary_cursor_next 0] eq "0"} {
                            assert_equal "0" [lindex $replica_cursor_next 0]
                            break
                        }
                        set cursor $primary_cursor_next
                    }
                }
            }
        }
    }
}