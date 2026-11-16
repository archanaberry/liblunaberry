#!/usr/bin/env bash
# wait_quiet_bench.sh -- jalankan benchmark hanya saat mesin cukup sepi.
#
# Host ini dipakai bersama (agent lain build/test262 paralel), jadi angka
# benchmark saat load tinggi TIDAK sahih. Script ini menunggu loadavg 1-menit
# di bawah ambang, lalu menjalankan perintah yang diberikan, dan mengulang
# bila kontaminasi terdeteksi (min jauh lebih buruk dari best sebelumnya).
#
# PEMAKAIAN: sh bench/wait_quiet_bench.sh <threshold_load> <max_wait_s> <cmd...>
set -uo pipefail
cd "$(dirname "$0")/.."
THRESH="${1:-3}"
MAXWAIT="${2:-900}"
shift 2 || true

start=$(date +%s)
while :; do
  now=$(date +%s)
  elapsed=$((now - start))
  load=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo 99)
  ok=$(awk -v l="$load" -v t="$THRESH" 'BEGIN{print (l<t)?1:0}')
  if [ "$ok" = "1" ]; then
    echo "[wait_quiet] load=$load < $THRESH setelah ${elapsed}s -> jalankan: $*"
    "$@"
    exit $?
  fi
  if [ "$elapsed" -ge "$MAXWAIT" ]; then
    echo "[wait_quiet] TIMEOUT ${elapsed}s, load masih $load -> jalankan saja (angka akan diwarn)" >&2
    "$@"
    exit $?
  fi
  sleep 15
done
