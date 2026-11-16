#!/bin/bash
# ============================================================================
#  size_ab.sh -- matriks A/B ukuran artefak LunaBerry
#
#  PEMAKAIAN
#    sh scripts/size_ab.sh slim
#    sh scripts/size_ab.sh core
#    sh scripts/size_ab.sh full
#
#  Setiap kandidat dibangun di build-opt-<tier>-<tag>/ terpisah (objek dari
#  flag berbeda TIDAK boleh bercampur), lalu dijalankan tests/test.lua.
#  Ukuran yang dicetak = stripped, sama seperti artefak yang dikirim.
#
#  CATATAN
#    * -flto default di MinGW = fat LTO object -> arsip .a membengkak ~2 MB.
#      Varian lto memakai -fno-fat-lto-objects supaya arsip tetap ramping.
#    * -Os hanya diuji untuk core/full (slim memang sudah -Os).
#    * -fno-jump-tables bisa MENYUSUT exe tapi menambah if-else chain di VM
#      dispatch; angka yang menentukan, bukan dugaan.
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."

TIER=${1:-slim}
# Compiler: pakai gcc/ar dari PATH; fallback ke WinLibs (Windows host ini)
# hanya kalau gcc belum ada di PATH.  MINGW bisa dioverride dari env.
if command -v gcc >/dev/null 2>&1 && command -v ar >/dev/null 2>&1; then
  CC=${CC:-gcc}; AR=${AR:-ar}
else
  MINGW=${MINGW:-C:/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin}
  export CC="${CC:-$MINGW/gcc.exe}" AR="${AR:-$MINGW/ar.exe}"
fi
npath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || printf '%s' "$1"; }
LUNA_TMP="$(npath "${TMPDIR:-/tmp}")/luna_tmp"; mkdir -p "$LUNA_TMP"; export TMPDIR="$LUNA_TMP" TEMP="$LUNA_TMP" TMP="$LUNA_TMP"
MAKE=${MAKE:-make}
GCSECT="-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-jump-tables"
LTOFLAGS="-flto -fno-fat-lto-objects -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables"

case "$TIER" in
  slim) BASEOPT="-Os -fno-ident"; EXTRA_DEF="-DLUNA_TIER_SLIM" ;;
  core) BASEOPT="-O2";            EXTRA_DEF="-DLUNA_TIER_CORE" ;;
  full) BASEOPT="-O3";            EXTRA_DEF="" ;;
  *) echo "tier tidak dikenal: $TIER"; exit 2 ;;
esac

# Ronde 1 (base/njt/os/lto) sudah selesa; ronde 2 = kandidat ICF/perataan/
# merge-konstanta.  "base" selalu = konfigurasi shipping saat ini.
if [ "$TIER" = "slim" ]; then
  CANDS=(
    "base|"
    "icf|-fipa-icf"
    "align1|-falign-functions=1 -falign-jumps=1 -falign-labels=1"
    "mergec|-fmerge-constants"
    "icf-align|-fipa-icf -falign-functions=1 -falign-jumps=1 -falign-labels=1"
  )
else
  CANDS=(
    "base|"
    "icf|-fipa-icf"
    "align1|-falign-functions=1 -falign-jumps=1 -falign-labels=1"
    "mergec|-fmerge-constants"
    "icf-align|-fipa-icf -falign-functions=1 -falign-jumps=1 -falign-labels=1"
    "os|-Os"
    "os-icf|-Os -fipa-icf"
  )
fi

printf '%-6s %-8s %-10s %-10s %-10s %-10s %-6s\n' TIER TAG lunavm lunac lunajit lib FUNC
printf '%s\n' "---------------------------------------------------------------------------"

sz() { if [ -f "$1" ]; then wc -c < "$1" | tr -d ' '; else echo "-"; fi; }

for c in "${CANDS[@]}"; do
  tag=${c%%|*}; extrafl=${c#*|}
  [ "$tag" = "$extrafl" ] && extrafl=""
  B="build-opt-$TIER-$tag"
  rm -rf "$B"

  AR_TOOL="$MINGW/ar.exe"; ldef="$GCSECT"
  case "$tag" in lto) AR_TOOL="$MINGW/gcc-ar.exe"; ldef="$LTOFLAGS";; esac

  OPT="$BASEOPT $extrafl"
  CFLAGS="$OPT $ldef -fno-ident -std=c99 -Wall -Wextra -I. -Ivendor/lua $EXTRA_DEF"
  LUAFLAGS="$OPT $ldef -fno-ident -std=c99 -Ivendor/lua -w"
  LDFLAGS="-Wl,--gc-sections"
  [ "$tag" = "lto" ] && LDFLAGS="$LDFLAGS -flto"

  if ! "$MAKE" -j4 TIER="$TIER" BUILD="$B" CC="$MINGW/gcc.exe" AR="$AR_TOOL" \
        STRIP="$MINGW/strip.exe" CFLAGS="$CFLAGS" LUAFLAGS="$LUAFLAGS" LDFLAGS="$LDFLAGS" \
        all > ".tmp/build-$TIER-$tag.log" 2>&1; then
    printf '%-6s %-8s BUILD GAGAL -- %s\n' "$TIER" "$tag" "$(tail -3 ".tmp/build-$TIER-$tag.log" | tr '\n' ' ')"
    continue
  fi

  VMSZ=$(sz "$B/lunavm.exe"); LCSZ=$(sz "$B/lunac.exe"); LJSZ=$(sz "$B/lunajit.exe"); LIBSZ=$(sz "$B/liblunaberry.a")
  FUNC="skip"
  if [ "$VMSZ" != "-" ]; then
    if "$B/lunavm.exe" tests/test.lua > ".tmp/test-$TIER-$tag.log" 2>&1; then FUNC="ok"; else FUNC="FAIL"; fi
  fi
  printf '%-6s %-8s %-10s %-10s %-10s %-10s %-6s\n' "$TIER" "$tag" "$VMSZ" "$LCSZ" "$LJSZ" "$LIBSZ" "$FUNC"
done
