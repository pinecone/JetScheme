# raytrace: 3D ray tracer (Burmister/Octane). Polymorphic shapes and
# materials, vector math, reflection/shadow/highlight recursion.
# Stresses float math, allocation, and dispatch. A correct run prints 2321.

import math


class Color:
    __slots__ = ("red", "green", "blue")

    def __init__(self, r=0.0, g=0.0, b=0.0):
        self.red = r
        self.green = g
        self.blue = b

    @staticmethod
    def add(c1, c2):
        return Color(c1.red + c2.red, c1.green + c2.green, c1.blue + c2.blue)

    @staticmethod
    def add_scalar(c1, s):
        r = c1.red + s
        g = c1.green + s
        b = c1.blue + s
        return Color(
            1.0 if r > 1.0 else (0.0 if r < 0.0 else r),
            1.0 if g > 1.0 else (0.0 if g < 0.0 else g),
            1.0 if b > 1.0 else (0.0 if b < 0.0 else b),
        )

    @staticmethod
    def subtract(c1, c2):
        return Color(c1.red - c2.red, c1.green - c2.green, c1.blue - c2.blue)

    @staticmethod
    def multiply(c1, c2):
        return Color(c1.red * c2.red, c1.green * c2.green, c1.blue * c2.blue)

    @staticmethod
    def multiply_scalar(c1, f):
        return Color(c1.red * f, c1.green * f, c1.blue * f)

    @staticmethod
    def divide_factor(c1, f):
        return Color(c1.red / f, c1.green / f, c1.blue / f)

    def limit(self):
        self.red = 1.0 if self.red > 1.0 else (0.0 if self.red < 0.0 else self.red)
        self.green = 1.0 if self.green > 1.0 else (0.0 if self.green < 0.0 else self.green)
        self.blue = 1.0 if self.blue > 1.0 else (0.0 if self.blue < 0.0 else self.blue)

    def distance(self, other):
        return (abs(self.red - other.red) + abs(self.green - other.green)
                + abs(self.blue - other.blue))

    @staticmethod
    def blend(c1, c2, w):
        return Color.add(Color.multiply_scalar(c1, 1 - w), Color.multiply_scalar(c2, w))

    def brightness(self):
        r = math.floor(self.red * 255)
        g = math.floor(self.green * 255)
        b = math.floor(self.blue * 255)
        return (r * 77 + g * 150 + b * 29) >> 8


class Vector:
    __slots__ = ("x", "y", "z")

    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.x = x
        self.y = y
        self.z = z

    def copy(self, v):
        self.x = v.x
        self.y = v.y
        self.z = v.z

    def magnitude(self):
        return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z)

    def normalize(self):
        m = self.magnitude()
        return Vector(self.x / m, self.y / m, self.z / m)

    def cross(self, w):
        return Vector(-self.z * w.y + self.y * w.z,
                       self.z * w.x - self.x * w.z,
                      -self.y * w.x + self.x * w.y)

    def dot(self, w):
        return self.x * w.x + self.y * w.y + self.z * w.z

    @staticmethod
    def add(v, w):
        return Vector(v.x + w.x, v.y + w.y, v.z + w.z)

    @staticmethod
    def subtract(v, w):
        return Vector(v.x - w.x, v.y - w.y, v.z - w.z)

    @staticmethod
    def multiply_vector(v, w):
        return Vector(v.x * w.x, v.y * w.y, v.z * w.z)

    @staticmethod
    def multiply_scalar(v, w):
        return Vector(v.x * w, v.y * w, v.z * w)


class Ray:
    __slots__ = ("position", "direction")

    def __init__(self, pos, direction):
        self.position = pos
        self.direction = direction


class Light:
    __slots__ = ("position", "color", "intensity")

    def __init__(self, pos, color, intensity=10.0):
        self.position = pos
        self.color = color
        self.intensity = intensity


class Background:
    __slots__ = ("color", "ambience")

    def __init__(self, color, ambience):
        self.color = color
        self.ambience = ambience


class IntersectionInfo:
    __slots__ = ("isHit", "hitCount", "shape", "position", "normal", "color", "distance")

    def __init__(self):
        self.isHit = False
        self.hitCount = 0
        self.shape = None
        self.position = None
        self.normal = None
        self.color = Color(0.0, 0.0, 0.0)
        self.distance = None


def _wrap_up(t):
    t = t % 2.0
    if t < -1:
        t += 2.0
    if t >= 1:
        t -= 2.0
    return t


class SolidMaterial:
    __slots__ = ("color", "reflection", "transparency", "gloss", "hasTexture")

    def __init__(self, color, reflection, refraction, transparency, gloss):
        self.color = color
        self.reflection = reflection
        self.transparency = transparency
        self.gloss = gloss
        self.hasTexture = False

    def get_color(self, u, v):
        return self.color


class ChessboardMaterial:
    __slots__ = ("colorEven", "colorOdd", "reflection", "transparency",
                 "gloss", "density", "hasTexture")

    def __init__(self, colorEven, colorOdd, reflection, transparency, gloss, density):
        self.colorEven = colorEven
        self.colorOdd = colorOdd
        self.reflection = reflection
        self.transparency = transparency
        self.gloss = gloss
        self.density = density
        self.hasTexture = True

    def get_color(self, u, v):
        t = _wrap_up(u * self.density) * _wrap_up(v * self.density)
        if t < 0.0:
            return self.colorEven
        return self.colorOdd


class Sphere:
    __slots__ = ("radius", "position", "material")

    def __init__(self, position, radius, material):
        self.radius = radius
        self.position = position
        self.material = material

    def intersect(self, ray):
        info = IntersectionInfo()
        info.shape = self
        dst = Vector.subtract(ray.position, self.position)
        B = dst.dot(ray.direction)
        C = dst.dot(dst) - (self.radius * self.radius)
        D = (B * B) - C
        if D > 0:
            info.isHit = True
            info.distance = (-B) - math.sqrt(D)
            info.position = Vector.add(ray.position,
                                       Vector.multiply_scalar(ray.direction, info.distance))
            info.normal = Vector.subtract(info.position, self.position).normalize()
            info.color = self.material.get_color(0, 0)
        else:
            info.isHit = False
        return info


class Plane:
    __slots__ = ("position", "d", "material")

    def __init__(self, position, d, material):
        self.position = position
        self.d = d
        self.material = material

    def intersect(self, ray):
        info = IntersectionInfo()
        Vd = self.position.dot(ray.direction)
        if Vd == 0:
            return info
        t = -(self.position.dot(ray.position) + self.d) / Vd
        if t <= 0:
            return info
        info.shape = self
        info.isHit = True
        info.position = Vector.add(ray.position,
                                   Vector.multiply_scalar(ray.direction, t))
        info.normal = self.position
        info.distance = t
        info.color = self.material.get_color(0, 0)
        if self.material.hasTexture:
            vU = Vector(self.position.y, self.position.z, -self.position.x)
            vV = vU.cross(self.position)
            u = info.position.dot(vU)
            v = info.position.dot(vV)
            info.color = self.material.get_color(u, v)
        return info


class Camera:
    __slots__ = ("position", "lookAt", "equator", "up", "screen")

    def __init__(self, pos, lookAt, up):
        self.position = pos
        self.lookAt = lookAt
        self.up = up
        self.equator = lookAt.normalize().cross(up)
        self.screen = Vector.add(pos, lookAt)

    def get_ray(self, vx, vy):
        pos = Vector.subtract(self.screen,
                Vector.subtract(Vector.multiply_scalar(self.equator, vx),
                                Vector.multiply_scalar(self.up, vy)))
        pos.y = pos.y * -1
        direction = Vector.subtract(pos, self.position)
        return Ray(pos, direction.normalize())


class Scene:
    __slots__ = ("camera", "shapes", "lights", "background")

    def __init__(self):
        self.camera = Camera(Vector(0, 0, -5), Vector(0, 0, 1), Vector(0, 1, 0))
        self.shapes = []
        self.lights = []
        self.background = Background(Color(0, 0, 0.5), 0.2)


class Engine:
    __slots__ = ("canvasHeight", "canvasWidth", "pixelWidth", "pixelHeight",
                 "renderDiffuse", "renderShadows", "renderHighlights",
                 "renderReflections", "rayDepth", "canvas", "checkNumber")

    def __init__(self, canvasHeight=100, canvasWidth=100, pixelWidth=2, pixelHeight=2,
                 renderDiffuse=False, renderShadows=False, renderHighlights=False,
                 renderReflections=False, rayDepth=2):
        self.pixelWidth = pixelWidth
        self.pixelHeight = pixelHeight
        self.canvasHeight = canvasHeight / pixelHeight
        self.canvasWidth = canvasWidth / pixelWidth
        self.renderDiffuse = renderDiffuse
        self.renderShadows = renderShadows
        self.renderHighlights = renderHighlights
        self.renderReflections = renderReflections
        self.rayDepth = rayDepth
        self.canvas = None
        self.checkNumber = 0

    def set_pixel(self, x, y, color):
        if self.canvas is not None:
            self.canvas.fillStyle = str(color)
        else:
            if x == y:
                self.checkNumber += color.brightness()

    def render_scene(self, scene, canvas=None):
        self.checkNumber = 0
        self.canvas = canvas
        canvasHeight = int(self.canvasHeight)
        canvasWidth = int(self.canvasWidth)
        for y in range(canvasHeight):
            for x in range(canvasWidth):
                yp = y * 1.0 / self.canvasHeight * 2 - 1
                xp = x * 1.0 / self.canvasWidth * 2 - 1
                ray = scene.camera.get_ray(xp, yp)
                color = self.get_pixel_color(ray, scene)
                self.set_pixel(x, y, color)

    def get_pixel_color(self, ray, scene):
        info = self.test_intersection(ray, scene, None)
        if info.isHit:
            return self.ray_trace(info, ray, scene, 0)
        return scene.background.color

    def test_intersection(self, ray, scene, exclude):
        hits = 0
        best = IntersectionInfo()
        best.distance = 2000
        for shape in scene.shapes:
            if shape is not exclude:
                info = shape.intersect(ray)
                if info.isHit and info.distance >= 0 and info.distance < best.distance:
                    best = info
                    hits += 1
        best.hitCount = hits
        return best

    def get_reflection_ray(self, P, N, V):
        c1 = -N.dot(V)
        R1 = Vector.add(Vector.multiply_scalar(N, 2 * c1), V)
        return Ray(P, R1)

    def ray_trace(self, info, ray, scene, depth):
        color = Color.multiply_scalar(info.color, scene.background.ambience)
        shininess = 10 ** (info.shape.material.gloss + 1)

        for light in scene.lights:
            v = Vector.subtract(light.position, info.position).normalize()

            if self.renderDiffuse:
                L = v.dot(info.normal)
                if L > 0.0:
                    color = Color.add(color,
                           Color.multiply(info.color,
                             Color.multiply_scalar(light.color, L)))

            if depth <= self.rayDepth:
                if self.renderReflections and info.shape.material.reflection > 0:
                    reflectionRay = self.get_reflection_ray(info.position, info.normal, ray.direction)
                    refl = self.test_intersection(reflectionRay, scene, info.shape)
                    if refl.isHit and refl.distance > 0:
                        refl.color = self.ray_trace(refl, reflectionRay, scene, depth + 1)
                    else:
                        refl.color = scene.background.color
                    color = Color.blend(color, refl.color, info.shape.material.reflection)

            shadowInfo = IntersectionInfo()
            if self.renderShadows:
                shadowRay = Ray(info.position, v)
                shadowInfo = self.test_intersection(shadowRay, scene, info.shape)
                if shadowInfo.isHit and shadowInfo.shape is not info.shape:
                    vA = Color.multiply_scalar(color, 0.5)
                    dB = 0.5 * (shadowInfo.shape.material.transparency ** 0.5)
                    color = Color.add_scalar(vA, dB)

            if (self.renderHighlights and not shadowInfo.isHit
                    and info.shape.material.gloss > 0):
                Lv = Vector.subtract(info.shape.position, light.position).normalize()
                E = Vector.subtract(scene.camera.position, info.shape.position).normalize()
                H = Vector.subtract(E, Lv).normalize()
                d = info.normal.dot(H)
                if d < 0:
                    d = 0
                glossWeight = d ** shininess
                color = Color.add(Color.multiply_scalar(light.color, glossWeight), color)

        color.limit()
        return color


def _build_scene():
    scene = Scene()
    scene.camera = Camera(Vector(0, 0, -15),
                          Vector(-0.2, 0, 5),
                          Vector(0, 1, 0))
    scene.background = Background(Color(0.5, 0.5, 0.5), 0.4)

    scene.shapes.append(Plane(Vector(0.1, 0.9, -0.5).normalize(), 1.2,
               ChessboardMaterial(Color(1, 1, 1), Color(0, 0, 0),
                                 0.2, 0.0, 1.0, 0.7)))
    scene.shapes.append(Sphere(Vector(-1.5, 1.5, 2), 1.5,
               SolidMaterial(Color(0, 0.5, 0.5), 0.3, 0.0, 0.0, 2.0)))
    scene.shapes.append(Sphere(Vector(1, 0.25, 1), 0.5,
               SolidMaterial(Color(0.9, 0.9, 0.9), 0.1, 0.0, 0.0, 1.5)))

    scene.lights.append(Light(Vector(5, 10, -1), Color(0.8, 0.8, 0.8)))
    scene.lights.append(Light(Vector(-3, 5, -15), Color(0.8, 0.8, 0.8), 100))

    return scene


def _run_once():
    scene = _build_scene()
    rt = Engine(canvasWidth=100, canvasHeight=100, pixelWidth=5, pixelHeight=5,
                renderDiffuse=True, renderShadows=True, renderHighlights=True,
                renderReflections=True, rayDepth=2)
    rt.render_scene(scene)
    return rt.checkNumber


_N = 60
result = 0
for _ in range(_N):
    result = _run_once()

print(result)
