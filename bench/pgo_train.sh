#!/usr/bin/env bash
# ============================================================================
#  pgo_train.sh -- fase "training" untuk PGO.
#
#  Dijalankan SETELAH build instrumented (-fprofile-generate) dan SEBELUM
#  build ulang memakai profil (-fprofile-use).  Binary yang dijalankan harus
#  yang instrumented; binary itu menulis .gcda ke .pgo/<tier>/ saat exit.
#
#  Beban training harus Mewakili hot path interpreter (dispatch opcode,
#  alokasi, table, string, pustaka dasar).  Beban terlalu sempit = profil
#  bias; beban terlalu luas = waktu training Buang.
#
#  PEMAKAIAN: sh bench/pgo_train.sh <exe> <tier>
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."

EXE="${1:?path exe instrumented wajib diisi}"
TIER="${2:-core}"

# Beban luar (algo sama dengan bench MochiBerry) + beban lokal.
# Path dikirim sebagai argv ke exe native -> konversi ke format native.
npath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || printf '%s' "$1"; }
WL_EXT="$(npath "$PWD/../bcf/mochiberry/benchmarks/bench_workload.lua")"
[ -f "$WL_EXT" ] || WL_EXT=""
WL_LOCAL="bench/workload_pgo.lua"

mkdir -p ".pgo/$TIER"

run() { # file script
  local f="$1" s="$2"
  [ -n "$s" ] && [ -f "$s" ] && "$EXE" "$s" >/dev/null 2>&1
}

# 1) beban eksternal (kalau ada)
for i in 1 2 3; do run "$EXE" "$WL_EXT"; done
# 2) beban lokal (selalu ada, agar tier slim pun punya profil)
for i in 1 2 3; do run "$EXE" "$WL_LOCAL"; done
# 3) test suite = cakupan pustaka lengkap
run "$EXE" "tests/test.lua"

echo "PGO training selesai: $(find ".pgo/$TIER" -name '*.gcda' 2>/dev/null | wc -l) file .gcda di .pgo/$TIER"
