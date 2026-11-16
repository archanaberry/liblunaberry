-- bench/out/jswork_equiv.lua -- beban Lua yang ALGORITMA-NYA IDENTIK dengan
-- E:/bcf/mochiberry/benchmarks/bench_workload.js (fib(26) + sumTo(100000) x20),
-- supaya perbandingan LunaBerry vs MochiBerry adil lintas bahasa.
local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end
local function sumTo(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
local t = 0
for k = 1, 400 do
  t = t + fib(26) + sumTo(100000)
end
print("BENCH_RESULT " .. t)
