# fib: tree recursion, non-tail calls, mixed arithmetic + branching.
# fib(35) = 9227465; ~30M function calls.


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(35))
