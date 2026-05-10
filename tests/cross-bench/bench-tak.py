# tak (Takeuchi): triple non-tail recursion, integer-only.
# Classic call-dispatch stress test since the 1980s. tak(28,18,9) = 10.

import sys

sys.setrecursionlimit(20000)


def tak(x, y, z):
    if y < x:
        return tak(tak(x - 1, y, z),
                   tak(y - 1, z, x),
                   tak(z - 1, x, y))
    return z


print(tak(28, 18, 9))
