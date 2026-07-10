-- fib: tree recursion, non-tail calls, mixed arithmetic + branching.
-- fib(35) = 9227465; ~30M function calls.

local function fib(n)
  if n < 2 then
    return n
  else
    return fib(n - 1) + fib(n - 2)
  end
end

print(fib(35))
