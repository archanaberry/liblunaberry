#!/usr/bin/env bash
# ============================================================================
#  build_stock_lua.sh -- baseline "Lua bawaan" (upstream PUC-Rio Lua 5.5.1)
#
#  TUJUAN
#    Membangun interpreter standalone Lua 5.5.1 resmi (src/lua.c) dengan
#    toolchain dan flag yang PERSIS sama dengan LunaBerry, supaya perbandingan
#    hanya mengukur selisih "shell + ekstensi Berry" vs Lua murni.
#
#  SUMBER
#    Tarball resmi https://www.lua.org/ftp/lua-5.5.1.tar.gz
#    (dicek: vendor/lua di repo ini byte-identik dengan src/ upstream,
#     59 file identik / 0 beda -- inti Lua tidak ditambal.)
#
#  VARIAN
#    -O2  -> pembanding tier core/full LunaBerry
#    -Os  -> pembanding tier slim/tiny LunaBerry
#
#  OUTPUT
#    <OUT>/lua-stock-O2.exe, <OUT>/lua-stock-Os.exe  (stripped, sama seperti
#    artefak LunaBerry shipping)
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# Compiler: pakai gcc/ar dari PATH; kalau tidak ada, baru fallback WinLibs
# (path Windows host ini).  Di Linux/macOS baris fallback diabaikan.
if ! command -v gcc >/dev/null 2>&1 || ! command -v ar >/dev/null 2>&1; then
  BIN="C:/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
  [ -d "$BIN" ] && export PATH="/c/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
fi

# npath: path POSIX -> native (dipakai gcc native sebagai -I).
npath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || printf '%s' "$1"; }
SRC=${LUA_SRC:-$(npath "$PWD/vendor/lua")}
OUT=${1:-bench/out-stock}

# Flag identik dengan Makefile LunaBerry (GCSECT + ALIGN + fno-ident).
GCS="-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-jump-tables"
ALIGN="-falign-functions=1 -falign-jumps=1 -falign-labels=1"
COMMON="$GCS $ALIGN -fno-ident -std=c99 -I$SRC -w"

if [ ! -f "$SRC/lua.c" ]; then
  echo "ERROR: src Lua upstream tidak ditemukan di $SRC" >&2
  echo "       unduh: https://www.lua.org/ftp/lua-5.5.1.tar.gz" >&2
  exit 1
fi

mkdir -p "$OUT"
CORE=$(ls "$SRC"/*.c | grep -vE '/(lua|luac)\.c$')

for opt in O2 Os; do
  exe="$OUT/lua-stock-$opt.exe"
  echo "=== build stock Lua 5.5.1 (-$opt) ==="
  # shellcheck disable=SC2086
  "$BIN/gcc.exe" "-$opt" $COMMON -o "$exe" "$SRC/lua.c" $CORE -lm || exit 1
  "$BIN/strip.exe" -s "$exe"
  printf '  %-34s %8d bytes\n' "$exe" "$(wc -c < "$exe" | tr -d ' ')"
done

# sanity: harus bisa menjalankan script
echo "=== sanity run ==="
cat > "$OUT/probe.lua" <<'EOF'
local function fib(n) if n < 2 then return n end return fib(n-1)+fib(n-2) end
print("stock ok", fib(20), _VERSION)
EOF
for opt in O2 Os; do
  printf '  %-34s ' "lua-stock-$opt"
  "$OUT/lua-stock-$opt.exe" "$OUT/probe.lua"
done
