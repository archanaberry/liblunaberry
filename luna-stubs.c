/*
 * luna-stubs.c — stand-ins for the optional native tiers.
 *
 * The slim/core builds leave the LLVM (clang) and Berry/BeASM backends out
 * of the binary so the interpreter stays small.  The CLIs still link, they
 * just report that the tier was not built in.
 *
 * This file is compiled *instead of* luna-llvm.c and luna-berry.c in those
 * builds, never together with them.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "luna.h"
#include "luna-jit.h"
#include "luna-llvm.h"
#include "luna-berry.h"

static int luna_stub_err(char *err, size_t errlen, const char *what)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s tier not built into this binary "
                 "(build with TIER=full)", what);
    return -1;
}

int luna_llvm_available(void)
{
    return 0;
}

int luna_llvm_aot(const unsigned char *bc, size_t len, const char *out_exe,
                  const char *chunk_name, const LunaLLvmOptions *opts,
                  char *err, size_t errlen)
{
    (void)bc; (void)len; (void)out_exe; (void)chunk_name; (void)opts;
    return luna_stub_err(err, errlen, "LLVM");
}

int luna_llvm_emit_ir(const unsigned char *bc, size_t len, const char *out_ll,
                      const char *chunk_name, const LunaLLvmOptions *opts,
                      char *err, size_t errlen)
{
    (void)bc; (void)len; (void)out_ll; (void)chunk_name; (void)opts;
    return luna_stub_err(err, errlen, "LLVM");
}

int luna_llvm_emit_c(const unsigned char *bc, size_t len, const char *out_c,
                     char *err, size_t errlen)
{
    (void)bc; (void)len; (void)out_c;
    return luna_stub_err(err, errlen, "LLVM");
}

int luna_llvm_emit_asm(const unsigned char *bc, size_t len, const char *out_s,
                       const char *chunk_name, const LunaLLvmOptions *opts,
                       char *err, size_t errlen)
{
    (void)bc; (void)len; (void)out_s; (void)chunk_name; (void)opts;
    return luna_stub_err(err, errlen, "LLVM");
}

int luna_berry_emit(const unsigned char *bc, size_t len, const char *stem,
                    const LunaBerryOptions *opts, char *err, size_t errlen)
{
    (void)bc; (void)len; (void)stem; (void)opts;
    return luna_stub_err(err, errlen, "Berry");
}

#ifdef LUNA_TIER_SLIM
/* The slim tier also leaves the native JIT out (luna-jit.c is not linked),
 * but the VM CLI still exposes --jit.  These stand-ins keep the binary
 * linkable and make the CLI report that the JIT was not built in instead of
 * silently misbehaving; the core tier links the real luna-jit.c and must
 * not see these definitions, hence the LUNA_TIER_SLIM guard. */

static char luna_jit_stub_obj[1];

LunaJit *luna_jit_new(LunaJitBackend backend, const LunaJitOptions *opts)
{
    (void)backend; (void)opts;
    return (LunaJit *)luna_jit_stub_obj;
}

void luna_jit_free(LunaJit *jit)
{
    (void)jit;
}

const char *luna_jit_backend_name(const LunaJit *jit)
{
    (void)jit;
    return "none";
}

const char *luna_jit_last_error(const LunaJit *jit)
{
    (void)jit;
    return "JIT tier not built into this binary (build with TIER=full)";
}

LunaJitFn luna_jit_compile(LunaJit *jit, const unsigned char *bc, size_t len,
                           const char *name)
{
    (void)jit; (void)bc; (void)len; (void)name;
    return NULL;
}

double luna_jit_call(LunaJit *jit, const unsigned char *bc, size_t len,
                     const char *name, char *err, size_t errlen)
{
    (void)jit; (void)bc; (void)len; (void)name;
    (void)luna_stub_err(err, errlen, "JIT");
    return 0.0;
}

LunaJitStats luna_jit_stats(const LunaJit *jit)
{
    LunaJitStats st;
    (void)jit;
    memset(&st, 0, sizeof(st));
    return st;
}

const char *luna_jit_last_ir(const LunaJit *jit)
{
    (void)jit;
    return NULL;
}

int luna_jit_emit_c(const unsigned char *bc, size_t len, const char *out_c,
                    char *err, size_t errlen)
{
    (void)bc; (void)len; (void)out_c;
    return luna_stub_err(err, errlen, "JIT");
}

int luna_jit_prebuild(LunaJit *jit, const unsigned char *bc, size_t len,
                      const char *shim_path, char *err, size_t errlen)
{
    (void)jit; (void)bc; (void)len; (void)shim_path;
    return luna_stub_err(err, errlen, "JIT");
}

double luna_jit_run_shim(const char *shim, char *err, size_t errlen)
{
    (void)shim;
    (void)luna_stub_err(err, errlen, "JIT");
    return 0.0;
}
#endif /* LUNA_TIER_SLIM */
