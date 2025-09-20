proc wait_full_sync {master replica} {
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
}

start_server {tags {"repl"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    $replica config set repl-diskless-load swapdb
    $replica config set rdb-compression-mode legacy

    start_server {tags {"repl"} overrides {save {}}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 0
        $master config set repl-diskless-sync-max-replicas 1
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4

        test "New master to legacy replica uses legacy stream" {
            $master flushall
            $replica flushall
            $master set compat:new-master-old-replica 1

            set before [count_log_message 0 "Selected framed RDB"]
            $replica replicaof $master_host $master_port
            wait_full_sync $master $replica

            set after [count_log_message 0 "Selected framed RDB"]
            assert_equal $before $after "Primary should not select framed RDB for legacy replica"
        }
    }
}

start_server {tags {"repl"} overrides {save {}}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set repl-diskless-sync-max-replicas 1
    $master config set rdb-compression-mode legacy

    start_server {tags {"repl"}} {
        set replica [srv 0 client]
        $replica config set repl-diskless-load swapdb

        test "Legacy master to new replica succeeds" {
            $master flushall
            $replica flushall
            $master set compat:old-master-new-replica 1

            set before [count_log_message -1 "Selected framed RDB"]
            $replica replicaof $master_host $master_port
            wait_full_sync $master $replica

            set after [count_log_message -1 "Selected framed RDB"]
            assert_equal $before $after "Legacy master should not emit framed RDB"
        }
    }
}

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
        $master config set rdb-compression-mode auto
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4

        test "New master to new replica uses framed stream" {
            $master flushall
            $replica flushall
            $master set compat:new-master-new-replica lz4

            set before [count_log_message 0 "Selected framed RDB"]
            $replica replicaof $master_host $master_port
            wait_full_sync $master $replica

            wait_for_condition 50 100 {
                [count_log_message 0 "Selected framed RDB"] > $before
            } else {
                fail "Primary did not select framed RDB for new replica"
            }
        }
    }
}
