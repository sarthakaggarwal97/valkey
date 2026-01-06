#include <stdio.h>
#include <string.h>
#include "test_help.h"

#include "../server.h"
#include "../rdb.h"
#include "../zmalloc.h"

/* External declarations for LZ4 streaming functions (defined in rdb.c) */
extern void *lz4StreamCreateContext(size_t dict_size);
extern void lz4StreamFreeContext(void *ctx);

/* Test LZ4 streaming context creation and destruction */
int test_lz4StreamContextLifecycle(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Test context creation with normal size (should be ignored) */
    void *ctx = lz4StreamCreateContext(64 * 1024);
    TEST_ASSERT_MESSAGE("Context creation should succeed", ctx != NULL);
    
    /* Free context */
    lz4StreamFreeContext(ctx);
    
    /* Test context creation with zero size (should still succeed) */
    ctx = lz4StreamCreateContext(0);
    TEST_ASSERT_MESSAGE("Context creation with zero size should succeed", ctx != NULL);
    lz4StreamFreeContext(ctx);
    
    /* Test context creation with large size (should be ignored) */
    ctx = lz4StreamCreateContext(128 * 1024);
    TEST_ASSERT_MESSAGE("Context creation with large size should succeed", ctx != NULL);
    lz4StreamFreeContext(ctx);
    
    /* Test freeing NULL context (should not crash) */
    lz4StreamFreeContext(NULL);
    
    return 0;
}

/* Test multiple context creation and destruction */
int test_lz4StreamMultipleContexts(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create multiple contexts */
    void *ctx1 = lz4StreamCreateContext(64 * 1024);
    void *ctx2 = lz4StreamCreateContext(64 * 1024);
    void *ctx3 = lz4StreamCreateContext(64 * 1024);
    
    TEST_ASSERT_MESSAGE("First context creation should succeed", ctx1 != NULL);
    TEST_ASSERT_MESSAGE("Second context creation should succeed", ctx2 != NULL);
    TEST_ASSERT_MESSAGE("Third context creation should succeed", ctx3 != NULL);
    
    /* Contexts should be independent (different pointers) */
    TEST_ASSERT_MESSAGE("Contexts should be independent", 
                       ctx1 != ctx2 && ctx2 != ctx3 && ctx1 != ctx3);
    
    /* Free contexts in different order */
    lz4StreamFreeContext(ctx2);
    lz4StreamFreeContext(ctx1);
    lz4StreamFreeContext(ctx3);
    
    return 0;
}

/* Test context creation and immediate destruction */
int test_lz4StreamCreateAndDestroy(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Create and destroy multiple times */
    for (int i = 0; i < 10; i++) {
        void *ctx = lz4StreamCreateContext(64 * 1024);
        TEST_ASSERT_MESSAGE("Context creation should succeed", ctx != NULL);
        lz4StreamFreeContext(ctx);
    }
    
    return 0;
}
