-- deltablue: incremental constraint solver. Builds an equality chain plus
-- a network of scale/offset projections, repeatedly edits one endpoint and
-- propagates. Stresses dispatch, table + list mutation, and worklist loops.
--
-- Derived from the SOM benchmarks (Are We Fast Yet?, Marr et al. 2016).
-- Flattened to a table + kind-tag style to match how richards is written
-- in this repo (and our mirrored Scheme port). No metatables / class hierarchy.

local CHAIN_N      = 12000
local PROJECTION_N = 12000

-- --- Strengths (just integers; smaller = stronger) ---------------------
local ABSOLUTE_STRONGEST = -10000
local REQUIRED           = -800
local STRONG_PREFERRED   = -600
local PREFERRED          = -400
local STRONG_DEFAULT     = -200
local DEFAULT_STRENGTH   = 0
local WEAK_DEFAULT       = 500
local ABSOLUTE_WEAKEST   = 10000

local function str_stronger(a, b) return a < b end
local function str_weaker(a, b)   return a > b end
local function str_weakest(a, b)  if a > b then return a else return b end end

-- --- Variable: 6-field table -----------------------------------------
-- [1]=value [2]=constraints [3]=determined_by [4]=walk_strength [5]=stay [6]=mark
local V_VALUE         = 1
local V_CONSTRAINTS   = 2
local V_DETERMINED_BY = 3
local V_WALK_STRENGTH = 4
local V_STAY          = 5
local V_MARK          = 6

local function make_var(initial)
  return { initial, {}, nil, ABSOLUTE_WEAKEST, true, 0 }
end

local function list_without(lst, x)
  local out = {}
  for i = 1, #lst do
    if lst[i] ~= x then out[#out + 1] = lst[i] end
  end
  return out
end

local function var_add_c(v, c)
  local cs = v[V_CONSTRAINTS]
  cs[#cs + 1] = c
end

local function var_remove_c(v, c)
  v[V_CONSTRAINTS] = list_without(v[V_CONSTRAINTS], c)
  if v[V_DETERMINED_BY] == c then
    v[V_DETERMINED_BY] = nil
  end
end

-- --- Constraint: 7-field table ---------------------------------------
local C_KIND     = 1
local C_STRENGTH = 2
local C_V1       = 3
local C_V2       = 4
local C_DIR      = 5  -- binary: DIR_*; unary: bool
local C_SCALE    = 6
local C_OFFSET   = 7

local K_STAY     = 0
local K_EDIT     = 1
local K_EQUALITY = 2
local K_SCALE    = 3

local DIR_NONE     = 0
local DIR_FORWARD  = 1
local DIR_BACKWARD = 2

local function c_unary(c)
  local k = c[C_KIND]
  return k == K_STAY or k == K_EDIT
end

local function c_input(c) return c[C_KIND] == K_EDIT end

local function c_satisfied(c)
  if c_unary(c) then return c[C_DIR] end
  return c[C_DIR] ~= DIR_NONE
end

local function c_mark_unsatisfied(c)
  if c_unary(c) then c[C_DIR] = false else c[C_DIR] = DIR_NONE end
end

local function c_output(c)
  if c_unary(c) then return c[C_V1] end
  if c[C_DIR] == DIR_FORWARD then return c[C_V2] else return c[C_V1] end
end

local function c_add_to_graph(c)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then
    var_add_c(c[C_V1], c)
    c[C_DIR] = false
  elseif k == K_EQUALITY then
    var_add_c(c[C_V1], c)
    var_add_c(c[C_V2], c)
    c[C_DIR] = DIR_NONE
  else  -- K_SCALE
    var_add_c(c[C_V1], c)
    var_add_c(c[C_V2], c)
    var_add_c(c[C_SCALE], c)
    var_add_c(c[C_OFFSET], c)
    c[C_DIR] = DIR_NONE
  end
end

local function c_remove_from_graph(c)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then
    var_remove_c(c[C_V1], c)
    c[C_DIR] = false
  elseif k == K_EQUALITY then
    var_remove_c(c[C_V1], c)
    var_remove_c(c[C_V2], c)
    c[C_DIR] = DIR_NONE
  else
    var_remove_c(c[C_V1], c)
    var_remove_c(c[C_V2], c)
    var_remove_c(c[C_SCALE], c)
    var_remove_c(c[C_OFFSET], c)
    c[C_DIR] = DIR_NONE
  end
end

local function c_choose_method(c, mark)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then
    local out = c[C_V1]
    c[C_DIR] = (out[V_MARK] ~= mark) and str_stronger(c[C_STRENGTH], out[V_WALK_STRENGTH])
    return
  end
  local v1, v2, s = c[C_V1], c[C_V2], c[C_STRENGTH]
  if v1[V_MARK] == mark then
    if v2[V_MARK] ~= mark and str_stronger(s, v2[V_WALK_STRENGTH]) then
      c[C_DIR] = DIR_FORWARD
    else
      c[C_DIR] = DIR_NONE
    end
    return
  end
  if v2[V_MARK] == mark then
    if v1[V_MARK] ~= mark and str_stronger(s, v1[V_WALK_STRENGTH]) then
      c[C_DIR] = DIR_BACKWARD
    else
      c[C_DIR] = DIR_NONE
    end
    return
  end
  if str_weaker(v1[V_WALK_STRENGTH], v2[V_WALK_STRENGTH]) then
    if str_stronger(s, v1[V_WALK_STRENGTH]) then
      c[C_DIR] = DIR_BACKWARD
    else
      c[C_DIR] = DIR_NONE
    end
  else
    if str_stronger(s, v2[V_WALK_STRENGTH]) then
      c[C_DIR] = DIR_FORWARD
    else
      c[C_DIR] = DIR_NONE
    end
  end
end

local function c_inputs_do(c, fn)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then
    return
  elseif k == K_EQUALITY then
    if c[C_DIR] == DIR_FORWARD then fn(c[C_V1]) else fn(c[C_V2]) end
  else
    if c[C_DIR] == DIR_FORWARD then
      fn(c[C_V1]); fn(c[C_SCALE]); fn(c[C_OFFSET])
    else
      fn(c[C_V2]); fn(c[C_SCALE]); fn(c[C_OFFSET])
    end
  end
end

local function input_known(v, mark)
  return v[V_MARK] == mark or v[V_STAY] or v[V_DETERMINED_BY] == nil
end

local function c_inputs_known(c, mark)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then return true end
  if k == K_EQUALITY then
    if c[C_DIR] == DIR_FORWARD then
      return input_known(c[C_V1], mark)
    else
      return input_known(c[C_V2], mark)
    end
  end
  -- K_SCALE
  local primary
  if c[C_DIR] == DIR_FORWARD then primary = c[C_V1] else primary = c[C_V2] end
  return input_known(primary, mark)
     and input_known(c[C_SCALE], mark)
     and input_known(c[C_OFFSET], mark)
end

local function c_execute(c)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then return end
  if k == K_EQUALITY then
    if c[C_DIR] == DIR_FORWARD then
      c[C_V2][V_VALUE] = c[C_V1][V_VALUE]
    else
      c[C_V1][V_VALUE] = c[C_V2][V_VALUE]
    end
    return
  end
  -- K_SCALE
  local scale = c[C_SCALE][V_VALUE]
  local offset = c[C_OFFSET][V_VALUE]
  if c[C_DIR] == DIR_FORWARD then
    c[C_V2][V_VALUE] = c[C_V1][V_VALUE] * scale + offset
  else
    c[C_V1][V_VALUE] = (c[C_V2][V_VALUE] - offset) / scale
  end
end

local function c_recalculate(c)
  local k = c[C_KIND]
  if k == K_STAY or k == K_EDIT then
    local out = c[C_V1]
    out[V_WALK_STRENGTH] = c[C_STRENGTH]
    out[V_STAY] = not c_input(c)
    if out[V_STAY] then c_execute(c) end
    return
  end
  local fwd = c[C_DIR] == DIR_FORWARD
  local inp, out
  if fwd then inp, out = c[C_V1], c[C_V2] else inp, out = c[C_V2], c[C_V1] end
  out[V_WALK_STRENGTH] = str_weakest(c[C_STRENGTH], inp[V_WALK_STRENGTH])
  if k == K_EQUALITY then
    out[V_STAY] = inp[V_STAY]
  else  -- K_SCALE
    out[V_STAY] = inp[V_STAY] and c[C_SCALE][V_STAY] and c[C_OFFSET][V_STAY]
  end
  if out[V_STAY] then c_execute(c) end
end

-- --- Constraint constructors -----------------------------------------
local planner_incremental_add  -- forward decl

local function planner_add_constraint(planner, c)
  c_add_to_graph(c)
  planner_incremental_add(planner, c)
end

local function new_stay(v, strength, planner)
  local c = { K_STAY, strength, v, nil, false, nil, nil }
  planner_add_constraint(planner, c)
  return c
end

local function new_edit(v, strength, planner)
  local c = { K_EDIT, strength, v, nil, false, nil, nil }
  planner_add_constraint(planner, c)
  return c
end

local function new_equality(v1, v2, strength, planner)
  local c = { K_EQUALITY, strength, v1, v2, DIR_NONE, nil, nil }
  planner_add_constraint(planner, c)
  return c
end

local function new_scale(src, scale, offset, dst, strength, planner)
  local c = { K_SCALE, strength, src, dst, DIR_NONE, scale, offset }
  planner_add_constraint(planner, c)
  return c
end

local planner_incremental_remove  -- forward decl

local function destroy_constraint(c, planner)
  if c_satisfied(c) then planner_incremental_remove(planner, c) end
  c_remove_from_graph(c)
end

-- --- Planner ---------------------------------------------------------
local function make_planner()
  return { current_mark = 1 }
end

local function planner_new_mark(p)
  local m = p.current_mark
  p.current_mark = m + 1
  return m
end

local function add_cct_to(v, determining_c, todo)
  local cs = v[V_CONSTRAINTS]
  for i = 1, #cs do
    local c = cs[i]
    if c ~= determining_c and c_satisfied(c) then
      todo[#todo + 1] = c
    end
  end
end

local function add_cct_out(v, todo)
  add_cct_to(v, v[V_DETERMINED_BY], todo)
end

local planner_add_propagate  -- forward decl

local function constraint_satisfy(c, mark, planner)
  c_choose_method(c, mark)
  if c_satisfied(c) then
    c_inputs_do(c, function(i) i[V_MARK] = mark end)
    local out = c_output(c)
    local overridden = out[V_DETERMINED_BY]
    if overridden ~= nil then c_mark_unsatisfied(overridden) end
    out[V_DETERMINED_BY] = c
    planner_add_propagate(planner, c, mark)
    out[V_MARK] = mark
    return overridden
  end
  if c[C_STRENGTH] == REQUIRED then
    error("failed to satisfy required")
  end
  return nil
end

planner_incremental_add = function(planner, c)
  local mark = planner_new_mark(planner)
  local overridden = constraint_satisfy(c, mark, planner)
  while overridden ~= nil do
    overridden = constraint_satisfy(overridden, mark, planner)
  end
end

planner_add_propagate = function(planner, c, mark)
  local todo = { c }
  while #todo > 0 do
    local d = table.remove(todo)
    local out = c_output(d)
    if out[V_MARK] == mark then
      planner_incremental_remove(planner, c)
      return false
    end
    c_recalculate(d)
    add_cct_out(out, todo)
  end
  return true
end

local function collect_unsatisfied(cs, acc)
  for i = 1, #cs do
    local c = cs[i]
    if not c_satisfied(c) then acc[#acc + 1] = c end
  end
  return acc
end

-- insertion sort: stronger (smaller strength) first
local function sort_by_strength(xs)
  for i = 2, #xs do
    local x = xs[i]
    local j = i - 1
    while j > 0 and str_stronger(x[C_STRENGTH], xs[j][C_STRENGTH]) do
      xs[j + 1] = xs[j]
      j = j - 1
    end
    xs[j + 1] = x
  end
  return xs
end

local function planner_remove_propagate_from(planner, out)
  out[V_DETERMINED_BY] = nil
  out[V_WALK_STRENGTH] = ABSOLUTE_WEAKEST
  out[V_STAY] = true
  local todo = { out }
  local unsatisfied = {}
  while #todo > 0 do
    local v = table.remove(todo)
    collect_unsatisfied(v[V_CONSTRAINTS], unsatisfied)
    local det = v[V_DETERMINED_BY]
    local cs = v[V_CONSTRAINTS]
    for i = 1, #cs do
      local c = cs[i]
      if c ~= det and c_satisfied(c) then
        c_recalculate(c)
        todo[#todo + 1] = c_output(c)
      end
    end
  end
  return sort_by_strength(unsatisfied)
end

planner_incremental_remove = function(planner, c)
  local out = c_output(c)
  c_mark_unsatisfied(c)
  c_remove_from_graph(c)
  local unsatisfied = planner_remove_propagate_from(planner, out)
  for i = 1, #unsatisfied do
    planner_incremental_add(planner, unsatisfied[i])
  end
end

local function planner_extract_plan(planner, sources)
  local mark = planner_new_mark(planner)
  local plan = {}
  local todo = sources
  while #todo > 0 do
    local c = table.remove(todo)
    local out = c_output(c)
    if out[V_MARK] ~= mark and c_inputs_known(c, mark) then
      out[V_MARK] = mark
      plan[#plan + 1] = c
      add_cct_out(out, todo)
    end
  end
  return plan
end

local function extract_plan_from_c(planner, c)
  local sources = {}
  if c_input(c) and c_satisfied(c) then sources[1] = c end
  return planner_extract_plan(planner, sources)
end

local function plan_execute(plan)
  for i = 1, #plan do c_execute(plan[i]) end
end

local function planner_change_var(planner, var, val)
  local edit = new_edit(var, PREFERRED, planner)
  local plan = extract_plan_from_c(planner, edit)
  for _ = 1, 10 do
    var[V_VALUE] = val
    plan_execute(plan)
  end
  destroy_constraint(edit, planner)
end

-- --- Tests -----------------------------------------------------------
local function assert_(ok, tag)
  if not ok then
    io.stderr:write("deltablue: " .. tag .. " failed\n")
    os.exit(1)
  end
end

local function chain_test(n)
  local planner = make_planner()
  local vars = {}
  for i = 0, n do vars[i] = make_var(0) end
  for i = 0, n - 1 do
    new_equality(vars[i], vars[i + 1], REQUIRED, planner)
  end
  new_stay(vars[n], STRONG_DEFAULT, planner)
  local edit = new_edit(vars[0], PREFERRED, planner)
  local plan = extract_plan_from_c(planner, edit)
  for v = 1, 100 do
    vars[0][V_VALUE] = v
    plan_execute(plan)
    assert_(vars[n][V_VALUE] == v, "chain")
  end
  destroy_constraint(edit, planner)
end

local function projection_test(n)
  local planner = make_planner()
  local scale = make_var(10)
  local offset = make_var(1000)
  local dests = {}
  local last_src, last_dst
  for i = 1, n do
    local src = make_var(i)
    local dst = make_var(i)
    dests[i - 1] = dst
    new_stay(src, DEFAULT_STRENGTH, planner)
    new_scale(src, scale, offset, dst, REQUIRED, planner)
    last_src, last_dst = src, dst
  end
  planner_change_var(planner, last_src, 17)
  assert_(last_dst[V_VALUE] == 1170, "projection-1")
  planner_change_var(planner, last_dst, 1050)
  assert_(last_src[V_VALUE] == 5, "projection-2")
  planner_change_var(planner, scale, 5)
  for j = 0, n - 2 do
    assert_(dests[j][V_VALUE] == (j + 1) * 5 + 1000, "projection-3")
  end
  planner_change_var(planner, offset, 2000)
  for j = 0, n - 2 do
    assert_(dests[j][V_VALUE] == (j + 1) * 5 + 2000, "projection-4")
  end
end

chain_test(CHAIN_N)
projection_test(PROJECTION_N)
print("ok")
