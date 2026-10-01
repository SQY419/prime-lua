-- fib.lua -- recursion, memoization, multiple returns, integer/float split
-- and a metatable, i.e. a small tour of what the interpreter must get right.
local function fib_naive(n)
  if n < 2 then return n end
  return fib_naive(n - 1) + fib_naive(n - 2)
end

local memo = setmetatable({}, {__index = function(t, n)
  local v = n < 2 and n or t[n - 1] + t[n - 2]
  rawset(t, n, v)
  return v
end})

local function fib_fast(n) return memo[n] end

print("naive recursion:")
for i = 0, 20 do
  if i % 5 == 0 then io.write(i, "=", fib_naive(i), "  ") end
end
print()

print("with a memo table and an __index metamethod:")
for _, n in ipairs{30, 50, 70, 90} do
  io.write(n, "=", fib_fast(n), "  ")
end
print()

-- big integers stay exact; big floats lose precision, which is the point
local function fib_iter(n)
  local a, b = 0, 1
  for _ = 1, n do a, b = b, a + b end
  return a
end
print("fib(100) as an integer: " .. fib_iter(100))
print("fib(100) as a float:    " .. string.format("%.14g", 2^100 * 0 + fib_iter(100) + 0.0))
print("integer arithmetic is exact: " .. tostring(fib_iter(100) == 354224848179261915075))
