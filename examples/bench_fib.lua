local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

local best = math.huge
for _ = 1, 3 do
  local t0 = os.clock()
  local v = fib(28)
  local dt = os.clock() - t0
  if dt < best then best = dt end
  print(string.format("fib(28)=%d  %.3fs", v, dt))
end
print(string.format("best: %.3fs", best))
return best
