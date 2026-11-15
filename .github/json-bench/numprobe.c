/* Prints how the engine stores and prints some numbers. For CI debugging. */
#include "json_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *a(size_t n) { return malloc(n); }
static void *r(void *p, size_t n) { return realloc(p, n); }
static void f(void *p) { free(p); }
static size_t u(void *p) { (void)p; return 0; }

int main(int argc, char **argv) {
    static const jsonAllocator al = {a, r, f, u};
    jsonEngineInit(&al);
    for (int i = 1; i < argc; i++) {
        jsonErr e;
        jsonVal *v = jsonValParse(argv[i], strlen(argv[i]), 0, 0, &e, NULL);
        if (!v) { printf("%-40s parse error %d\n", argv[i], (int)e); continue; }
        jsonDoc d = jsonValToDoc(v);
        jsonRef root;
        jsonDocRoot(d, &root);
        char buf[JSON_NUM_BUF];
        uint32_t len;
        const char *t = jsonRefNumText(&root, buf, &len);
        printf("%-40s type %d enc %s value %.17g text %.*s\n", argv[i], (int)jsonRefType(&root), jsonDocEncoding(d),
               jsonRefDouble(&root), (int)len, t);
        jsonDocFree(d);
    }
    return 0;
}
