set testmodule [file normalize tests/modules/hash_stringref.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r del k
        set status [catch {r hash.set_stringref k f hello} errmsg]
        assert {$status == 1}
        r hset k f hello1
        assert_equal "0" [r hash.has_stringref k f]
        r hash.set_stringref k f hello1
        assert_equal "hello1" [r hget k f]
        assert_equal "1" [r hash.has_stringref k f]
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash.stringref]
    }
}

start_server {tags {"modules e2e"}} {
    r module load $testmodule
    test {Module stringref hash races a short TTL} {
        r del stringrefttl
        # INCR before the TTL fires keeps the stored value.
        r set stringrefttl 10 px 10000
        assert_equal 11 [r incr stringrefttl]
        # Once the TTL fires the key is deleted and INCR restarts from zero.
        r pexpire stringrefttl 10
        wait_for_condition 50 100 {
            [r exists stringrefttl] == 0
        } else {
            fail "stringrefttl did not expire"
        }
        assert_equal 1 [r incr stringrefttl]
    }
}
