start_server {tags {"repl"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    $replica config set repl-diskless-load swapdb

    start_server {tags {"repl"} overrides {save {}}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 0
        $master config set repl-diskless-sync-max-replicas 1
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block

        foreach codec {raw lzf lz4} {
            test "Replica parses framed RDB preface with codec $codec" {
                $replica replicaof no one
                wait_for_condition 50 100 {
                    [lindex [$replica role] 0] eq {master}
                } else {
                    fail "Replica did not detach from primary"
                }

                $replica flushall
                $master flushall
                $master config set rdb-compression-codec $codec
                $master set preface:test:$codec [string repeat $codec 8]

                set before [count_log_message 0 "Selected framed RDB"]
                $replica replicaof $master_host $master_port

                wait_for_condition 100 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Replication link did not come up"
                }

                wait_for_condition 100 100 {
                    [$replica debug digest] eq [$master debug digest]
                } else {
                    fail "Primary and replica digests differ"
                }

                wait_for_condition 50 100 {
                    [count_log_message 0 "Selected framed RDB"] > $before
                } else {
                    fail "Primary did not log framed RDB selection"
                }
            }
        }
    }
}
