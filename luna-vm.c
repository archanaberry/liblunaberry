/*
 * luna-vm.c — lunavm / lunaberry: the LunaBerry runner and REPL.
 *
 *   lunavm script.lua          run a Lua source file
 *   lunavm script.lbc          run compiled bytecode
 *   lunavm -e 'print(1+1)'     evaluate a snippet
 *   lunavm --jit app.lua       run through the LunaJIT tier
 *   lunavm --aot app.lua       compile and run through LunaAOT
 *   lunavm                     interactive REPL
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "luna.h"
#include "lua.h"
#include "luna-common.h"
#include "luna-jit.h"
#include "luna-opt.h"
#include "luna-llvm.h"
#include "luna-berry.h"

static const char *prog = "lunavm";

static volatile sig_atomic_t g_repl_interrupt = 0;

static void repl_sigint(int sig)
{
    (void)sig;
    g_repl_interrupt = 1;
}

static void usage(void)
{
    printf("%s %s — %s (Lua %s)\n"
           "usage: %s [options] [file [args...]]\n"
           "\n"
           "options:\n"
           "  -e CODE          execute CODE\n"
           "  -i               enter the REPL after the script\n"
           "  -c               compile the input to bytecode (.lbc)\n"
           "  -o FILE          output file for -c (default <stem>.lbc)\n"
           "  -s               strip debug info when compiling\n"
           "  --jit[=BACKEND]  run through LunaJIT (cc|llvm)\n"
           "  --aot[=BACKEND]  compile+run via LunaAOT (llvm|berry|lbc)\n"
           "  --backend NAME   backend for --jit/--aot\n"
           "  --target NAME    exe|lunavm|both (tool CLIs)\n"
           "  --cache-dir DIR  persistent LunaJIT shim cache\n"
           "  --mem-limit N    Lua allocator budget in bytes (0=unlimited)\n"
           "  --autoline       REPL: evaluate as soon as input is complete\n"
           "  --autoclean      REPL: rebuild context after every evaluation\n"
           "  --jit-level N    JIT optimization level -O0..-O3\n"
           "  -O, --opt-level N  optimization ladder 0..10 (Raw..The Stripper)\n"
           "  --native         JIT: add -march=native -mtune=native\n"
           "  --internal-loop  JIT: add -funroll-loops for tight loops\n"
           "  --hot-threshold N JIT: reserved (shim compiles on first use)\n"
           "  --opt-flags N    print the compiler flags of ladder level N\n"
           "  --llvm-flags N   print the clang/LLVM flags of ladder level N\n"
           "  --llvm-passes N  print the `opt -passes=` pipeline of level N\n"
           "  --opt-summary N  print what ladder level N does\n"
           "  --stats          print LunaJIT statistics\n"
           "  -v, --version    print version\n"
           "  -h, --help       show this help\n",
           LUNA_ENGINE_NAME, LUNA_VERSION_STRING, LUNA_PACKAGE_NAME,
           LUNA_VERSION_STRING, prog);
}

static void print_results(LunaState *s)
{
    int n = luna_result_count(s);
    int i;

    for (i = 1; i <= n; i++) {
        int t = luna_result_type(s, i);
        if (i > 1)
            printf("\t");
        switch (t) {
        case 1:
            printf("%.14g", luna_result_number(s, i));
            break;
        case 2:
            printf("%s", luna_result_string(s, i));
            break;
        case 3:
            printf("%s", luna_result_boolean(s, i) ? "true" : "false");
            break;
        case 0:
            printf("nil");
            break;
        default:
            printf("<value>");
            break;
        }
    }
    if (n > 0)
        printf("\n");
}

static int is_bytecode_name(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && strcmp(path + n - 4, ".lbc") == 0;
}

/* Lua binary chunks start with the 4-byte signature "\x1bLua".  Sniffing it
 * lets lunavm run whatever lunac produced, whatever the file is called
 * (lunac's default output is "luac.out", not ".lbc"). */
static int is_binary_chunk(const char *buf, size_t len)
{
    return buf != NULL && len >= 4 && memcmp(buf, LUA_SIGNATURE, 4) == 0;
}

static int run_file(LunaState *s, const char *path)
{
    size_t len = 0;
    char *buf = luna_read_file(path, &len);
    char chunk[512];
    int rc;

    if (buf == NULL)
        return luna_dofile(s, path);  /* sets "cannot read file" */
    snprintf(chunk, sizeof(chunk), "@%s", path);
    if (is_bytecode_name(path) || is_binary_chunk(buf, len))
        rc = luna_dobytecode(s, (const unsigned char *)buf, len, path);
    else
        rc = luna_dostring(s, buf, len, chunk);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* REPL                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
} ReplBuf;

typedef struct {
    int autoline;
    int autoclean;
} ReplOpt;

static void rb_init(ReplBuf *b)
{
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

static void rb_free(ReplBuf *b)
{
    free(b->ptr);
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

static void rb_clear(ReplBuf *b)
{
    if (b->ptr != NULL)
        b->ptr[0] = '\0';
    b->len = 0;
}

static int rb_append(ReplBuf *b, const char *s)
{
    size_t n = strlen(s);
    size_t cap;

    if (b->len + n + 1 > b->cap) {
        cap = b->cap != 0 ? b->cap * 2 : 256;
        while (b->len + n + 1 > cap)
            cap *= 2;
        {
            char *p = (char *)realloc(b->ptr, cap);
            if (p == NULL)
                return -1;
            b->ptr = p;
            b->cap = cap;
        }
    }
    memcpy(b->ptr + b->len, s, n);
    b->len += n;
    b->ptr[b->len] = '\0';
    return 0;
}

static int rb_append_line(ReplBuf *b, const char *s)
{
    if (b->len > 0 && rb_append(b, "\n") != 0)
        return -1;
    return rb_append(b, s);
}

static int repl_isatty(void)
{
#if defined(_WIN32)
    return _isatty(0) != 0;
#else
    return isatty(0) != 0;
#endif
}

static void repl_set_error_global(LunaState *s)
{
    lua_State *L = luna_raw(s);
    if (L != NULL) {
        lua_pushstring(L, luna_last_error(s));
        lua_setglobal(L, "_error");
    }
}

static void repl_set_result_global(LunaState *s)
{
    lua_State *L = luna_raw(s);
    int n = luna_result_count(s);
    if (L != NULL && n > 0) {
        lua_pushvalue(L, n);
        lua_setglobal(L, "_");
    }
}

static int repl_incomplete(LunaState *s, const char *src, size_t len)
{
    unsigned char *bc = NULL;
    size_t n = 0;
    int rc = luna_compile(s, src, len, "=stdin", &bc, &n, 0);

    if (rc == 0) {
        free(bc);
        return 0;
    }
    {
        const char *e = luna_last_error(s);
        return e != NULL && strstr(e, "<eof>") != NULL;
    }
}

static int repl_reset(LunaState **sp, const LunaOptions *opts)
{
    LunaState *ns = luna_new(opts);

    if (ns == NULL)
        return -1;
    luna_free(*sp);
    *sp = ns;
    return 0;
}

static void repl_autoclean(LunaState **sp, const LunaOptions *opts,
                           ReplOpt *o)
{
    if (!o->autoclean)
        return;
    if (repl_reset(sp, opts) == 0)
        printf("  context rebuilt (autoclean). scope is fresh.\n");
    else
        printf("  reset failed: could not rebuild context.\n");
}

static void repl_eval(LunaState **sp, const LunaOptions *opts,
                      const char *src, size_t len, ReplOpt *o)
{
    if (luna_dostring(*sp, src, len, "=stdin") != 0) {
        printf("luna: %s\n", luna_last_error(*sp));
        repl_set_error_global(*sp);
    } else {
        print_results(*sp);
        repl_set_result_global(*sp);
    }
    repl_autoclean(sp, opts, o);
}

static void repl_run_buffer(LunaState **sp, const LunaOptions *opts,
                            ReplBuf *buf, int *multi, ReplOpt *o)
{
    const char *src = buf->ptr;
    size_t len = buf->len;
    char *tmp = NULL;
    int rc;

    if (buf->len == 0) {
        *multi = 0;
        return;
    }
    if (src[0] == '=' && memchr(src, '\n', len) == NULL) {
        size_t n = len + 8;
        tmp = (char *)malloc(n);
        if (tmp == NULL) {
            printf("  out of memory.\n");
            rb_clear(buf);
            *multi = 0;
            return;
        }
        snprintf(tmp, n, "return %s", src + 1);
        src = tmp;
        len = strlen(tmp);
    }
    rc = luna_dostring(*sp, src, len, "=stdin");
    free(tmp);
    if (rc != 0) {
        printf("luna: %s\n", luna_last_error(*sp));
        repl_set_error_global(*sp);
        printf("  buffer cleared (one-shot). scope persists; "
               "use .reset for isolation.\n");
    } else {
        print_results(*sp);
        repl_set_result_global(*sp);
    }
    rb_clear(buf);
    *multi = 0;
    repl_autoclean(sp, opts, o);
}

static void repl_load(LunaState **sp, const LunaOptions *opts,
                      const char *path, ReplOpt *o)
{
    size_t len = 0;
    char *buf = luna_read_file(path, &len);
    char chunk[512];
    int rc;

    if (buf == NULL) {
        printf("  cannot open: %s\n", path);
        return;
    }
    snprintf(chunk, sizeof(chunk), "@%s", path);
    if (is_bytecode_name(path) || is_binary_chunk(buf, len))
        rc = luna_dobytecode(*sp, (const unsigned char *)buf, len, path);
    else
        rc = luna_dostring(*sp, buf, len, chunk);
    free(buf);
    if (rc != 0) {
        printf("luna: %s\n", luna_last_error(*sp));
        repl_set_error_global(*sp);
    } else {
        print_results(*sp);
        repl_set_result_global(*sp);
    }
    repl_autoclean(sp, opts, o);
}

static void repl_help(void)
{
    printf(
        "LunaBerry REPL — Type .help for this list\n"
        "\n"
        "Info:     .help .version .quit .exit .reset\n"
        "I/O:      .load <file>\n"
        "Buffer:   /begin /run /clear /break   (.break == /break)\n"
        "          /run is one-shot: the buffer always clears.\n"
        "          .autoclean toggles fresh-context-after-run.\n"
        "          .reset clears scope (stays in |).\n"
        "Engine:   --autoline flag (no /begin needed)\n"
        "Input:    =expr evaluates an expression; _ holds the last result.\n");
}

static void repl_version(void)
{
    printf("%s %s (Lua %s)\n", LUNA_ENGINE_NAME, LUNA_VERSION_STRING,
           LUNA_VERSION_STRING);
}

static int repl_dot_command(LunaState **sp, const LunaOptions *opts,
                            const char *line, ReplBuf *buf, int *multi,
                            ReplOpt *o)
{
    char cmd[64];
    const char *arg = line + 1;
    size_t ci = 0;

    while (*arg != '\0' && *arg != ' ' && ci + 1 < sizeof(cmd))
        cmd[ci++] = *arg++;
    cmd[ci] = '\0';
    while (*arg == ' ')
        arg++;

    if (strcmp(cmd, "help") == 0) {
        repl_help();
        return 1;
    }
    if (strcmp(cmd, "version") == 0) {
        repl_version();
        return 1;
    }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
        return 2;
    if (strcmp(cmd, "break") == 0) {
        if (!*multi) {
            printf("Error: .break requires an active multi-line buffer.\n");
        } else {
            rb_clear(buf);
            *multi = 0;
        }
        return 1;
    }
    if (strcmp(cmd, "reset") == 0) {
        if (repl_reset(sp, opts) == 0) {
            printf("  REPL context reset (fresh scope).");
            if (*multi)
                printf(" still in multi-line mode.");
            printf("\n");
        } else {
            printf("  reset failed: could not rebuild context.\n");
        }
        rb_clear(buf);
        return 1;
    }
    if (strcmp(cmd, "autoclean") == 0) {
        o->autoclean = !o->autoclean;
        printf(o->autoclean
                   ? "  autoclean ON: /run rebuilds context after execution.\n"
                   : "  autoclean OFF: scope persists across /run.\n");
        return 1;
    }
    if (strcmp(cmd, "autoline") == 0) {
        o->autoline = !o->autoline;
        printf(o->autoline
                   ? "  autoline ON: no /begin needed.\n"
                   : "  autoline OFF: use /begin ... /run.\n");
        return 1;
    }
    if (strcmp(cmd, "clear") == 0) {
        printf("\x1b[2J\x1b[H");
        fflush(stdout);
        return 1;
    }
    if (strcmp(cmd, "load") == 0) {
        if (*arg == '\0') {
            printf("  usage: .load <file>\n");
        } else {
            repl_load(sp, opts, arg, o);
        }
        return 1;
    }
    return 0;
}

static void repl(LunaState **sp, const LunaOptions *opts, int autoline,
                 int autoclean)
{
    char line[4096];
    ReplBuf buf;
    ReplOpt o;
    int multi = 0;
    int interrupted_once = 0;

    rb_init(&buf);
    o.autoline = autoline != 0;
    o.autoclean = autoclean != 0;

    signal(SIGINT, repl_sigint);

    printf("%s %s (Lua %s)\n", LUNA_ENGINE_NAME, LUNA_VERSION_STRING,
           LUNA_VERSION_STRING);
    printf("Type .help for commands. %s\n",
           o.autoline ? "autoline mode: no /begin needed."
                      : "multi-line: /begin ... /run");
    if (o.autoclean)
        printf("autoclean ON: context is rebuilt after every evaluation.\n");
    printf("\n");

    for (;;) {
        size_t len;
        const char *prompt = multi ? "lunaberry | " : "lunaberry > ";

        printf("%s", prompt);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (g_repl_interrupt) {
                g_repl_interrupt = 0;
                putchar('\n');
                if (multi) {
                    printf("(buffer discarded)\n");
                    rb_clear(&buf);
                    multi = 0;
                    interrupted_once = 0;
                } else if (interrupted_once) {
                    break;
                } else {
                    printf("Press Ctrl+C again to exit\n");
                    interrupted_once = 1;
                }
                continue;
            }
            if (multi && buf.len > 0 && repl_isatty()) {
                repl_run_buffer(sp, opts, &buf, &multi, &o);
                continue;
            }
            break;
        }
        interrupted_once = 0;

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;

        if (strcmp(line, "/begin") == 0 || strcmp(line, "begin") == 0) {
            if (multi) {
                printf("  already in multi-line mode. Use /clear or /break.\n");
            } else {
                rb_clear(&buf);
                multi = 1;
            }
            continue;
        }
        if (strcmp(line, "/run") == 0 || strcmp(line, "run") == 0) {
            if (!multi) {
                printf("Error: /run requires an active multi-line buffer.\n"
                       "  start one with /begin\n");
                continue;
            }
            repl_run_buffer(sp, opts, &buf, &multi, &o);
            continue;
        }
        if (strcmp(line, "/clear") == 0) {
            if (!multi) {
                printf("Error: /clear requires an active multi-line buffer.\n"
                       "  start one with /begin\n");
                continue;
            }
            rb_clear(&buf);
            printf("  buffer cleared. still in multi-line mode.\n");
            continue;
        }
        if (strcmp(line, "/break") == 0 || strcmp(line, ".break") == 0) {
            if (!multi) {
                printf("Error: /break requires an active multi-line buffer.\n"
                       "  start one with /begin\n");
                continue;
            }
            rb_clear(&buf);
            multi = 0;
            continue;
        }
        if (line[0] == '.') {
            int r = repl_dot_command(sp, opts, line, &buf, &multi, &o);
            if (r == 2)
                break;
            if (r == 1)
                continue;
        }
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
            break;

        if (multi) {
            if (rb_append_line(&buf, line) != 0) {
                printf("  buffer overflow.\n");
                rb_clear(&buf);
                multi = 0;
                continue;
            }
            if (o.autoline && !repl_incomplete(*sp, buf.ptr, buf.len))
                repl_run_buffer(sp, opts, &buf, &multi, &o);
            continue;
        }

        if (o.autoline && repl_incomplete(*sp, line, len)) {
            rb_clear(&buf);
            rb_append(&buf, line);
            multi = 1;
            continue;
        }

        if (line[0] == '=') {
            size_t n = len + 8;
            char *src = (char *)malloc(n);
            if (src == NULL) {
                printf("  out of memory.\n");
                continue;
            }
            snprintf(src, n, "return %s", line + 1);
            repl_eval(sp, opts, src, strlen(src), &o);
            free(src);
        } else {
            repl_eval(sp, opts, line, len, &o);
        }
    }
    rb_free(&buf);
    printf("\n");
}

static int run_jit(LunaState *s, const char *path, const char *backend_name,
                   const char *cache_dir, const char *jit_flags,
                   int internal_loop, unsigned hot_threshold, int stats)
{
    size_t len = 0;
    unsigned char *bc;
    LunaJitBackend backend = LUNA_JIT_AUTO;
    LunaJitOptions opts;
    LunaJit *jit;
    char err[512];
    double result;

    if (is_bytecode_name(path)) {
        bc = (unsigned char *)luna_read_file(path, &len);
    } else {
        if (luna_compile_file(s, path, &bc, &len, 0) != 0) {
            fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
            return -1;
        }
    }
    if (bc == NULL) {
        fprintf(stderr, "%s: cannot read '%s'\n", prog, path);
        return -1;
    }

    if (backend_name != NULL) {
        if (strcmp(backend_name, "cc") == 0)
            backend = LUNA_JIT_CC;
        else if (strcmp(backend_name, "llvm") == 0)
            backend = LUNA_JIT_LLVM;
        else if (strcmp(backend_name, "berry") == 0)
            backend = LUNA_JIT_BERRY;
        else if (strcmp(backend_name, "none") == 0)
            backend = LUNA_JIT_NONE;
    }

    memset(&opts, 0, sizeof(opts));
    opts.cache_dir = cache_dir;
    opts.cc_flags = (jit_flags != NULL && jit_flags[0] != '\0') ? jit_flags
                                                               : NULL;
    opts.internal_loop = internal_loop;
    opts.hot_threshold = hot_threshold;
    jit = luna_jit_new(backend, &opts);
    if (jit == NULL) {
        free(bc);
        fprintf(stderr, "%s: cannot create JIT\n", prog);
        return -1;
    }

    printf("luna: jit backend=%s\n", luna_jit_backend_name(jit));
    result = luna_jit_call(jit, bc, len, path, err, sizeof(err));
    if (err[0] != '\0') {
        fprintf(stderr, "%s: jit: %s\n", prog, err);
        fprintf(stderr, "%s: falling back to interpreter\n", prog);
        if (run_file(s, path) == 0)
            print_results(s);
        else
            fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
    } else {
        printf("%.14g\n", result);
    }

    if (stats) {
        LunaJitStats st = luna_jit_stats(jit);
        printf("jit stats: attempts=%lu ok=%lu failed=%lu native=%lu "
               "interp=%lu cache=%lu disk_hits=%lu disk_misses=%lu "
               "disk_writes=%lu\n",
               st.compile_attempts, st.compile_ok, st.compile_failed,
               st.native_calls, st.interp_calls, st.cache_hits,
               st.cache_disk_hits, st.cache_disk_misses, st.cache_disk_writes);
    }

    luna_jit_free(jit);
    free(bc);
    return err[0] == '\0' ? 0 : -1;
}

static int run_aot(LunaState *s, const char *path, const char *backend_name)
{
    size_t len = 0;
    unsigned char *bc = NULL;
    char tmp[1024];
    char dir[1600];
    char root[1024];
    char lib[1024];
    char stem[256];
    char out[2048];
    char err[512];
    int rc = 0;

    if (backend_name == NULL || strcmp(backend_name, "lbc") == 0) {
        if (run_file(s, path) != 0)
            return -1;
        print_results(s);
        return 0;
    }

    if (is_bytecode_name(path)) {
        bc = (unsigned char *)luna_read_file(path, &len);
    } else if (luna_compile_file(s, path, &bc, &len, 0) != 0) {
        fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
        return -1;
    }
    if (bc == NULL) {
        fprintf(stderr, "%s: cannot read '%s'\n", prog, path);
        return -1;
    }

    luna_tmp_dir(tmp, sizeof(tmp));
    luna_stem(path, stem, sizeof(stem));
    snprintf(dir, sizeof(dir), "%s/luna-aot-%s", tmp, stem);
    luna_mkdir_p(dir);
    luna_find_include_dir(root, sizeof(root));
    luna_exe_dir(lib, sizeof(lib));

    if (strcmp(backend_name, "llvm") == 0) {
        LunaLLvmOptions lo;
        if (!luna_llvm_available()) {
            fprintf(stderr, "%s: clang not found; running bytecode instead\n",
                    prog);
            if (run_file(s, path) != 0)
                rc = -1;
            else
                print_results(s);
            free(bc);
            return rc;
        }
        memset(&lo, 0, sizeof(lo));
        lo.flags = "-O2";
        lo.lib_dir = lib;
        lo.include_dir = root;
#if defined(_WIN32)
        snprintf(out, sizeof(out), "%s/%s.exe", dir, stem);
#else
        snprintf(out, sizeof(out), "%s/%s", dir, stem);
#endif
        if (luna_llvm_aot(bc, len, out, path, &lo, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: aot: %s\n", prog, err);
            rc = -1;
        } else if (luna_run_cmd_inherit(out) != 0) {
            rc = -1;
        }
    } else if (strcmp(backend_name, "berry") == 0) {
        LunaBerryOptions bo;
        memset(&bo, 0, sizeof(bo));
        bo.out_dir = dir;
        bo.include_dir = root;
        bo.lib_dir = lib;
        bo.emit_host = 1;
        if (luna_berry_emit(bc, len, stem, &bo, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: berry: %s\n", prog, err);
            rc = -1;
        } else {
#if defined(_WIN32)
            snprintf(out, sizeof(out), "%s/%s.host.exe", dir, stem);
#else
            snprintf(out, sizeof(out), "%s/%s.host", dir, stem);
#endif
            if (luna_run_cmd_inherit(out) != 0)
                rc = -1;
        }
    } else {
        fprintf(stderr, "%s: unknown AOT backend '%s'\n", prog, backend_name);
        rc = -1;
    }

    free(bc);
    return rc;
}

int main(int argc, char **argv)
{
    LunaOptions opts;
    LunaState *s;
    const char *file = NULL;
    const char *code = NULL;
    const char *out = NULL;
    const char *backend = NULL;
    const char *cache_dir = NULL;
    size_t mem_limit = 0;
    int interactive = 0;
    int compile_only = 0;
    int strip = 0;
    int stats = 0;
    int jit_mode = 0;
    int aot_mode = 0;
    int autoline = 0;
    int autoclean = 0;
    int internal_loop = 0;
    int native_tune = 0;
    int jit_level = -1;
    int opt_level = -1;
    unsigned hot_threshold = 100;
    char jit_flags[512] = "";
    int i;
    int rc = 0;

    prog = luna_basename(argv[0]);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s (Lua %s)\n%s\n", LUNA_ENGINE_NAME,
                   LUNA_VERSION_STRING, LUNA_VERSION_STRING,
                   LUNA_PACKAGE_NAME);
            return 0;
        } else if (strcmp(a, "-e") == 0 && i + 1 < argc) {
            code = argv[++i];
        } else if (strcmp(a, "-i") == 0) {
            interactive = 1;
        } else if (strcmp(a, "-c") == 0) {
            compile_only = 1;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "-s") == 0) {
            strip = 1;
        } else if (strcmp(a, "--stats") == 0) {
            stats = 1;
        } else if (strcmp(a, "--autoline") == 0) {
            autoline = 1;
        } else if (strcmp(a, "--autoclean") == 0) {
            autoclean = 1;
        } else if (strcmp(a, "--internal-loop") == 0) {
            internal_loop = 1;
        } else if (strcmp(a, "--native") == 0) {
            native_tune = 1;
        } else if (strcmp(a, "--jit-level") == 0 && i + 1 < argc) {
            /* Deprecated alias, kept for compatibility: clamps to -O0..-O3. */
            jit_level = atoi(argv[++i]);
            if (jit_level < 0)
                jit_level = 0;
            if (jit_level > 3)
                jit_level = 3;
        } else if (strcmp(a, "-O") == 0 && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
        } else if (strncmp(a, "-O", 2) == 0 && a[2] >= '0' && a[2] <= '9') {
            opt_level = atoi(a + 2);
        } else if (strcmp(a, "--opt-level") == 0 && i + 1 < argc) {
            opt_level = atoi(argv[++i]);
        } else if (strcmp(a, "--hot-threshold") == 0 && i + 1 < argc) {
            hot_threshold = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--opt-flags") == 0 && i + 1 < argc) {
            int lvl = atoi(argv[++i]);
            if (lvl < LUNA_OPT_MIN || lvl > LUNA_OPT_MAX) {
                fprintf(stderr, "%s: invalid ladder level %d\n", prog, lvl);
                return 2;
            }
            printf("%s\n", luna_opt_flags(lvl));
            return 0;
        } else if (strcmp(a, "--llvm-flags") == 0 && i + 1 < argc) {
            int lvl = atoi(argv[++i]);
            if (lvl < LUNA_OPT_MIN || lvl > LUNA_OPT_MAX) {
                fprintf(stderr, "%s: invalid ladder level %d\n", prog, lvl);
                return 2;
            }
            printf("%s\n", luna_opt_llvm_flags(lvl));
            return 0;
        } else if (strcmp(a, "--llvm-passes") == 0 && i + 1 < argc) {
            int lvl = atoi(argv[++i]);
            if (lvl < LUNA_OPT_MIN || lvl > LUNA_OPT_MAX) {
                fprintf(stderr, "%s: invalid ladder level %d\n", prog, lvl);
                return 2;
            }
            printf("%s\n", luna_opt_llvm_passes(lvl));
            return 0;
        } else if (strcmp(a, "--opt-summary") == 0 && i + 1 < argc) {
            int lvl = atoi(argv[++i]);
            if (lvl < LUNA_OPT_MIN || lvl > LUNA_OPT_MAX) {
                fprintf(stderr, "%s: invalid ladder level %d\n", prog, lvl);
                return 2;
            }
            printf("O%d %s: %s\n", lvl, luna_opt_name(lvl),
                   luna_opt_summary(lvl));
            return 0;
        } else if (strcmp(a, "--jit") == 0) {
            jit_mode = 1;
        } else if (strncmp(a, "--jit=", 6) == 0) {
            jit_mode = 1;
            backend = a + 6;
        } else if (strcmp(a, "--aot") == 0) {
            aot_mode = 1;
        } else if (strncmp(a, "--aot=", 6) == 0) {
            aot_mode = 1;
            backend = a + 6;
        } else if (strcmp(a, "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            ++i; /* accepted for CLI symmetry; the tool CLIs use it */
        } else if (strcmp(a, "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(a, "--mem-limit") == 0 && i + 1 < argc) {
            mem_limit = (size_t)strtoull(argv[++i], NULL, 0);
        } else if (strcmp(a, "--") == 0) {
            if (i + 1 < argc)
                file = argv[++i];
            break;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
            return 2;
        } else {
            file = a;
        }
    }

    memset(&opts, 0, sizeof(opts));
    opts.mem_limit = mem_limit;
    opts.open_libs = 1;
    s = luna_new(&opts);
    if (s == NULL) {
        fprintf(stderr, "%s: cannot create engine\n", prog);
        return 1;
    }

    if (compile_only) {
        unsigned char *bc = NULL;
        size_t len = 0;
        char default_out[1024];
        const char *target = out;

        if (file == NULL) {
            fprintf(stderr, "%s: -c needs an input file\n", prog);
            luna_free(s);
            return 2;
        }
        if (opt_level >= 0 && (opt_level < LUNA_OPT_MIN || opt_level > LUNA_OPT_MAX)) {
            fprintf(stderr,
                    "%s: invalid optimization level %d (valid: %d..%d)\n",
                    prog, opt_level, LUNA_OPT_MIN, LUNA_OPT_MAX);
            luna_free(s);
            return 2;
        }
        if (target == NULL) {
            char stem[256];
            luna_stem(file, stem, sizeof(stem));
            snprintf(default_out, sizeof(default_out), "%s.lbc", stem);
            target = default_out;
        }
        if (luna_compile_file(s, file, &bc, &len, strip) != 0) {
            fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
            luna_free(s);
            return 1;
        }
        if (luna_write_file(target, bc, len) != 0) {
            fprintf(stderr, "%s: cannot write '%s'\n", prog, target);
            free(bc);
            luna_free(s);
            return 1;
        }
        printf("%s: wrote %s (%lu bytes)\n", prog, target,
               (unsigned long)len);
        free(bc);
        luna_free(s);
        return 0;
    }

    if (code != NULL) {
        if (luna_dostring(s, code, strlen(code), "=(command line)") != 0) {
            fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
            rc = 1;
        } else {
            print_results(s);
        }
    }

    if (jit_mode) {
        int lvl = luna_opt_clamp(opt_level >= 0 ? opt_level
                                 : (jit_level >= 0 ? jit_level : 2));
        opt_level = lvl;
        luna_opt_warn_redundant(lvl, 0, 0);
        snprintf(jit_flags, sizeof(jit_flags), "%s", luna_opt_flags(lvl));
        if (native_tune)
            snprintf(jit_flags + strlen(jit_flags),
                     sizeof(jit_flags) - strlen(jit_flags),
                     " -march=native -mtune=native");
    }

    if (file != NULL) {
        if (jit_mode) {
            rc = run_jit(s, file, backend, cache_dir, jit_flags,
                         internal_loop, hot_threshold, stats);
        } else if (aot_mode) {
            if (run_aot(s, file, backend != NULL ? backend : "llvm") != 0)
                rc = 1;
        } else {
            if (run_file(s, file) != 0) {
                fprintf(stderr, "%s: %s\n", prog, luna_last_error(s));
                rc = 1;
            } else {
                print_results(s);
            }
        }
    }

    if (interactive || (file == NULL && code == NULL && !jit_mode)) {
        repl(&s, &opts, autoline, autoclean);
    }

    luna_free(s);
    return rc;
}
