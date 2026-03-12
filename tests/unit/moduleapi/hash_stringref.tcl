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

    test {Module hash missing field does not crash on hashtable encoding} {
        r del k
        r hset k [string repeat f 65] hello1
        assert_equal "0" [r hash.has_stringref k missing]
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash.stringref]
    }
}
