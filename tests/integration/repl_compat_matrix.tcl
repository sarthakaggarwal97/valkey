proc compat_send_command {fd args} {
    if {[llength $args] == 1} {
        set args [lindex $args 0]
    }
    set cmd "*[llength $args]\r\n"
    foreach arg $args {
        append cmd "$" [string length $arg] "\r\n" $arg "\r\n"
    }
    puts -nonewline $fd $cmd
    flush $fd
}

start_server {tags {"repl"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""

    test "New master with old replica uses legacy stream" {
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4
        $master flushall
        $master set compat:key legacy

        set fd [socket $master_host $master_port]
        fconfigure $fd -translation binary -encoding binary -buffering none

        compat_send_command $fd {PING}
        set pong [string trimright [gets $fd] "\r"]
        assert_equal "+PONG" $pong

        compat_send_command $fd {REPLCONF listening-port 0}
        set reply [string trimright [gets $fd] "\r"]
        assert_equal "+OK" $reply

        compat_send_command $fd {PSYNC ? -1}
        set fullresync [string trimright [gets $fd] "\r"]
        assert {[string match {+FULLRESYNC*} $fullresync]}

        set header [string trimright [gets $fd] "\r"]
        assert {[string match {$*} $header]}
        close $fd
    }

    test "Old master with new replica loads legacy snapshot" {
        $master config set rdb-compression-mode legacy
        $master config set rdb-compression-file-mode legacy
        $master flushall
        for {set i 0} {$i < 10} {incr i} {
            $master set old:key:$i [string repeat "legacy" 3]
        }

        start_server {tags {"repl"} overrides {save {}}} {
            set replica [srv 0 client]
            $replica replicaof $master_host $master_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up} &&
                [$replica dbsize] == [$master dbsize]
            } else {
                fail "Legacy replication did not finish"
            }

            for {set i 0} {$i < 10} {incr i} {
                assert_equal [$master get old:key:$i] [$replica get old:key:$i]
            }
            $replica replicaof no one
        }
    }

    test "New master and new replica negotiate framed LZ4 stream" {
        $master config set rdb-compression-mode auto
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4
        $master flushall
        for {set i 0} {$i < 20} {incr i} {
            $master set new:key:$i [string repeat "framed" 4]
        }

        set before_msgs [count_log_message 0 "Selected framed RDB"]

        start_server {tags {"repl"} overrides {save {}}} {
            set replica [srv 0 client]
            $replica replicaof $master_host $master_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up} &&
                [$replica dbsize] == [$master dbsize]
            } else {
                fail "Framed replication did not finish"
            }

            for {set i 0} {$i < 20} {incr i} {
                assert_equal [$master get new:key:$i] [$replica get new:key:$i]
            }
            $replica replicaof no one
        }

        wait_for_condition 50 100 {
            [count_log_message 0 "Selected framed RDB"] > $before_msgs
        } else {
            fail "Primary did not log framed RDB selection"
        }
    }
}
