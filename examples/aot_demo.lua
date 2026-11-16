local function collatz(n)
  local steps = 0
  while n ~= 1 do
    if n % 2 == 0 then
      n = n // 2
    else
      n = 3 * n + 1
    end
    steps = steps + 1
  end
  return steps
end

local longest, arg = 0, 0
for i = 1, 10000 do
  local s = collatz(i)
  if s > longest then
    longest, arg = s, i
  end
end
print("longest collatz under 10000:", arg, longest)
return longest
