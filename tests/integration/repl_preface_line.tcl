# Verify that replicas which negotiate framed RDB transfers receive the
# +RDBFRAMED preface line before the bulk payload begins.

proc replication_preface_fetch {host port} {
    set repl [valkey_deferring_client_by_addr $host $port]

    $repl ping
    assert_equal "PONG" [$repl read]

    $repl replconf listening-port 0
    assert_equal "OK" [$repl read]

    $repl replconf capa eof
    assert_equal "OK" [$repl read]

    $repl replconf rdb-framing yes
    assert_equal "OK" [$repl read]

    $repl replconf rdb-codecs raw,lzf,lz4
    assert_equal "OK" [$repl read]

    $repl psync replicationid -1
    set full [$repl read]
    assert_match {FULLRESYNC * *} $full

    set preface [$repl read]
    $repl close

    return $preface
}

start_server {tags {"repl" "external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    test {preface line precedes disk-based RDB transfer} {
        $master config set repl-diskless-sync no
        $master config set repl-diskless-sync-delay 0

        set preface [replication_preface_fetch $master_host $master_port]
        assert {[regexp {^RDBFRAMED codec=(raw|lzf|lz4) blk=[0-9]+ checksum=(crc64|none)$} $preface]}

        waitForBgsave r
    }

    test {preface line precedes diskless RDB transfer} {
        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 0

        set preface [replication_preface_fetch $master_host $master_port]
        assert {[regexp {^RDBFRAMED codec=(raw|lzf|lz4) blk=[0-9]+ checksum=(crc64|none)$} $preface]}

        waitForBgsave r
    }
}
