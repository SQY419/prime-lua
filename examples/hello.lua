-- hello.lua -- the traditional first program.
print("Hello from " .. _VERSION .. " on the HP Prime")
print("int:      " .. tostring(7 // 2) .. "  float: " .. tostring(7 / 2))
print("pi ~= " .. string.format("%.14g", math.pi))
print("host:     " .. (jit and jit.version or "PUC-Rio interpreter"))

for i = 1, 5 do
  print(string.rep("*", i), i, i * i, 1 / i)
end
