-- sieve.lua -- primes below 200 by the sieve of Eratosthenes, then a couple
-- of things Lua is good at: string building and pattern matching.
local N = 200
local composite = {}
local primes = {}
for i = 2, N do
  if not composite[i] then
    primes[#primes + 1] = i
    for j = i * i, N, i do composite[j] = true end
  end
end

print("primes below " .. N .. ": " .. #primes)
print(table.concat(primes, " ", 1, 15) .. " ...")
print("sum = " .. (function()
  local s = 0
  for _, p in ipairs(primes) do s = s + p end
  return s
end)())
print("largest = " .. primes[#primes])

-- twin primes, to use gmatch/gsub on the list we just built
local list = table.concat(primes, " ")
local twins = 0
for a, b in list:gmatch("(%d+)%s+(%d+)") do
  if tonumber(b) - tonumber(a) == 2 then twins = twins + 1 end
end
print("twin prime pairs: " .. twins)

-- and once more with an iterator, the idiomatic way
local function primes_upto(n)
  local i = 1
  return function()
    i = i + 1
    while i <= n and composite[i] do i = i + 1 end
    if i <= n then return i end
  end
end
local count = 0
for p in primes_upto(N) do count = count + 1 end
print("recount with an iterator: " .. count)
