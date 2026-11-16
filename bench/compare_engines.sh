#!/usr/bin/env bash
# ============================================================================
#  compare_engines.sh -- pembanding lintas engine: Lua upstream vs LunaBerry
#                        vs MochiBerry (QuickJS-ng fork) vs QuickJS-ng stock
#
#  PRINSIP
#    * Beban kerja SETARA, bukan sama-sama file: Lua engine menjalankan
#      bench_workload.lua (fib(26) + sumTo(100000) x20), JS engine menjalankan
#      bench_workload.js dengan algoritma & jumlah iterasi IDENTIK.
#    * INTERLEAVED: semua engine dijalankan round-robin per round, bukan
#      satu engine lalu berikutnya -- beban background mesin (build agent
#      lain, Defender) memukul semua engine sama rata.
#    * MIN dipakai untuk keputusan (estimasi biaya terendah), median
#      ditampilkan sebagai penstabil.  loadavg dicetak: kalau > 5, angka
#      ini cuma indikatif dan harus diulang saat mesin sepi.
#    * STARTUP diukur terpisah (script kosong), jadi "execution" =
#      total - startup bisa dihitung.
#    * Ukuran binary = file yang benar-benar dijalankan (stripped kalau
#      artefaknya memang stripped), supaya kolom size tidak menipu.
#
#  PEMAKAIAN
#    sh bench/compare_engines.sh [rounds]
#    MBJS_DIR=... QJS=... LUNA_DIR=... STOCK_LUA=... sh bench/compare_engines.sh 9
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
ROUNDS="${1:-7}"

# npath: konversi path POSIX -> native (Windows) saat diperlukan, karena path
# ini dikirim sebagai argv ke binary native (bukan hanya dipakai bash).
npath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || printf '%s' "$1"; }

LUNA_DIR="${LUNA_DIR:-$(npath "$PWD")}"
MBJS_DIR="${MBJS_DIR:-$(npath "$PWD/../bcf/mochiberry")}"
QJS="${QJS:-}"
STOCK_O2="${STOCK_O2:-$(npath "$PWD/bench/out-stock/lua-stock-O2.exe")}"
STOCK_OS="${STOCK_OS:-$(npath "$PWD/bench/out-stock/lua-stock-Os.exe")}"

WJS="$MBJS_DIR/benchmarks/bench_workload.js"
WLU="$MBJS_DIR/benchmarks/bench_workload.lua"
EMPTY_JS="$(npath "${TMPDIR:-/tmp}")/bench_empty.js"
EMPTY_LUA="$(npath "${TMPDIR:-/tmp}")/bench_empty.lua"
: > "$EMPTY_JS"; : > "$EMPTY_LUA"

# label | exe | script | token yang harus muncul di output
ENGINES=(
  "lua-5.5.1-O2|$STOCK_O2|$WLU|BENCH_RESULT"
  "lua-5.5.1-Os|$STOCK_OS|$WLU|BENCH_RESULT"
  "lunaberry-slim|$LUNA_DIR/build-slim/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-slim-fast|$LUNA_DIR/build-slim-fast/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-core|$LUNA_DIR/build-core/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-core-tiny|$LUNA_DIR/build-core-tiny/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-full|$LUNA_DIR/build-full/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-slim-pgo|$LUNA_DIR/build-slim-pgo/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-core-pgo|$LUNA_DIR/build-core-pgo/lunavm.exe|$WLU|BENCH_RESULT"
  "lunaberry-full-pgo|$LUNA_DIR/build-full-pgo/lunavm.exe|$WLU|BENCH_RESULT"
  "mochiberry-slim|$MBJS_DIR/build/tier-slim/mbjs.strip.exe|$WJS|BENCH_RESULT"
  "mochiberry-core|$MBJS_DIR/build/tier-core/mbjs.strip.exe|$WJS|BENCH_RESULT"
  "mochiberry-full|$MBJS_DIR/build/tier-full/mbjs.strip.exe|$WJS|BENCH_RESULT"
  "quickjs-ng-stock|$QJS|$WJS|BENCH_RESULT"
)

valid=()
for row in "${ENGINES[@]}"; do
  IFS='|' read -r label exe script token <<< "$row"
  if [ ! -f "$exe" ]; then printf '  (skip %-20s exe hilang: %s)\n' "$label" "$exe"; continue; fi
  out=$("$exe" "$script" 2>&1 | tail -1)
  case "$out" in
    *"$token"*) ;;
    *) printf '  (skip %-20s output tak cocok: %s)\n' "$label" "$out"; continue ;;
  esac
  valid+=("$label|$exe|$script|$token")
done

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

echo "=== validasi beban kerja: setiap engine harus menghasilkan token yang sama ==="
for row in "${valid[@]}"; do
  IFS='|' read -r label exe script token <<< "$row"
  printf '  %-20s %s\n' "$label" "$("$exe" "$script" 2>&1 | tail -1)"
done

echo
echo "=== total (process wall time, script + startup), interleaved, rounds=$ROUNDS ==="
declare -A T
declare -A C
# CPU time (user+sys) per proses: kebal terhadap beban mesin lain, jadi
# inilah angka yang dipakai untuk membandingkan engine secara adil.
cpu_ms() {
  local exe="$1" script="$2" out
  out=$( { TIMEFORMAT='%3U %3S'; time "$exe" "$script" >/dev/null 2>&1; } 2>&1 | tail -1 )
  echo "$out" | awk '{printf "%d", ($1+$2)*1000}'
}
for row in "${valid[@]}"; do IFS='|' read -r label _ _ _ <<< "$row"; T["$label"]=""; C["$label"]=""; done
for ((r=0;r<ROUNDS;r++)); do
  for row in "${valid[@]}"; do
    IFS='|' read -r label exe script _ <<< "$row"
    s=$(now_ms); "$exe" "$script" >/dev/null 2>&1; e=$(now_ms)
    T["$label"]="${T[$label]} $((e-s))"
    C["$label"]="${C[$label]} $(cpu_ms "$exe" "$script")"
  done
done

echo
echo "=== startup baseline (script kosong) ==="
declare -A S
for row in "${valid[@]}"; do IFS='|' read -r label _ _ _ <<< "$row"; S["$label"]=""; done
for ((r=0;r<ROUNDS;r++)); do
  for row in "${valid[@]}"; do
    IFS='|' read -r label exe script _ <<< "$row"
    case "$script" in *.js) empty="$EMPTY_JS";; *) empty="$EMPTY_LUA";; esac
    s=$(now_ms); "$exe" "$empty" >/dev/null 2>&1; e=$(now_ms)
    S["$label"]="${S[$label]} $((e-s))"
  done
done

printf 'loadavg: %s\n' "$(cat /proc/loadavg 2>/dev/null || echo '?')"
printf '\n%-20s %9s %9s %9s %9s %9s %12s\n' ENGINE wall_min wall_med cpu_min cpu_med cpu_exe bytes
printf '%s\n' "---------------------------------------------------------------------------------------------"
for row in "${valid[@]}"; do
  IFS='|' read -r label exe _ _ <<< "$row"
  tt=$(printf '%s\n' ${T[$label]} | sort -g)
  cc=$(printf '%s\n' ${C[$label]} | sort -g)
  tmin=$(printf '%s\n' "$tt" | head -1)
  tmed=$(printf '%s\n' "$tt" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  cmin=$(printf '%s\n' "$cc" | head -1)
  cmed=$(printf '%s\n' "$cc" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  sz=$(wc -c < "$exe" | tr -d ' ')
  printf '%-20s %9s %9s %9s %9s %9s %12s\n' "$label" "$tmin" "$tmed" "$cmin" "$cmed" "$cmed" "$sz"
done
echo
echo "catatan: kolom cpu_* = CPU time (user+sys) ms -- metrik yang tidak dipengaruhi"
echo "         beban mesin bersama; wall_* boleh naik-turun karena kontensi host." 
