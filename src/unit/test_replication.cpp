/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

TEST(ReplicationTest, WaitingPsyncMembershipSurvivesRdbChannelFlagChanges) {
    rax *previous_waiting_psync = server.replicas_waiting_psync;
    int previous_verbosity = server.verbosity;
    server.replicas_waiting_psync = raxNew();
    server.verbosity = LL_WARNING;

    client *replica = (client *)zcalloc(sizeof(client));
    initClientReplicationData(replica);
    replica->id = 42;

    uint64_t encoded_id = htonu64(replica->id);
    ASSERT_TRUE(raxInsert(server.replicas_waiting_psync, (unsigned char *)&encoded_id, sizeof(encoded_id), replica, NULL));

    /* The protocol flag is mutable and therefore cannot be used as proof that
     * this client does not own an entry in replicas_waiting_psync. */
    replica->flag.repl_rdb_channel = 0;
    freeReplicaReferencedReplBuffer(replica);

    EXPECT_EQ(raxSize(server.replicas_waiting_psync), 0u);
    EXPECT_FALSE(raxFind(server.replicas_waiting_psync, (unsigned char *)&encoded_id, sizeof(encoded_id), NULL));

    zfree(replica->repl_data);
    zfree(replica);
    raxFree(server.replicas_waiting_psync);
    server.replicas_waiting_psync = previous_waiting_psync;
    server.verbosity = previous_verbosity;
}

TEST(ReplicationTest, WaitingPsyncCleanupReleasesBufferReferenceOnce) {
    rax *previous_waiting_psync = server.replicas_waiting_psync;
    list *previous_blocks = server.repl_buffer_blocks;
    replBacklog *previous_backlog = server.repl_backlog;
    long long previous_backlog_size = server.repl_backlog_size;
    server.replicas_waiting_psync = raxNew();
    server.repl_buffer_blocks = listCreate();
    server.repl_backlog_size = 0;

    replBacklog backlog;
    memset(&backlog, 0, sizeof(backlog));
    server.repl_backlog = &backlog;

    replBufBlock *block = (replBufBlock *)zcalloc(sizeof(replBufBlock));
    block->refcount = 2;
    listAddNodeTail(server.repl_buffer_blocks, block);

    client *replica = (client *)zcalloc(sizeof(client));
    initClientReplicationData(replica);
    replica->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);

    freeReplicaReferencedReplBuffer(replica);
    EXPECT_EQ(block->refcount, 1);
    EXPECT_EQ(replica->repl_data->ref_repl_buf_node, nullptr);

    freeReplicaReferencedReplBuffer(replica);
    EXPECT_EQ(block->refcount, 1);

    zfree(replica->repl_data);
    zfree(replica);
    listRelease(server.repl_buffer_blocks);
    zfree(block);
    raxFree(server.replicas_waiting_psync);
    server.replicas_waiting_psync = previous_waiting_psync;
    server.repl_buffer_blocks = previous_blocks;
    server.repl_backlog = previous_backlog;
    server.repl_backlog_size = previous_backlog_size;
}

TEST(ReplicationTest, WaitingPsyncCleanupPreservesReplacementWithSameId) {
    rax *previous_waiting_psync = server.replicas_waiting_psync;
    server.replicas_waiting_psync = raxNew();

    client *replica = (client *)zcalloc(sizeof(client));
    client *replacement = (client *)zcalloc(sizeof(client));
    initClientReplicationData(replica);
    replica->id = 42;
    replacement->id = replica->id;

    uint64_t encoded_id = htonu64(replica->id);
    EXPECT_TRUE(
        raxInsert(server.replicas_waiting_psync, (unsigned char *)&encoded_id, sizeof(encoded_id), replacement, NULL));

    freeReplicaReferencedReplBuffer(replica);

    void *registered_replica = NULL;
    EXPECT_TRUE(
        raxFind(server.replicas_waiting_psync, (unsigned char *)&encoded_id, sizeof(encoded_id), &registered_replica));
    EXPECT_EQ(registered_replica, replacement);

    zfree(replica->repl_data);
    zfree(replica);
    zfree(replacement);
    raxFree(server.replicas_waiting_psync);
    server.replicas_waiting_psync = previous_waiting_psync;
}
