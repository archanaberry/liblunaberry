-- A tiny application used by `make native` and `make berry`.
local app = {
  name = "LunaBerry",
  version = "5.5.1",
}

local function greet(who)
  return string.format("%s %s greets %s", app.name, app.version, who)
end

print(greet("the world"))

local total = 0
for i = 1, 100 do
  total = total + i
end
print("sum(1..100) =", total)

return total
