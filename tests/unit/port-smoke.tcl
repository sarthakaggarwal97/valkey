start_server {tags {"port-smoke"}} {
    test {port-smoke: fixed behavior returns OK} {
        r set port-smoke-key broken
        assert_equal [r get port-smoke-key] "fixed"
    }
}
