-- count.lua - count the Lua VM instructions that fib.lua and loop.lua execute,
-- with a count hook that fires on every instruction. The functions are copies of
-- the ones in fib.lua and loop.lua. Usage: lua count.lua
local n = 0
local function hook() n = n + 1 end
local function fib(x)
  if x < 2 then return x end
  return fib(x - 1) + fib(x - 2)
end
local function loop(N)
  local acc, i = 0, 0
  while i < N do
    acc = (acc + (i * i) % 7) ~ 0x55
    i = i + 1
  end
  return acc % 100000000
end
debug.sethook(hook, "", 1); fib(20); debug.sethook()
print(string.format("fib %.2f", n / 21891))          -- fib(20) makes 21,891 calls
n = 0
debug.sethook(hook, "", 1); loop(100000); debug.sethook()
print(string.format("loop %.2f", n / 100000))
