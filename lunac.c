/*
 * lunac.c — LunaBerry bytecode compiler (Lua -> .lbc).
 *
 *   lunac -o app.lbc app.lua
 *   lunac -s -o app.lbc app.lua
 *
 * The output is standard Lua 5.5 bytecode and runs on lunavm, LunaJIT,
 * LunaAOT and the Berry adapter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luna.h"
#include "luna-common.h"

static void usage(const char *prog)
{
    printf("%s %s — LunaBerry bytecode compiler\n"
           "usage: %s [options] file\n"
           "\n"
           "options:\n"
           "  -o FILE   output file (default luac.out)\n"
           "  -s        strip debug information\n"
           "  -v        show version\n"
           "  -h        show this help\n",
           LUNA_ENGINE_NAME, LUNA_VERSION_STRING, prog);
}

int main(int argc, char **argv)
{
    const char *prog = luna_basename(argv[0]);
    const char *out = "luac.out";
    const char *input = NULL;
    int strip = 0;
    int i;
    LunaOptions opts;
    LunaState *s;
    unsigned char *bc = NULL;
    size_t len = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(prog);
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s (Lua %s)\n", LUNA_ENGINE_NAME,
                   LUNA_VERSION_STRING, LUNA_VERSION_STRING);
            return 0;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "-s") == 0) {
            strip = 1;
        } else if (strcmp(a, "-l") == 0) {
            fprintf(stderr, "%s: -l listing is not implemented; use lunavm\n",
                    prog);
            return 2;
        } else if (a[0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
            return 2;
        } else if (input == NULL) {
            input = a;
        } else {
            fprintf(stderr, "%s: only one input file is supported\n", prog);
            return 2;
        }
    }

    if (input == NULL) {
        usage(prog);
        return 2;
    }

    memset(&opts, 0, sizeof(opts));
    opts.open_libs = 0;
    s = luna_new(&opts);
    if (s == NULL) {
        fprintf(stderr, "%s: cannot create engine\n", prog);
        return 1;
    }

    if (luna_compile_file(s, input, &bc, &len, strip) != 0) {
        fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
        luna_free(s);
        return 1;
    }
    if (luna_write_file(out, bc, len) != 0) {
        fprintf(stderr, "%s: cannot write '%s'\n", prog, out);
        free(bc);
        luna_free(s);
        return 1;
    }
    printf("%s: %s -> %s (%lu bytes)\n", prog, input, out,
           (unsigned long)len);

    free(bc);
    luna_free(s);
    return 0;
}
