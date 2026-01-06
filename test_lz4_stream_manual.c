/* Manual test for LZ4 streaming context */
#include <stdio.h>
#include <stdlib.h>

/* Forward declarations */
void *lz4StreamCreateContext(size_t dict_size);
void lz4StreamFreeContext(void *ctx);

int main() {
    printf("Testing LZ4 streaming context...\n");
    
    /* Test 1: Create and free context */
    printf("Test 1: Create and free context... ");
    void *ctx = lz4StreamCreateContext(64 * 1024);
    if (ctx == NULL) {
        printf("FAILED - context creation returned NULL\n");
        return 1;
    }
    lz4StreamFreeContext(ctx);
    printf("PASSED\n");
    
    /* Test 2: Create with zero size (should still work) */
    printf("Test 2: Create with zero size... ");
    ctx = lz4StreamCreateContext(0);
    if (ctx == NULL) {
        printf("FAILED - context creation returned NULL\n");
        return 1;
    }
    lz4StreamFreeContext(ctx);
    printf("PASSED\n");
    
    /* Test 3: Free NULL context (should not crash) */
    printf("Test 3: Free NULL context... ");
    lz4StreamFreeContext(NULL);
    printf("PASSED\n");
    
    /* Test 4: Multiple contexts */
    printf("Test 4: Multiple independent contexts... ");
    void *ctx1 = lz4StreamCreateContext(64 * 1024);
    void *ctx2 = lz4StreamCreateContext(64 * 1024);
    void *ctx3 = lz4StreamCreateContext(64 * 1024);
    
    if (ctx1 == NULL || ctx2 == NULL || ctx3 == NULL) {
        printf("FAILED - one or more contexts returned NULL\n");
        return 1;
    }
    
    if (ctx1 == ctx2 || ctx2 == ctx3 || ctx1 == ctx3) {
        printf("FAILED - contexts are not independent\n");
        return 1;
    }
    
    lz4StreamFreeContext(ctx2);
    lz4StreamFreeContext(ctx1);
    lz4StreamFreeContext(ctx3);
    printf("PASSED\n");
    
    printf("\nAll tests passed!\n");
    return 0;
}
