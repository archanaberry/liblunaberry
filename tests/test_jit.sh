#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

make >/dev/null

echo "== JIT (cc) =="
./build/lunajit$EXE --backend cc --target lunavm --run --stats \
  -o build/test_jit_cc examples/fib.lua

echo "== JIT (llvm) =="
./build/lunajit$EXE --backend llvm --target lunavm --run --stats \
  -o build/test_jit_llvm examples/fib.lua

echo "== JIT launcher =="
./build/lunajit$EXE --backend llvm --target exe \
  -o build/test_jit_exe examples/fib.lua
./build/test_jit_exe$EXE

echo "jit: ok"
