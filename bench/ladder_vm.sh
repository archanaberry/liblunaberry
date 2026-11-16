#!/usr/bin/env bash
# bench/ladder_vm.sh — build the engine at each O0..O10 ladder level and
# measure the interpreter.
#
#   bench/ladder_vm.sh              # levels 0 2 3 9 10
#   bench/ladder_vm.sh 0 1 2 3 ...  # explicit levels
#
# The VM is native code, so its optimization level is a *build* property:
# the engine is rebuilt with "make VM_LADDER=N".  A separate build directory
# is used so the normal build/ tree is left alone.
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

LEVELS=("$@")
if [ ${#LEVELS[@]} -eq 0 ]; then
  LEVELS=(0 2 3 9 10)
fi

NAMES=(Raw Clean Solid Fast Mochi Mulch Akari Trace Silicon Nirvana "The Stripper")
LBUILD=build-ladder

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

printf "%-6s %-14s %-12s %-12s\n" "level" "name" "vm_ms" "vm_bytes"
for lvl in "${LEVELS[@]}"; do
  rm -rf "$LBUILD"
  make BUILD="$LBUILD" VM_LADDER="$lvl" -j4 >/dev/null 2>&1
  best=999999999
  for i in 1 2 3; do
    t0=$(date +%s%N)
    "./$LBUILD/lunavm$EXE" bench/out/vmwork.lua >/dev/null 2>&1
    t1=$(date +%s%N)
    dt=$(( (t1 - t0) / 1000000 ))
    if [ "$dt" -lt "$best" ]; then best=$dt; fi
  done
  size=$(stat -c%s "./$LBUILD/lunavm$EXE" 2>/dev/null || echo 0)
  printf "%-6s %-14s %-12d %-12d\n" "O$lvl" "${NAMES[$lvl]}" "$best" "$size"
done

rm -rf "$LBUILD" bench/out
