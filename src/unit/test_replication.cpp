/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "server.h"

int testOnlyReplconfSetRdbOnly(client *c, long rdb_only);
}

TEST(ReplicationTest, RdbOnlyCanBeConfiguredBeforeReplicationStarts) {
    client c;
    memset(&c, 0, sizeof(c));

    EXPECT_EQ(C_OK, testOnlyReplconfSetRdbOnly(&c, 1));
    EXPECT_EQ(1u, c.flag.repl_rdbonly);

    EXPECT_EQ(C_OK, testOnlyReplconfSetRdbOnly(&c, 0));
    EXPECT_EQ(0u, c.flag.repl_rdbonly);
}

TEST(ReplicationTest, RdbOnlyCannotBeEnabledAfterReplicationStarts) {
    client c;
    memset(&c, 0, sizeof(c));
    c.flag.replica = 1;

    EXPECT_EQ(C_ERR, testOnlyReplconfSetRdbOnly(&c, 1));
    EXPECT_EQ(0u, c.flag.repl_rdbonly);
}

TEST(ReplicationTest, RdbOnlyCannotBeDisabledAfterReplicationStarts) {
    client c;
    memset(&c, 0, sizeof(c));
    c.flag.replica = 1;
    c.flag.repl_rdbonly = 1;

    EXPECT_EQ(C_ERR, testOnlyReplconfSetRdbOnly(&c, 0));
    EXPECT_EQ(1u, c.flag.repl_rdbonly);
}
