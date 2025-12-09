#include "valkeymodule.h"

#define UNUSED(x) (void)(x)

void cluster_timer_handler(ValkeyModuleCtx *ctx, void *data) {
    VALKEYMODULE_NOT_USED(data);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "CLUSTER", "c", "SLOTS");

    if (rep) {
        if (ValkeyModule_CallReplyType(rep) == VALKEYMODULE_REPLY_ARRAY) {
            ValkeyModule_Log(ctx, "notice", "Timer: CLUSTER SLOTS success");
        } else {
            ValkeyModule_Log(ctx, "notice",
                             "Timer: CLUSTER SLOTS unexpected reply type %d",
                             ValkeyModule_CallReplyType(rep));
        }
        ValkeyModule_FreeCallReply(rep);
    } else {
        ValkeyModule_Log(ctx, "warning", "Timer: CLUSTER SLOTS failed");
    }
}

int test_start_cluster_timer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_CreateTimer(ctx, 1, cluster_timer_handler, NULL);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}


int test_cluster_slots(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);

    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "CLUSTER", "c", "SLOTS");
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "ERR NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

int test_cluster_shards(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);

    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "CLUSTER", "c", "SHARDS");
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "ERR NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "cluster", 1, VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.cluster_slots", test_cluster_slots, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.cluster_shards", test_cluster_shards, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.start_cluster_timer", test_start_cluster_timer, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
