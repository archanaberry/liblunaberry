/*
 * luna-aot-cli.c — lunaaot: LunaAOT driver.
 *
 *   lunaaot --backend llvm app.lua          -> dist/app-native(.exe)
 *   lunaaot --backend berry app.lua         -> dist/berry/app.{basm,bex,host,lbc}
 *   lunaaot --target both --emit-ir app.lua -> exe + .ll + .lbc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luna.h"
#include "luna-common.h"
#include "luna-llvm.h"
#include "luna-berry.h"
#include "luna-opt.h"

static const char *prog = "lunaaot";

static void usage(void)
{
    printf("%s %s — LunaAOT\n"
           "usage: %s [options] file\n"
           "\n"
           "options:\n"
           "  --backend NAME   llvm | berry (default llvm)\n"
           "  --target NAME    exe | lunavm | both (default exe)\n"
           "  -o PATH          output executable (llvm) or base (berry)\n"
           "  --emit-ir        keep the LLVM .ll next to the executable\n"
           "  -o-llvm          alias for --emit-ir (emit LLVM IR)\n"
           "  -o-c, --emit-c   emit the standalone C wrapper source\n"
            "  --strip          strip the produced executable\n"
            "  --lto            enable link-time optimization (clang -flto=thin)\n"
            "  --gc-sections    dead-code elimination via --gc-sections (default on)\n"
            "  --no-gc-sections keep every section\n"
            "  --ir-passes      run the ladder IR pipeline on the .ll (default on)\n"
            "  --no-ir-passes   skip the IR pipeline\n"
            "  --o3             compile at -O3 (alias for -O3)\n"
            "  -O, --opt-level  optimization level 0..10 (default 2)\n"
            "  --emit-s         emit assembly next to the executable\n"
            "  --native         add -march=native -mtune=native\n"
           "  --static         link liblunaberry.a statically (default)\n"
           "  --verbose        print toolchain commands\n"
           "  --run            run the produced executable/host\n"
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

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *backend = "llvm";
    const char *target = "exe";
    const char *out = NULL;
    const char *root;
    char root_buf[1024];
    const char *lib_dir = NULL;
    int emit_ir = 0;
    int emit_c = 0;
    int strip = 0;
    int lto = 0;
    int gc_sections = 1;
    int ir_passes = 1;
    int native_tune = 0;
    int opt_level = 2;
    int emit_s = 0;
    int verbose = 0;
    int run = 0;
    int i;
    LunaOptions lopts;
    LunaState *s;
    unsigned char *bc = NULL;
    size_t len = 0;
    char stem[256];
    char lbc[1400];
    char lbc_base[1200];
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
            out = argv[++i];
        } else if (strcmp(a, "--emit-ir") == 0) {
            emit_ir = 1;
        } else if (strcmp(a, "-o-llvm") == 0) {
            emit_ir = 1;
        } else if (strcmp(a, "-o-c") == 0 || strcmp(a, "--emit-c") == 0) {
            emit_c = 1;
        } else if (strcmp(a, "--strip") == 0) {
            strip = 1;
        } else if (strcmp(a, "--lto") == 0) {
            lto = 1;
        } else if (strcmp(a, "--gc-sections") == 0) {
            gc_sections = 1;
        } else if (strcmp(a, "--no-gc-sections") == 0) {
            gc_sections = 0;
        } else if (strcmp(a, "--ir-passes") == 0) {
            ir_passes = 1;
        } else if (strcmp(a, "--no-ir-passes") == 0) {
            ir_passes = 0;
        } else if (strcmp(a, "--o3") == 0) {
            opt_level = 3;
        } else if (strcmp(a, "--native") == 0) {
            native_tune = 1;
        } else if (strcmp(a, "--static") == 0) {
            /* default */
        } else if (strcmp(a, "--emit-s") == 0) {
            emit_s = 1;
        } else if (strcmp(a, "-O") == 0 && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
        } else if (strncmp(a, "-O", 2) == 0 && a[2] >= '0' && a[2] <= '9') {
            opt_level = atoi(a + 2);
        } else if (strcmp(a, "--opt-level") == 0 && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
        } else if (strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "--run") == 0) {
            run = 1;
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

    if (opt_level < LUNA_OPT_MIN || opt_level > LUNA_OPT_MAX) {
        fprintf(stderr,
                "%s: invalid optimization level %d (valid: %d..%d)\n",
                prog, opt_level, LUNA_OPT_MIN, LUNA_OPT_MAX);
        return 2;
    }
    luna_opt_warn_redundant(opt_level, strip, 0);

    memset(&lopts, 0, sizeof(lopts));
    lopts.open_libs = 0;
    s = luna_new(&lopts);
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
    if (out != NULL) {
        char od[1024];
        char os[256];
        luna_dirname(out, od, sizeof(od));
        luna_stem(out, os, sizeof(os));
        luna_path_join(lbc_base, sizeof(lbc_base), od, os);
    } else {
        snprintf(lbc_base, sizeof(lbc_base), "dist/%s", stem);
    }

    if (emit_c) {
        char c_out[1400];
        char err[512];
        if (out != NULL)
            snprintf(c_out, sizeof(c_out), "%s.c", out);
        else
            snprintf(c_out, sizeof(c_out), "%s.c", lbc_base);
        if (luna_llvm_emit_c(bc, len, c_out, err, sizeof(err)) == 0)
            printf("%s: emitted C %s\n", prog, c_out);
        else {
            fprintf(stderr, "%s: emit-c: %s\n", prog, err);
            rc = 1;
        }
    }
    if (emit_ir && strcmp(backend, "berry") != 0 &&
        strcmp(target, "lunavm") == 0) {
        LunaLLvmOptions lo;
        char err[512];
        char ll_out[1400];
        char flags[512];
        if (out != NULL)
            snprintf(ll_out, sizeof(ll_out), "%s.ll", out);
        else
            snprintf(ll_out, sizeof(ll_out), "%s.ll", lbc_base);
        memset(&lo, 0, sizeof(lo));
        snprintf(flags, sizeof(flags), "%s", luna_opt_llvm_flags(opt_level));
        lo.flags = flags;
        lo.include_dir = root;
        lo.lib_dir = lib_dir;
        lo.verbose = verbose;
        if (luna_llvm_emit_ir(bc, len, ll_out, file, &lo, err,
                              sizeof(err)) == 0)
            printf("%s: emitted IR %s\n", prog, ll_out);
        else {
            fprintf(stderr, "%s: emit-ir: %s\n", prog, err);
            rc = 1;
        }
    }

    if (strcmp(backend, "berry") == 0) {
        LunaBerryOptions bo;
        char err[512];
        char out_dir[1024];
        char out_stem[256];
        const char *bstem = stem;
        if (out != NULL) {
            luna_dirname(out, out_dir, sizeof(out_dir));
            luna_stem(out, out_stem, sizeof(out_stem));
            bstem = out_stem;
        } else {
            snprintf(out_dir, sizeof(out_dir), "dist/berry");
        }
        memset(&bo, 0, sizeof(bo));
        bo.out_dir = out_dir;
        bo.include_dir = root;
        bo.lib_dir = lib_dir;
        bo.opt_level = opt_level;
        bo.emit_host = strcmp(target, "lunavm") != 0;
        bo.verbose = verbose;
        if (luna_berry_emit(bc, len, bstem, &bo, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: berry: %s\n", prog, err);
            rc = 1;
        } else {
            printf("%s: berry artifacts in %s/%s.*\n", prog, out_dir, bstem);
        }
    } else {
        LunaLLvmOptions lo;
        char err[512];
        char flags[1024];
        char out_exe[1200];
        int want_exe = strcmp(target, "lunavm") != 0;

        /* The LLVM backend gets the LLVM side of the ladder, not the
         * portable C flags (e.g. -march=native at O8). */
        snprintf(flags, sizeof(flags), "%s", luna_opt_llvm_flags(opt_level));
        if (lto)
            snprintf(flags + strlen(flags), sizeof(flags) - strlen(flags),
                     " -flto=thin");
        if (native_tune)
            snprintf(flags + strlen(flags), sizeof(flags) - strlen(flags),
                     " -march=native -mtune=native");

        if (want_exe) {
            if (out != NULL) {
                snprintf(out_exe, sizeof(out_exe), "%s", out);
#if defined(_WIN32)
                /* clang appends .exe on Windows when the name has no
                 * extension; keep out_exe in sync so strip/emit-s target
                 * the file that actually gets produced. */
                {
                    size_t n = strlen(out_exe);
                    if (n < 4 || strcmp(out_exe + n - 4, ".exe") != 0)
                        snprintf(out_exe + n, sizeof(out_exe) - n, ".exe");
                }
#endif
            } else {
#if defined(_WIN32)
                snprintf(out_exe, sizeof(out_exe), "dist/%s-native.exe", stem);
#else
                snprintf(out_exe, sizeof(out_exe), "dist/%s-native", stem);
#endif
            }
            luna_ensure_parent(out_exe);

            memset(&lo, 0, sizeof(lo));
            lo.flags = flags;
            lo.lib_dir = lib_dir;
            lo.include_dir = root;
            lo.emit_ir = emit_ir;
            lo.verbose = verbose;
            lo.lto = lto;
            lo.gc_sections = gc_sections;
            lo.ir_passes = ir_passes;
            lo.passes = luna_opt_llvm_passes(opt_level);

            if (!luna_llvm_available()) {
                fprintf(stderr,
                        "%s: clang not found; LLVM backend unavailable\n",
                        prog);
                rc = 1;
            } else if (luna_llvm_aot(bc, len, out_exe, file, &lo, err,
                                     sizeof(err)) != 0) {
                fprintf(stderr, "%s: llvm: %s\n", prog, err);
                rc = 1;
            } else {
                printf("%s: wrote %s\n", prog, out_exe);
                if (emit_ir)
                    printf("%s: wrote %s.ll\n", prog, out_exe);
                if (strip || luna_opt_implies_strip(opt_level)) {
                    if (luna_opt_strip_binary(out_exe) == 0)
                        printf("%s: stripped %s\n", prog, out_exe);
                    else if (strip)
                        fprintf(stderr,
                                "%s: no strip tool found; binary left "
                                "unstripped\n",
                                prog);
                }
                if (emit_s) {
                    char raw_s[1400];
                    char final_s[1400];
                    char raw_err[512];
                    LunaLLvmOptions so;
                    memset(&so, 0, sizeof(so));
                    so.flags = flags;
                    so.lib_dir = lib_dir;
                    so.include_dir = root;
                    so.verbose = verbose;
                    snprintf(raw_s, sizeof(raw_s), "%s.raw.s", out_exe);
                    snprintf(final_s, sizeof(final_s), "%s.s", out_exe);
                    if (luna_llvm_emit_asm(bc, len, raw_s, file, &so, raw_err,
                                           sizeof(raw_err)) != 0) {
                        fprintf(stderr, "%s: emit-s: %s\n", prog, raw_err);
                        rc = 1;
                    } else if (luna_opt_implies_dead_asm(opt_level)) {
                        if (luna_opt_dead_asm(raw_s, final_s) == 0) {
                            remove(raw_s);
                            printf("%s: wrote %s (dead assembly)\n", prog,
                                   final_s);
                        } else {
                            fprintf(stderr,
                                    "%s: cannot write dead assembly %s\n",
                                    prog, final_s);
                            rc = 1;
                        }
                    } else {
                        remove(final_s);
                        if (rename(raw_s, final_s) == 0)
                            printf("%s: wrote %s\n", prog, final_s);
                        else {
                            fprintf(stderr, "%s: cannot rename %s\n", prog,
                                    raw_s);
                            rc = 1;
                        }
                    }
                }
            }
        }

        if (strcmp(target, "lunavm") == 0 || strcmp(target, "both") == 0) {
            if (write_lbc(bc, len, lbc_base, lbc, sizeof(lbc)) == 0)
                printf("%s: lunavm artifact %s\n", prog, lbc);
            else
                rc = 1;
            if (emit_s && rc == 0) {
                char raw_s[1400];
                char final_s[1400];
                char raw_err[512];
                LunaLLvmOptions so;
                memset(&so, 0, sizeof(so));
                so.flags = flags;
                so.lib_dir = lib_dir;
                so.include_dir = root;
                so.verbose = verbose;
                snprintf(raw_s, sizeof(raw_s), "%s.raw.s", lbc_base);
                snprintf(final_s, sizeof(final_s), "%s.s", lbc_base);
                if (luna_llvm_emit_asm(bc, len, raw_s, file, &so, raw_err,
                                       sizeof(raw_err)) != 0) {
                    fprintf(stderr, "%s: emit-s: %s\n", prog, raw_err);
                    rc = 1;
                } else if (luna_opt_implies_dead_asm(opt_level)) {
                    if (luna_opt_dead_asm(raw_s, final_s) == 0) {
                        remove(raw_s);
                        printf("%s: wrote %s (dead assembly)\n", prog,
                               final_s);
                    } else {
                        fprintf(stderr,
                                "%s: cannot write dead assembly %s\n", prog,
                                final_s);
                        rc = 1;
                    }
                } else {
                    remove(final_s);
                    if (rename(raw_s, final_s) == 0)
                        printf("%s: wrote %s\n", prog, final_s);
                    else {
                        fprintf(stderr, "%s: cannot rename %s\n", prog,
                                raw_s);
                        rc = 1;
                    }
                }
            }
        }
        if (run && want_exe && rc == 0)
            rc = luna_run_cmd_inherit(out_exe) == 0 ? 0 : 1;
    }

    if (strcmp(backend, "berry") == 0 && run && rc == 0 &&
        strcmp(target, "lunavm") != 0) {
        char host[1400];
        char host_dir[1024];
        char host_stem[256];
        if (out != NULL) {
            luna_dirname(out, host_dir, sizeof(host_dir));
            luna_stem(out, host_stem, sizeof(host_stem));
        } else {
            snprintf(host_dir, sizeof(host_dir), "%s", "dist/berry");
            snprintf(host_stem, sizeof(host_stem), "%s", stem);
        }
#if defined(_WIN32)
        snprintf(host, sizeof(host), "%s/%s.host.exe", host_dir, host_stem);
#else
        snprintf(host, sizeof(host), "%s/%s.host", host_dir, host_stem);
#endif
        rc = luna_run_cmd_inherit(host) == 0 ? 0 : 1;
    }

    free(bc);
    return rc;
}
