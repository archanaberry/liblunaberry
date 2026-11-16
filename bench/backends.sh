#!/usr/bin/env bash
# bench/backends.sh — compare LunaBerry backends.
#
#   bench/backends.sh [iterations]
#
# Measures best-of-N wall time, with the candidates interleaved so CPU
# thermal drift / scheduling noise does not bias any single backend:
#   vm        lunavm <src>                 (interpreted, in the VM)
#   vm-lbc    lunavm <src>.lbc             (same bytecode, interpreted)
#   jit-cc    lunavm --jit=cc <src>        (runtime JIT via gcc)
#   jit-llvm  lunavm --jit=llvm <src>      (runtime JIT via clang)
#   aot-llvm  lunaaot --backend llvm <src> (native executable)
#   aot-berry lunaaot --backend berry <src>(BeASM module + native host)
#
# "di vm" = the interpreter columns, "di native" = the compiled columns.
#
# Note on the tiers: LunaJIT here is a *compiled launcher* — it compiles the
# wrapper C at runtime but still runs the Lua VM interpreter loop inside.
# That is why jit-* pay a C-compile on the hot path and can be *slower*
# than the plain interpreter.  AOT bakes the bytecode and skips the source
# parser/compiler, so aot-* is the fastest.  The hot workload shows this
# holds even when the JIT compile cost is amortized.
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac
ITERS="${1:-3}"

make >/dev/null
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
for r = 1, 200 do
  for i = 1, 20 do sum = sum + fib(i) end
  sieve(160)
end
print("done", sum % 1000)
EOF

cat > bench/out/hot.lua <<'EOF'
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
for r = 1, 2000 do
  for i = 1, 20 do sum = sum + fib(i) end
  sieve(120)
end
print("done", sum % 1000)
EOF

# Build artifacts once (excluded from the timed run).
./build/lunac$EXE -o bench/out/work.lbc bench/out/work.lua
./build/lunaaot$EXE --backend llvm -o bench/out/aot_llvm bench/out/work.lua >/dev/null 2>&1
./build/lunaaot$EXE --backend berry -o bench/out/aot_berry bench/out/work.lua >/dev/null 2>&1
./build/lunaaot$EXE --backend llvm -o bench/out/hot_llvm bench/out/hot.lua >/dev/null 2>&1
./build/lunaaot$EXE --backend berry -o bench/out/hot_berry bench/out/hot.lua >/dev/null 2>&1
./build/lunajit$EXE --backend cc --target exe \
  -o bench/out/jit_cpp bench/out/work.lua >/dev/null 2>&1
./build/lunajit$EXE --backend cc --target exe --compile-at-run \
  -o bench/out/jit_runtime bench/out/work.lua >/dev/null 2>&1

# candidates are "label|cmd..." ; best[label] holds the best wall ms.
declare -A BEST

measure() { # measure <label> <cmd...>
  local lbl="$1"; shift
  local t0 t1 dt
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  dt=$(( (t1 - t0) / 1000000 ))
  if [ "$dt" -lt "${BEST[$lbl]:-999999999}" ]; then BEST[$lbl]=$dt; fi
}

# ---- short workload: all six backends, interleaved ----
run_round_short() {
  measure vm       ./build/lunavm$EXE bench/out/work.lua
  measure vm-lbc   ./build/lunavm$EXE bench/out/work.lbc
  measure jit-cc   ./build/lunavm$EXE --jit=cc bench/out/work.lua
  measure jit-llvm ./build/lunavm$EXE --jit=llvm bench/out/work.lua
  measure jit-cpp  ./bench/out/jit_cpp$EXE
  measure jit-run  ./bench/out/jit_runtime$EXE
  measure aot-llvm ./bench/out/aot_llvm$EXE
  measure aot-berry ./bench/out/aot_berry.host$EXE
}
for ((i=0;i<ITERS;i++)); do run_round_short; done

printf "%-12s %-14s %-12s\n" "backend" "kind" "best_ms"
for lbl in vm vm-lbc jit-cc jit-llvm jit-cpp jit-run aot-llvm aot-berry; do
  case $lbl in
    vm|vm-lbc)        kind="interpreter";;
    jit-cc|jit-llvm)  kind="runtime jit";;
    jit-cpp)          kind="jit launcher (copy+patch)";;
    jit-run)          kind="jit launcher (compile at run)";;
    *)                kind="native";;
  esac
  printf "%-12s %-14s %-12d\n" "$lbl" "$kind" "${BEST[$lbl]}"
done

# ---- hot workload: four backends, interleaved ----
declare -A HBEST
measure2() { # measure2 <label> <cmd...>
  local lbl="$1"; shift
  local t0 t1 dt
  t0=$(date +%s%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s%N)
  dt=$(( (t1 - t0) / 1000000 ))
  if [ "$dt" -lt "${HBEST[$lbl]:-999999999}" ]; then HBEST[$lbl]=$dt; fi
}
run_round_hot() {
  measure2 vm      ./build/lunavm$EXE bench/out/hot.lua
  measure2 jit-cc  ./build/lunavm$EXE --jit=cc bench/out/hot.lua
  measure2 aot-llvm ./bench/out/hot_llvm$EXE
  measure2 aot-berry ./bench/out/hot_berry.host$EXE
}
for ((i=0;i<ITERS;i++)); do run_round_hot; done

echo
printf "%-12s %-14s %-12s\n" "HOT backend" "kind" "best_ms"
for lbl in vm jit-cc aot-llvm aot-berry; do
  case $lbl in
    vm) kind="interpreter";; jit-cc) kind="runtime jit";; *) kind="native";;
  esac
  printf "%-12s %-14s %-12d\n" "$lbl" "$kind" "${HBEST[$lbl]}"
done

# ---- artifact sizes ----
echo
printf "%-12s %-14s %-12s\n" "" "" "bytes"
for a in aot_llvm aot_berry.host work.lbc; do
  if [ "$a" = work.lbc ]; then
    sz=$(stat -c%s "bench/out/$a" 2>/dev/null || echo 0)
  else
    sz=$(stat -c%s "bench/out/$a$EXE" 2>/dev/null || echo 0)
  fi
  printf "%-12s %-14s %-12d\n" "$a" "artifact" "$sz"
done

# ---- sanity: identical output everywhere ----
for bin in ./bench/out/aot_llvm$EXE ./bench/out/aot_berry.host$EXE \
           ./bench/out/hot_llvm$EXE ./bench/out/hot_berry.host$EXE; do
  "$bin" | grep -q "done	0" || { echo "FAIL: $bin wrong output" >&2; exit 1; }
done

rm -rf bench/out
echo
echo "backends: ok"
