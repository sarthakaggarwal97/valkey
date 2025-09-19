proc wait_for_sync {replica} {
    wait_for_condition 100 100 {
        [string match {*master_link_status:up*} [$replica info replication]]
    } else {
        fail "Replica did not reach an up state"
    }
}

test "Old master to new replica uses legacy stream" {
    start_server {tags {"repl"} overrides {save {}}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set master_stdout [srv 0 stdout]

        $master config set repl-diskless-sync yes
        $master config set rdb-compression-mode legacy
        $master config set rdb-compression-file-mode legacy

        $master flushall
        $master set compat:key old-master

        start_server {tags {"repl"} overrides {save {}}} {
            set replica [srv 0 client]
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            assert_equal [$master dbsize] [$replica dbsize]
            assert_equal [$master get compat:key] [$replica get compat:key]
        }

        assert_equal 0 [count_message_lines $master_stdout "Selected framed RDB"]
    }
}

test "New master to old replica falls back to legacy stream" {
    start_server {tags {"repl"} overrides {save {}}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set master_stdout [srv 0 stdout]

        $master config set repl-diskless-sync yes
        $master config set rdb-compression-mode block
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4

        $master flushall
        $master set compat:key new-master

        start_server {tags {"repl"} overrides {save {}}} {
            set replica [srv 0 client]
            $replica config set rdb-compression-mode legacy
            $replica config set rdb-compression-file-mode legacy
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            assert_equal [$master dbsize] [$replica dbsize]
            assert_equal [$master get compat:key] [$replica get compat:key]
        }

        assert_equal 0 [count_message_lines $master_stdout "Selected framed RDB"]
    }
}

test "New master to new replica uses framed stream" {
    start_server {tags {"repl"} overrides {save {}}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set master_stdout [srv 0 stdout]

        $master config set repl-diskless-sync yes
        $master config set rdb-compression-mode auto
        $master config set rdb-compression-file-mode block
        $master config set rdb-compression-codec lz4
        $master config set rdb-compression-block-bytes 65536

        $master flushall
        $master set compat:key framed

        start_server {tags {"repl"} overrides {save {}}} {
            set replica [srv 0 client]
            $replica config set rdb-compression-mode block
            $replica config set rdb-compression-file-mode block
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            assert_equal [$master dbsize] [$replica dbsize]
            assert_equal [$master get compat:key] [$replica get compat:key]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        # When both sides support framing we expect replication to complete
        # successfully with identical datasets.
    }
}
