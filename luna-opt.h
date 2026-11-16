/*
 * luna-opt.h — LunaBerry optimization ladder O0..O10.
 *
 * A single source of truth for the optimization levels shared by LunaAOT,
 * LunaJIT and lunavm --jit.  The ladder mirrors the Berry Lab "O0..O10"
 * design:
 *
 *   O0  Raw          no optimization, debug info kept
 *   O1  Clean        mem2reg / sroa / instcombine territory
 *   O2  Solid        standard -O2 (production default)
 *   O3  Fast         standard -O3 (vectorization, aggressive inlining)
 *   O4  Mochi        + loop unrolling
 *   O5  Mulch        + frame-pointer omission
 *   O6  Akari        + errno-free math
 *   O7  Trace        + hidden visibility
 *   O8  Silicon      + native tuning (-march/-mtune on the LLVM side)
 *   O9  Nirvana      + IPA: no FP traps, speculative devirtualization,
 *                     no semantic interposition.  -ffast-math is NOT used:
 *                     Lua must keep exact NaN/Inf/signed-zero semantics
 *                     (math.sqrt(-1) -> NaN, 1/-0 -> -inf).
 *   O10 The Stripper + no debug info, no unwind tables, implicit strip,
 *                     "dead assembly" (no readable source identity)
 *
 * Every level carries three real flag sets:
 *   luna_opt_flags()        CC/backend flags (gcc + clang compatible)
 *   luna_opt_llvm_flags()   clang-specific flags for the LLVM tier
 *   luna_opt_llvm_passes()  new-PM `opt -passes=` pipeline for LLVM IR
 *
 * The compiler flags are plain clang/gcc flags, so every level is a real,
 * measurable change in the generated code -- not just a label.
 */

#ifndef LUNA_OPT_H
#define LUNA_OPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUNA_OPT_MIN 0
#define LUNA_OPT_MAX 10

/* Clamp any integer into the valid 0..10 range. */
int luna_opt_clamp(int level);

/* Human readable level name ("Raw", "Solid", "The Stripper", ...). */
const char *luna_opt_name(int level);

/* Compiler flags for a level (gcc + clang compatible). Never NULL. */
const char *luna_opt_flags(int level);

/* clang/LLVM-specific flags for a level (the "LLVM" side of the ladder).
 * Never NULL.  Example: O8 adds -march=native -mtune=native. */
const char *luna_opt_llvm_flags(int level);

/* New pass manager pipeline for a level, usable with `opt -passes=...`.
 * Never NULL.  When `opt` is not installed, the AOT tier reaches the same
 * pipeline by re-emitting the IR through clang at the matching -O level. */
const char *luna_opt_llvm_passes(int level);

/* One-line human description of what a level does. Never NULL. */
const char *luna_opt_summary(int level);

/* 1 when the level implies stripping the produced binary (O10). */
int luna_opt_implies_strip(int level);

/* 1 when the level implies "dead assembly" output (O10). */
int luna_opt_implies_dead_asm(int level);

/* 1 when the level implies that no debug information is emitted (O0/O10). */
int luna_opt_implies_no_debug(int level);

/* Emit warnings for flags that are redundant at this level.
 * `strip_given` / `debug_given` report whether the user also passed
 * --strip / -g style flags. */
void luna_opt_warn_redundant(int level, int strip_given, int debug_given);

/* Rewrite an assembly file into "dead assembly": comments, source identity
 * (.file), .ident, .comment, debug directives are removed and every
 * non-exported label is renamed to an anonymous .L<n>.  Exported symbols
 * (.globl / .def) keep their names because the ABI needs them.
 * Returns 0 on success. */
int luna_opt_dead_asm(const char *in_path, const char *out_path);

/* Strip a produced binary as hard as the host tools allow
 * (strip --strip-all, falling back to objcopy --strip-all).
 * Returns 0 on success, -1 if no strip tool was found. */
int luna_opt_strip_binary(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_OPT_H */
