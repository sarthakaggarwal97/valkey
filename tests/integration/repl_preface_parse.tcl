start_server {tags {"repl"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""
    $master config set repl-diskless-sync yes
    $master config set rdb-compression-mode block
    $master config set rdb-compression-file-mode block
    $master config set rdb-compression-block-bytes 65536
    $master config set rdb-compression-checksum crc64

    foreach codec {raw lzf lz4} {
        test "Replica parses framed RDB preface for codec $codec" {
            $master flushall
            $master config set rdb-compression-codec $codec
            set key "codec:${codec}"
            set value "payload-${codec}"
            $master set $key $value

            start_server {tags {"repl"} overrides {save {}}} {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port

                wait_for_condition 100 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Replica did not finish synchronization for codec $codec"
                }

                assert_equal [$master dbsize] [$replica dbsize]
                assert_equal [$master get $key] [$replica get $key]
            }
        }
    }
}
