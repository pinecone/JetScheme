# cdjs: aircraft collision detection. 1000 aircraft, 18 frames.
# Ported from JetStream2 cdjs. Expects 1336 total collisions.

import math
import sys

_ITERS = 1
_NUM_AIRCRAFT = 1000
_NUM_FRAMES = 18
_EXPECTED_COLLISIONS = 1336

_MIN_X = 0
_MIN_Y = 0
_MAX_X = 1000
_MAX_Y = 1000
_MIN_Z = 0
_MAX_Z = 10
_PROXIMITY_RADIUS = 1
_GOOD_VOXEL_SIZE = 2


def _compare_numbers(a, b):
    if a == b:
        return 0
    if a < b:
        return -1
    if a > b:
        return 1
    if a == a:
        return 1
    return -1


class _Vector2D:
    __slots__ = ("x", "y")

    def __init__(self, x=0, y=0):
        self.x = x
        self.y = y

    def plus(self, o):
        return _Vector2D(self.x + o.x, self.y + o.y)

    def minus(self, o):
        return _Vector2D(self.x - o.x, self.y - o.y)

    def compare_to(self, o):
        r = _compare_numbers(self.x, o.x)
        if r != 0:
            return r
        return _compare_numbers(self.y, o.y)


class _Vector3D:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def plus(self, o):
        return _Vector3D(self.x + o.x, self.y + o.y, self.z + o.z)

    def minus(self, o):
        return _Vector3D(self.x - o.x, self.y - o.y, self.z - o.z)

    def dot(self, o):
        return self.x * o.x + self.y * o.y + self.z * o.z

    def squared_magnitude(self):
        return self.dot(self)

    def magnitude(self):
        return math.sqrt(self.squared_magnitude())

    def times(self, c):
        return _Vector3D(self.x * c, self.y * c, self.z * c)


class _CallSign:
    __slots__ = ("_value",)

    def __init__(self, v):
        self._value = v

    def compare_to(self, o):
        if self._value == o._value:
            return 0
        if self._value < o._value:
            return -1
        return 1


class _Motion:
    __slots__ = ("callsign", "pos_one", "pos_two")

    def __init__(self, cs, p1, p2):
        self.callsign = cs
        self.pos_one = p1
        self.pos_two = p2

    def delta(self):
        return self.pos_two.minus(self.pos_one)

    def find_intersection(self, other):
        init1 = self.pos_one
        init2 = other.pos_one
        vec1 = self.delta()
        vec2 = other.delta()
        radius = _PROXIMITY_RADIUS
        a = vec2.minus(vec1).squared_magnitude()
        if a != 0:
            b = 2 * init1.minus(init2).dot(vec1.minus(vec2))
            c = -radius * radius + init2.minus(init1).squared_magnitude()
            discr = b * b - 4 * a * c
            if discr < 0:
                return None
            sq = math.sqrt(discr)
            v1 = (-b - sq) / (2 * a)
            v2 = (-b + sq) / (2 * a)
            if v1 <= v2 and ((v1 <= 1 <= v2) or
                             (v1 <= 0 <= v2) or
                             (0 <= v1 and v2 <= 1)):
                v = 0 if v1 <= 0 else v1
                result1 = init1.plus(vec1.times(v))
                result2 = init2.plus(vec2.times(v))
                result = result1.plus(result2).times(0.5)
                if (_MIN_X <= result.x <= _MAX_X and
                        _MIN_Y <= result.y <= _MAX_Y and
                        _MIN_Z <= result.z <= _MAX_Z):
                    return result
            return None
        dist = init2.minus(init1).magnitude()
        if dist <= radius:
            return init1.plus(init2).times(0.5)
        return None


class _Node:
    __slots__ = ("key", "value", "left", "right", "parent", "color")

    def __init__(self, key, value):
        self.key = key
        self.value = value
        self.left = None
        self.right = None
        self.parent = None
        self.color = "red"


def _tree_min(x):
    while x.left is not None:
        x = x.left
    return x


def _node_successor(n):
    if n.right is not None:
        return _tree_min(n.right)
    x = n
    y = n.parent
    while y is not None and x is y.right:
        x = y
        y = y.parent
    return y


class _RedBlackTree:
    __slots__ = ("_root",)

    def __init__(self):
        self._root = None

    def put(self, key, value):
        is_new, payload = self._tree_insert(key, value)
        if not is_new:
            return payload
        x = payload
        while x is not self._root and x.parent.color == "red":
            if x.parent is x.parent.parent.left:
                y = x.parent.parent.right
                if y is not None and y.color == "red":
                    x.parent.color = "black"
                    y.color = "black"
                    x.parent.parent.color = "red"
                    x = x.parent.parent
                else:
                    if x is x.parent.right:
                        x = x.parent
                        self._left_rotate(x)
                    x.parent.color = "black"
                    x.parent.parent.color = "red"
                    self._right_rotate(x.parent.parent)
            else:
                y = x.parent.parent.left
                if y is not None and y.color == "red":
                    x.parent.color = "black"
                    y.color = "black"
                    x.parent.parent.color = "red"
                    x = x.parent.parent
                else:
                    if x is x.parent.left:
                        x = x.parent
                        self._right_rotate(x)
                    x.parent.color = "black"
                    x.parent.parent.color = "red"
                    self._left_rotate(x.parent.parent)
        self._root.color = "black"
        return None

    def remove(self, key):
        z = self._find_node(key)
        if z is None:
            return None

        if z.left is None or z.right is None:
            y = z
        else:
            y = _node_successor(z)

        x = y.left if y.left is not None else y.right

        if x is not None:
            x.parent = y.parent
            x_parent = x.parent
        else:
            x_parent = y.parent

        if y.parent is None:
            self._root = x
        else:
            if y is y.parent.left:
                y.parent.left = x
            else:
                y.parent.right = x

        if y is not z:
            if y.color == "black":
                self._remove_fixup(x, x_parent)
            y.parent = z.parent
            y.color = z.color
            y.left = z.left
            y.right = z.right
            if z.left is not None:
                z.left.parent = y
            if z.right is not None:
                z.right.parent = y
            if z.parent is not None:
                if z.parent.left is z:
                    z.parent.left = y
                else:
                    z.parent.right = y
            else:
                self._root = y
        elif y.color == "black":
            self._remove_fixup(x, x_parent)
        return z.value

    def get(self, key):
        n = self._find_node(key)
        if n is None:
            return None
        return n.value

    def for_each(self, callback):
        if self._root is None:
            return
        cur = _tree_min(self._root)
        while cur is not None:
            callback(cur.key, cur.value)
            cur = _node_successor(cur)

    def _find_node(self, key):
        cur = self._root
        while cur is not None:
            r = key.compare_to(cur.key)
            if r == 0:
                return cur
            if r < 0:
                cur = cur.left
            else:
                cur = cur.right
        return None

    def _tree_insert(self, key, value):
        y = None
        x = self._root
        while x is not None:
            y = x
            r = key.compare_to(x.key)
            if r < 0:
                x = x.left
            elif r > 0:
                x = x.right
            else:
                old = x.value
                x.value = value
                return (False, old)
        z = _Node(key, value)
        z.parent = y
        if y is None:
            self._root = z
        else:
            if key.compare_to(y.key) < 0:
                y.left = z
            else:
                y.right = z
        return (True, z)

    def _left_rotate(self, x):
        y = x.right
        x.right = y.left
        if y.left is not None:
            y.left.parent = x
        y.parent = x.parent
        if x.parent is None:
            self._root = y
        else:
            if x is x.parent.left:
                x.parent.left = y
            else:
                x.parent.right = y
        y.left = x
        x.parent = y

    def _right_rotate(self, y):
        x = y.left
        y.left = x.right
        if x.right is not None:
            x.right.parent = y
        x.parent = y.parent
        if y.parent is None:
            self._root = x
        else:
            if y is y.parent.left:
                y.parent.left = x
            else:
                y.parent.right = x
        x.right = y
        y.parent = x

    def _remove_fixup(self, x, x_parent):
        while x is not self._root and (x is None or x.color == "black"):
            if x is x_parent.left:
                w = x_parent.right
                if w.color == "red":
                    w.color = "black"
                    x_parent.color = "red"
                    self._left_rotate(x_parent)
                    w = x_parent.right
                if ((w.left is None or w.left.color == "black") and
                        (w.right is None or w.right.color == "black")):
                    w.color = "red"
                    x = x_parent
                    x_parent = x.parent
                else:
                    if w.right is None or w.right.color == "black":
                        w.left.color = "black"
                        w.color = "red"
                        self._right_rotate(w)
                        w = x_parent.right
                    w.color = x_parent.color
                    x_parent.color = "black"
                    if w.right is not None:
                        w.right.color = "black"
                    self._left_rotate(x_parent)
                    x = self._root
                    x_parent = x.parent
            else:
                w = x_parent.left
                if w.color == "red":
                    w.color = "black"
                    x_parent.color = "red"
                    self._right_rotate(x_parent)
                    w = x_parent.left
                if ((w.right is None or w.right.color == "black") and
                        (w.left is None or w.left.color == "black")):
                    w.color = "red"
                    x = x_parent
                    x_parent = x.parent
                else:
                    if w.left is None or w.left.color == "black":
                        w.right.color = "black"
                        w.color = "red"
                        self._left_rotate(w)
                        w = x_parent.left
                    w.color = x_parent.color
                    x_parent.color = "black"
                    if w.left is not None:
                        w.left.color = "black"
                    self._right_rotate(x_parent)
                    x = self._root
                    x_parent = x.parent
        if x is not None:
            x.color = "black"


def _voxel_hash(position):
    vs = _GOOD_VOXEL_SIZE
    xdiv = int(position.x / vs)
    ydiv = int(position.y / vs)
    result = _Vector2D(vs * xdiv, vs * ydiv)
    if position.x < 0:
        result.x -= vs
    if position.y < 0:
        result.y -= vs
    return result


def _draw_motion_on_voxel_map(voxel_map, motion):
    seen = _RedBlackTree()
    vs = _GOOD_VOXEL_SIZE
    horizontal = _Vector2D(vs, 0)
    vertical = _Vector2D(0, vs)

    def is_in_voxel(voxel):
        if (voxel.x > _MAX_X or voxel.x < _MIN_X or
                voxel.y > _MAX_Y or voxel.y < _MIN_Y):
            return False
        init = motion.pos_one
        fin = motion.pos_two
        v_s = vs
        r = _PROXIMITY_RADIUS / 2
        v_x = voxel.x
        x0 = init.x
        xv = fin.x - x0
        v_y = voxel.y
        y0 = init.y
        yv = fin.y - y0
        low_x = (v_x - r - x0) / xv if xv != 0 else 0
        high_x = (v_x + v_s + r - x0) / xv if xv != 0 else 0
        if xv < 0:
            low_x, high_x = high_x, low_x
        low_y = (v_y - r - y0) / yv if yv != 0 else 0
        high_y = (v_y + v_s + r - y0) / yv if yv != 0 else 0
        if yv < 0:
            low_y, high_y = high_y, low_y
        return (((xv == 0 and v_x <= x0 + r and x0 - r <= v_x + v_s) or
                 (xv != 0 and ((low_x <= 1 <= high_x) or
                               (low_x <= 0 <= high_x) or
                               (0 <= low_x and high_x <= 1)))) and
                ((yv == 0 and v_y <= y0 + r and y0 - r <= v_y + v_s) or
                 (yv != 0 and ((low_y <= 1 <= high_y) or
                               (low_y <= 0 <= high_y) or
                               (0 <= low_y and high_y <= 1)))) and
                (xv == 0 or yv == 0 or
                 (low_y <= high_x <= high_y) or
                 (low_y <= low_x <= high_y) or
                 (low_x <= low_y and high_y <= high_x)))

    def put_into_map(voxel):
        arr = voxel_map.get(voxel)
        if arr is None:
            arr = []
            voxel_map.put(voxel, arr)
        arr.append(motion)

    def recurse(nv):
        if not is_in_voxel(nv):
            return
        if seen.put(nv, True):
            return
        put_into_map(nv)
        recurse(nv.minus(horizontal))
        recurse(nv.plus(horizontal))
        recurse(nv.minus(vertical))
        recurse(nv.plus(vertical))
        recurse(nv.minus(horizontal).minus(vertical))
        recurse(nv.minus(horizontal).plus(vertical))
        recurse(nv.plus(horizontal).minus(vertical))
        recurse(nv.plus(horizontal).plus(vertical))

    recurse(_voxel_hash(motion.pos_one))


def _reduce_collision_set(motions):
    voxel_map = _RedBlackTree()
    for m in motions:
        _draw_motion_on_voxel_map(voxel_map, m)
    result = []

    def collect(_k, v):
        if len(v) > 1:
            result.append(v)

    voxel_map.for_each(collect)
    return result


class _Simulator:
    __slots__ = ("_aircraft",)

    def __init__(self, n):
        self._aircraft = [_CallSign("foo" + str(i)) for i in range(n)]

    def simulate(self, time):
        frame = []
        ct = math.cos(time)
        st = math.sin(time)
        n = len(self._aircraft)
        i = 0
        while i < n:
            frame.append((self._aircraft[i], _Vector3D(time, ct * 2 + i * 3, 10)))
            frame.append((self._aircraft[i + 1], _Vector3D(time, st * 2 + i * 3, 10)))
            i += 2
        return frame


class _CollisionDetector:
    __slots__ = ("_state",)

    def __init__(self):
        self._state = _RedBlackTree()

    def handle_new_frame(self, frame):
        motions = []
        seen = _RedBlackTree()
        for cs, pos in frame:
            old_pos = self._state.put(cs, pos)
            seen.put(cs, True)
            if old_pos is None:
                old_pos = pos
            motions.append(_Motion(cs, old_pos, pos))
        to_remove = []

        def collect(cs, _p):
            if seen.get(cs) is None:
                to_remove.append(cs)

        self._state.for_each(collect)
        for cs in to_remove:
            self._state.remove(cs)
        all_reduced = _reduce_collision_set(motions)
        count = 0
        for reduced in all_reduced:
            n = len(reduced)
            for i in range(n):
                m1 = reduced[i]
                for j in range(i + 1, n):
                    if m1.find_intersection(reduced[j]) is not None:
                        count += 1
        return count


def _run_bench():
    sim = _Simulator(_NUM_AIRCRAFT)
    det = _CollisionDetector()
    total = 0
    for i in range(_NUM_FRAMES):
        total += det.handle_new_frame(sim.simulate(i / 10))
    return total


for _ in range(_ITERS):
    c = _run_bench()
    if c != _EXPECTED_COLLISIONS:
        raise SystemExit(f"cdjs: bad collision count: {c} (expected {_EXPECTED_COLLISIONS})")

print(_EXPECTED_COLLISIONS)
