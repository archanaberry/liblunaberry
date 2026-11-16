/*
 * luna-jit.c — LunaJIT implementation.
 *
 * Pipeline (CC / LLVM backends):
 *   .lbc bytes -> C shim with embedded chunk -> gcc/clang -shared -> load
 *
 * The shim is intentionally tiny; all engine logic stays in the host. This
 * keeps the JIT portable and lets the interpreter remain the single source
 * of truth for Lua semantics.
 */

#include "luna-jit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luna-common.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <process.h>
#  define LUNA_SHLIB_EXT ".dll"
#  define LUNA_GETPID ((long)_getpid())
static void *luna_dlopen(const char *path) { return (void *)LoadLibraryA(path); }
static void *luna_dlsym(void *h, const char *n)
{
    return (void *)GetProcAddress((HMODULE)h, n);
}
static void luna_dlclose(void *h)
{
    if (h != NULL)
        FreeLibrary((HMODULE)h);
}
static const char *luna_dlerror(void)
{
    static char buf[256];
    DWORD e = GetLastError();
    snprintf(buf, sizeof(buf), "win32 error %lu", (unsigned long)e);
    return buf;
}
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  define LUNA_SHLIB_EXT ".so"
#  define LUNA_GETPID ((long)getpid())
static void *luna_dlopen(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *luna_dlsym(void *h, const char *n) { return dlsym(h, n); }
static void luna_dlclose(void *h)
{
    if (h != NULL)
        dlclose(h);
}
static const char *luna_dlerror(void) { return dlerror(); }
#endif

#define LUNA_JIT_ENTRY "luna_jit_entry"
#define LUNA_JIT_CACHE_MAX 64

typedef struct LunaJitEntry {
    unsigned long long key;
    LunaJitFn fn;
    void *handle;
    char *path;
} LunaJitEntry;

struct LunaJit {
    LunaJitBackend backend;
    char *cc;
    char *cc_flags;
    char *lib_dir;
    char *tmp_dir;
    char *cache_dir;
    char *last_ir;
    unsigned hot_threshold;
    int internal_loop;
    unsigned long counter;
    LunaJitStats stats;
    LunaJitEntry cache[LUNA_JIT_CACHE_MAX];
    size_t cache_count;
    char last_error[512];
};

static char *luna_strdup(const char *s)
{
    size_t n;
    char *p;
    if (s == NULL)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p != NULL)
        memcpy(p, s, n);
    return p;
}

static void luna_jit_error(LunaJit *jit, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(jit->last_error, sizeof(jit->last_error), fmt, ap);
    va_end(ap);
}

static void luna_jit_clear_error(LunaJit *jit)
{
    jit->last_error[0] = '\0';
}

static int luna_cmd_ok(const char *cmd)
{
    return luna_run_cmd(cmd, NULL, 0) == 0;
}

static const char *luna_default_cc(void)
{
    const char *cc = getenv("CC");
    if (cc != NULL && cc[0] != '\0')
        return cc;
#if defined(_WIN32)
    return "gcc";
#else
    return "cc";
#endif
}

LunaJit *luna_jit_new(LunaJitBackend backend, const LunaJitOptions *opts)
{
    LunaJit *jit = (LunaJit *)calloc(1, sizeof(*jit));
    char dir[1024];

    if (jit == NULL)
        return NULL;

    jit->backend = backend;
    if (opts != NULL && opts->cc != NULL) {
        jit->cc = luna_strdup(opts->cc);
    } else {
        char tool[1200];
#if defined(_WIN32)
        const char *want = backend == LUNA_JIT_LLVM ? "clang" : "gcc";
#else
        const char *want = backend == LUNA_JIT_LLVM ? "clang" : "cc";
#endif
        if (luna_find_tool(want, tool, sizeof(tool)) == 0)
            jit->cc = luna_strdup(tool);
        else
            jit->cc = luna_strdup(backend == LUNA_JIT_LLVM ? "clang"
                                                           : luna_default_cc());
    }
    jit->cc_flags = luna_strdup(opts != NULL ? opts->cc_flags : NULL);
    jit->hot_threshold = opts != NULL ? opts->hot_threshold : 100;
    jit->internal_loop = opts != NULL ? opts->internal_loop : 0;

    luna_exe_dir(dir, sizeof(dir));
    jit->lib_dir = luna_strdup(opts != NULL && opts->lib_dir ? opts->lib_dir : dir);
    if (opts != NULL && opts->tmp_dir != NULL) {
        jit->tmp_dir = luna_strdup(opts->tmp_dir);
    } else {
        char tmp[1024];
        luna_tmp_dir(tmp, sizeof(tmp));
        jit->tmp_dir = luna_strdup(tmp);
    }
    jit->cache_dir = luna_strdup(opts != NULL ? opts->cache_dir : NULL);

    if (jit->cc == NULL || jit->lib_dir == NULL || jit->tmp_dir == NULL) {
        luna_jit_free(jit);
        return NULL;
    }

    if (backend == LUNA_JIT_AUTO) {
        jit->backend = LUNA_JIT_CC;
    }

    if (backend == LUNA_JIT_LLVM) {
        char probe[1400];
        char tool[1200];
        snprintf(probe, sizeof(probe), "\"%s\" --version", jit->cc);
        if (!luna_cmd_ok(probe)) {
            free(jit->cc);
            if (luna_find_tool("clang", tool, sizeof(tool)) == 0)
                jit->cc = luna_strdup(tool);
            else
                jit->cc = luna_strdup("clang");
            snprintf(probe, sizeof(probe), "\"%s\" --version", jit->cc);
            if (!luna_cmd_ok(probe)) {
                /* Graceful degradation: stay usable with the C backend. */
                free(jit->cc);
#if defined(_WIN32)
                if (luna_find_tool("gcc", tool, sizeof(tool)) == 0)
#else
                if (luna_find_tool("cc", tool, sizeof(tool)) == 0)
#endif
                    jit->cc = luna_strdup(tool);
                else
                    jit->cc = luna_strdup(luna_default_cc());
                jit->backend = LUNA_JIT_CC;
                luna_jit_error(jit, "clang unavailable; using CC backend");
            }
        }
    }

    if (jit->cache_dir != NULL && jit->cache_dir[0] != '\0')
        luna_mkdir_p(jit->cache_dir);

    return jit;
}

void luna_jit_free(LunaJit *jit)
{
    size_t i;

    if (jit == NULL)
        return;
    for (i = 0; i < jit->cache_count; i++) {
        luna_dlclose(jit->cache[i].handle);
        free(jit->cache[i].path);
    }
    free(jit->cc);
    free(jit->cc_flags);
    free(jit->lib_dir);
    free(jit->tmp_dir);
    free(jit->cache_dir);
    free(jit->last_ir);
    free(jit);
}

const char *luna_jit_backend_name(const LunaJit *jit)
{
    if (jit == NULL)
        return "none";
    switch (jit->backend) {
    case LUNA_JIT_CC:
        return "cc";
    case LUNA_JIT_LLVM:
        return "llvm";
    case LUNA_JIT_BERRY:
        return "berry";
    case LUNA_JIT_NONE:
        return "none";
    default:
        return "auto";
    }
}

const char *luna_jit_last_error(const LunaJit *jit)
{
    return jit != NULL ? jit->last_error : "no jit";
}

const char *luna_jit_last_ir(const LunaJit *jit)
{
    return jit != NULL ? jit->last_ir : NULL;
}

LunaJitStats luna_jit_stats(const LunaJit *jit)
{
    LunaJitStats empty;
    memset(&empty, 0, sizeof(empty));
    return jit != NULL ? jit->stats : empty;
}

static LunaJitEntry *luna_jit_find(LunaJit *jit, unsigned long long key)
{
    size_t i;
    for (i = 0; i < jit->cache_count; i++) {
        if (jit->cache[i].key == key)
            return &jit->cache[i];
    }
    return NULL;
}

static LunaJitEntry *luna_jit_add(LunaJit *jit, unsigned long long key,
                                  LunaJitFn fn, void *handle, const char *path)
{
    LunaJitEntry *e;

    if (jit->cache_count == LUNA_JIT_CACHE_MAX) {
        luna_dlclose(jit->cache[0].handle);
        free(jit->cache[0].path);
        memmove(&jit->cache[0], &jit->cache[1],
                sizeof(jit->cache[0]) * (LUNA_JIT_CACHE_MAX - 1));
        jit->cache_count--;
    }
    e = &jit->cache[jit->cache_count++];
    e->key = key;
    e->fn = fn;
    e->handle = handle;
    e->path = luna_strdup(path);
    return e;
}

static unsigned long long luna_jit_key(const LunaJit *jit,
                                       const unsigned char *bc, size_t len,
                                       const char *name)
{
    unsigned long long h = luna_hash64(bc, len);
    h ^= luna_hash64_str(name != NULL ? name : "");
    h ^= luna_hash64_str(LUNA_VERSION_STRING);
    h ^= luna_hash64_str(jit->cc_flags != NULL ? jit->cc_flags : "");
    h ^= luna_hash64_str(jit->cc != NULL ? jit->cc : "");
    h ^= (unsigned long long)jit->backend << 56;
    if (jit->internal_loop)
        h ^= 0x9E3779B97F4A7C15ULL;
    return h;
}

static LunaJitFn luna_jit_load(LunaJit *jit, const char *path, void **phandle)
{
    void *handle = luna_dlopen(path);
    LunaJitFn fn;

    if (handle == NULL) {
        luna_jit_error(jit, "cannot load %s: %s", path, luna_dlerror());
        return NULL;
    }
    fn = (LunaJitFn)(void *)luna_dlsym(handle, LUNA_JIT_ENTRY);
    if (fn == NULL) {
        luna_jit_error(jit, "%s: missing %s", path, LUNA_JIT_ENTRY);
        luna_dlclose(handle);
        return NULL;
    }
    *phandle = handle;
    return fn;
}

static int luna_jit_write_shim(const char *c_path, const unsigned char *bc,
                               size_t len, char *err, size_t errlen)
{
    FILE *f = fopen(c_path, "wb");
    if (f == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "cannot write %s", c_path);
        return -1;
    }

    fprintf(f,
            "/* generated by LunaJIT — do not edit */\n"
            "#include <stddef.h>\n"
            "#include <stdio.h>\n"
            "extern double luna_jit_host_run(const char *, size_t, char *, size_t);\n\n");
    luna_emit_c_array(f, "luna_bc", bc, len);
    fprintf(f,
            "\n/* must stay visible even when the shim is built at ladder\n"
            "   level 7+ (-fvisibility=hidden) */\n"
            "#if defined(_WIN32)\n"
            "__declspec(dllexport)\n"
            "#elif defined(__GNUC__)\n"
            "__attribute__((visibility(\"default\")))\n"
            "#endif\n"
            "double luna_jit_entry(void)\n"
            "{\n"
            "    char err[512];\n"
            "    double r;\n"
            "    err[0] = 0;\n"
            "    r = luna_jit_host_run((const char *)luna_bc, luna_bc_len,\n"
            "                          err, sizeof(err));\n"
            "    if (err[0])\n"
            "        fprintf(stderr, \"luna jit: %%s\\n\", err);\n"
            "    return r;\n"
            "}\n");
    if (fclose(f) != 0) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "cannot close %s", c_path);
        return -1;
    }
    return 0;
}

static int luna_jit_emit_shim(LunaJit *jit, const char *c_path,
                              const unsigned char *bc, size_t len)
{
    char err[256];
    if (luna_jit_write_shim(c_path, bc, len, err, sizeof(err)) != 0) {
        luna_jit_error(jit, "%s", err);
        return -1;
    }
    return 0;
}

int luna_jit_emit_c(const unsigned char *bc, size_t len, const char *out_c,
                    char *err, size_t errlen)
{
    return luna_jit_write_shim(out_c, bc, len, err, errlen);
}

static int luna_jit_compile_shim(LunaJit *jit, const char *c_path,
                                 const char *so_path)
{
    char cmd[4096];
    char out[2048];
    char tflags[1400];
    const char *flags = jit->cc_flags;
    int rc;

    if (flags == NULL)
        flags = jit->backend == LUNA_JIT_LLVM ? "-O3" : "-O2";

    tflags[0] = '\0';
    if (jit->internal_loop) {
        snprintf(tflags, sizeof(tflags), "-funroll-loops");
        if (jit->backend == LUNA_JIT_LLVM) {
#if defined(_WIN32)
            char inc[1024];
            if (luna_mingw_include(inc, sizeof(inc)) == 0)
                snprintf(tflags + strlen(tflags),
                         sizeof(tflags) - strlen(tflags),
                         " --target=x86_64-w64-windows-gnu -isystem \"%s\"",
                         inc);
#endif
        }
    } else if (jit->backend == LUNA_JIT_LLVM) {
#if defined(_WIN32)
        char inc[1024];
        if (luna_mingw_include(inc, sizeof(inc)) == 0)
            snprintf(tflags, sizeof(tflags),
                     "--target=x86_64-w64-windows-gnu -isystem \"%s\"", inc);
#endif
    }

    if (jit->backend == LUNA_JIT_LLVM) {
        char ll_path[1200];
        snprintf(ll_path, sizeof(ll_path), "%s.ll", so_path);
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" %s %s -S -emit-llvm -o \"%s\" \"%s\"",
                 jit->cc, tflags, flags, ll_path, c_path);
        (void)luna_run_cmd(cmd, out, sizeof(out));
        free(jit->last_ir);
        jit->last_ir = luna_read_file(ll_path, NULL);
    }

#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
             "\"%s\" %s %s -shared -o \"%s\" \"%s\" \"%s/liblunaberry.a\" -lm",
             jit->cc, tflags, flags, so_path, c_path, jit->lib_dir);
#else
    snprintf(cmd, sizeof(cmd),
             "\"%s\" %s %s -fPIC -shared -o \"%s\" \"%s\" \"%s/liblunaberry.a\" -lm",
             jit->cc, tflags, flags, so_path, c_path, jit->lib_dir);
#endif
    rc = luna_run_cmd(cmd, out, sizeof(out));
    if (rc != 0) {
        luna_jit_error(jit, "compiler failed (%d): %s", rc, out);
        return -1;
    }
    return 0;
}

LunaJitFn luna_jit_compile(LunaJit *jit, const unsigned char *bc, size_t len,
                           const char *name)
{
    unsigned long long key;
    LunaJitEntry *e;
    char base[1024];
    char c_path[1100];
    char so_path[1100];
    char cache_path[1100];
    char keybuf[32];
    void *handle = NULL;
    LunaJitFn fn;

    if (jit == NULL) {
        return NULL;
    }
    if (jit->backend == LUNA_JIT_NONE || jit->backend == LUNA_JIT_BERRY) {
        luna_jit_error(jit, "backend '%s' cannot produce shared shims",
                       luna_jit_backend_name(jit));
        jit->stats.compile_failed++;
        return NULL;
    }
    if (bc == NULL || len == 0) {
        luna_jit_error(jit, "empty bytecode");
        jit->stats.compile_failed++;
        return NULL;
    }

    luna_jit_clear_error(jit);
    key = luna_jit_key(jit, bc, len, name);
    e = luna_jit_find(jit, key);
    if (e != NULL) {
        jit->stats.cache_hits++;
        return e->fn;
    }

    snprintf(keybuf, sizeof(keybuf), "%016llx", key);

    if (jit->cache_dir != NULL && jit->cache_dir[0] != '\0') {
        snprintf(cache_path, sizeof(cache_path), "%s/%s%s", jit->cache_dir,
                 keybuf, LUNA_SHLIB_EXT);
        if (luna_file_exists(cache_path)) {
            fn = luna_jit_load(jit, cache_path, &handle);
            if (fn != NULL) {
                luna_jit_add(jit, key, fn, handle, cache_path);
                jit->stats.cache_disk_hits++;
                return fn;
            }
            jit->stats.cache_disk_errors++;
        }
    }

    jit->stats.compile_attempts++;
    jit->counter++;
    snprintf(base, sizeof(base), "%s/lunajit_%ld_%lu", jit->tmp_dir,
             LUNA_GETPID, jit->counter);
    snprintf(c_path, sizeof(c_path), "%s.c", base);
    snprintf(so_path, sizeof(so_path), "%s%s", base, LUNA_SHLIB_EXT);

    if (luna_jit_emit_shim(jit, c_path, bc, len) != 0) {
        jit->stats.compile_failed++;
        return NULL;
    }
    if (luna_jit_compile_shim(jit, c_path, so_path) != 0) {
        remove(c_path);
        jit->stats.compile_failed++;
        return NULL;
    }
    remove(c_path);

    fn = luna_jit_load(jit, so_path, &handle);
    if (fn == NULL) {
        jit->stats.compile_failed++;
        remove(so_path);
        return NULL;
    }

    jit->stats.compile_ok++;
    luna_jit_add(jit, key, fn, handle, so_path);

    if (jit->cache_dir != NULL && jit->cache_dir[0] != '\0') {
        size_t n = 0;
        char *data = luna_read_file(so_path, &n);
        if (data != NULL) {
            if (luna_write_file(cache_path, data, n) == 0)
                jit->stats.cache_disk_writes++;
            else
                jit->stats.cache_disk_errors++;
            free(data);
        } else {
            jit->stats.cache_disk_errors++;
        }
    }

    return fn;
}

double luna_jit_call(LunaJit *jit, const unsigned char *bc, size_t len,
                     const char *name, char *err, size_t errlen)
{
    LunaJitFn fn;

    if (err != NULL && errlen > 0)
        err[0] = '\0';
    fn = luna_jit_compile(jit, bc, len, name);
    if (fn == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "%s", luna_jit_last_error(jit));
        if (jit != NULL)
            jit->stats.interp_calls++;
        return 0.0;
    }
    jit->stats.native_calls++;
    return fn();
}

/* ------------------------------------------------------------------ */
/* Copy path and patch: prebuild at build time, dlopen at runtime      */
/* ------------------------------------------------------------------ */

int luna_jit_prebuild(LunaJit *jit, const unsigned char *bc, size_t len,
                      const char *shim_path, char *err, size_t errlen)
{
    char c_path[1200];

    if (err != NULL && errlen > 0)
        err[0] = '\0';
    if (jit == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "invalid JIT");
        return -1;
    }

    snprintf(c_path, sizeof(c_path), "%s.jit.c", shim_path);
    if (luna_jit_emit_shim(jit, c_path, bc, len) != 0) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "%s", luna_jit_last_error(jit));
        return -1;
    }
    if (luna_jit_compile_shim(jit, c_path, shim_path) != 0) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "%s", luna_jit_last_error(jit));
        remove(c_path);
        return -1;
    }
    remove(c_path);
    return 0;
}

double luna_jit_run_shim(const char *shim, char *err, size_t errlen)
{
    char path[1400];
    void *handle;
    LunaJitFn fn;
    double r;

    if (err != NULL && errlen > 0)
        err[0] = '\0';
    if (shim == NULL || shim[0] == '\0') {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "no shim path");
        return 0.0;
    }

    /* Resolve a relative name against the calling executable's own
     * directory so a launcher can just name the shim it ships beside it. */
    if (shim[0] == '/' || shim[0] == '\\' ||
        ((unsigned char)shim[0] != 0 && shim[1] == ':')) {
        snprintf(path, sizeof(path), "%s", shim);
    } else {
        char dir[1024];
        luna_exe_dir(dir, sizeof(dir));
        luna_path_join(path, sizeof(path), dir, shim);
    }

    handle = luna_dlopen(path);
    if (handle == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "cannot load shim %s: %s", path,
                     luna_dlerror());
        return 0.0;
    }
    fn = (LunaJitFn)luna_dlsym(handle, "luna_jit_entry");
    if (fn == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "shim %s has no luna_jit_entry: %s", path,
                     luna_dlerror());
        luna_dlclose(handle);
        return 0.0;
    }
    r = fn();
    luna_dlclose(handle);
    return r;
}
