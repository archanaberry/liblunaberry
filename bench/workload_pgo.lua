-- workload_pgo.lua -- beban training untuk PGO (re Alone).
-- Meniru bentuk hot path interpreter: dispatch fungsi (opcode VM),
-- alokasi table, string concat/format, aritmetika, dan pustaka dasar.
-- Sengaja dibuat stabil (tanpa I/O) supaya profil bisa direproduksi.
local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

local function sieve(n)
  local t = {}
  for i = 2, n do t[i] = true end
  for i = 2, n do
    if t[i] then
      for j = i * i, n, i do t[j] = false end
    end
  end
  local c = 0
  for i = 2, n do if t[i] then c = c + 1 end end
  return c
end

local function strwork(n)
  local parts = {}
  for i = 1, n do parts[#parts + 1] = string.format("%d:%s", i, "berry") end
  local joined = table.concat(parts, ",")
  local len = #joined
  for _ = 1, 4 do
    local up = string.upper(joined)
    len = len + #up
  end
  return len
end

local t = 0
for k = 1, 20 do
  t = t + fib(26)
  t = t + sieve(160)
  t = t + strwork(40)
  t = t + math.floor(t * 1.5) % 1000
end
print("PGO_WORKLOAD " .. t)
