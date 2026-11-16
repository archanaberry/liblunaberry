/*
 * luna-jit-cli.c — lunajit: LunaJIT driver.
 *
 *   lunajit --backend llvm --target both app.lua
 *
 * Outputs:
 *   <out>.lbc          bytecode for lunavm
 *   <out>.exe          native launcher that JIT-compiles at runtime
 *   Berry backend:     <stem>.basm, <stem>.bex, <stem>.host(.exe), .lbc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luna.h"
#include "luna-common.h"
#include "luna-jit.h"
#include "luna-llvm.h"
#include "luna-berry.h"
#include "luna-opt.h"

static const char *prog = "lunajit";

static void usage(void)
{
    printf("%s %s — LunaJIT\n"
           "usage: %s [options] file\n"
           "\n"
           "options:\n"
           "  --backend NAME   cc | llvm | berry (default auto/cc)\n"
           "  --target NAME    exe | lunavm | both (default both)\n"
           "  -o BASE          output base (default dist/<stem>)\n"
           "  --cache-dir DIR  persistent shim cache\n"
           "  --stats          print JIT statistics\n"
           "  --run            run the chunk after compiling\n"
           "  --emit-ir        keep the LLVM .ll for inspection\n"
           "  -o-llvm          emit the LLVM IR for the shim (needs --backend llvm)\n"
           "  -o-c, --emit-c   emit the C shim source\n"
            "  --jit-level N     optimization level -O0..-O3 (default -O2/-O3)\n"
           "  -O, --opt-level N  optimization ladder 0..10 (Raw..The Stripper)\n"
            "  --native          add -march=native -mtune=native\n"
           "  --internal-loop   add -funroll-loops for tight loops\n"
           "  --compile-at-run  compile the shim at runtime (default: prebuilt "
           "shim, copy path and patch)\n"
           "  --hot-threshold N reserved (shim compiles on first use)\n"
           "  --verbose        print toolchain commands\n"
           "  -v, --version    show version\n"
           "  -h, --help       show this help\n",
           LUNA_ENGINE_NAME, LUNA_VERSION_STRING, prog);
}

static int write_lbc(const unsigned char *bc, size_t len, const char *base,
                     char *out_path, size_t out_size)
{
    snprintf(out_path, out_size, "%s.lbc", base);
    if (luna_write_file(out_path, bc, len) != 0) {
        fprintf(stderr, "%s: cannot write %s\n", prog, out_path);
        return -1;
    }
    return 0;
}

static int emit_launcher(const unsigned char *bc, size_t len, const char *base,
                         const char *backend, const char *cache_dir,
                         const char *root, const char *lib_dir,
                         const char *jit_flags, int internal_loop,
                         unsigned hot_threshold, int prebuild, int verbose)
{
    char c_path[1200];
    char out_exe[1200];
    char cmd[6000];
    char outbuf[2048];
    char lib_src[1200];
    char lib_dst[1200];
    char auto_lib[1024];
    FILE *f;
    LunaJitBackend be = LUNA_JIT_AUTO;
    const char *be_name = "LUNA_JIT_AUTO";
#if defined(_WIN32)
    const char *shim_ext = ".dll";
#else
    const char *shim_ext = ".so";
#endif

    if (lib_dir == NULL || lib_dir[0] == '\0') {
        luna_exe_dir(auto_lib, sizeof(auto_lib));
        lib_dir = auto_lib;
    }

    if (backend != NULL) {
        if (strcmp(backend, "llvm") == 0) {
            be = LUNA_JIT_LLVM;
            be_name = "LUNA_JIT_LLVM";
        } else if (strcmp(backend, "cc") == 0) {
            be = LUNA_JIT_CC;
            be_name = "LUNA_JIT_CC";
        }
    }

#if defined(_WIN32)
    snprintf(out_exe, sizeof(out_exe), "%s.exe", base);
#else
    snprintf(out_exe, sizeof(out_exe), "%s", base);
#endif
    snprintf(c_path, sizeof(c_path), "%s.jit.c", base);

    if (prebuild) {
        /* --- copy path and patch ---
         * Pre-build the native shim now (toolchain paid once) and emit a
         * launcher that merely dlopens the shim it ships next to itself. */
        char shim_path[1200];
        char shim_name[512];
        char jerr[512];
        LunaJitOptions jo;
        LunaJit *jit;

        snprintf(shim_path, sizeof(shim_path), "%s%s", base, shim_ext);
        memset(&jo, 0, sizeof(jo));
        jo.lib_dir = lib_dir;
        jo.cache_dir = cache_dir;
        jo.cc_flags = (jit_flags != NULL && jit_flags[0]) ? jit_flags : NULL;
        jo.internal_loop = internal_loop;
        jo.hot_threshold = hot_threshold;
        jit = luna_jit_new(be, &jo);
        if (jit == NULL) {
            fprintf(stderr, "%s: prebuild: cannot create JIT\n", prog);
            return -1;
        }
        if (luna_jit_prebuild(jit, bc, len, shim_path, jerr, sizeof(jerr)) !=
            0) {
            fprintf(stderr, "%s: prebuild shim failed: %s\n", prog, jerr);
            luna_jit_free(jit);
            return -1;
        }
        luna_jit_free(jit);
        snprintf(shim_name, sizeof(shim_name), "%s",
                 luna_basename(shim_path));
        if (verbose)
            fprintf(stderr, "  prebuilt shim %s\n", shim_name);

        f = fopen(c_path, "wb");
        if (f == NULL) {
            fprintf(stderr, "%s: cannot write %s\n", prog, c_path);
            return -1;
        }
        fprintf(f,
                "/* generated by LunaJIT — copy-path-and-patch launcher */\n"
                "#include <stdio.h>\n"
                "#include \"luna-jit.h\"\n\n"
                "int main(void)\n"
                "{\n"
                "    char err[512];\n"
                "    double r;\n"
                "    r = luna_jit_run_shim(\"%s\", err, sizeof(err));\n"
                "    if (err[0]) {\n"
                "        fprintf(stderr, \"luna: %%s\\n\", err);\n"
                "        return 1;\n"
                "    }\n"
                "    printf(\"%%.14g\\n\", r);\n"
                "    return 0;\n"
                "}\n",
                shim_name);
        if (fclose(f) != 0) {
            fprintf(stderr, "%s: cannot close %s\n", prog, c_path);
            return -1;
        }
    } else {
        f = fopen(c_path, "wb");
        if (f == NULL) {
            fprintf(stderr, "%s: cannot write %s\n", prog, c_path);
            return -1;
        }
        fprintf(f,
                "/* generated by LunaJIT — runtime JIT launcher */\n"
                "#include <stdio.h>\n"
                "#include \"luna.h\"\n"
                "#include \"luna-jit.h\"\n\n");
        luna_emit_c_array(f, "luna_bc", bc, len);
        if (cache_dir != NULL && cache_dir[0] != '\0') {
            luna_emit_c_string(f, "luna_cache_dir", cache_dir);
        } else {
            fprintf(f, "static const char luna_cache_dir[] = \"\";\n");
        }
        if (jit_flags != NULL && jit_flags[0] != '\0') {
            luna_emit_c_string(f, "luna_jit_flags", jit_flags);
        } else {
            fprintf(f, "static const char luna_jit_flags[] = \"\";\n");
        }
        fprintf(f,
                "\nint main(void)\n"
                "{\n"
                "    LunaJitOptions o;\n"
                "    LunaJit *j;\n"
                "    char err[512];\n"
                "    double r;\n"
                "    o.cc = NULL;\n"
                "    o.cc_flags = luna_jit_flags[0] ? luna_jit_flags : NULL;\n"
                "    o.lib_dir = NULL;\n"
                "    o.tmp_dir = NULL;\n"
                "    o.cache_dir = luna_cache_dir[0] ? luna_cache_dir : NULL;\n"
                "    o.hot_threshold = %u;\n"
                "    o.internal_loop = %d;\n"
                "    j = luna_jit_new(%s, &o);\n"
                "    if (j == NULL) {\n"
                "        fprintf(stderr, \"luna: jit init failed\\n\");\n"
                "        return 1;\n"
                "    }\n"
                "    err[0] = 0;\n"
                "    r = luna_jit_call(j, luna_bc, luna_bc_len, \"=jit-exe\",\n"
                "                      err, sizeof(err));\n"
                "    if (err[0]) {\n"
                "        fprintf(stderr, \"luna: %%s\\n\", err);\n"
                "        luna_jit_free(j);\n"
                "        return 1;\n"
                "    }\n"
                "    printf(\"%%.14g\\n\", r);\n"
                "    luna_jit_free(j);\n"
                "    return 0;\n"
                "}\n",
                hot_threshold, internal_loop, be_name);
        if (fclose(f) != 0) {
            fprintf(stderr, "%s: cannot close %s\n", prog, c_path);
            return -1;
        }
    }

    snprintf(lib_src, sizeof(lib_src), "%s/liblunaberry.a", lib_dir);
    {
        char out_dir[1024];
        luna_dirname(out_exe, out_dir, sizeof(out_dir));
        luna_path_join(lib_dst, sizeof(lib_dst), out_dir, "liblunaberry.a");
    }
    if (!luna_file_exists(lib_dst)) {
        size_t n = 0;
        char *data = luna_read_file(lib_src, &n);
        if (data != NULL) {
            luna_write_file(lib_dst, data, n);
            free(data);
        }
    }

    {
        char cc[1200];
#if defined(_WIN32)
        const char *want = "gcc";
#else
        const char *want = "cc";
#endif
        if (luna_find_tool(want, cc, sizeof(cc)) != 0)
            snprintf(cc, sizeof(cc), "%s", want);
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -O2 -I\"%s\" -o \"%s\" \"%s\" \"%s\" -lm", cc, root,
                 out_exe, c_path, lib_src);
    }
    if (verbose)
        fprintf(stderr, "  %s\n", cmd);
    if (luna_run_cmd(cmd, outbuf, sizeof(outbuf)) != 0) {
        fprintf(stderr, "%s: launcher link failed: %s\n", prog, outbuf);
        remove(c_path);
        return -1;
    }
    remove(c_path);
    printf("%s: wrote %s\n", prog, out_exe);
    return 0;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *out_base = NULL;
    const char *backend = NULL;
    const char *target = "both";
    const char *cache_dir = NULL;
    const char *root;
    char root_buf[1024];
    const char *lib_dir = NULL;
    int stats = 0;
    int run = 0;
    int emit_ir = 0;
    int emit_c = 0;
    int verbose = 0;
    int internal_loop = 0;
    int native_tune = 0;
    int opt_level = -1;
    int prebuild = 1;
    unsigned hot_threshold = 100;
    int i;
    LunaOptions opts;
    LunaState *s;
    unsigned char *bc = NULL;
    size_t len = 0;
    char base[1024];
    char stem[256];
    char lbc[1200];
    char jit_flags[512] = "";
    int rc = 0;

    prog = luna_basename(argv[0]);
    luna_find_include_dir(root_buf, sizeof(root_buf));
    root = root_buf;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s (Lua %s)\n", LUNA_ENGINE_NAME,
                   LUNA_VERSION_STRING, LUNA_VERSION_STRING);
            return 0;
        } else if (strcmp(a, "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            target = argv[++i];
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out_base = argv[++i];
        } else if (strcmp(a, "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(a, "--stats") == 0) {
            stats = 1;
        } else if (strcmp(a, "--run") == 0) {
            run = 1;
        } else if (strcmp(a, "--emit-ir") == 0) {
            emit_ir = 1;
        } else if (strcmp(a, "-o-llvm") == 0) {
            emit_ir = 1;
        } else if (strcmp(a, "-o-c") == 0 || strcmp(a, "--emit-c") == 0) {
            emit_c = 1;
        } else if (strcmp(a, "--internal-loop") == 0) {
            internal_loop = 1;
        } else if (strcmp(a, "--compile-at-run") == 0) {
            /* opt out of copy-path-and-patch: compile the shim at runtime */
            prebuild = 0;
        } else if (strcmp(a, "--native") == 0) {
            native_tune = 1;
        } else if (strcmp(a, "--jit-level") == 0 && i + 1 < argc) {
            /* Deprecated alias, kept for compatibility: clamps to -O0..-O3. */
            opt_level = atoi(argv[++i]);
            if (opt_level < 0)
                opt_level = 0;
            if (opt_level > 3)
                opt_level = 3;
        } else if (strcmp(a, "-O") == 0 && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
        } else if (strncmp(a, "-O", 2) == 0 && a[2] >= '0' && a[2] <= '9') {
            opt_level = atoi(a + 2);
        } else if (strcmp(a, "--opt-level") == 0 && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
        } else if (strcmp(a, "--hot-threshold") == 0 && i + 1 < argc) {
            hot_threshold = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
            return 2;
        } else if (file == NULL) {
            file = a;
        } else {
            fprintf(stderr, "%s: only one input file is supported\n", prog);
            return 2;
        }
    }

    if (file == NULL) {
        usage();
        return 2;
    }

    if (opt_level >= 0 && (opt_level < LUNA_OPT_MIN || opt_level > LUNA_OPT_MAX)) {
        fprintf(stderr,
                "%s: invalid optimization level %d (valid: %d..%d)\n",
                prog, opt_level, LUNA_OPT_MIN, LUNA_OPT_MAX);
        return 2;
    }

    memset(&opts, 0, sizeof(opts));
    opts.open_libs = 0;
    s = luna_new(&opts);
    if (s == NULL) {
        fprintf(stderr, "%s: cannot create engine\n", prog);
        return 1;
    }

    if (luna_compile_file(s, file, &bc, &len, 0) != 0) {
        fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
        luna_free(s);
        return 1;
    }
    luna_free(s);

    luna_stem(file, stem, sizeof(stem));
    if (out_base == NULL) {
        if (backend != NULL && strcmp(backend, "berry") == 0)
            snprintf(base, sizeof(base), "dist/berry/%s", stem);
        else
            snprintf(base, sizeof(base), "dist/%s", stem);
    } else {
        snprintf(base, sizeof(base), "%s", out_base);
    }
    luna_ensure_parent(base);

    {
        int lvl = luna_opt_clamp(opt_level >= 0 ? opt_level : 2);
        opt_level = lvl;
        luna_opt_warn_redundant(lvl, 0, 0);
        snprintf(jit_flags, sizeof(jit_flags), "%s", luna_opt_flags(lvl));
        if (native_tune)
            snprintf(jit_flags + strlen(jit_flags),
                     sizeof(jit_flags) - strlen(jit_flags),
                     " -march=native -mtune=native");
        if (verbose && jit_flags[0])
            fprintf(stderr, "  jit flags: %s\n", jit_flags);
        if (emit_c) {
            char c_out[1200];
            char err[512];
            snprintf(c_out, sizeof(c_out), "%s.c", base);
            if (luna_jit_emit_c(bc, len, c_out, err, sizeof(err)) == 0)
                printf("%s: emitted C %s\n", prog, c_out);
            else {
                fprintf(stderr, "%s: emit-c: %s\n", prog, err);
                rc = 1;
            }
        }
        if (emit_ir) {
            LunaJitBackend be = LUNA_JIT_LLVM;
            LunaJitOptions jo;
            LunaJit *jit;
            char ll_out[1200];
            const char *ir;

            snprintf(ll_out, sizeof(ll_out), "%s.ll", base);
            memset(&jo, 0, sizeof(jo));
            jo.lib_dir = lib_dir;
            jo.cc_flags = jit_flags[0] ? jit_flags : NULL;
            jo.internal_loop = internal_loop;
            jo.hot_threshold = hot_threshold;
            jit = luna_jit_new(be, &jo);
            if (jit == NULL) {
                fprintf(stderr, "%s: emit-ir: cannot create LLVM JIT\n",
                        prog);
                rc = 1;
            } else if (luna_jit_compile(jit, bc, len, file) == NULL) {
                fprintf(stderr, "%s: emit-ir: %s\n", prog,
                        luna_jit_last_error(jit));
                rc = 1;
            } else {
                ir = luna_jit_last_ir(jit);
                if (ir == NULL) {
                    fprintf(stderr,
                            "%s: emit-ir: no IR; use --backend llvm\n",
                            prog);
                    rc = 1;
                } else if (luna_write_file(ll_out, ir, strlen(ir)) != 0) {
                    fprintf(stderr, "%s: emit-ir: cannot write %s\n", prog,
                            ll_out);
                    rc = 1;
                } else {
                    printf("%s: emitted IR %s\n", prog, ll_out);
                }
            }
            luna_jit_free(jit);
        }

        if (strcmp(target, "lunavm") == 0 || strcmp(target, "both") == 0) {
            if (write_lbc(bc, len, base, lbc, sizeof(lbc)) != 0)
                rc = 1;
            else
                printf("%s: lunavm artifact %s\n", prog, lbc);
        }

        if (strcmp(target, "exe") == 0 || strcmp(target, "both") == 0) {
            if (backend != NULL && strcmp(backend, "berry") == 0) {
                LunaBerryOptions bo;
                char err[512];
                char bdir[1024];
                char bstem[256];
                if (out_base != NULL) {
                    luna_dirname(base, bdir, sizeof(bdir));
                    luna_stem(base, bstem, sizeof(bstem));
                } else {
                    snprintf(bdir, sizeof(bdir), "%s", "dist/berry");
                    snprintf(bstem, sizeof(bstem), "%s", stem);
                }
                memset(&bo, 0, sizeof(bo));
                bo.out_dir = bdir;
                bo.include_dir = root;
                bo.lib_dir = lib_dir;
                bo.opt_level = opt_level;
                bo.verbose = verbose;
                bo.emit_host = 1;
                if (luna_berry_emit(bc, len, bstem, &bo, err,
                                    sizeof(err)) != 0) {
                    fprintf(stderr, "%s: berry: %s\n", prog, err);
                    rc = 1;
                } else {
                    printf("%s: berry artifacts in %s/%s.*\n", prog, bdir,
                           bstem);
                }
            } else {
                if (emit_launcher(bc, len, base, backend, cache_dir, root,
                                  lib_dir, jit_flags, internal_loop,
                                  hot_threshold, prebuild, verbose) != 0)
                    rc = 1;
            }
        }
    }

    if (run) {
        if (backend != NULL && strcmp(backend, "berry") == 0 &&
            strcmp(target, "lunavm") == 0) {
            /* Berry host is only emitted for exe/both targets. */
        } else if (backend != NULL && strcmp(backend, "berry") == 0) {
            char host[1400];
            char bdir[1024];
            char bstem[256];
            if (out_base != NULL) {
                luna_dirname(base, bdir, sizeof(bdir));
                luna_stem(base, bstem, sizeof(bstem));
            } else {
                snprintf(bdir, sizeof(bdir), "%s", "dist/berry");
                snprintf(bstem, sizeof(bstem), "%s", stem);
            }
#if defined(_WIN32)
            snprintf(host, sizeof(host), "%s/%s.host.exe", bdir, bstem);
#else
            snprintf(host, sizeof(host), "%s/%s.host", bdir, bstem);
#endif
            if (luna_run_cmd_inherit(host) != 0)
                rc = 1;
        } else {
            LunaJitBackend be = LUNA_JIT_AUTO;
            LunaJitOptions jo;
            LunaJit *jit;
            char err[512];
            double r;
            if (backend != NULL) {
                if (strcmp(backend, "cc") == 0)
                    be = LUNA_JIT_CC;
                else if (strcmp(backend, "llvm") == 0)
                    be = LUNA_JIT_LLVM;
            }
            memset(&jo, 0, sizeof(jo));
            jo.cache_dir = cache_dir;
            jo.lib_dir = lib_dir;
            jo.cc_flags = jit_flags[0] ? jit_flags : NULL;
            jo.internal_loop = internal_loop;
            jo.hot_threshold = hot_threshold;
            jit = luna_jit_new(be, &jo);
            if (jit == NULL) {
                fprintf(stderr, "%s: cannot create JIT\n", prog);
                rc = 1;
            } else {
                printf("%s: jit backend=%s\n", prog,
                       luna_jit_backend_name(jit));
                r = luna_jit_call(jit, bc, len, file, err, sizeof(err));
                if (err[0] != '\0') {
                    fprintf(stderr, "%s: jit: %s\n", prog, err);
                    rc = 1;
                } else {
                    printf("%.14g\n", r);
                }
                if (stats) {
                    LunaJitStats st = luna_jit_stats(jit);
                    printf("jit stats: attempts=%lu ok=%lu failed=%lu "
                           "native=%lu cache=%lu disk_hits=%lu "
                           "disk_writes=%lu\n",
                           st.compile_attempts, st.compile_ok,
                           st.compile_failed, st.native_calls, st.cache_hits,
                           st.cache_disk_hits, st.cache_disk_writes);
                }
                luna_jit_free(jit);
            }
        }
    }

    free(bc);
    return rc;
}
