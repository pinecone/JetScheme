-- richards: OS kernel scheduling simulation. Five tasks (idler, worker,
-- two handlers, two devices) driven by a packet-passing scheduler.
-- Stresses allocation, linked-list mutation, and indirect dispatch.
-- A correct run produces queueCount = 2322 and holdCount = 928.

local ID_IDLE       = 0
local ID_WORKER     = 1
local ID_HANDLER_A  = 2
local ID_HANDLER_B  = 3
local ID_DEVICE_A   = 4
local ID_DEVICE_B   = 5
local NUMBER_OF_IDS = 6

local KIND_DEVICE = 0
local KIND_WORK   = 1

local DATA_SIZE = 4
local COUNT     = 100000

local band, bor, bnot, rshift, bxor
do
  local ok, bitlib = pcall(require, "bit")   -- LuaJIT / LuaBitOp
  if ok then
    band, bor, bnot, rshift, bxor =
      bitlib.band, bitlib.bor, bitlib.bnot, bitlib.rshift, bitlib.bxor
  else                                        -- Lua 5.3+: native operators
    band   = load("return function(a, b) return a & b end")()
    bor    = load("return function(a, b) return a | b end")()
    bnot   = load("return function(a) return ~a end")()
    rshift = load("return function(a, n) return a >> n end")()
    bxor   = load("return function(a, b) return a ~ b end")()
  end
end

local STATE_RUNNING            = 0
local STATE_RUNNABLE           = 1
local STATE_SUSPENDED          = 2
local STATE_HELD               = 4
local STATE_SUSPENDED_RUNNABLE = 3
local STATE_NOT_HELD           = bnot(STATE_HELD)

local EXPECTED_QUEUE_COUNT = 232625
local EXPECTED_HOLD_COUNT  = 93050

local queueCount  = 0
local holdCount   = 0
local blocks      = {}
local list_head   = nil
local currentTcb  = nil
local currentId   = nil

local sched_release, sched_hold_current, sched_suspend_current, sched_queue
local task_run

-- ---- Packet ----
local function pkt_new(link, id, kind)
  return { link = link, id = id, kind = kind, a1 = 0, a2 = { 0, 0, 0, 0 } }
end

local function pkt_addto(packet, queue)
  packet.link = nil
  if queue == nil then return packet end
  local next = queue
  local peek = next.link
  while peek ~= nil do
    next = peek
    peek = next.link
  end
  next.link = packet
  return queue
end

-- ---- TCB ----
local function tcb_new(link, id, priority, queue, task)
  local state
  if queue == nil then state = STATE_SUSPENDED
  else state = STATE_SUSPENDED_RUNNABLE end
  return {
    link = link, id = id, priority = priority,
    queue = queue, task = task, state = state,
  }
end

local function tcb_held_or_suspended(t)
  return band(t.state, STATE_HELD) ~= 0 or t.state == STATE_SUSPENDED
end

local function tcb_check_priority_add(t, task, packet)
  if t.queue == nil then
    t.queue = packet
    t.state = bor(t.state, STATE_RUNNABLE)
    if t.priority > task.priority then return t end
  else
    t.queue = pkt_addto(packet, t.queue)
  end
  return task
end

local function tcb_run(t)
  local packet
  if t.state == STATE_SUSPENDED_RUNNABLE then
    packet = t.queue
    t.queue = packet.link
    if t.queue == nil then t.state = STATE_RUNNING
    else t.state = STATE_RUNNABLE end
  else
    packet = nil
  end
  return task_run(t.task, packet)
end

-- ---- Scheduler ops ----
local function add_task(id, priority, queue, task)
  local tcb = tcb_new(list_head, id, priority, queue, task)
  currentTcb = tcb
  list_head = tcb
  blocks[id] = tcb
end

local function add_running_task(id, priority, queue, task)
  add_task(id, priority, queue, task)
  currentTcb.state = STATE_RUNNING
end

sched_release = function(id)
  local tcb = blocks[id]
  if tcb == nil then return tcb end
  tcb.state = band(tcb.state, STATE_NOT_HELD)
  if tcb.priority > currentTcb.priority then return tcb end
  return currentTcb
end

sched_hold_current = function()
  holdCount = holdCount + 1
  currentTcb.state = bor(currentTcb.state, STATE_HELD)
  return currentTcb.link
end

sched_suspend_current = function()
  currentTcb.state = bor(currentTcb.state, STATE_SUSPENDED)
  return currentTcb
end

sched_queue = function(packet)
  local t = blocks[packet.id]
  if t == nil then return t end
  queueCount = queueCount + 1
  packet.link = nil
  packet.id = currentId
  return tcb_check_priority_add(t, currentTcb, packet)
end

-- ---- Tasks ----
local function idle_run(t, packet)
  t.count = t.count - 1
  if t.count == 0 then return sched_hold_current() end
  if band(t.v1, 1) == 0 then
    t.v1 = rshift(t.v1, 1)
    return sched_release(ID_DEVICE_A)
  else
    t.v1 = bxor(rshift(t.v1, 1), 0xD008)
    return sched_release(ID_DEVICE_B)
  end
end

local function device_run(t, packet)
  if packet == nil then
    if t.v1 == nil then return sched_suspend_current() end
    local v = t.v1
    t.v1 = nil
    return sched_queue(v)
  else
    t.v1 = packet
    return sched_hold_current()
  end
end

local function worker_run(t, packet)
  if packet == nil then return sched_suspend_current() end
  if t.v1 == ID_HANDLER_A then t.v1 = ID_HANDLER_B
  else t.v1 = ID_HANDLER_A end
  packet.id = t.v1
  packet.a1 = 0
  for i = 0, DATA_SIZE - 1 do
    t.v2 = t.v2 + 1
    if t.v2 > 26 then t.v2 = 1 end
    packet.a2[i + 1] = t.v2
  end
  return sched_queue(packet)
end

local function handler_run(t, packet)
  if packet ~= nil then
    if packet.kind == KIND_WORK then t.v1 = pkt_addto(packet, t.v1)
    else t.v2 = pkt_addto(packet, t.v2) end
  end
  if t.v1 ~= nil then
    local count = t.v1.a1
    if count < DATA_SIZE then
      if t.v2 ~= nil then
        local v = t.v2
        t.v2 = t.v2.link
        v.a1 = t.v1.a2[count + 1]
        t.v1.a1 = count + 1
        return sched_queue(v)
      end
    else
      local v = t.v1
      t.v1 = t.v1.link
      return sched_queue(v)
    end
  end
  return sched_suspend_current()
end

task_run = function(task, packet)
  return task.run(task, packet)
end

-- ---- Build helpers ----
local function add_idle_task(id, priority, queue, count)
  add_running_task(id, priority, queue,
    { run = idle_run, v1 = 1, count = count })
end
local function add_worker_task(id, priority, queue)
  add_task(id, priority, queue,
    { run = worker_run, v1 = ID_HANDLER_A, v2 = 0 })
end
local function add_handler_task(id, priority, queue)
  add_task(id, priority, queue,
    { run = handler_run, v1 = nil, v2 = nil })
end
local function add_device_task(id, priority, queue)
  add_task(id, priority, queue,
    { run = device_run, v1 = nil })
end

local function schedule()
  currentTcb = list_head
  while currentTcb ~= nil do
    if tcb_held_or_suspended(currentTcb) then
      currentTcb = currentTcb.link
    else
      currentId = currentTcb.id
      currentTcb = tcb_run(currentTcb)
    end
  end
end

-- ---- Run ----
add_idle_task(ID_IDLE, 0, nil, COUNT)

local q
q = pkt_new(nil, ID_WORKER, KIND_WORK)
q = pkt_new(q,   ID_WORKER, KIND_WORK)
add_worker_task(ID_WORKER, 1000, q)

q = pkt_new(nil, ID_DEVICE_A, KIND_DEVICE)
q = pkt_new(q,   ID_DEVICE_A, KIND_DEVICE)
q = pkt_new(q,   ID_DEVICE_A, KIND_DEVICE)
add_handler_task(ID_HANDLER_A, 2000, q)

q = pkt_new(nil, ID_DEVICE_B, KIND_DEVICE)
q = pkt_new(q,   ID_DEVICE_B, KIND_DEVICE)
q = pkt_new(q,   ID_DEVICE_B, KIND_DEVICE)
add_handler_task(ID_HANDLER_B, 3000, q)

add_device_task(ID_DEVICE_A, 4000, nil)
add_device_task(ID_DEVICE_B, 5000, nil)

schedule()

if queueCount ~= EXPECTED_QUEUE_COUNT or holdCount ~= EXPECTED_HOLD_COUNT then
  error("bad: queueCount=" .. queueCount .. " holdCount=" .. holdCount)
end

print(queueCount + holdCount)
