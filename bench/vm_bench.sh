#!/usr/bin/env bash
# ============================================================================
#  vm_bench.sh -- waktu eksekusi interpreter untuk satu atau beberapa binary
#
#  PEMAKAIAN
#    sh bench/vm_bench.sh build-opt-slim-base/lunavm.exe build-opt-slim-njt/lunavm.exe
#    sh bench/vm_bench.sh --iters 7 <exe> ...
#
#  Beban kerja: fib + sieve (bahan yang sama dengan bench/ladder_vm.sh) supaya
#  angkanya bisa dibandingkan dengan ladder lama.
#
#  Output: <label> <best_ms> <median_ms> <size_bytes>
#  Catatan: yang dipakai untuk keputusan adalah BEST (noise terendah), median
#  ditampilkan sebagai penstabil.
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."

ITERS=5
if [ "${1:-}" = "--iters" ]; then ITERS="$2"; shift 2; fi

mkdir -p bench/out
cat > bench/out/vmwork.lua <<'EOF'
local function fib(n) if n < 2 then return n end return fib(n-1)+fib(n-2) end
local function sieve(n)
  local t = {}
  for i = 2, n do t[i] = true end
  for i = 2, n do
    if t[i] then for j = i*i, n, i do t[j] = false end end
  end
  local c = 0
  for i = 2, n do if t[i] then c = c + 1 end end
  return c
end
local sum = 0
for r = 1, 600 do
  for i = 1, 20 do sum = sum + fib(i) end
  sieve(160)
end
print("done", sum % 1000)
EOF

printf '%-44s %-10s %-10s %-10s %-10s\n' BINARY wall_best wall_med cpu_best bytes
printf '%s\n' "-------------------------------------------------------------------------------------"
# Mode INTERLEAVED: tiap round menjalankan SEMUA kandidat bergiliran, jadi
# beban background mesin tidak memihak satu kandidat.  Yang dipakai untuk
# keputusan = MIN per kandidat (noise terendah), median ditampilkan sebagai
# penstabil.  Lihat juga loadavg sebelum membandingkan angka antar sesi.
declare -A TIMES
declare -A CPUS
VALID=()
for exe in "$@"; do
  if [ ! -f "$exe" ]; then printf '%-44s %s\n' "$exe" "(hilang)"; continue; fi
  out=$("$exe" bench/out/vmwork.lua 2>&1 | tail -1)
  case "$out" in
    done*) ;;
    *) printf '%-44s %s\n' "$exe" "OUTPUT SALAH: $out"; continue ;;
  esac
  TIMES["$exe"]=""
  CPUS["$exe"]=""
  VALID+=("$exe")
done
# CPU time (user+sys) per proses: TIDAK dipengaruhi beban mesin lain,
# jadi inilah metrik yang bisa dipercaya di host yang dipakai bersama.
# Wall time tetap dicetak sebagai "apa yang dirasakan user".
cpu_ms() {
  local exe="$1" out
  out=$( { TIMEFORMAT='%3U %3S'; time "$exe" bench/out/vmwork.lua >/dev/null 2>&1; } 2>&1 | tail -1 )
  echo "$out" | awk '{printf "%d", ($1+$2)*1000}'
}
for ((r=0;r<ITERS;r++)); do
  for exe in "${VALID[@]}"; do
    t0=$(date +%s%N)
    "$exe" bench/out/vmwork.lua >/dev/null 2>&1
    t1=$(date +%s%N)
    TIMES["$exe"]="${TIMES[$exe]} $(( (t1-t0)/1000000 ))"
    CPUS["$exe"]="${CPUS[$exe]} $(cpu_ms "$exe")"
  done
done
printf 'loadavg: %s\n' "$(cat /proc/loadavg 2>/dev/null || echo '?')"
printf '%-44s %-10s %-10s %-10s %-10s\n' BINARY wall_best wall_med cpu_best bytes
for exe in "${VALID[@]}"; do
  sorted=$(printf '%s\n' ${TIMES[$exe]} | sort -n)
  best=$(printf '%s\n' "$sorted" | head -1)
  mid=$(printf '%s\n' "$sorted" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  csort=$(printf '%s\n' ${CPUS[$exe]} | sort -n)
  cbest=$(printf '%s\n' "$csort" | head -1)
  sz=$(wc -c < "$exe" | tr -d ' ')
  printf '%-44s %-10s %-10s %-10s %-10s\n' "$exe" "$best" "$mid" "$cbest" "$sz"
done
rm -f bench/out/vmwork.lua
