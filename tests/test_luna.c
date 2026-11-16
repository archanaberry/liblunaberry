#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luna.h"

static int failures = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what);    \
            failures++;                                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    LunaOptions opts;
    LunaState *s;
    unsigned char *bc = NULL;
    size_t len = 0;

    memset(&opts, 0, sizeof(opts));
    opts.open_libs = 1;

    s = luna_new(&opts);
    CHECK(s != NULL, "luna_new");
    if (s == NULL)
        return 1;

    CHECK(strcmp(luna_version(), LUNA_VERSION_STRING) == 0, "version");
    CHECK(strcmp(luna_engine_name(), LUNA_ENGINE_NAME) == 0, "engine name");
    CHECK(strcmp(luna_package_name(), LUNA_PACKAGE_NAME) == 0, "package");

    CHECK(luna_dostring(s, "return 1+2", strlen("return 1+2"), "=test") == 0,
          "dostring");
    CHECK(luna_result_count(s) == 1, "result count");
    CHECK(luna_result_type(s, 1) == 1, "result type number");
    CHECK(luna_result_number(s, 1) == 3.0, "result number");

    CHECK(luna_dostring(s, "error('boom')", strlen("error('boom')"),
                        "=test") != 0,
          "runtime error reported");
    CHECK(luna_last_error(s) != NULL && luna_last_error(s)[0] != '\0',
          "last error text");

    CHECK(luna_compile(s, "return 6*7", strlen("return 6*7"), "=compile", &bc,
                       &len, 0) == 0,
          "compile");
    CHECK(bc != NULL && len > 0, "bytecode buffer");
    if (bc != NULL) {
        CHECK(luna_dobytecode(s, bc, len, "=bc") == 0, "dobytecode");
        CHECK(luna_result_number(s, 1) == 42.0, "bytecode result");
        free(bc);
    }

    CHECK(luna_mem_used(s) > 0, "mem used");
    CHECK(luna_mem_peak(s) >= luna_mem_used(s), "mem peak");

    luna_free(s);

    if (failures != 0) {
        fprintf(stderr, "luna: %d failure(s)\n", failures);
        return 1;
    }
    printf("luna: C API ok\n");
    return 0;
}
