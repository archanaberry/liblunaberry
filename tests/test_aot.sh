#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

make >/dev/null

echo "== AOT (llvm) =="
./build/lunaaot$EXE --backend llvm --target both --o3 --emit-ir \
  -o build/test_aot examples/fib.lua
./build/test_aot$EXE

echo "== AOT (berry) =="
./build/lunaaot$EXE --backend berry --target both \
  -o build/test_berry examples/fib.lua
./build/test_berry.host$EXE

echo "== lunavm --aot =="
./build/lunavm$EXE --aot=llvm examples/hello.lua | grep -q "Hello from Lua 5.5"
./build/lunavm$EXE --aot=berry examples/hello.lua | grep -q "Hello from Lua 5.5"

echo "aot: ok"
