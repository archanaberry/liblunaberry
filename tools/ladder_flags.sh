#!/usr/bin/env bash
# tools/ladder_flags.sh — print the flags of an O0..O10 ladder level.
#
#   tools/ladder_flags.sh 3            -> C/compiler flags (default)
#   tools/ladder_flags.sh --llvm 3     -> clang/LLVM-specific flags
#   tools/ladder_flags.sh --passes 3   -> new-PM `opt -passes=` pipeline
#
# This mirrors the table in luna-opt.c so the engine itself can be built at
# a ladder level ("make VM_LADDER=10").  tests/test_opt.sh verifies that the
# C-flags table stays identical to this one for every level.
set -euo pipefail

mode="c"
while [ $# -gt 1 ]; do
  case "$1" in
    --llvm)   mode="llvm";   shift ;;
    --passes) mode="passes"; shift ;;
    *) echo "ladder_flags: unknown option '$1'" >&2; exit 2 ;;
  esac
done
lvl="${1:-2}"

case "$lvl" in
  0|1|2|3|4|5|6|7|8|9|10) ;;
  *) echo "ladder_flags: invalid level '$lvl' (0..10)" >&2; exit 2 ;;
esac

case "$lvl" in
  0)
    C="-O0 -g0"
    LLVM="-O0 -g"
    PASSES="default<O0>"
    ;;
  1)
    C="-O1"
    LLVM="-O1"
    PASSES="default<O1>"
    ;;
  2)
    C="-O2"
    LLVM="-O2"
    PASSES="default<O2>"
    ;;
  3)
    C="-O3"
    LLVM="-O3"
    PASSES="default<O3>"
    ;;
  4)
    C="-O3 -funroll-loops"
    LLVM="-O3 -funroll-loops"
    PASSES="default<O3>,loop-unroll"
    ;;
  5)
    C="-O3 -funroll-loops -fomit-frame-pointer"
    LLVM="-O3 -funroll-loops -fomit-frame-pointer"
    PASSES="default<O3>,loop-unroll,loop-deletion"
    ;;
  6)
    C="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno"
    LLVM="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno"
    PASSES="default<O3>,loop-unroll,loop-deletion,slp-vectorizer"
    ;;
  7)
    C="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden"
    LLVM="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -fvisibility-inlines-hidden"
    PASSES="default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine"
    ;;
  8)
    C="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -mtune=native"
    LLVM="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -fvisibility-inlines-hidden -march=native -mtune=native"
    PASSES="default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine,inline"
    ;;
  9)
    C="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -mtune=native -fno-trapping-math -fdevirtualize-speculatively -fno-semantic-interposition"
    LLVM="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -fvisibility-inlines-hidden -march=native -mtune=native -fno-trapping-math -fno-semantic-interposition"
    PASSES="default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine,inline,gvn,licm,tailcallelim"
    ;;
  10)
    C="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -mtune=native -fno-trapping-math -fdevirtualize-speculatively -fno-semantic-interposition -g0 -fno-asynchronous-unwind-tables -fno-ident"
    LLVM="-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno -fvisibility=hidden -fvisibility-inlines-hidden -march=native -mtune=native -fno-trapping-math -fno-semantic-interposition -g0 -fno-asynchronous-unwind-tables -fno-ident"
    PASSES="default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine,inline,gvn,licm,tailcallelim,strip-dead-prototypes,globaldce,globalopt"
    ;;
esac

case "$mode" in
  c)      echo "$C" ;;
  llvm)   echo "$LLVM" ;;
  passes) echo "$PASSES" ;;
esac
