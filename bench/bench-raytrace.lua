-- raytrace: 3D ray tracer (Burmister/Octane). Polymorphic shapes and
-- materials, vector math, reflection/shadow/highlight recursion.
-- Stresses float math, allocation, and dispatch. A correct run prints 2321.

local check_number = 0

local color = {}
color.__index = color

local function new_color(r, g, b)
    return setmetatable({red = r or 0, green = g or 0, blue = b or 0}, color)
end

function color.add(c1, c2)
    return new_color(c1.red + c2.red, c1.green + c2.green, c1.blue + c2.blue)
end

function color.add_scalar(c1, s)
    local r = color.limit_value(c1.red + s)
    local g = color.limit_value(c1.green + s)
    local b = color.limit_value(c1.blue + s)
    return new_color(r, g, b)
end

function color.subtract(c1, c2)
    return new_color(c1.red - c2.red, c1.green - c2.green, c1.blue - c2.blue)
end

function color.multiply(c1, c2)
    return new_color(c1.red * c2.red, c1.green * c2.green, c1.blue * c2.blue)
end

function color.multiply_scalar(c1, f)
    return new_color(c1.red * f, c1.green * f, c1.blue * f)
end

function color.divide_factor(c1, f)
    return new_color(c1.red / f, c1.green / f, c1.blue / f)
end

function color.limit_value(v)
    if v > 1.0 then return 1.0 end
    if v < 0.0 then return 0.0 end
    return v
end

function color:limit()
    self.red = color.limit_value(self.red)
    self.green = color.limit_value(self.green)
    self.blue = color.limit_value(self.blue)
end

function color:distance(other)
    return math.abs(self.red - other.red) + math.abs(self.green - other.green)
                + math.abs(self.blue - other.blue)
end

function color.blend(c1, c2, w)
    return color.add(color.multiply_scalar(c1, 1 - w), color.multiply_scalar(c2, w))
end

function color:brightness()
    local r = math.floor(self.red * 255)
    local g = math.floor(self.green * 255)
    local b = math.floor(self.blue * 255)
    return math.floor((r * 77 + g * 150 + b * 29) / 256)
end

local vector = {}
vector.__index = vector

local function new_vector(x, y, z)
    return setmetatable({x = x or 0, y = y or 0, z = z or 0}, vector)
end

function vector.dot(a, b)
    return a.x * b.x + a.y * b.y + a.z * b.z
end

function vector:magnitude()
    return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z)
end

function vector:normalize()
    local m = self:magnitude()
    return new_vector(self.x / m, self.y / m, self.z / m)
end

function vector:cross(w)
    return new_vector(-self.z * w.y + self.y * w.z,
                       self.z * w.x - self.x * w.z,
                      -self.y * w.x + self.x * w.y)
end

function vector.add(v, w)
    return new_vector(v.x + w.x, v.y + w.y, v.z + w.z)
end

function vector.subtract(v, w)
    return new_vector(v.x - w.x, v.y - w.y, v.z - w.z)
end

function vector.multiply_scalar(v, w)
    return new_vector(v.x * w, v.y * w, v.z * w)
end

function vector.multiply_vector(v, w)
    return new_vector(v.x * w.x, v.y * w.y, v.z * w.z)
end

local function new_ray(position, direction)
    return {position = position, direction = direction}
end

local function new_light(position, col, intensity)
    return {position = position, color = col, intensity = intensity or 10.0}
end

local function new_background(col, ambience)
    return {color = col, ambience = ambience}
end

local function new_intersection_info()
    return {
        isHit = false,
        hitCount = 0,
        shape = nil,
        position = nil,
        normal = nil,
        color = new_color(0, 0, 0),
        distance = nil,
    }
end

local function wrap_up(t)
    t = t % 2.0
    if t < -1 then t = t + 2.0 end
    if t >= 1 then t = t - 2.0 end
    return t
end

local solid = {}
solid.__index = solid

local function new_solid_material(col, reflection, refraction, transparency, gloss)
    return setmetatable({
        color = col,
        reflection = reflection,
        transparency = transparency,
        gloss = gloss,
        hasTexture = false,
    }, solid)
end

function solid:getColor(u, v)
    return self.color
end

local chessboard = {}
chessboard.__index = chessboard

local function new_chessboard_material(colorEven, colorOdd, reflection, transparency, gloss, density)
    return setmetatable({
        colorEven = colorEven,
        colorOdd = colorOdd,
        reflection = reflection,
        transparency = transparency,
        gloss = gloss,
        density = density,
        hasTexture = true,
    }, chessboard)
end

function chessboard:getColor(u, v)
    local t = wrap_up(u * self.density) * wrap_up(v * self.density)
    if t < 0.0 then
        return self.colorEven
    end
    return self.colorOdd
end

local sphere = {}
sphere.__index = sphere

local function new_sphere(position, radius, material)
    return setmetatable({
        radius = radius,
        position = position,
        material = material,
    }, sphere)
end

function sphere:intersect(ray)
    local info = new_intersection_info()
    info.shape = self
    local dst = vector.subtract(ray.position, self.position)
    local B = vector.dot(dst, ray.direction)
    local C = vector.dot(dst, dst) - (self.radius * self.radius)
    local D = (B * B) - C
    if D > 0 then
        info.isHit = true
        info.distance = (-B) - math.sqrt(D)
        info.position = vector.add(ray.position,
                                   vector.multiply_scalar(ray.direction, info.distance))
        info.normal = vector.subtract(info.position, self.position):normalize()
        info.color = self.material:getColor(0, 0)
    else
        info.isHit = false
    end
    return info
end

local plane = {}
plane.__index = plane

local function new_plane(position, d, material)
    return setmetatable({
        position = position,
        d = d,
        material = material,
    }, plane)
end

function plane:intersect(ray)
    local info = new_intersection_info()
    local Vd = vector.dot(self.position, ray.direction)
    if Vd == 0 then return info end
    local t = -(vector.dot(self.position, ray.position) + self.d) / Vd
    if t <= 0 then return info end
    info.shape = self
    info.isHit = true
    info.position = vector.add(ray.position,
                               vector.multiply_scalar(ray.direction, t))
    info.normal = self.position
    info.distance = t
    info.color = self.material:getColor(0, 0)
    if self.material.hasTexture then
        local vU = new_vector(self.position.y, self.position.z, -self.position.x)
        local vV = vU:cross(self.position)
        local u = vector.dot(info.position, vU)
        local v = vector.dot(info.position, vV)
        info.color = self.material:getColor(u, v)
    end
    return info
end

local camera = {}
camera.__index = camera

local function new_camera(position, lookAt, up)
    local equator = lookAt:normalize():cross(up)
    local screen = vector.add(position, lookAt)
    return setmetatable({
        position = position,
        lookAt = lookAt,
        up = up,
        equator = equator,
        screen = screen,
    }, camera)
end

function camera:getRay(vx, vy)
    local pos = vector.subtract(self.screen,
                  vector.subtract(vector.multiply_scalar(self.equator, vx),
                                  vector.multiply_scalar(self.up, vy)))
    pos.y = pos.y * -1
    local dir = vector.subtract(pos, self.position)
    return new_ray(pos, dir:normalize())
end

local function new_scene()
    return {
        camera = new_camera(new_vector(0, 0, -5), new_vector(0, 0, 1), new_vector(0, 1, 0)),
        shapes = {},
        lights = {},
        background = new_background(new_color(0, 0, 0.5), 0.2),
    }
end

local engine = {}
engine.__index = engine

local function new_engine(options)
    options = options or {}
    local o = {
        canvasHeight = (options.canvasHeight or 100) / (options.pixelHeight or 2),
        canvasWidth = (options.canvasWidth or 100) / (options.pixelWidth or 2),
        pixelWidth = options.pixelWidth or 2,
        pixelHeight = options.pixelHeight or 2,
        renderDiffuse = options.renderDiffuse or false,
        renderShadows = options.renderShadows or false,
        renderHighlights = options.renderHighlights or false,
        renderReflections = options.renderReflections or false,
        rayDepth = options.rayDepth or 2,
        canvas = nil,
    }
    return setmetatable(o, engine)
end

function engine:setPixel(x, y, col)
    if self.canvas then
        self.canvas.fillStyle = tostring(col)
    else
        if x == y then
            check_number = check_number + col:brightness()
        end
    end
end

function engine:testIntersection(ray, scene, exclude)
    local hits = 0
    local best = new_intersection_info()
    best.distance = 2000
    for i = 1, #scene.shapes do
        local shape = scene.shapes[i]
        if shape ~= exclude then
            local info = shape:intersect(ray)
            if info.isHit and info.distance >= 0 and info.distance < best.distance then
                best = info
                hits = hits + 1
            end
        end
    end
    best.hitCount = hits
    return best
end

function engine:getReflectionRay(P, N, V)
    local c1 = -vector.dot(N, V)
    local R1 = vector.add(vector.multiply_scalar(N, 2 * c1), V)
    return new_ray(P, R1)
end

function engine:rayTrace(info, ray, scene, depth)
    local col = color.multiply_scalar(info.color, scene.background.ambience)
    local shininess = 10 ^ (info.shape.material.gloss + 1)

    for i = 1, #scene.lights do
        local light = scene.lights[i]
        local v = vector.subtract(light.position, info.position):normalize()

        if self.renderDiffuse then
            local L = vector.dot(v, info.normal)
            if L > 0.0 then
                col = color.add(col,
                       color.multiply(info.color,
                         color.multiply_scalar(light.color, L)))
            end
        end

        if depth <= self.rayDepth then
            if self.renderReflections and info.shape.material.reflection > 0 then
                local reflectionRay = self:getReflectionRay(info.position, info.normal, ray.direction)
                local refl = self:testIntersection(reflectionRay, scene, info.shape)
                if refl.isHit and refl.distance > 0 then
                    refl.color = self:rayTrace(refl, reflectionRay, scene, depth + 1)
                else
                    refl.color = scene.background.color
                end
                col = color.blend(col, refl.color, info.shape.material.reflection)
            end
        end

        local shadowInfo = new_intersection_info()
        if self.renderShadows then
            local shadowRay = new_ray(info.position, v)
            shadowInfo = self:testIntersection(shadowRay, scene, info.shape)
            if shadowInfo.isHit and shadowInfo.shape ~= info.shape then
                local vA = color.multiply_scalar(col, 0.5)
                local dB = 0.5 * (shadowInfo.shape.material.transparency ^ 0.5)
                col = color.add_scalar(vA, dB)
            end
        end

        if self.renderHighlights and not shadowInfo.isHit and info.shape.material.gloss > 0 then
            local Lv = vector.subtract(info.shape.position, light.position):normalize()
            local E = vector.subtract(scene.camera.position, info.shape.position):normalize()
            local H = vector.subtract(E, Lv):normalize()
            local d = vector.dot(info.normal, H)
            if d < 0 then d = 0 end
            local glossWeight = d ^ shininess
            col = color.add(color.multiply_scalar(light.color, glossWeight), col)
        end
    end
    col:limit()
    return col
end

function engine:getPixelColor(ray, scene)
    local info = self:testIntersection(ray, scene, nil)
    if info.isHit then
        return self:rayTrace(info, ray, scene, 0)
    end
    return scene.background.color
end

function engine:renderScene(scene, canvas)
    check_number = 0
    if canvas then
        self.canvas = canvas
    else
        self.canvas = nil
    end
    local canvasHeight = self.canvasHeight
    local canvasWidth = self.canvasWidth
    for y = 0, canvasHeight - 1 do
        for x = 0, canvasWidth - 1 do
            local yp = y * 1.0 / canvasHeight * 2 - 1
            local xp = x * 1.0 / canvasWidth * 2 - 1
            local ray = scene.camera:getRay(xp, yp)
            local col = self:getPixelColor(ray, scene)
            self:setPixel(x, y, col)
        end
    end
end

local function build_scene()
    local scene = new_scene()
    scene.camera = new_camera(new_vector(0, 0, -15),
                              new_vector(-0.2, 0, 5),
                              new_vector(0, 1, 0))
    scene.background = new_background(new_color(0.5, 0.5, 0.5), 0.4)

    local sphere1 = new_sphere(new_vector(-1.5, 1.5, 2), 1.5,
                 new_solid_material(new_color(0, 0.5, 0.5), 0.3, 0.0, 0.0, 2.0))
    local sphere2 = new_sphere(new_vector(1, 0.25, 1), 0.5,
                 new_solid_material(new_color(0.9, 0.9, 0.9), 0.1, 0.0, 0.0, 1.5))
    local plane1 = new_plane(new_vector(0.1, 0.9, -0.5):normalize(), 1.2,
                 new_chessboard_material(new_color(1, 1, 1), new_color(0, 0, 0),
                                         0.2, 0.0, 1.0, 0.7))

    scene.shapes[1] = plane1
    scene.shapes[2] = sphere1
    scene.shapes[3] = sphere2

    scene.lights[1] = new_light(new_vector(5, 10, -1), new_color(0.8, 0.8, 0.8))
    scene.lights[2] = new_light(new_vector(-3, 5, -15), new_color(0.8, 0.8, 0.8), 100)

    return scene
end

local function run_once()
    local scene = build_scene()
    local rt = new_engine {
        canvasWidth = 100,
        canvasHeight = 100,
        pixelWidth = 5,
        pixelHeight = 5,
        renderDiffuse = true,
        renderShadows = true,
        renderHighlights = true,
        renderReflections = true,
        rayDepth = 2,
    }
    rt:renderScene(scene, nil)
end

local N = 60
for _ = 1, N do
    run_once()
end

io.write(check_number, "\n")
