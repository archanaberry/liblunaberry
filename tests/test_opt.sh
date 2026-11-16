#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

make >/dev/null

cat > build/optbench.lua <<'EOF'
local function fib(n) if n < 2 then return n end
  return fib(n-1) + fib(n-2) end
local function sieve(n)
  local t = {}
  for i = 2, n do t[i] = true end
  for i = 2, n do if t[i] then for j = i*i, n, i do t[j] = false end end end
  local c = 0
  for i = 2, n do if t[i] then c = c + 1 end end
  return c
end
local sum = 0
for i = 1, 20 do sum = sum + fib(i) end
print("fib20=", sum, "primes200=", sieve(200))
EOF

echo "== lunaaot O0..O10 =="
for lvl in 0 1 2 3 4 5 6 7 8 9 10; do
  ./build/lunaaot$EXE -O$lvl build/optbench.lua -o build/opt_o$lvl >/dev/null
  ./build/opt_o$lvl$EXE | grep -q "fib20=	17710"
done

echo "== lunaaot -O10 implicit strip + dead asm =="
./build/lunaaot$EXE -O10 --emit-s build/optbench.lua -o build/opt_strip
./build/opt_strip$EXE | grep -q "fib20=	17710"
grep -q "^	.globl	main" build/opt_strip$EXE.s
if grep -q "\.file" build/opt_strip$EXE.s; then
  echo "FAIL: dead assembly still has .file" >&2; exit 1
fi

echo "== lunaaot -O2 keeps source identity =="
./build/lunaaot$EXE -O2 --emit-s build/optbench.lua -o build/opt_id
grep -q "\.file" build/opt_id$EXE.s

echo "== lunaaot redundant --strip warning =="
./build/lunaaot$EXE -O10 --strip build/optbench.lua -o build/opt_warn \
  2>&1 | grep -q "redundant at -O10"

echo "== lunaaot invalid level =="
if ./build/lunaaot$EXE -O11 build/optbench.lua >/dev/null 2>&1; then
  echo "FAIL: -O11 should be rejected" >&2; exit 1
fi

echo "== lunajit -O10 =="
./build/lunajit$EXE -O10 --backend cc --run build/optbench.lua \
  | grep -q "fib20=	17710"

echo "== lunavm --jit -O10 =="
./build/lunavm$EXE --jit -O10 build/optbench.lua | grep -q "fib20=	17710"

echo "== ladder table sync (shell vs C) =="
for lvl in 0 1 2 3 4 5 6 7 8 9 10; do
  c=$(./build/lunavm$EXE --opt-flags $lvl)
  s=$(sh tools/ladder_flags.sh $lvl)
  if [ "$c" != "$s" ]; then
    echo "FAIL: level $lvl mismatch" >&2
    echo "  C : $c" >&2
    echo "  sh: $s" >&2
    exit 1
  fi
done

echo "== berry host honors -O (O10 strips) =="
./build/lunaaot$EXE --backend berry -O10 --target exe \
  -o build/opt_berry examples/fib.lua >/dev/null 2>&1
./build/opt_berry.host$EXE | grep -q "fib(20) =	6765"

echo "== jit launcher copy-path-and-patch =="
./build/lunajit$EXE --backend cc --target exe -o build/opt_cpp \
  examples/fib.lua >/dev/null 2>&1
if [ ! -f "build/opt_cpp$EXE" ]; then
  echo "FAIL: launcher not emitted" >&2; exit 1
fi
./build/opt_cpp$EXE | grep -q "fib(20) =	6765"

echo "opt: ok"
