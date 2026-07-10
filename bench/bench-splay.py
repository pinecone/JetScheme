# splay: top-down splay tree via recursive path-copy. Allocation- and
# pointer-heavy; non-tail recursive on tree depth. Insert N deterministic
# keys, then look up the same N keys. Output: hit count (== N).

import sys

sys.setrecursionlimit(50000)


def node(k, l, r):
    return (k, l, r)


def splay(t, key):
    if t is None:
        return t
    k, l, r = t
    if key == k:
        return t
    if key < k:
        if l is None:
            return t
        lk, ll, lr = l
        if key == lk:
            return node(lk, ll, node(k, lr, r))
        if key < lk:
            if ll is None:
                return node(lk, ll, node(k, lr, r))
            s = splay(ll, key)
            return node(s[0], s[1], node(lk, s[2], node(k, lr, r)))
        else:
            if lr is None:
                return node(lk, ll, node(k, lr, r))
            s = splay(lr, key)
            return node(s[0], node(lk, ll, s[1]), node(k, s[2], r))
    else:
        if r is None:
            return t
        rk, rl, rr = r
        if key == rk:
            return node(rk, node(k, l, rl), rr)
        if key > rk:
            if rr is None:
                return node(rk, node(k, l, rl), rr)
            s = splay(rr, key)
            return node(s[0], node(rk, node(k, l, rl), s[1]), s[2])
        else:
            if rl is None:
                return node(rk, node(k, l, rl), rr)
            s = splay(rl, key)
            return node(s[0], node(k, l, s[1]), node(rk, s[2], rr))


def tree_insert(t, key):
    if t is None:
        return node(key, None, None)
    s = splay(t, key)
    if s[0] == key:
        return s
    if key < s[0]:
        return node(key, s[1], node(s[0], None, s[2]))
    return node(key, node(s[0], s[1], None), s[2])


N = 10000


def gen_key(i):
    return (i * 31) % N


tree = None
for i in range(1, N + 1):
    tree = tree_insert(tree, gen_key(i))

hits = 0
for i in range(1, N + 1):
    tree = splay(tree, gen_key(i))
    if tree is not None and tree[0] == gen_key(i):
        hits += 1

print(hits)
