-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2011 Google Inc.
-- Havlak loop recognition over a synthetic control-flow graph.

local UNVISITED = 2147483647
local MAX_NON_BACK_PREDS = 32 * 1024
local NONHEADER, REDUCIBLE, SELF, IRREDUCIBLE, DEAD = 1, 2, 3, 4, 5

local function new_cfg()
  return { blocks = {}, start = nil }
end

local function node(cfg, name)
  local block = cfg.blocks[name + 1]
  if block == nil then
    block = { name = name, in_edges = {}, out_edges = {} }
    cfg.blocks[name + 1] = block
    if cfg.start == nil then cfg.start = block end
  end
  return block
end

local function connect(cfg, source, target)
  source = node(cfg, source)
  target = node(cfg, target)
  source.out_edges[#source.out_edges + 1] = target.name
  target.in_edges[#target.in_edges + 1] = source.name
end

local function new_loop(id, header, reducible)
  return {
    id = id, header = header, reducible = reducible, parent = nil,
    children = {}, child_ids = {}, blocks = {}, nesting = 0, depth = 0,
  }
end

local function add_child(parent, child)
  if not parent.child_ids[child.id] then
    parent.child_ids[child.id] = true
    parent.children[#parent.children + 1] = child
  end
end

local function set_parent(child, parent)
  child.parent = parent
  add_child(parent, child)
end

local function new_loop_graph()
  local root = new_loop(0, nil, true)
  return { root = root, loops = { root } }
end

local function add_loop(graph, header, reducible)
  local loop = new_loop(#graph.loops, header, reducible)
  graph.loops[#graph.loops + 1] = loop
  return loop
end

local function calculate_loop(graph, loop, depth)
  loop.depth = depth
  for _, child in ipairs(loop.children) do
    calculate_loop(graph, child, depth + 1)
    loop.nesting = math.max(loop.nesting, child.nesting + 1)
  end
end

local function calculate_nesting(graph)
  for i = 2, #graph.loops do
    local loop = graph.loops[i]
    if loop.parent == nil then set_parent(loop, graph.root) end
  end
  calculate_loop(graph, graph.root, 0)
end

local function new_finder(cfg, loops)
  return { cfg = cfg, loops = loops }
end

local function find_set(finder, number)
  local current = number
  local path = {}
  while finder.nodes[current + 1].parent ~= current do
    path[#path + 1] = current
    current = finder.nodes[current + 1].parent
  end
  for _, item in ipairs(path) do finder.nodes[item + 1].parent = current end
  return current
end

local function dfs(finder, block_id, number)
  finder.nodes[number + 1] = { parent = number, block = block_id, number = number, loop = nil }
  finder.number[block_id] = number
  local last = number
  for _, target in ipairs(finder.cfg.blocks[block_id + 1].out_edges) do
    if finder.number[target] == UNVISITED then last = dfs(finder, target, last + 1) end
  end
  finder.last[number + 1] = last
  return last
end

local function find_loops(finder)
  if finder.cfg.start == nil then return end
  local size = #finder.cfg.blocks
  finder.non_back, finder.back, finder.number = {}, {}, {}
  finder.header, finder.kind, finder.last, finder.nodes = {}, {}, {}, {}
  for i = 1, size do
    finder.non_back[i] = {}
    finder.back[i] = {}
    finder.number[finder.cfg.blocks[i].name] = UNVISITED
    finder.header[i] = 0
    finder.kind[i] = NONHEADER
    finder.last[i] = 0
  end
  dfs(finder, finder.cfg.start.name, 0)

  for w = 0, size - 1 do
    local current = finder.nodes[w + 1]
    if current == nil then
      finder.kind[w + 1] = DEAD
    else
      for _, source in ipairs(finder.cfg.blocks[current.block + 1].in_edges) do
        local v = finder.number[source]
        if v ~= UNVISITED then
          if w <= v and v <= finder.last[w + 1] then
            local back = finder.back[w + 1]
            back[#back + 1] = v
          else
            finder.non_back[w + 1][v] = true
          end
        end
      end
    end
  end

  finder.header[1] = 0
  for w = size - 1, 0, -1 do
    local node_w = finder.nodes[w + 1]
    local pool, pool_ids = {}, {}
    if node_w ~= nil then
      for _, v in ipairs(finder.back[w + 1]) do
        if v == w then
          finder.kind[w + 1] = SELF
        else
          local root = find_set(finder, v)
          if not pool_ids[root] then
            pool_ids[root] = true
            pool[#pool + 1] = root
          end
        end
      end

      if #pool > 0 then finder.kind[w + 1] = REDUCIBLE end
      local at = 1
      while at <= #pool do
        local x = pool[at]
        at = at + 1
        local preds = finder.non_back[finder.nodes[x + 1].number + 1]
        local count = 0
        for _ in pairs(preds) do count = count + 1 end
        if count > MAX_NON_BACK_PREDS then return end
        for pred in pairs(preds) do
          local root = find_set(finder, pred)
          local root_number = finder.nodes[root + 1].number
          if not (w <= root_number and root_number <= finder.last[w + 1]) then
            finder.kind[w + 1] = IRREDUCIBLE
            finder.non_back[w + 1][root_number] = true
          elseif root ~= w and not pool_ids[root] then
            pool_ids[root] = true
            pool[#pool + 1] = root
          end
        end
      end
    end

    if #pool > 0 or finder.kind[w + 1] == SELF then
      local loop = add_loop(finder.loops, node_w.block, finder.kind[w + 1] ~= IRREDUCIBLE)
      loop.blocks[node_w.block] = true
      finder.nodes[w + 1].loop = loop
      for _, number in ipairs(pool) do
        local current = finder.nodes[number + 1]
        finder.header[current.number + 1] = w
        current.parent = w
        if current.loop ~= nil then
          set_parent(current.loop, loop)
        else
          loop.blocks[current.block] = true
        end
      end
    end
  end
end

local function build_diamond(cfg, start)
  connect(cfg, start, start + 1)
  connect(cfg, start, start + 2)
  connect(cfg, start + 1, start + 3)
  connect(cfg, start + 2, start + 3)
  return start + 3
end

local function build_straight(cfg, start, length)
  for offset = 0, length - 1 do connect(cfg, start + offset, start + offset + 1) end
  return start + length
end

local function build_base_loop(cfg, start)
  local header = build_straight(cfg, start, 1)
  local diamond1 = build_diamond(cfg, header)
  local middle = build_straight(cfg, diamond1, 1)
  local diamond2 = build_diamond(cfg, middle)
  local footer = build_straight(cfg, diamond2, 1)
  connect(cfg, diamond2, middle)
  connect(cfg, diamond1, header)
  connect(cfg, footer, start)
  return build_straight(cfg, footer, 1)
end

local function run()
  local cfg = new_cfg()
  local loops = new_loop_graph()
  node(cfg, 0)
  build_base_loop(cfg, 0)
  node(cfg, 1)
  connect(cfg, 0, 2)
  find_loops(new_finder(cfg, loops))

  local n = 2
  for _ = 1, 10 do
    node(cfg, n + 1)
    connect(cfg, 2, n + 1)
    n = n + 1
    for _ = 1, 10 do
      local top = n
      n = build_straight(cfg, n, 1)
      for _ = 1, 5 do n = build_base_loop(cfg, n) end
      local bottom = build_straight(cfg, n, 1)
      connect(cfg, n, top)
      n = bottom
    end
    connect(cfg, n, 1)
  end

  find_loops(new_finder(cfg, loops))
  for _ = 1, 50 do find_loops(new_finder(cfg, new_loop_graph())) end
  calculate_nesting(loops)
  return #loops.loops, #cfg.blocks
end

local loop_count, block_count = run()
if loop_count ~= 1605 or block_count ~= 5213 then
  error(string.format("verify failed: %d, %d", loop_count, block_count))
end
print("ok")
