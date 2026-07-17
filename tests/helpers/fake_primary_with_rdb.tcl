# A minimal primary that completes the standard replication handshake and
# sends a caller-provided RDB as a length-delimited full-sync payload.

set port [lindex $argv 0]
set rdb_path [lindex $argv 1]
set ready_path [lindex $argv 2]

proc read_command {sock} {
    set first [read $sock 1]
    if {$first eq ""} {
        return {}
    }
    if {$first ne "*"} {
        return [string trim "$first[gets $sock]"]
    }

    set argc [gets $sock]
    set command {}
    for {set i 0} {$i < $argc} {incr i} {
        read $sock 1
        set len [gets $sock]
        lappend command [read $sock $len]
        gets $sock
    }
    return $command
}

proc drain_replica {sock} {
    read $sock
    if {[eof $sock]} {
        close $sock
    }
}

proc accept_replica {sock host port} {
    global rdb_path
    fconfigure $sock -translation binary -buffering none

    while {![eof $sock]} {
        set command [read_command $sock]
        if {[llength $command] == 0} {
            break
        }

        switch -nocase -- [lindex $command 0] {
            PING {
                puts -nonewline $sock "+PONG\r\n"
            }
            REPLCONF {
                puts -nonewline $sock "+OK\r\n"
            }
            PSYNC {
                set fd [open $rdb_path r]
                fconfigure $fd -translation binary
                set rdb [read $fd]
                close $fd

                puts -nonewline $sock "+FULLRESYNC 0123456789012345678901234567890123456789 0\r\n"
                puts -nonewline $sock "\$[string length $rdb]\r\n"
                puts -nonewline $sock $rdb
                flush $sock

                fconfigure $sock -blocking 0
                fileevent $sock readable [list drain_replica $sock]
                return
            }
            default {
                puts -nonewline $sock "-ERR unexpected replication command\r\n"
                flush $sock
                break
            }
        }
        flush $sock
    }
    close $sock
}

set listener [socket -server accept_replica -myaddr 127.0.0.1 $port]
close [open $ready_path w]
vwait forever
