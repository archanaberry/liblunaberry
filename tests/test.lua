-- LunaBerry smoke test: run through lunavm and through .lbc.
print("luna test: " .. _VERSION)

local t = {}
for i = 1, 10 do
  t[i] = i * i
end

local sum = 0
for _, v in ipairs(t) do
  sum = sum + v
end

assert(sum == 385, "unexpected sum: " .. sum)
assert(#t == 10, "unexpected length")

print("sum", sum)
return sum
