-- nbody: 3D gravitational N-body simulation. 5 bodies (sun + 4 outer
-- planets), Euler integration. Stresses float math (mul/div/sqrt) and
-- table mutation. Each body is a 7-field table indexed positionally.
--
-- Derived from the AWFY corpus (Marr et al.); flattened to no-metatable
-- positional tables to match how richards/deltablue are written here.

local ITERS = 250000
local EXPECTED = -0.1690859889909308

-- --- Body fields: 7-field table --------------------------------------
local B_X, B_Y, B_Z, B_VX, B_VY, B_VZ, B_MASS = 1, 2, 3, 4, 5, 6, 7

local PI            = 3.141592653589793
local SOLAR_MASS    = 4.0 * PI * PI
local DAYS_PER_YEAR = 365.24

local function make_body(x, y, z, vx, vy, vz, mass)
  return { x, y, z,
           vx * DAYS_PER_YEAR,
           vy * DAYS_PER_YEAR,
           vz * DAYS_PER_YEAR,
           mass * SOLAR_MASS }
end

local function sun()     return make_body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0) end
local function jupiter() return make_body( 4.84143144246472090e+00,
                                          -1.16032004402742839e+00,
                                          -1.03622044471123109e-01,
                                           1.66007664274403694e-03,
                                           7.69901118419740425e-03,
                                          -6.90460016972063023e-05,
                                           9.54791938424326609e-04) end
local function saturn()  return make_body( 8.34336671824457987e+00,
                                           4.12479856412430479e+00,
                                          -4.03523417114321381e-01,
                                          -2.76742510726862411e-03,
                                           4.99852801234917238e-03,
                                           2.30417297573763929e-05,
                                           2.85885980666130812e-04) end
local function uranus()  return make_body( 1.28943695621391310e+01,
                                          -1.51111514016986312e+01,
                                          -2.23307578892655734e-01,
                                           2.96460137564761618e-03,
                                           2.37847173959480950e-03,
                                          -2.96589568540237556e-05,
                                           4.36624404335156298e-05) end
local function neptune() return make_body( 1.53796971148509165e+01,
                                          -2.59193146099879641e+01,
                                           1.79258772950371181e-01,
                                           2.68067772490389322e-03,
                                           1.62824170038242295e-03,
                                          -9.51592254519715870e-05,
                                           5.15138902046611451e-05) end

local BODIES = { sun(), jupiter(), saturn(), uranus(), neptune() }
local NBODIES = #BODIES

local function offset_momentum()
  local px, py, pz = 0.0, 0.0, 0.0
  for i = 1, NBODIES do
    local b = BODIES[i]
    local m = b[B_MASS]
    px = px + b[B_VX] * m
    py = py + b[B_VY] * m
    pz = pz + b[B_VZ] * m
  end
  local s = BODIES[1]
  s[B_VX] = -(px / SOLAR_MASS)
  s[B_VY] = -(py / SOLAR_MASS)
  s[B_VZ] = -(pz / SOLAR_MASS)
end

local function advance(dt)
  for i = 1, NBODIES do
    local bi = BODIES[i]
    for j = i + 1, NBODIES do
      local bj = BODIES[j]
      local dx = bi[B_X] - bj[B_X]
      local dy = bi[B_Y] - bj[B_Y]
      local dz = bi[B_Z] - bj[B_Z]
      local d2 = dx * dx + dy * dy + dz * dz
      local dist = math.sqrt(d2)
      local mag = dt / (d2 * dist)
      local mi = bi[B_MASS]
      local mj = bj[B_MASS]
      bi[B_VX] = bi[B_VX] - dx * mj * mag
      bi[B_VY] = bi[B_VY] - dy * mj * mag
      bi[B_VZ] = bi[B_VZ] - dz * mj * mag
      bj[B_VX] = bj[B_VX] + dx * mi * mag
      bj[B_VY] = bj[B_VY] + dy * mi * mag
      bj[B_VZ] = bj[B_VZ] + dz * mi * mag
    end
  end
  for i = 1, NBODIES do
    local b = BODIES[i]
    b[B_X] = b[B_X] + dt * b[B_VX]
    b[B_Y] = b[B_Y] + dt * b[B_VY]
    b[B_Z] = b[B_Z] + dt * b[B_VZ]
  end
end

local function energy()
  local e = 0.0
  for i = 1, NBODIES do
    local bi = BODIES[i]
    local mi = bi[B_MASS]
    local vx = bi[B_VX]
    local vy = bi[B_VY]
    local vz = bi[B_VZ]
    e = e + 0.5 * mi * (vx * vx + vy * vy + vz * vz)
    for j = i + 1, NBODIES do
      local bj = BODIES[j]
      local dx = bi[B_X] - bj[B_X]
      local dy = bi[B_Y] - bj[B_Y]
      local dz = bi[B_Z] - bj[B_Z]
      local d = math.sqrt(dx * dx + dy * dy + dz * dz)
      e = e - (mi * bj[B_MASS]) / d
    end
  end
  return e
end

offset_momentum()
for _ = 1, ITERS do advance(0.01) end

local e = energy()
if math.abs(e - EXPECTED) < 1e-13 then
  print("ok")
else
  print("bad energy: " .. e)
  os.exit(1)
end
