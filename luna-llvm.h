/*
 * luna-llvm.h — LunaAOT's LLVM/clang code generator.
 *
 * The AOT tier turns a compiled chunk into a real standalone executable:
 *
 *   .lbc -> generated C wrapper -> LLVM IR (.ll) -> native .exe
 *
 * clang is the LLVM front-end.  On Windows the connector automatically
 * selects the GNU target and MinGW headers, so the produced COFF binary
 * links cleanly with liblunaberry.a.  When clang is unavailable the caller
 * can fall back to the CC backend without changing the generated wrapper.
 */

#ifndef LUNA_LLVM_H
#define LUNA_LLVM_H

#include <stddef.h>

#include "luna.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LunaLLvmOptions {
    const char *clang;       /* compiler binary; NULL = "clang"          */
    const char *flags;       /* extra flags; NULL = "-O2"                */
    const char *lib_dir;     /* dir with liblunaberry.a; NULL = exe dir  */
    const char *include_dir; /* dir with luna.h; NULL = "."              */
    int emit_ir;             /* also write <out>.ll                      */
    int verbose;             /* print commands                           */
    int lto;                 /* 1 = -flto=thin (whole-program on the
                              * wrapper; the engine archive must also be
                              * built by clang for cross-object LTO)     */
    int gc_sections;         /* 1 = -ffunction-sections -fdata-sections
                              * -Wl,--gc-sections (dead-code removal)    */
    int ir_passes;           /* 1 = run the ladder's new-PM pipeline on
                              * the emitted IR (opt when installed, else
                              * a clang -x ir round trip)                */
    const char *passes;      /* new-PM pipeline string for ir_passes,
                              * e.g. luna_opt_llvm_passes(level)        */
} LunaLLvmOptions;

/* 1 when a working clang is found, 0 otherwise. */
LUNA_API int luna_llvm_available(void);

/* Build a standalone executable. Returns 0 on success. */
LUNA_API int luna_llvm_aot(const unsigned char *bc, size_t len,
                           const char *out_exe, const char *chunk_name,
                           const LunaLLvmOptions *opts, char *err,
                           size_t errlen);

/* Emit only the textual LLVM IR. Returns 0 on success. */
LUNA_API int luna_llvm_emit_ir(const unsigned char *bc, size_t len,
                               const char *out_ll, const char *chunk_name,
                               const LunaLLvmOptions *opts, char *err,
                               size_t errlen);

/* Emit the standalone C wrapper (the source that luna_llvm_aot would
 * compile).  Returns 0 on success. */
LUNA_API int luna_llvm_emit_c(const unsigned char *bc, size_t len,
                              const char *out_c, char *err, size_t errlen);

/* Emit textual assembly (-S).  Returns 0 on success. */
LUNA_API int luna_llvm_emit_asm(const unsigned char *bc, size_t len,
                                const char *out_s, const char *chunk_name,
                                const LunaLLvmOptions *opts, char *err,
                                size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_LLVM_H */
