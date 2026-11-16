#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

make
./build/lunaaot$EXE --backend llvm --target both --emit-ir --o3 \
  examples/app_lunaberry.lua

echo "native artifacts:"
ls -lh dist/*.exe dist/*.lbc dist/*.ll 2>/dev/null || ls -lh dist/* 2>/dev/null || true
