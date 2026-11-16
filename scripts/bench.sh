#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

make >/dev/null

echo "== interpreter =="
./build/lunavm$EXE examples/bench_fib.lua

echo "== JIT (cc) =="
./build/lunajit$EXE --backend cc --target lunavm --run examples/bench_fib.lua

echo "== JIT (llvm) =="
./build/lunajit$EXE --backend llvm --target lunavm --run examples/bench_fib.lua

echo "== AOT (llvm) =="
./build/lunaaot$EXE --backend llvm --target exe --o3 \
  -o build/bench_fib examples/bench_fib.lua
./build/bench_fib$EXE
