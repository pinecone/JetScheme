-- splay: top-down splay tree via recursive path-copy. Allocation- and
-- pointer-heavy; non-tail recursive on tree depth. Insert N deterministic
-- keys, then look up the same N keys. Output: hit count (== N).

local function node(k, l, r) return {k, l, r} end

local function splay(t, key)
  if t == nil then return t end
  local k, l, r = t[1], t[2], t[3]
  if key == k then return t end
  if key < k then
    if l == nil then return t end
    local lk, ll, lr = l[1], l[2], l[3]
    if key == lk then return node(lk, ll, node(k, lr, r)) end
    if key < lk then
      if ll == nil then return node(lk, ll, node(k, lr, r)) end
      local s = splay(ll, key)
      return node(s[1], s[2], node(lk, s[3], node(k, lr, r)))
    else
      if lr == nil then return node(lk, ll, node(k, lr, r)) end
      local s = splay(lr, key)
      return node(s[1], node(lk, ll, s[2]), node(k, s[3], r))
    end
  else
    if r == nil then return t end
    local rk, rl, rr = r[1], r[2], r[3]
    if key == rk then return node(rk, node(k, l, rl), rr) end
    if key > rk then
      if rr == nil then return node(rk, node(k, l, rl), rr) end
      local s = splay(rr, key)
      return node(s[1], node(rk, node(k, l, rl), s[2]), s[3])
    else
      if rl == nil then return node(rk, node(k, l, rl), rr) end
      local s = splay(rl, key)
      return node(s[1], node(k, l, s[2]), node(rk, s[3], rr))
    end
  end
end

local function tree_insert(t, key)
  if t == nil then return node(key, nil, nil) end
  local s = splay(t, key)
  if s[1] == key then return s end
  if key < s[1] then return node(key, s[2], node(s[1], nil, s[3])) end
  return node(key, node(s[1], s[2], nil), s[3])
end

local tree = nil

local N = 10000
local function gen_key(i) return (i * 31) % N end

for i = 1, N do
  tree = tree_insert(tree, gen_key(i))
end

local hits = 0
for i = 1, N do
  tree = splay(tree, gen_key(i))
  if tree ~= nil and tree[1] == gen_key(i) then
    hits = hits + 1
  end
end

print(hits)
