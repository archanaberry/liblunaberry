#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

make
./build/lunaaot$EXE --backend berry --target both \
  examples/app_lunaberry.lua

echo "berry artifacts:"
ls -lh dist/berry/* 2>/dev/null || true
