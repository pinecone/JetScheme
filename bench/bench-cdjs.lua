-- cdjs: aircraft collision detection. 1000 aircraft, 18 frames.
-- Ported from JetStream2 cdjs. Expects 1336 total collisions.

local ITERS              = 1
local NUM_AIRCRAFT       = 1000
local NUM_FRAMES         = 18
local EXPECTED_COLLISIONS = 1336

local MIN_X = 0
local MIN_Y = 0
local MAX_X = 1000
local MAX_Y = 1000
local MIN_Z = 0
local MAX_Z = 10
local PROXIMITY_RADIUS = 1
local GOOD_VOXEL_SIZE  = 2

local function compare_numbers(a, b)
  if a == b then return 0 end
  if a <  b then return -1 end
  if a >  b then return 1 end
  if a == a then return 1 end
  return -1
end

-- ---- Vector2D ----
local Vector2D = {}
Vector2D.__index = Vector2D

function Vector2D.new(x, y) return setmetatable({x = x, y = y}, Vector2D) end
function Vector2D:plus(o)   return Vector2D.new(self.x + o.x, self.y + o.y) end
function Vector2D:minus(o)  return Vector2D.new(self.x - o.x, self.y - o.y) end

function Vector2D:compare_to(o)
  local r = compare_numbers(self.x, o.x)
  if r ~= 0 then return r end
  return compare_numbers(self.y, o.y)
end

-- ---- Vector3D ----
local Vector3D = {}
Vector3D.__index = Vector3D

function Vector3D.new(x, y, z) return setmetatable({x = x, y = y, z = z}, Vector3D) end
function Vector3D:plus(o)  return Vector3D.new(self.x + o.x, self.y + o.y, self.z + o.z) end
function Vector3D:minus(o) return Vector3D.new(self.x - o.x, self.y - o.y, self.z - o.z) end
function Vector3D:dot(o)   return self.x*o.x + self.y*o.y + self.z*o.z end
function Vector3D:squared_magnitude() return self:dot(self) end
function Vector3D:magnitude()         return math.sqrt(self:squared_magnitude()) end
function Vector3D:times(c) return Vector3D.new(self.x * c, self.y * c, self.z * c) end

-- ---- CallSign ----
local CallSign = {}
CallSign.__index = CallSign

function CallSign.new(v) return setmetatable({_value = v}, CallSign) end

function CallSign:compare_to(o)
  if self._value == o._value then return 0 end
  if self._value <  o._value then return -1 end
  return 1
end

-- ---- Motion ----
local Motion = {}
Motion.__index = Motion

function Motion.new(cs, p1, p2)
  return setmetatable({callsign = cs, pos_one = p1, pos_two = p2}, Motion)
end

function Motion:delta() return self.pos_two:minus(self.pos_one) end

function Motion:find_intersection(other)
  local init1 = self.pos_one
  local init2 = other.pos_one
  local vec1  = self:delta()
  local vec2  = other:delta()
  local radius = PROXIMITY_RADIUS

  local a = vec2:minus(vec1):squared_magnitude()
  if a ~= 0 then
    local b = 2 * init1:minus(init2):dot(vec1:minus(vec2))
    local c = -radius * radius + init2:minus(init1):squared_magnitude()
    local discr = b * b - 4 * a * c
    if discr < 0 then return nil end
    local sq = math.sqrt(discr)
    local v1 = (-b - sq) / (2 * a)
    local v2 = (-b + sq) / (2 * a)
    if v1 <= v2 and ((v1 <= 1 and 1 <= v2) or
                     (v1 <= 0 and 0 <= v2) or
                     (0 <= v1 and v2 <= 1)) then
      local v
      if v1 <= 0 then v = 0 else v = v1 end
      local r1 = init1:plus(vec1:times(v))
      local r2 = init2:plus(vec2:times(v))
      local result = r1:plus(r2):times(0.5)
      if MIN_X <= result.x and result.x <= MAX_X and
         MIN_Y <= result.y and result.y <= MAX_Y and
         MIN_Z <= result.z and result.z <= MAX_Z then
        return result
      end
    end
    return nil
  end

  local dist = init2:minus(init1):magnitude()
  if dist <= radius then return init1:plus(init2):times(0.5) end
  return nil
end

-- ---- Red-Black tree ----
local function node_new(key, value)
  return { key = key, value = value,
           left = nil, right = nil, parent = nil, color = "red" }
end

local function tree_min(x)
  while x.left ~= nil do x = x.left end
  return x
end

local function node_successor(n)
  if n.right ~= nil then return tree_min(n.right) end
  local x = n
  local y = n.parent
  while y ~= nil and x == y.right do
    x = y
    y = y.parent
  end
  return y
end

local RBTree = {}
RBTree.__index = RBTree

function RBTree.new() return setmetatable({_root = nil}, RBTree) end

function RBTree:_find_node(key)
  local cur = self._root
  while cur ~= nil do
    local r = key:compare_to(cur.key)
    if r == 0 then return cur end
    if r < 0 then cur = cur.left else cur = cur.right end
  end
  return nil
end

function RBTree:get(key)
  local n = self:_find_node(key)
  if n == nil then return nil end
  return n.value
end

function RBTree:_left_rotate(x)
  local y = x.right
  x.right = y.left
  if y.left ~= nil then y.left.parent = x end
  y.parent = x.parent
  if x.parent == nil then
    self._root = y
  elseif x == x.parent.left then
    x.parent.left = y
  else
    x.parent.right = y
  end
  y.left = x
  x.parent = y
end

function RBTree:_right_rotate(y)
  local x = y.left
  y.left = x.right
  if x.right ~= nil then x.right.parent = y end
  x.parent = y.parent
  if y.parent == nil then
    self._root = x
  elseif y == y.parent.left then
    y.parent.left = x
  else
    y.parent.right = x
  end
  x.right = y
  y.parent = x
end

function RBTree:_tree_insert(key, value)
  local y = nil
  local x = self._root
  while x ~= nil do
    y = x
    local r = key:compare_to(x.key)
    if r < 0 then
      x = x.left
    elseif r > 0 then
      x = x.right
    else
      local old = x.value
      x.value = value
      return false, old
    end
  end
  local z = node_new(key, value)
  z.parent = y
  if y == nil then
    self._root = z
  elseif key:compare_to(y.key) < 0 then
    y.left = z
  else
    y.right = z
  end
  return true, z
end

function RBTree:put(key, value)
  local is_new, payload = self:_tree_insert(key, value)
  if not is_new then return payload end
  local x = payload
  while x ~= self._root and x.parent.color == "red" do
    if x.parent == x.parent.parent.left then
      local y = x.parent.parent.right
      if y ~= nil and y.color == "red" then
        x.parent.color = "black"
        y.color = "black"
        x.parent.parent.color = "red"
        x = x.parent.parent
      else
        if x == x.parent.right then
          x = x.parent
          self:_left_rotate(x)
        end
        x.parent.color = "black"
        x.parent.parent.color = "red"
        self:_right_rotate(x.parent.parent)
      end
    else
      local y = x.parent.parent.left
      if y ~= nil and y.color == "red" then
        x.parent.color = "black"
        y.color = "black"
        x.parent.parent.color = "red"
        x = x.parent.parent
      else
        if x == x.parent.left then
          x = x.parent
          self:_right_rotate(x)
        end
        x.parent.color = "black"
        x.parent.parent.color = "red"
        self:_left_rotate(x.parent.parent)
      end
    end
  end
  self._root.color = "black"
  return nil
end

function RBTree:_remove_fixup(x, x_parent)
  while x ~= self._root and (x == nil or x.color == "black") do
    if x == x_parent.left then
      local w = x_parent.right
      if w.color == "red" then
        w.color = "black"
        x_parent.color = "red"
        self:_left_rotate(x_parent)
        w = x_parent.right
      end
      if (w.left == nil or w.left.color == "black") and
         (w.right == nil or w.right.color == "black") then
        w.color = "red"
        x = x_parent
        x_parent = x.parent
      else
        if w.right == nil or w.right.color == "black" then
          w.left.color = "black"
          w.color = "red"
          self:_right_rotate(w)
          w = x_parent.right
        end
        w.color = x_parent.color
        x_parent.color = "black"
        if w.right ~= nil then w.right.color = "black" end
        self:_left_rotate(x_parent)
        x = self._root
        x_parent = x.parent
      end
    else
      local w = x_parent.left
      if w.color == "red" then
        w.color = "black"
        x_parent.color = "red"
        self:_right_rotate(x_parent)
        w = x_parent.left
      end
      if (w.right == nil or w.right.color == "black") and
         (w.left == nil or w.left.color == "black") then
        w.color = "red"
        x = x_parent
        x_parent = x.parent
      else
        if w.left == nil or w.left.color == "black" then
          w.right.color = "black"
          w.color = "red"
          self:_left_rotate(w)
          w = x_parent.left
        end
        w.color = x_parent.color
        x_parent.color = "black"
        if w.left ~= nil then w.left.color = "black" end
        self:_right_rotate(x_parent)
        x = self._root
        x_parent = x.parent
      end
    end
  end
  if x ~= nil then x.color = "black" end
end

function RBTree:remove(key)
  local z = self:_find_node(key)
  if z == nil then return nil end

  local y
  if z.left == nil or z.right == nil then
    y = z
  else
    y = node_successor(z)
  end

  local x
  if y.left ~= nil then x = y.left else x = y.right end

  local x_parent
  if x ~= nil then
    x.parent = y.parent
    x_parent = x.parent
  else
    x_parent = y.parent
  end

  if y.parent == nil then
    self._root = x
  elseif y == y.parent.left then
    y.parent.left = x
  else
    y.parent.right = x
  end

  if y ~= z then
    if y.color == "black" then self:_remove_fixup(x, x_parent) end
    y.parent = z.parent
    y.color = z.color
    y.left = z.left
    y.right = z.right
    if z.left  ~= nil then z.left.parent  = y end
    if z.right ~= nil then z.right.parent = y end
    if z.parent ~= nil then
      if z.parent.left == z then
        z.parent.left = y
      else
        z.parent.right = y
      end
    else
      self._root = y
    end
  elseif y.color == "black" then
    self:_remove_fixup(x, x_parent)
  end
  return z.value
end

function RBTree:for_each(callback)
  if self._root == nil then return end
  local cur = tree_min(self._root)
  while cur ~= nil do
    callback(cur.key, cur.value)
    cur = node_successor(cur)
  end
end

-- ---- Voxel hashing ----
local function trunc_toward_zero(x)
  local i = math.modf(x)
  return i
end

local function voxel_hash(position)
  local vs = GOOD_VOXEL_SIZE
  local xd = trunc_toward_zero(position.x / vs)
  local yd = trunc_toward_zero(position.y / vs)
  local result = Vector2D.new(vs * xd, vs * yd)
  if position.x < 0 then result.x = result.x - vs end
  if position.y < 0 then result.y = result.y - vs end
  return result
end

local function draw_motion_on_voxel_map(voxel_map, motion)
  local seen = RBTree.new()
  local vs = GOOD_VOXEL_SIZE
  local horizontal = Vector2D.new(vs, 0)
  local vertical   = Vector2D.new(0, vs)

  local function is_in_voxel(voxel)
    if voxel.x > MAX_X or voxel.x < MIN_X or
       voxel.y > MAX_Y or voxel.y < MIN_Y then
      return false
    end
    local init = motion.pos_one
    local fin  = motion.pos_two
    local v_s = vs
    local r = PROXIMITY_RADIUS / 2
    local v_x = voxel.x
    local x0 = init.x
    local xv = fin.x - x0
    local v_y = voxel.y
    local y0 = init.y
    local yv = fin.y - y0

    local low_x, high_x
    if xv ~= 0 then
      low_x  = (v_x - r - x0) / xv
      high_x = (v_x + v_s + r - x0) / xv
      if xv < 0 then low_x, high_x = high_x, low_x end
    else
      low_x, high_x = 0, 0
    end

    local low_y, high_y
    if yv ~= 0 then
      low_y  = (v_y - r - y0) / yv
      high_y = (v_y + v_s + r - y0) / yv
      if yv < 0 then low_y, high_y = high_y, low_y end
    else
      low_y, high_y = 0, 0
    end

    return (((xv == 0 and v_x <= x0 + r and x0 - r <= v_x + v_s) or
             (xv ~= 0 and ((low_x <= 1 and 1 <= high_x) or
                           (low_x <= 0 and 0 <= high_x) or
                           (0 <= low_x and high_x <= 1)))) and
            ((yv == 0 and v_y <= y0 + r and y0 - r <= v_y + v_s) or
             (yv ~= 0 and ((low_y <= 1 and 1 <= high_y) or
                           (low_y <= 0 and 0 <= high_y) or
                           (0 <= low_y and high_y <= 1)))) and
            (xv == 0 or yv == 0 or
             (low_y <= high_x and high_x <= high_y) or
             (low_y <= low_x and low_x <= high_y) or
             (low_x <= low_y and high_y <= high_x)))
  end

  local function put_into_map(voxel)
    local arr = voxel_map:get(voxel)
    if arr == nil then
      arr = {}
      voxel_map:put(voxel, arr)
    end
    arr[#arr + 1] = motion
  end

  local function recurse(nv)
    if not is_in_voxel(nv) then return end
    if seen:put(nv, true) then return end
    put_into_map(nv)
    recurse(nv:minus(horizontal))
    recurse(nv:plus(horizontal))
    recurse(nv:minus(vertical))
    recurse(nv:plus(vertical))
    recurse(nv:minus(horizontal):minus(vertical))
    recurse(nv:minus(horizontal):plus(vertical))
    recurse(nv:plus(horizontal):minus(vertical))
    recurse(nv:plus(horizontal):plus(vertical))
  end

  recurse(voxel_hash(motion.pos_one))
end

local function reduce_collision_set(motions)
  local voxel_map = RBTree.new()
  for i = 1, #motions do
    draw_motion_on_voxel_map(voxel_map, motions[i])
  end
  local result = {}
  voxel_map:for_each(function(_k, v)
    if #v > 1 then result[#result + 1] = v end
  end)
  return result
end

-- ---- Simulator ----
local Simulator = {}
Simulator.__index = Simulator

function Simulator.new(n)
  local aircraft = {}
  for i = 0, n - 1 do
    aircraft[i + 1] = CallSign.new("foo" .. tostring(i))
  end
  return setmetatable({_aircraft = aircraft}, Simulator)
end

function Simulator:simulate(time)
  local frame = {}
  local ct = math.cos(time)
  local st = math.sin(time)
  local n = #self._aircraft
  local i = 0
  while i < n do
    frame[#frame + 1] = { cs = self._aircraft[i + 1],
                          pos = Vector3D.new(time, ct * 2 + i * 3, 10) }
    frame[#frame + 1] = { cs = self._aircraft[i + 2],
                          pos = Vector3D.new(time, st * 2 + i * 3, 10) }
    i = i + 2
  end
  return frame
end

-- ---- CollisionDetector ----
local CollisionDetector = {}
CollisionDetector.__index = CollisionDetector

function CollisionDetector.new()
  return setmetatable({_state = RBTree.new()}, CollisionDetector)
end

function CollisionDetector:handle_new_frame(frame)
  local motions = {}
  local seen = RBTree.new()
  for i = 1, #frame do
    local entry = frame[i]
    local cs = entry.cs
    local newpos = entry.pos
    local oldpos = self._state:put(cs, newpos)
    seen:put(cs, true)
    if oldpos == nil then oldpos = newpos end
    motions[#motions + 1] = Motion.new(cs, oldpos, newpos)
  end
  local to_remove = {}
  self._state:for_each(function(cs, _p)
    if seen:get(cs) == nil then
      to_remove[#to_remove + 1] = cs
    end
  end)
  for i = 1, #to_remove do
    self._state:remove(to_remove[i])
  end
  local all_reduced = reduce_collision_set(motions)
  local count = 0
  for r = 1, #all_reduced do
    local reduced = all_reduced[r]
    local n = #reduced
    for i = 1, n do
      local m1 = reduced[i]
      for j = i + 1, n do
        if m1:find_intersection(reduced[j]) ~= nil then
          count = count + 1
        end
      end
    end
  end
  return count
end

-- ---- Driver ----
local function run_bench()
  local sim = Simulator.new(NUM_AIRCRAFT)
  local det = CollisionDetector.new()
  local total = 0
  for i = 0, NUM_FRAMES - 1 do
    total = total + det:handle_new_frame(sim:simulate(i / 10))
  end
  return total
end

for _ = 1, ITERS do
  local c = run_bench()
  if c ~= EXPECTED_COLLISIONS then
    error("cdjs: bad collision count: " .. c .. " (expected " .. EXPECTED_COLLISIONS .. ")")
  end
end

print(EXPECTED_COLLISIONS)
