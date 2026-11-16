/*
 * luna-jit.h — LunaJIT: compile-on-first-use native tier for LunaBerry.
 *
 * The JIT compiles a precompiled Luna chunk (.lbc) into a tiny native shim
 * at runtime, loads it, and calls it through a plain function pointer:
 *
 *     double luna_jit_entry(void)
 *
 * The shim embeds the chunk and calls the engine's exported
 * luna_jit_host_run().  This mirrors the "CC JIT" design used by
 * libmochiberry: the toolchain is paid once, the result is cached, and
 * hosts without a compiler simply fall back to the interpreter.
 *
 * Backends:
 *   CC    — system C compiler (gcc/cc), -O2
 *   LLVM  — clang/LLVM, -O3 (and optional .ll emission)
 *   BERRY — not a shared-library backend; the CLI dispatches it to the
 *           BeASM emitter in luna-berry.h
 */

#ifndef LUNA_JIT_H
#define LUNA_JIT_H

#include <stddef.h>

#include "luna.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LunaJitBackend {
    LUNA_JIT_AUTO = 0,
    LUNA_JIT_NONE,
    LUNA_JIT_CC,
    LUNA_JIT_LLVM,
    LUNA_JIT_BERRY
} LunaJitBackend;

typedef struct LunaJitOptions {
    const char *cc;         /* compiler binary; NULL = $CC, then gcc/cc */
    const char *cc_flags;   /* extra flags; NULL = backend default      */
    const char *lib_dir;    /* dir with liblunaberry; NULL = exe dir    */
    const char *tmp_dir;    /* scratch dir; NULL = "."                  */
    const char *cache_dir;  /* persistent shim cache; NULL = disabled   */
    unsigned hot_threshold; /* reserved: shim is compiled on first use     */
    int internal_loop;      /* add -funroll-loops for tight-loop codegen    */
} LunaJitOptions;

typedef struct LunaJitStats {
    unsigned long compile_attempts;
    unsigned long compile_ok;
    unsigned long compile_failed;
    unsigned long native_calls;
    unsigned long interp_calls;
    unsigned long cache_hits;
    unsigned long cache_disk_hits;
    unsigned long cache_disk_misses;
    unsigned long cache_disk_writes;
    unsigned long cache_disk_errors;
} LunaJitStats;

typedef double (*LunaJitFn)(void);

typedef struct LunaJit LunaJit;

LUNA_API LunaJit *luna_jit_new(LunaJitBackend backend,
                               const LunaJitOptions *opts);
LUNA_API void luna_jit_free(LunaJit *jit);

LUNA_API const char *luna_jit_backend_name(const LunaJit *jit);
LUNA_API const char *luna_jit_last_error(const LunaJit *jit);

/* Compile (or fetch from cache) a native shim for `bc`. The returned
 * pointer is owned by the JIT and stays valid until luna_jit_free(). */
LUNA_API LunaJitFn luna_jit_compile(LunaJit *jit, const unsigned char *bc,
                                    size_t len, const char *name);

/* Compile-or-fallback call. On failure returns 0.0 and fills `err`. */
LUNA_API double luna_jit_call(LunaJit *jit, const unsigned char *bc,
                              size_t len, const char *name, char *err,
                              size_t errlen);

LUNA_API LunaJitStats luna_jit_stats(const LunaJit *jit);

/* Emit the LLVM IR for the last LLVM-compiled shim (or NULL). */
LUNA_API const char *luna_jit_last_ir(const LunaJit *jit);

/* Write the C shim source for `bc` (the same source the JIT compiles).
 * Returns 0 on success. */
LUNA_API int luna_jit_emit_c(const unsigned char *bc, size_t len,
                             const char *out_c, char *err, size_t errlen);

/* Pre-build a native shim for `bc` directly to `shim_path` (.dll/.so).
 * This is the "copy path" half of the mochiberry-style JIT launcher: the
 * toolchain is paid once at build time, so the launcher never compiles at
 * runtime.  Returns 0 on success. */
LUNA_API int luna_jit_prebuild(LunaJit *jit, const unsigned char *bc,
                               size_t len, const char *shim_path,
                               char *err, size_t errlen);

/* Load and run a pre-built shim — the "patch" half: the path is resolved
 * relative to the calling executable's own directory when it is relative,
 * so a launcher can simply name the shim it ships next to itself and never
 * needs a compiler at runtime.  Returns the chunk result; on failure
 * returns 0.0 and fills `err`. */
LUNA_API double luna_jit_run_shim(const char *shim, char *err,
                                  size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_JIT_H */
