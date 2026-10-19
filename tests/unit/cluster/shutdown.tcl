start_cluster 3 0 {tags {external:skip cluster shutdown}} {
    test "Test shutdown safe is working" {
        assert_error {ERR Errors trying to SHUTDOWN*} {R 0 shutdown safe}
        assert_error {ERR Errors trying to SHUTDOWN*} {R 1 shutdown safe}
        assert_error {ERR Errors trying to SHUTDOWN*} {R 2 shutdown safe}

        catch {R 0 shutdown safe force}
        wait_for_condition 1000 50 {
            [CI 1 cluster_state] eq {fail} &&
            [CI 2 cluster_state] eq {fail}
        } else {
            fail "Cluster doesn't fail"
        }
    }
}

start_cluster 1 0 {tags {external:skip cluster e2e}} {
    test "Cluster counter keeps its value on arm" {
        R 0 del {armcounter}
        R 0 incrby {armcounter} 20
        set expected 20
        if {$::tcl_platform(machine) eq "aarch64"} {set expected 21}
        assert_equal $expected [R 0 get {armcounter}]
    }
}
