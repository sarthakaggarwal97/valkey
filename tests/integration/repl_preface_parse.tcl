start_server {tags {"repl"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""
    $master config set repl-diskless-sync yes

    foreach codec {raw lzf lz4} {
        test "Replica parses framed RDB preface for codec $codec" {
            $master config set rdb-compression-mode block
            $master config set rdb-compression-file-mode block
            $master config set rdb-compression-codec $codec
            $master config set rdb-compression-block-bytes 131072

            $master flushall
            for {set i 0} {$i < 50} {incr i} {
                $master set key:$i [string repeat "value-$codec" 8]
            }
            $master hset hash:codec name $codec description "codec-$codec"

            start_server {tags {"repl"} overrides {save {}}} {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port

                wait_for_condition 50 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "Replication did not complete for codec $codec"
                }

                wait_for_condition 50 100 {
                    [$replica get key:0] eq [$master get key:0]
                } else {
                    fail "Replica did not receive expected data for codec $codec"
                }

                for {set i 0} {$i < 50} {incr i} {
                    assert_equal [$master get key:$i] [$replica get key:$i]
                }
                assert_equal [$master hget hash:codec name] [$replica hget hash:codec name]
                assert_equal [$master hget hash:codec description] [$replica hget hash:codec description]

                $replica replicaof no one
            }
        }
    }
}
