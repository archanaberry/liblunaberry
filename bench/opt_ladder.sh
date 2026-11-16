#!/usr/bin/env bash
# bench/opt_ladder.sh — measure the O0..O10 optimization ladder.
#
#   bench/opt_ladder.sh [iterations]
#
# The ladder tunes the *shim/wrapper* code generation: `lunavm --jit` and
# `lunaaot` hand the level's flags to clang/gcc.  The Lua VM itself is
# linked from liblunaberry.a (built at -O2 by the Makefile), so runtime
# speed does not move with the ladder; what moves is compile time and the
# binary size.  Both are reported here.
#
#   aot_ms      time to AOT-compile the chunk at that level
#   aot_bytes   size of the produced executable (O10 strips implicitly)
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac
ITERS="${1:-3}"

NAMES=(Raw Clean Solid Fast Mochi Mulch Akari Trace Silicon Nirvana "The Stripper")

make >/dev/null

mkdir -p bench/out
cat > bench/out/optbench.lua <<'EOF'
local function fib(n)
  if n < 2 then return n end
  return fib(n-1) + fib(n-2)
end
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
for i = 1, 18 do sum = sum + fib(i) end
print("fib18=", sum, "primes120=", sieve(120))
EOF

printf "%-6s %-14s %-10s %-10s\n" "level" "name" "aot_ms" "aot_bytes"
for lvl in 0 1 2 3 4 5 6 7 8 9 10; do
  # --- AOT compile time (best of ITERS runs) ---
  best=999999999
  for ((i=0;i<ITERS;i++)); do
    t0=$(date +%s%N)
    ./build/lunaaot$EXE -O$lvl bench/out/optbench.lua -o bench/out/aot_$lvl \
      >/dev/null 2>&1
    t1=$(date +%s%N)
    dt=$(( (t1 - t0) / 1000000 ))
    if [ "$dt" -lt "$best" ]; then best=$dt; fi
  done
  size=$(stat -c%s "bench/out/aot_${lvl}${EXE}" 2>/dev/null || echo 0)
  # sanity: every level must produce the identical result
  out=$("./bench/out/aot_${lvl}${EXE}" 2>/dev/null || true)
  echo "$out" | grep -q "fib18=	6764" || { echo "FAIL: O$lvl wrong output" >&2; exit 1; }
  printf "%-6s %-14s %-10d %-10d\n" "O$lvl" "${NAMES[$lvl]}" "$best" "$size"
done
rm -rf bench/out
