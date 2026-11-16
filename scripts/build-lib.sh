#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

make lib shared
ls -lh build/liblunaberry.a build/lunaberry.* 2>/dev/null || true
