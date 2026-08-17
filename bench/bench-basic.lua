-- Port of the ARES-6 "Basic" benchmark (a BASIC interpreter) to Lua 5.4.
-- A coroutine drives the program and yields I/O
-- events, a recursive-descent parser, case-insensitive variable maps, nested
-- arrays for DIM. Idiomatic Lua: metatable classes, coroutine lexer, table.concat.

-- forward declarations for mutually-recursive pieces
local evaluate, num_to_str, sign
local e_number_apply, e_variable, e_const, e_pow, e_mul, e_div, e_neg, e_add, e_sub
local e_string_var, e_equals, e_not_equals, e_less_than, e_greater_than, e_less_equal, e_greater_equal
local s_goto, s_gosub, s_def, s_let, s_if, s_return, s_stop, s_on, s_for, s_next
local s_print, s_input, s_read, s_restore, s_dim, s_randomize, s_end
local program_run, BLOCK_END

-- ---------------- RNG: Robert Jenkins' 32-bit integer hash (from Octane) ----------------
local function create_rng(seed)
  return function()
    seed = (seed + 0x7ed55d16 + (seed << 12)) & 0xffffffff
    seed = (seed ~ 0xc761c23c ~ (seed >> 19)) & 0xffffffff
    seed = (seed + 0x165667b1 + (seed << 5)) & 0xffffffff
    seed = ((seed + 0xd3a2646c) ~ (seed << 9)) & 0xffffffff
    seed = (seed + 0xfd7046c5 + (seed << 3)) & 0xffffffff
    seed = (seed ~ 0xb55a4f09 ~ (seed >> 16)) & 0xffffffff
    return (seed & 0xfffffff) / 0x10000000
  end
end

local function create_rng_with_fixed_seed() return create_rng(49734321) end
local function create_rng_with_random_seed() return create_rng(math.random(0, 0xffffffff)) end

sign = function(x)
  if x > 0 then return 1.0 elseif x < 0 then return -1.0 else return 0.0 end
end

num_to_str = function(x)
  if x ~= x then return "NaN" end
  if math.type(x) == "integer" then return tostring(x) end
  if x == math.floor(x) and math.abs(x) < 1e21 then return string.format("%d", x) end
  return tostring(x)
end

-- ---------------- values ----------------
local CaselessMap = {}
CaselessMap.__index = CaselessMap
function CaselessMap.new(other)
  local self = setmetatable({map = {}}, CaselessMap)
  if other then for k, v in pairs(other.map) do self.map[k] = v end end
  return self
end
function CaselessMap:set(key, value) self.map[string.lower(key)] = value end
function CaselessMap:has(key) return self.map[string.lower(key)] ~= nil end
function CaselessMap:get(key) return self.map[string.lower(key)] end

local NumberValue = {}
NumberValue.__index = NumberValue
function NumberValue.new(value) return setmetatable({value = value or 0}, NumberValue) end
function NumberValue:apply(state, parameters)
  state:validate(#parameters == 0, "Should not pass arguments to simple numeric variables")
  return self.value
end
function NumberValue:leftApply(state, parameters)
  state:validate(#parameters == 0, "Should not pass arguments to simple numeric variables")
  return self
end
function NumberValue:assign(value) self.value = value end

local NumberArray = {}
NumberArray.__index = NumberArray
function NumberArray.new(dim)
  dim = dim or {11}
  local function allocate(index)
    local count = math.floor(dim[index])
    local arr = {n = count}
    if index + 1 <= #dim then
      for i = 0, count - 1 do arr[i] = allocate(index + 1) end
    else
      for i = 0, count - 1 do arr[i] = NumberValue.new() end
    end
    return arr
  end
  return setmetatable({array = allocate(1), dim = dim}, NumberArray)
end
function NumberArray:apply(state, parameters)
  return self:leftApply(state, parameters):apply(state, {})
end
function NumberArray:leftApply(state, parameters)
  if #self.dim ~= #parameters then
    state:abort("Expected " .. #self.dim .. " arguments but " .. #parameters .. " were passed.")
  end
  local result = self.array
  for i = 1, #parameters do
    local index = math.floor(parameters[i])
    if not (index >= state.program.base) or not (index < result.n) then
      state:abort("Index out of bounds: " .. index)
    end
    result = result[index]
  end
  return result
end

local NativeFunction = {}
NativeFunction.__index = NativeFunction
function NativeFunction.new(arity, callback)
  return setmetatable({arity = arity, callback = callback}, NativeFunction)
end
function NativeFunction:apply(state, parameters)
  if self.arity ~= #parameters then
    state:abort("Expected " .. self.arity .. " arguments but " .. #parameters .. " were passed")
  end
  return self.callback(table.unpack(parameters))
end
function NativeFunction:leftApply(state, parameters)
  state:abort("Cannot use a native function as an lvalue")
end

local NumberFunction = {}
NumberFunction.__index = NumberFunction
function NumberFunction.new(parameters, code)
  return setmetatable({parameters = parameters, code = code}, NumberFunction)
end
function NumberFunction:apply(state, parameters)
  if #self.parameters ~= #parameters then state:abort("Wrong number of arguments") end
  local old = state.values
  state.values = CaselessMap.new(old)
  for i = 1, #parameters do state.values:set(self.parameters[i], parameters[i]) end
  local result = evaluate(self.code, state)
  state.values = old
  return result
end
function NumberFunction:leftApply(state, parameters)
  state:abort("Cannot use a function as an lvalue")
end

local State = {}
State.__index = State
function State.new(program)
  local self = setmetatable({}, State)
  self.values = CaselessMap.new()
  self.stringValues = CaselessMap.new()
  self.statement = nil
  self.nextLineNumber = 0
  self.subStack = {}
  self.dataIndex = 0
  self.program = program
  self.rng = create_rng_with_fixed_seed()
  local function addNative(name, arity, callback) self.values:set(name, NativeFunction.new(arity, callback)) end
  addNative("abs", 1, function(x) return math.abs(x) end)
  addNative("atn", 1, function(x) return math.atan(x) end)
  addNative("cos", 1, function(x) return math.cos(x) end)
  addNative("exp", 1, function(x) return math.exp(x) end)
  addNative("int", 1, function(x) return math.floor(x) end)
  addNative("log", 1, function(x) return math.log(x) end)
  addNative("rnd", 0, function() return self.rng() end)
  addNative("sgn", 1, function(x) return sign(x) end)
  addNative("sin", 1, function(x) return math.sin(x) end)
  addNative("sqr", 1, function(x) return math.sqrt(x) end)
  addNative("tan", 1, function(x) return math.tan(x) end)
  return self
end
function State:getValue(name, numParameters)
  if self.values:has(name) then return self.values:get(name) end
  local result
  if numParameters == 0 then
    result = NumberValue.new()
  else
    local dim = {}
    for i = 1, numParameters do dim[i] = 11 end
    result = NumberArray.new(dim)
  end
  self.values:set(name, result)
  return result
end
function State:abort(text)
  if not self.statement then error("At beginning of execution: " .. text) end
  error("At " .. tostring(self.statement.sourceLineNumber) .. ": " .. text)
end
function State:validate(predicate, text) if not predicate then self:abort(text) end end

-- ---------------- expression evaluators ----------------
evaluate = function(node, state) return node.ev(node, state) end

e_number_apply = function(node, state)
  local params = {}
  for i = 1, #node.parameters do params[i] = evaluate(node.parameters[i], state) end
  return state:getValue(node.name, #params):apply(state, params)
end
e_variable = function(node, state)
  local params = {}
  for i = 1, #node.parameters do params[i] = evaluate(node.parameters[i], state) end
  return state:getValue(node.name, #params):leftApply(state, params)
end
e_const = function(node, state) return node.value end
e_pow = function(node, state) return evaluate(node.left, state) ^ evaluate(node.right, state) end
e_mul = function(node, state) return evaluate(node.left, state) * evaluate(node.right, state) end
e_div = function(node, state) return evaluate(node.left, state) / evaluate(node.right, state) end
e_neg = function(node, state) return -evaluate(node.term, state) end
e_add = function(node, state) return evaluate(node.left, state) + evaluate(node.right, state) end
e_sub = function(node, state) return evaluate(node.left, state) - evaluate(node.right, state) end
e_string_var = function(node, state)
  local value = state.stringValues:get(node.name)
  if value == nil then state:abort("Could not find string variable " .. node.name) end
  return value
end
e_equals = function(node, state) return evaluate(node.left, state) == evaluate(node.right, state) end
e_not_equals = function(node, state) return evaluate(node.left, state) ~= evaluate(node.right, state) end
e_less_than = function(node, state) return evaluate(node.left, state) < evaluate(node.right, state) end
e_greater_than = function(node, state) return evaluate(node.left, state) > evaluate(node.right, state) end
e_less_equal = function(node, state) return evaluate(node.left, state) <= evaluate(node.right, state) end
e_greater_equal = function(node, state) return evaluate(node.left, state) >= evaluate(node.right, state) end

-- ---------------- statement processors (only print/input yield events) ----------------
s_goto = function(node, state) state.nextLineNumber = node.target end
s_gosub = function(node, state)
  state.subStack[#state.subStack + 1] = state.nextLineNumber
  state.nextLineNumber = node.target
end
s_def = function(node, state)
  state:validate(not state.values:has(node.name), "Cannot redefine function")
  state.values:set(node.name, NumberFunction.new(node.parameters, node.expression))
end
s_let = function(node, state)
  evaluate(node.variable, state):assign(evaluate(node.expression, state))
end
s_if = function(node, state)
  if evaluate(node.condition, state) then state.nextLineNumber = node.target end
end
s_return = function(node, state)
  state:validate(#state.subStack > 0, "Not in a subroutine")
  state.nextLineNumber = state.subStack[#state.subStack]
  state.subStack[#state.subStack] = nil
end
s_stop = function(node, state) state.nextLineNumber = nil end
s_on = function(node, state)
  local index = evaluate(node.expression, state)
  if not (index >= 1) or not (index <= #node.targets) then
    state:abort("Index out of bounds: " .. num_to_str(index))
  end
  state.nextLineNumber = node.targets[math.floor(index)]
end
s_for = function(node, state)
  local side = {}
  node.sideState = side
  side.variable = state:getValue(node.variable, 0):leftApply(state, {})
  side.initialValue = evaluate(node.initial, state)
  side.limitValue = evaluate(node.limit, state)
  side.stepValue = evaluate(node.step, state)
  side.variable:assign(side.initialValue)
  side.shouldStop = function()
    return (side.variable.value - side.limitValue) * sign(side.stepValue) > 0
  end
  if side.shouldStop() then state.nextLineNumber = node.target.lineNumber + 1 end
end
s_next = function(node, state)
  local side = node.target.sideState
  side.variable:assign(side.variable.value + side.stepValue)
  if side.shouldStop() then return end
  state.nextLineNumber = node.target.lineNumber + 1
end
s_print = function(node, state)
  local str = ""
  for _, item in ipairs(node.items) do
    if item.kind == "comma" then
      while #str % 14 ~= 0 do str = str .. " " end
    elseif item.kind == "tab" then
      local value = evaluate(item.value, state)
      value = math.max(math.floor(value + 0.5), 1)
      while #str % value ~= 0 do str = str .. " " end
    elseif item.kind == "string" or item.kind == "number" then
      local value = evaluate(item.value, state)
      if type(value) == "string" then str = str .. value else str = str .. num_to_str(value) end
    else
      error("Bad item kind: " .. tostring(item.kind))
    end
  end
  coroutine.yield({kind = "output", string = str})
end
s_input = function(node, state)
  local results = coroutine.yield({kind = "input", numItems = #node.items})
  state:validate(results ~= nil and #results == #node.items, "Input did not get the right number of items")
  for i = 1, #results do evaluate(node.items[i], state):assign(results[i]) end
end
s_read = function(node, state)
  for _, item in ipairs(node.items) do
    state:validate(state.dataIndex < #state.program.data, "Attempting to read past the end of data")
    evaluate(item, state):assign(state.program.data[state.dataIndex + 1])
    state.dataIndex = state.dataIndex + 1
  end
end
s_restore = function(node, state) state.dataIndex = 0 end
s_dim = function(node, state)
  for _, item in ipairs(node.items) do
    state:validate(not state.values:has(item.name), "Variable " .. item.name .. " already exists")
    state:validate(#item.bounds > 0, "Dim statement is for arrays")
    local dim = {}
    for i = 1, #item.bounds do dim[i] = item.bounds[i] + 1 end
    state.values:set(item.name, NumberArray.new(dim))
  end
end
s_randomize = function(node, state) state.rng = create_rng_with_random_seed() end
s_end = function(node, state) state.nextLineNumber = nil end

BLOCK_END = {[s_next] = true, [s_end] = true}

program_run = function(program, state)
  local statements = program.statements
  local maxLine = 0
  for k in pairs(statements) do if k > maxLine then maxLine = k end end
  while state.nextLineNumber ~= nil do
    state:validate(state.nextLineNumber <= maxLine, "Went out of bounds of the program")
    local statement = statements[state.nextLineNumber]
    state.nextLineNumber = state.nextLineNumber + 1
    if statement ~= nil and statement.proc ~= nil then
      state.statement = statement
      statement.proc(statement, state)
    end
  end
end

-- ---------------- lexer (lazy, via coroutine) ----------------
local KEYWORDS = {
  base = true, data = true, def = true, dim = true, ["end"] = true, ["for"] = true,
  go = true, gosub = true, ["goto"] = true, ["if"] = true, input = true, let = true,
  next = true, on = true, option = true, print = true, randomize = true, read = true,
  restore = true, ["return"] = true, step = true, stop = true, sub = true, ["then"] = true, ["to"] = true,
}

local function split_lines(s)
  local lines, start = {}, 1
  while true do
    local nl = string.find(s, "\n", start, true)
    if not nl then lines[#lines + 1] = string.sub(s, start); break end
    lines[#lines + 1] = string.sub(s, start, nl - 1)
    start = nl + 1
  end
  return lines
end

local function lex(source)
  return coroutine.wrap(function()
    local sourceLineNumber = 0
    for _, line in ipairs(split_lines(source)) do
      sourceLineNumber = sourceLineNumber + 1
      local pos = 1
      local function skip_ws()
        local s, e = string.find(line, "^%s+", pos)
        if s then pos = e + 1 end
      end
      skip_ws()
      local s, e = string.find(line, "^%d+", pos)
      if not s then error("At line " .. sourceLineNumber .. ": Expect line number: " .. string.sub(line, pos)) end
      local userLineNumber = tonumber(string.sub(line, s, e))
      pos = e + 1
      coroutine.yield({kind = "userLineNumber", string = string.sub(line, s, e),
                       sourceLineNumber = sourceLineNumber, userLineNumber = userLineNumber})
      skip_ws()
      while pos <= #line do
        local ws, we = string.find(line, "^[%a_][%w_]*", pos)
        if ws then
          local word = string.sub(line, ws, we)
          local kind = KEYWORDS[string.lower(word)] and "keyword" or "identifier"
          coroutine.yield({kind = kind, string = word, sourceLineNumber = sourceLineNumber, userLineNumber = userLineNumber})
          pos = we + 1
        else
          local ns, ne = string.find(line, "^%d+%.?%d*", pos)
          if not ns then ns, ne = string.find(line, "^%.%d+", pos) end
          if ns then
            local numstr = string.sub(line, ns, ne)
            coroutine.yield({kind = "number", string = numstr, value = tonumber(numstr),
                             sourceLineNumber = sourceLineNumber, userLineNumber = userLineNumber})
            pos = ne + 1
          elseif string.sub(line, pos, pos) == '"' then
            local i = pos + 1
            while i <= #line do
              if string.sub(line, i, i) == '"' then
                if string.sub(line, i + 1, i + 1) == '"' then i = i + 2 else break end
              else
                i = i + 1
              end
            end
            local raw = string.sub(line, pos, i)
            local value, j = {}, pos + 1
            while j < i do
              local ch = string.sub(line, j, j)
              if ch == '"' then j = j + 1 end
              value[#value + 1] = ch
              j = j + 1
            end
            coroutine.yield({kind = "string", string = raw, value = table.concat(value),
                             sourceLineNumber = sourceLineNumber, userLineNumber = userLineNumber})
            pos = i + 1
          else
            local two = string.sub(line, pos, pos + 1)
            local one = string.sub(line, pos, pos)
            local op
            if two == "<>" or two == "<=" or two == ">=" then
              op = two
            elseif string.find("-+*/^(),$;=<>", one, 1, true) then
              op = one
            end
            if op then
              coroutine.yield({kind = "operator", string = op, sourceLineNumber = sourceLineNumber, userLineNumber = userLineNumber})
              pos = pos + #op
            else
              error("At line " .. sourceLineNumber .. ": Cannot lex token: " .. string.sub(line, pos))
            end
          end
        end
        skip_ws()
      end
      coroutine.yield({kind = "newLine", string = "\n", sourceLineNumber = sourceLineNumber, userLineNumber = userLineNumber})
    end
  end)
end

-- ---------------- parser ----------------
local Parser = {}
Parser.__index = Parser
function Parser.new(iter) return setmetatable({iter = iter, pushback = {}, program = nil}, Parser) end
function Parser:nextToken()
  local n = #self.pushback
  if n > 0 then local t = self.pushback[n]; self.pushback[n] = nil; return t end
  local t = self.iter()
  if t == nil then return {kind = "endOfFile", string = "<end of file>"} end
  return t
end
function Parser:pushToken(t) self.pushback[#self.pushback + 1] = t end
function Parser:peekToken() local t = self:nextToken(); self:pushToken(t); return t end
function Parser:consumeKind(kind)
  local t = self:nextToken()
  if t.kind ~= kind then error("At " .. tostring(t.sourceLineNumber) .. ": expected " .. kind .. " but got: " .. tostring(t.string)) end
  return t
end
function Parser:consumeToken(str)
  local t = self:nextToken()
  if string.lower(t.string) ~= string.lower(str) then error("At " .. tostring(t.sourceLineNumber) .. ": expected " .. str .. " but got: " .. tostring(t.string)) end
  return t
end
function Parser:parseVariable()
  local name = self:consumeKind("identifier").string
  local result = {ev = e_variable, name = name, parameters = {}}
  if self:peekToken().string == "(" then
    repeat
      self:nextToken()
      result.parameters[#result.parameters + 1] = self:parseNumericExpression()
    until self:peekToken().string ~= ","
    self:consumeToken(")")
  end
  return result
end
function Parser:parsePrimary()
  local token = self:nextToken()
  if token.kind == "identifier" then
    local result = {ev = e_number_apply, name = token.string, parameters = {}}
    if self:peekToken().string == "(" then
      repeat
        self:nextToken()
        result.parameters[#result.parameters + 1] = self:parseNumericExpression()
      until self:peekToken().string ~= ","
      self:consumeToken(")")
    end
    return result
  end
  if token.kind == "number" then return {ev = e_const, value = token.value} end
  if token.kind == "operator" and token.string == "(" then
    local result = self:parseNumericExpression()
    self:consumeToken(")")
    return result
  end
  error("At " .. tostring(token.sourceLineNumber) .. ": expected identifier, number, or (, but got: " .. tostring(token.string))
end
function Parser:parseFactor()
  local primary = self:parsePrimary()
  while self:peekToken().string == "^" do
    self:nextToken()
    primary = {ev = e_pow, left = primary, right = self:parsePrimary()}
  end
  return primary
end
function Parser:parseTerm()
  local factor = self:parseFactor()
  while true do
    local s = self:peekToken().string
    if s == "*" then
      self:nextToken(); factor = {ev = e_mul, left = factor, right = self:parseFactor()}
    elseif s == "/" then
      self:nextToken(); factor = {ev = e_div, left = factor, right = self:parseFactor()}
    else
      break
    end
  end
  return factor
end
function Parser:parseNumericExpression()
  local negate = false
  local s = self:peekToken().string
  if s == "+" then self:nextToken() elseif s == "-" then negate = true; self:nextToken() end
  local term = self:parseTerm()
  if negate then term = {ev = e_neg, term = term} end
  while true do
    s = self:peekToken().string
    if s == "+" then
      self:nextToken(); term = {ev = e_add, left = term, right = self:parseTerm()}
    elseif s == "-" then
      self:nextToken(); term = {ev = e_sub, left = term, right = self:parseTerm()}
    else
      break
    end
  end
  return term
end
function Parser:parseConstant()
  local s = self:peekToken().string
  if s == "+" then self:nextToken(); return self:consumeKind("number").value end
  if s == "-" then self:nextToken(); return -self:consumeKind("number").value end
  if self:isStringExpression() then return self:consumeKind("string").value end
  return self:consumeKind("number").value
end
function Parser:parseStringExpression()
  local token = self:nextToken()
  if token.kind == "string" then return {ev = e_const, value = token.value} end
  if token.kind == "identifier" then
    self:consumeToken("$")
    return {ev = e_string_var, name = token.string}
  end
  error("At " .. tostring(token.sourceLineNumber) .. ": expected string expression but got " .. tostring(token.string))
end
function Parser:isStringExpression()
  local token = self:nextToken()
  if token.kind == "string" then self:pushToken(token); return true end
  if token.kind == "identifier" then
    local result = self:peekToken().string == "$"
    self:pushToken(token)
    return result
  end
  self:pushToken(token)
  return false
end
function Parser:parseRelationalExpression()
  if self:isStringExpression() then
    local left = self:parseStringExpression()
    local operator = self:nextToken()
    local ev
    if operator.string == "=" then ev = e_equals
    elseif operator.string == "<>" then ev = e_not_equals
    else error("At " .. tostring(operator.sourceLineNumber) .. ": expected a string comparison operator but got: " .. tostring(operator.string)) end
    return {ev = ev, left = left, right = self:parseStringExpression()}
  end
  local left = self:parseNumericExpression()
  local operator = self:nextToken()
  local ev
  if operator.string == "=" then ev = e_equals
  elseif operator.string == "<>" then ev = e_not_equals
  elseif operator.string == "<" then ev = e_less_than
  elseif operator.string == ">" then ev = e_greater_than
  elseif operator.string == "<=" then ev = e_less_equal
  elseif operator.string == ">=" then ev = e_greater_equal
  else error("At " .. tostring(operator.sourceLineNumber) .. ": expected a numeric comparison operator but got: " .. tostring(operator.string)) end
  return {ev = ev, left = left, right = self:parseNumericExpression()}
end
function Parser:parseNonNegativeInteger()
  local token = self:nextToken()
  if not string.match(token.string, "^%d+$") then
    error("At " .. tostring(token.sourceLineNumber) .. ": expected a line number but got: " .. tostring(token.string))
  end
  return token.value
end
function Parser:parseGoto(st) st.proc = s_goto; st.target = self:parseNonNegativeInteger() end
function Parser:parseGosub(st) st.proc = s_gosub; st.target = self:parseNonNegativeInteger() end
function Parser:parseStatement()
  local st = {}
  st.lineNumber = self:consumeKind("userLineNumber").userLineNumber
  self.program.statements[st.lineNumber] = st
  local command = self:nextToken()
  st.sourceLineNumber = command.sourceLineNumber
  if command.kind == "keyword" then
    local kw = string.lower(command.string)
    if kw == "def" then
      st.proc = s_def
      st.name = self:consumeKind("identifier").string
      st.parameters = {}
      if self:peekToken().string == "(" then
        repeat
          self:nextToken()
          st.parameters[#st.parameters + 1] = self:consumeKind("identifier").string
        until self:peekToken().string ~= ","
      end
      st.expression = self:parseNumericExpression()
    elseif kw == "let" then
      st.proc = s_let
      st.variable = self:parseVariable()
      self:consumeToken("=")
      st.expression = self:parseNumericExpression()
    elseif kw == "go" then
      local nxt = self:nextToken()
      if nxt.string == "to" then self:parseGoto(st)
      elseif nxt.string == "sub" then self:parseGosub(st)
      else error("At " .. tostring(nxt.sourceLineNumber) .. ": expected to or sub but got: " .. tostring(nxt.string)) end
    elseif kw == "goto" then self:parseGoto(st)
    elseif kw == "gosub" then self:parseGosub(st)
    elseif kw == "if" then
      st.proc = s_if
      st.condition = self:parseRelationalExpression()
      self:consumeToken("then")
      st.target = self:parseNonNegativeInteger()
    elseif kw == "return" then st.proc = s_return
    elseif kw == "stop" then st.proc = s_stop
    elseif kw == "on" then
      st.proc = s_on
      st.expression = self:parseNumericExpression()
      if self:peekToken().string == "go" then self:consumeToken("go"); self:consumeToken("to") else self:consumeToken("goto") end
      st.targets = {}
      while true do
        st.targets[#st.targets + 1] = self:parseNonNegativeInteger()
        if self:peekToken().string ~= "," then break end
        self:nextToken()
      end
    elseif kw == "for" then
      st.proc = s_for
      st.variable = self:consumeKind("identifier").string
      self:consumeToken("=")
      st.initial = self:parseNumericExpression()
      self:consumeToken("to")
      st.limit = self:parseNumericExpression()
      if self:peekToken().string == "step" then self:nextToken(); st.step = self:parseNumericExpression()
      else st.step = {ev = e_const, value = 1} end
      self:consumeKind("newLine")
      local last = self:parseStatements()
      if last.proc ~= s_next then error("At " .. tostring(last.sourceLineNumber) .. ": expected next statement") end
      if last.variable ~= st.variable then error("At " .. tostring(last.sourceLineNumber) .. ": expected next for " .. st.variable .. " but got " .. tostring(last.variable)) end
      last.target = st
      st.target = last
      return st
    elseif kw == "next" then
      st.proc = s_next
      st.variable = self:consumeKind("identifier").string
    elseif kw == "print" then
      st.proc = s_print
      st.items = {}
      while true do
        local s = self:peekToken().string
        if s == "," then self:nextToken(); st.items[#st.items + 1] = {kind = "comma"}
        elseif s == ";" then self:nextToken()
        elseif s == "tab" then self:nextToken(); self:consumeToken("("); st.items[#st.items + 1] = {kind = "tab", value = self:parseNumericExpression()}
        elseif s == "\n" then break
        else
          if self:isStringExpression() then st.items[#st.items + 1] = {kind = "string", value = self:parseStringExpression()}
          else st.items[#st.items + 1] = {kind = "number", value = self:parseNumericExpression()} end
        end
      end
    elseif kw == "input" then
      st.proc = s_input
      st.items = {}
      while true do
        st.items[#st.items + 1] = self:parseVariable()
        if self:peekToken().string ~= "," then break end
        self:nextToken()
      end
    elseif kw == "read" then
      st.proc = s_read
      st.items = {}
      while true do
        st.items[#st.items + 1] = self:parseVariable()
        if self:peekToken().string ~= "," then break end
        self:nextToken()
      end
    elseif kw == "restore" then st.proc = s_restore
    elseif kw == "data" then
      while true do
        self.program.data[#self.program.data + 1] = self:parseConstant()
        if self:peekToken().string ~= "," then break end
        self:nextToken()
      end
    elseif kw == "dim" then
      st.proc = s_dim
      st.items = {}
      while true do
        local name = self:consumeKind("identifier").string
        self:consumeToken("(")
        local bounds = {self:parseNonNegativeInteger()}
        if self:peekToken().string == "," then self:nextToken(); bounds[#bounds + 1] = self:parseNonNegativeInteger() end
        self:consumeToken(")")
        st.items[#st.items + 1] = {name = name, bounds = bounds}
        if self:peekToken().string ~= "," then break end
        self:consumeToken(",")
      end
    elseif kw == "option" then
      self:consumeToken("base")
      local base = self:parseNonNegativeInteger()
      if base ~= 0 and base ~= 1 then error("At " .. tostring(command.sourceLineNumber) .. ": unexpected base: " .. tostring(base)) end
      self.program.base = base
    elseif kw == "randomize" then st.proc = s_randomize
    elseif kw == "end" then st.proc = s_end
    else error("At " .. tostring(command.sourceLineNumber) .. ": unexpected command but got: " .. tostring(command.string)) end
  elseif command.kind == "remark" then
    -- ignore
  else
    error("At " .. tostring(command.sourceLineNumber) .. ": expected command but got: " .. tostring(command.string) .. " (of kind " .. tostring(command.kind) .. ")")
  end
  self:consumeKind("newLine")
  return st
end
function Parser:parseStatements()
  local st
  repeat
    st = self:parseStatement()
  until st.proc ~= nil and BLOCK_END[st.proc]
  return st
end
function Parser:makeProgram()
  self.program = {process = program_run, statements = {}, data = {}, base = 0}
  local last = self:parseStatements()
  if last.proc ~= s_end then error("At " .. tostring(last.sourceLineNumber) .. ": expected end") end
  return self.program
end

-- ---------------- glue ----------------
local function prepare(source)
  local parser = Parser.new(lex(source))
  local program = parser:makeProgram()
  return program, State.new(program)
end

local function simulate(program, state, inputs)
  local out = {}
  local co = coroutine.create(program_run)
  local ok, event = coroutine.resume(co, program, state)
  while true do
    if not ok then error(event) end
    if event == nil then break end
    if event.kind == "output" then
      out[#out + 1] = event.string
      out[#out + 1] = "\n"
      ok, event = coroutine.resume(co)
    elseif event.kind == "input" then
      local args = {}
      for i = 1, event.numItems do args[i] = table.remove(inputs, 1) end
      ok, event = coroutine.resume(co, args)
    else
      error("Unknown event kind")
    end
  end
  return table.concat(out)
end

local function run_iteration(cases)
  for _, case in ipairs(cases) do
    local program, state = prepare(case.src)
    local result = simulate(program, state, {})
    if result ~= case.expected then
      error("Program produced unexpected output")
    end
  end
end

local cases = (function()
-- golden BASIC programs and their expected output (generated from the reference)
return {
  { src = [[10 print "hello, world!"
20 end]], expected = [[hello, world!
]] },
  { src = [[10 let x = 0
20 let x = x + 1
30 print x
40 if x < 10 then 20
50 end]], expected = [[1
2
3
4
5
6
7
8
9
10
]] },
  { src = [[10 print int(rnd * 100)
20 end
]], expected = [[98
]] },
  { src = [[10 let value = int(rnd * 2000)
20 print value
30 if value <> 100 then 10
40 end]], expected = [[1974
697
1126
1998
1658
264
1650
1677
226
117
492
861
877
1969
38
1039
197
1261
1102
1522
916
1683
1943
1835
476
1898
939
176
966
908
474
614
1326
564
1916
728
524
162
1303
758
832
1279
1856
1876
982
6
1613
1781
681
1238
494
1583
1953
788
1026
347
1116
1465
514
583
463
1970
1573
412
1256
1453
838
1538
1984
1598
209
411
1700
546
861
91
132
884
378
693
11
433
1719
860
164
472
231
1786
806
811
106
1697
118
980
890
1199
227
1667
1933
1903
1390
1595
923
1746
39
1361
117
1297
923
901
1180
818
1444
269
933
327
1744
1082
1527
1260
622
528
318
856
296
1796
1574
585
1871
111
827
1725
1320
1868
1695
1914
216
63
1847
156
671
893
127
1867
811
279
913
310
814
907
1363
1624
1670
478
714
436
355
1484
1628
1208
800
611
917
829
830
273
1791
340
214
992
1444
442
1555
144
1194
282
180
1228
1251
1883
678
1555
347
72
1661
1828
1090
1183
957
1685
930
475
103
759
1725
1902
1662
1587
61
614
863
1418
321
1050
505
1622
1425
803
589
1511
1098
1051
1554
1898
27
747
813
1544
332
728
1363
771
759
1145
1098
1991
385
230
520
1369
1840
1285
1562
1845
102
760
1874
748
361
575
277
1661
1764
1117
332
757
1766
1722
143
474
1507
1294
1180
1578
904
845
321
496
1911
1784
1116
938
1591
1403
1374
533
1085
452
708
1096
1634
522
564
1397
1357
980
978
1760
1088
1361
1184
314
1242
217
133
1187
1723
646
605
591
46
135
1420
1821
1147
1211
61
244
1307
1551
449
1122
1336
140
880
22
1155
1326
590
1499
1376
112
1771
1897
1071
938
1685
1963
1203
1296
804
1275
453
1387
482
1262
1883
1381
418
1417
1222
1208
1263
632
450
1422
1285
1408
644
665
275
363
1012
165
354
80
609
291
1661
1724
117
407
59
906
1224
136
855
1275
1468
482
1537
1283
1784
1568
1832
452
867
1546
1467
800
45
1225
1890
465
1372
47
1608
193
1345
1847
1059
1788
518
52
1052
1003
1210
1135
1433
519
1558
39
1249
1017
39
1713
1449
1245
1354
82
1140
916
1595
838
607
389
1270
821
247
1692
1305
1211
1960
429
1703
1635
575
1618
1490
1495
682
1256
964
420
1520
1429
1997
396
382
856
1182
296
1295
298
1892
990
711
934
1939
1339
682
1631
1533
742
1520
1281
1332
1042
656
1576
1253
1608
375
169
14
414
1586
1562
1508
1245
303
715
1053
340
915
160
1796
111
925
1872
735
350
107
1913
1653
987
825
1893
1601
460
1228
1526
1613
1359
1854
1352
542
665
109
1874
467
533
1188
1629
851
630
1060
1530
1853
743
765
126
1540
1411
858
1741
284
299
577
1848
1495
283
1886
284
129
1077
1245
1364
1505
176
1012
1663
1306
1586
410
315
660
256
1102
1289
1292
939
762
601
1140
574
1851
44
560
1948
1142
1787
947
948
280
1210
1139
1072
1033
92
1244
1589
1079
22
1514
163
157
1742
1058
514
196
1858
565
354
1413
792
183
526
1724
1007
158
1229
1802
99
1514
708
1276
1802
1564
1387
1235
1132
715
1584
617
1664
1559
1625
1037
601
1175
1713
107
88
384
1634
904
1835
1472
212
1145
443
1617
866
1963
937
1917
855
1215
1867
520
892
1483
1898
1747
1441
289
1609
328
566
271
458
1616
843
1107
507
1090
854
1094
806
166
408
661
334
230
1917
1323
927
1912
673
311
952
1783
1549
1714
1500
450
1498
530
442
607
609
1226
370
1769
1815
788
536
293
115
947
290
1764
243
1219
1851
289
599
1528
150
1859
297
279
1542
1719
1910
551
401
952
1764
946
1835
647
1309
271
275
70
129
1518
972
1164
816
1125
575
588
1456
1154
290
1681
1133
561
343
1360
1035
1158
1365
744
781
58
531
271
1612
1774
28
1480
1312
1855
666
1574
613
42
456
351
727
1503
1115
333
1972
822
1575
848
1087
1262
1671
710
460
1816
287
172
492
1079
582
1236
1756
1792
1095
1205
1894
22
1930
1529
1547
1383
1768
364
1108
1972
287
200
230
1335
187
486
1722
20
963
792
1114
633
1862
1433
829
737
215
1570
378
1677
944
1301
1160
500
150
886
1337
662
1062
290
460
592
1867
872
155
1613
1913
1548
1847
855
1702
952
1894
587
1813
1021
21
654
254
910
1696
1606
679
1222
696
1319
368
447
549
905
1194
189
1766
616
278
1418
1965
872
998
1268
1673
1647
1163
533
1650
1849
1124
1252
1412
703
944
468
1485
1352
681
864
1432
1771
497
956
1794
363
1099
1804
457
1227
1487
446
1993
1576
272
709
1810
330
876
1107
1187
122
1625
472
676
314
1257
1509
350
741
366
33
536
293
1663
1039
1527
126
923
1937
1767
1302
1510
1518
1343
91
1551
1614
1687
1748
137
75
738
1977
751
237
313
566
24
202
889
1716
1460
129
1760
1597
96
1057
1323
1188
1373
537
955
65
1679
1441
1315
398
647
1470
1335
617
331
796
129
1635
1497
836
855
1472
1828
568
862
690
1370
1657
819
45
420
258
1980
672
615
358
852
1148
1897
1306
1092
1405
719
1752
1456
1338
332
351
479
747
249
1977
1671
1061
1685
306
254
1060
764
420
1139
1452
426
835
929
1424
1336
697
191
1697
1897
644
546
982
359
1201
1095
1623
1947
215
10
855
297
551
1037
945
396
211
1059
423
1521
1770
203
1828
879
1179
1912
1028
1416
1845
698
715
1857
817
50
473
1122
126
70
1773
40
1970
1311
826
355
1921
23
526
1717
1397
1932
1075
1652
997
1039
1481
779
415
49
1330
317
1701
690
245
1824
639
799
1240
422
344
1639
20
546
912
1930
1368
1541
1109
369
66
1564
444
1928
1963
1899
744
1593
1702
100
]] },
  { src = [[10 dim a(2000)
20 for i = 2 to 2000
30 let a(i) = 1
40 next i
50 for i = 2 to sqr(2000)
60 if a(i) = 0 then 100
70 for j = i ^ 2 to 2000 step i
80 let a(j) = 0
90 next j
100 next i
110 for i = 2 to 2000
120 if a(i) = 0 then 140
130 print i
140 next i
150 end
]], expected = [[2
3
5
7
11
13
17
19
23
29
31
37
41
43
47
53
59
61
67
71
73
79
83
89
97
101
103
107
109
113
127
131
137
139
149
151
157
163
167
173
179
181
191
193
197
199
211
223
227
229
233
239
241
251
257
263
269
271
277
281
283
293
307
311
313
317
331
337
347
349
353
359
367
373
379
383
389
397
401
409
419
421
431
433
439
443
449
457
461
463
467
479
487
491
499
503
509
521
523
541
547
557
563
569
571
577
587
593
599
601
607
613
617
619
631
641
643
647
653
659
661
673
677
683
691
701
709
719
727
733
739
743
751
757
761
769
773
787
797
809
811
821
823
827
829
839
853
857
859
863
877
881
883
887
907
911
919
929
937
941
947
953
967
971
977
983
991
997
1009
1013
1019
1021
1031
1033
1039
1049
1051
1061
1063
1069
1087
1091
1093
1097
1103
1109
1117
1123
1129
1151
1153
1163
1171
1181
1187
1193
1201
1213
1217
1223
1229
1231
1237
1249
1259
1277
1279
1283
1289
1291
1297
1301
1303
1307
1319
1321
1327
1361
1367
1373
1381
1399
1409
1423
1427
1429
1433
1439
1447
1451
1453
1459
1471
1481
1483
1487
1489
1493
1499
1511
1523
1531
1543
1549
1553
1559
1567
1571
1579
1583
1597
1601
1607
1609
1613
1619
1621
1627
1637
1657
1663
1667
1669
1693
1697
1699
1709
1721
1723
1733
1741
1747
1753
1759
1777
1783
1787
1789
1801
1811
1823
1831
1847
1861
1867
1871
1873
1877
1879
1889
1901
1907
1913
1931
1933
1949
1951
1973
1979
1987
1993
1997
1999
]] },
}

end)()

local function main()
  local iterations = 200
  for _ = 1, iterations do run_iteration(cases) end
  print("ok")
end

main()
