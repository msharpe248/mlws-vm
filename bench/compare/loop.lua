-- loop.lua - bench/loop.masm in Lua. See docs/REGISTER_VS_STACK.md. Usage: lua loop.lua [n]
local N = tonumber(arg[1] or 30000000)
local acc, i = 0, 0
while i < N do
  acc = (acc + (i * i) % 7) ~ 0x55
  i = i + 1
end
print(acc % 100000000)
