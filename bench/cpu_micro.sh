#!/usr/bin/env bash
# ============================================================================
#  cpu_micro.sh -- ukur CPU time (user+kernel) + wall time beberapa binary
#  pada beban yang sama, dengan tools/cputime.exe (GetProcessTimes Win32).
#
#  KENAPA METRIK INI
#    Host ini dipakai bersama (agent lain build/benchmark paralel) sehingga
#    wall-clock tidak bisa dipakai untuk membandingkan engine: beban mereka
#    menaikkan wall time engine yang kebetulan jalan saat itu.  CPU time
#    milik proses sendiri tidak terpengaruh beban luar.
#
#  RESOLUSI
#    GetProcessTimes membulatkan ke tick 15,625 ms, jadi beban default
#    (REP=20 -> ~20x workload normal, ~0,5-0,8 s CPU) memberi resolusi ~3 %.
#    Ambil MIN dari beberapa round: estimasi biaya terendah, paling kebal
#    noise cache/frekuensi.
#
#  INTERLEAVED
#    Semua binary dijalankan round-robin per round (A/B/C/A/B/C), bukan satu
#    binary habis lalu berikutnya, supaya drift mesin (cache, frekuensi,
#    temperatura) menimpa semua kandidat sama rata.
#
#  PEMAKAIAN
#    sh bench/cpu_micro.sh [--rounds N] [--rep N] <exe> ...
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."

ROUNDS=5
REP=20
SCRIPT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2 ;;
    --rep)    REP="$2"; shift 2 ;;
    --script) SCRIPT="$2"; shift 2 ;;
    *) break ;;
  esac
done

mkdir -p bench/out
cat > bench/out/vmwork_base.lua <<'EOF'
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
for r = 1, 20 do
  for i = 1, 20 do sum = sum + fib(i) end
  sieve(160)
end
print("done", sum % 1000)
EOF
if [ -n "$SCRIPT" ]; then
  # Skrip eksternal (mis. beban setara lintas bahasa): TIDAK diskalakan lagi,
  # caller yang menentukan jumlah iterasinya.
  W="$SCRIPT"
else
  sed "s/for r = 1, 20 do/for r = 1, $((20*REP)) do/" bench/out/vmwork_base.lua > bench/out/vmwork_rep.lua
  W=bench/out/vmwork_rep.lua
fi

# Token yang menandakan beban benar-benar dijalankan (bukan crash/keluar
# diam-diam).  Skrip beda bisa beda token, jadi bisa dioverride via TOKEN=.
# ${TOKEN-done} (bukan :-) supaya TOKEN yang sengaja dikosongkan dipakai apa adanya
# (untuk ukur startup, script kosong tidak printing apa pun).
TOKEN="${TOKEN-done}"
declare -A C
declare -A Wl
VALID=()
for exe in "$@"; do
  if [ ! -f "$exe" ]; then printf '  (hilang %s)\n' "$exe"; continue; fi
  out=$("$exe" "$W" 2>&1 | tail -1)
  case "$out" in
    *"$TOKEN"*) ;;
    *) printf '  (output salah %s: %s)\n' "$exe" "$out"; continue ;;
  esac
  C["$exe"]=""; Wl["$exe"]=""; VALID+=("$exe")
done

for ((r=0;r<ROUNDS;r++)); do
  for exe in "${VALID[@]}"; do
    out=$(./tools/cputime.exe "$exe" "$W")
    set -- $out
    C["$exe"]="${C[$exe]} $3"
    t0=$(date +%s%N); "$exe" "$W" >/dev/null 2>&1; t1=$(date +%s%N)
    Wl["$exe"]="${Wl[$exe]} $(( (t1-t0)/1000000 ))"
  done
done

printf 'loadavg: %s   rounds: %s   rep: %s\n' "$(cat /proc/loadavg 2>/dev/null || echo '?')" "$ROUNDS" "$REP"
printf '\n%-42s %10s %10s %10s\n' BINARY cpu_min cpu_med wall_min
printf '%s\n' "------------------------------------------------------------------------------------"
for exe in "${VALID[@]}"; do
  cm=$(printf '%s\n' ${C[$exe]} | sort -n | head -1)
  cd_=$(printf '%s\n' ${C[$exe]} | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  wm=$(printf '%s\n' ${Wl[$exe]} | sort -n | head -1)
  printf '%-42s %10s %10s %10s\n' "$exe" "$cm" "$cd_" "$wm"
done
[ -z "$SCRIPT" ] && rm -f bench/out/vmwork_rep.lua
exit 0