#!/usr/bin/env bash
# bench/ladder_all.sh — the whole stack at every O0..O10 ladder level.
#
#   bench/ladder_all.sh              # levels 0 2 3 7 10
#   bench/ladder_all.sh 0 1 2 ...    # explicit levels
#
# For each level the engine is rebuilt with "make VM_LADDER=N" (the VM is
# native code, so its level is a build-time property) and every tier is
# produced against that very engine:
#
#   vm          build-ladder/lunavm  <src>              (interpreter)
#   jit-rt      build-ladder/lunavm --jit=cc -O N <src> (runtime JIT)
#   jit-cpp     lunajit --target exe -O N  (copy path and patch launcher)
#   aot-llvm    lunaaot --backend llvm -O N
#   aot-berry   lunaaot --backend berry -O N
#
# All candidates are timed interleaved so CPU drift cannot bias a level.
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac
ITERS="${LADDER_ITERS:-3}"

LEVELS=("$@")
if [ ${#LEVELS[@]} -eq 0 ]; then
  LEVELS=(0 2 3 7 10)
fi

NAMES=(Raw Clean Solid Fast Mochi Mulch Akari Trace Silicon Nirvana "The Stripper")
LBUILD=build-ladder

mkdir -p bench/out
cat > bench/out/work.lua <<'EOF'
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

declare -A BEST
measure() {
  local lbl="$1"; shift
  local t0 t1 dt
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  dt=$(( (t1 - t0) / 1000000 ))
  if [ "$dt" -lt "${BEST[$lbl]:-999999999}" ]; then BEST[$lbl]=$dt; fi
}

printf "%-4s %-13s %-9s %-9s %-9s %-10s %-10s\n" \
  "lvl" "name" "vm" "jit-rt" "jit-cpp" "aot-llvm" "aot-berry"

for lvl in "${LEVELS[@]}"; do
  rm -rf "$LBUILD"
  make BUILD="$LBUILD" VM_LADDER="$lvl" -j4 >/dev/null 2>&1

  BEST=()
  "./$LBUILD/lunaaot$EXE" --backend llvm -O$lvl \
    -o bench/out/aot_llvm bench/out/work.lua >/dev/null 2>&1
  "./$LBUILD/lunaaot$EXE" --backend berry -O$lvl \
    -o bench/out/aot_berry bench/out/work.lua >/dev/null 2>&1
  "./$LBUILD/lunajit$EXE" --backend cc --target exe -O$lvl \
    -o bench/out/jit_cpp bench/out/work.lua >/dev/null 2>&1

  for ((i=0;i<ITERS;i++)); do
    measure vm       "./$LBUILD/lunavm$EXE" bench/out/work.lua
    measure jit-rt   "./$LBUILD/lunavm$EXE" --jit=cc -O$lvl bench/out/work.lua
    measure jit-cpp  ./bench/out/jit_cpp$EXE
    measure aot-llvm ./bench/out/aot_llvm$EXE
    measure aot-berry ./bench/out/aot_berry.host$EXE
  done

  printf "%-4s %-13s %-9d %-9d %-9d %-10d %-10d\n" \
    "O$lvl" "${NAMES[$lvl]}" "${BEST[vm]}" "${BEST[jit-rt]}" \
    "${BEST[jit-cpp]}" "${BEST[aot-llvm]}" "${BEST[aot-berry]}"
done

# --- artifact sizes of the last level ---
echo
printf "%-20s %-10s\n" "artifact" "bytes"
for a in aot_llvm aot_berry.host jit_cpp; do
  sz=$(stat -c%s "bench/out/$a$EXE" 2>/dev/null || echo 0)
  printf "%-20s %-10d\n" "$a" "$sz"
done

rm -rf "$LBUILD" bench/out
