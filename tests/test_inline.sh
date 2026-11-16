#!/usr/bin/env bash
# tests/test_inline.sh — luna.compile / luna.asm inline-language smoke test.
set -u
cd "$(dirname "$0")/.."
BIN=build/lunavm.exe
[ -f "$BIN" ] || BIN=build/lunavm

echo "== inline language =="
$BIN examples/inline_c.lua || { echo "inline: FAIL"; exit 1; }
echo "inline: ok"
