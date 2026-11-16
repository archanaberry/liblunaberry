local function fib(n)
  if n < 2 then
    return n
  end
  return fib(n - 1) + fib(n - 2)
end

local value = fib(20)
print("fib(20) =", value)
return value
