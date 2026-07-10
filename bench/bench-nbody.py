# nbody: 3D gravitational N-body simulation. 5 bodies (sun + 4 outer
# planets), Euler integration. Stresses float math (mul/div/sqrt) and
# field mutation.
#
# Ported from the Are-We-Fast-Yet Python benchmark (nbody.py); harness
# stripped. Same ITERS=250000 and expected energy as bench-nbody.lua.

from math import sqrt

_ITERS = 250000
_EXPECTED = -0.1690859889909308

PI = 3.141592653589793
SOLAR_MASS = 4 * PI * PI
DAYS_PER_YEAR = 365.24


class _Body:
    __slots__ = ("x", "y", "z", "vx", "vy", "vz", "mass")

    def __init__(self, x, y, z, vx, vy, vz, mass):
        self.x = x
        self.y = y
        self.z = z
        self.vx = vx * DAYS_PER_YEAR
        self.vy = vy * DAYS_PER_YEAR
        self.vz = vz * DAYS_PER_YEAR
        self.mass = mass * SOLAR_MASS

    def offset_momentum(self, px, py, pz):
        self.vx = -(px / SOLAR_MASS)
        self.vy = -(py / SOLAR_MASS)
        self.vz = -(pz / SOLAR_MASS)


def _jupiter():
    return _Body(
        4.84143144246472090e00,
        -1.16032004402742839e00,
        -1.03622044471123109e-01,
        1.66007664274403694e-03,
        7.69901118419740425e-03,
        -6.90460016972063023e-05,
        9.54791938424326609e-04,
    )


def _saturn():
    return _Body(
        8.34336671824457987e00,
        4.12479856412430479e00,
        -4.03523417114321381e-01,
        -2.76742510726862411e-03,
        4.99852801234917238e-03,
        2.30417297573763929e-05,
        2.85885980666130812e-04,
    )


def _uranus():
    return _Body(
        1.28943695621391310e01,
        -1.51111514016986312e01,
        -2.23307578892655734e-01,
        2.96460137564761618e-03,
        2.37847173959480950e-03,
        -2.96589568540237556e-05,
        4.36624404335156298e-05,
    )


def _neptune():
    return _Body(
        1.53796971148509165e01,
        -2.59193146099879641e01,
        1.79258772950371181e-01,
        2.68067772490389322e-03,
        1.62824170038242295e-03,
        -9.51592254519715870e-05,
        5.15138902046611451e-05,
    )


def _sun():
    return _Body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0)


class NBodySystem:
    def __init__(self):
        self._bodies = self._create_bodies()

    @staticmethod
    def _create_bodies():
        bodies = [_sun(), _jupiter(), _saturn(), _uranus(), _neptune()]

        px = 0.0
        py = 0.0
        pz = 0.0

        for b in bodies:
            px += b.vx * b.mass
            py += b.vy * b.mass
            pz += b.vz * b.mass

        bodies[0].offset_momentum(px, py, pz)
        return bodies

    def advance(self, dt):
        for i in range(len(self._bodies)):
            i_body = self._bodies[i]

            for j in range(i + 1, len(self._bodies)):
                j_body = self._bodies[j]

                dx = i_body.x - j_body.x
                dy = i_body.y - j_body.y
                dz = i_body.z - j_body.z

                d_squared = dx * dx + dy * dy + dz * dz
                distance = sqrt(d_squared)
                mag = dt / (d_squared * distance)

                i_body.vx = i_body.vx - (dx * j_body.mass * mag)
                i_body.vy = i_body.vy - (dy * j_body.mass * mag)
                i_body.vz = i_body.vz - (dz * j_body.mass * mag)

                j_body.vx = j_body.vx + (dx * i_body.mass * mag)
                j_body.vy = j_body.vy + (dy * i_body.mass * mag)
                j_body.vz = j_body.vz + (dz * i_body.mass * mag)

        for body in self._bodies:
            body.x = body.x + dt * body.vx
            body.y = body.y + dt * body.vy
            body.z = body.z + dt * body.vz

    def energy(self):
        e = 0.0

        for i in range(len(self._bodies)):
            i_body = self._bodies[i]
            e += (
                0.5
                * i_body.mass
                * (
                    i_body.vx * i_body.vx
                    + i_body.vy * i_body.vy
                    + i_body.vz * i_body.vz
                )
            )

            for j in range(i + 1, len(self._bodies)):
                j_body = self._bodies[j]
                dx = i_body.x - j_body.x
                dy = i_body.y - j_body.y
                dz = i_body.z - j_body.z

                distance = sqrt(dx * dx + dy * dy + dz * dz)
                e -= (i_body.mass * j_body.mass) / distance
        return e


system = NBodySystem()
for _ in range(_ITERS):
    system.advance(0.01)

e = system.energy()
if abs(e - _EXPECTED) < 1e-13:
    print("ok")
else:
    raise SystemExit(f"bad energy: {e}")
