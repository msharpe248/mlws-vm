-- fib.lua - bench/fib.masm in Lua (a register VM with fixed 3-operand instructions).
-- See docs/REGISTER_VS_STACK.md. Usage: lua fib.lua [n]
local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end
print(fib(tonumber(arg[1] or 32)))
