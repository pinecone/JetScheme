# deltablue: incremental constraint solver. Builds an equality chain plus
# a network of scale/offset projections, repeatedly edits one endpoint and
# propagates. Stresses dispatch, table + list mutation, and worklist loops.
#
# Ported from the Are-We-Fast-Yet Python benchmark (deltablue.py); harness
# stripped, som.Vector replaced with a thin list-backed shim, IdentityDictionary
# replaced with a plain dict (Sym instances are unique singletons). N=12000 to
# match bench-deltablue.lua.

from abc import abstractmethod
from enum import Enum

_CHAIN_N = 12000
_PROJECTION_N = 12000


# --- Vector shim (just what deltablue uses) -----------------------------
# Backed by a Python list with a moving _first cursor so remove_first is O(1)
# (matches AWFY som.Vector behavior; pop(0) would be O(n) and unfairly slow).
class Vector:
    def __init__(self, size=0):
        self._items = []
        self._first = 0

    def append(self, elem):
        self._items.append(elem)

    def remove(self, elem):
        self._items = [x for x in self._items[self._first:] if x is not elem]
        self._first = 0

    def for_each(self, fn):
        for i in range(self._first, len(self._items)):
            fn(self._items[i])

    def is_empty(self):
        return self._first >= len(self._items)

    def remove_first(self):
        x = self._items[self._first]
        self._first += 1
        return x

    def at(self, idx):
        return self._items[self._first + idx]

    def sort(self, comp):
        # Insertion sort over the live region [_first, len).
        a = self._items
        for i in range(self._first + 1, len(a)):
            x = a[i]
            j = i - 1
            while j >= self._first and comp(x, a[j]) < 0:
                a[j + 1] = a[j]
                j -= 1
            a[j + 1] = x


def vector_with(elem):
    v = Vector()
    v.append(elem)
    return v


# --- Strengths -----------------------------------------------------------
class _Sym:
    pass


_ABSOLUTE_STRONGEST = _Sym()
_REQUIRED = _Sym()
_STRONG_PREFERRED = _Sym()
_PREFERRED = _Sym()
_STRONG_DEFAULT = _Sym()
_DEFAULT = _Sym()
_WEAK_DEFAULT = _Sym()
_ABSOLUTE_WEAKEST = _Sym()


_strength_table = {
    _ABSOLUTE_STRONGEST: -10000,
    _REQUIRED: -800,
    _STRONG_PREFERRED: -600,
    _PREFERRED: -400,
    _STRONG_DEFAULT: -200,
    _DEFAULT: 0,
    _WEAK_DEFAULT: 500,
    _ABSOLUTE_WEAKEST: 10000,
}


class _Strength:
    def __init__(self, strength_sym):
        self._symbolic_value = strength_sym
        self.arithmetic_value = _strength_table[strength_sym]

    def same_as(self, s):
        return self.arithmetic_value == s.arithmetic_value

    def stronger(self, s):
        return self.arithmetic_value < s.arithmetic_value

    def weaker(self, s):
        return self.arithmetic_value > s.arithmetic_value

    def strongest(self, s):
        return s if s.stronger(self) else self

    def weakest(self, s):
        return s if s.weaker(self) else self

    @staticmethod
    def of(strength):
        return _strength_constant[strength]


_strength_constant = {sym: _Strength(sym) for sym in _strength_table}

_absolute_weakest = _Strength.of(_ABSOLUTE_WEAKEST)
_required = _Strength.of(_REQUIRED)


class _Direction(Enum):
    FORWARD = 1
    BACKWARD = 2


# --- Constraints ---------------------------------------------------------
class _AbstractConstraint:
    def __init__(self, strength):
        self.strength = _Strength.of(strength)

    def is_input(self):
        return False

    @abstractmethod
    def is_satisfied(self):
        pass

    def add_constraint(self, planner):
        self.add_to_graph()
        planner.incremental_add(self)

    @abstractmethod
    def add_to_graph(self):
        pass

    def destroy_constraint(self, planner):
        if self.is_satisfied():
            planner.incremental_remove(self)
        self.remove_from_graph()

    @abstractmethod
    def remove_from_graph(self):
        pass

    @abstractmethod
    def choose_method(self, mark):
        pass

    @abstractmethod
    def execute(self):
        pass

    @abstractmethod
    def inputs_do(self, fn):
        pass

    @abstractmethod
    def inputs_has_one(self, fn):
        pass

    def inputs_known(self, mark):
        return not self.inputs_has_one(
            lambda v: not (v.mark == mark or v.stay or v.determined_by is None)
        )

    @abstractmethod
    def mark_unsatisfied(self):
        pass

    @abstractmethod
    def get_output(self):
        pass

    @abstractmethod
    def recalculate(self):
        pass

    def satisfy(self, mark, planner):
        self.choose_method(mark)
        if self.is_satisfied():
            def each(input_):
                input_.mark = mark
            self.inputs_do(each)
            out = self.get_output()
            overridden = out.determined_by
            if overridden is not None:
                overridden.mark_unsatisfied()
            out.determined_by = self
            if not planner.add_propagate(self, mark):
                raise Exception("Cycle encountered")
            out.mark = mark
        else:
            overridden = None
            if self.strength.same_as(_required):
                raise Exception("Could not satisfy a required constraint")
        return overridden


class _BinaryConstraint(_AbstractConstraint):
    def __init__(self, var1, var2, strength, _planner):
        super().__init__(strength)
        self._v1 = var1
        self._v2 = var2
        self._direction = None

    def is_satisfied(self):
        return self._direction is not None

    def add_to_graph(self):
        self._v1.add_constraint(self)
        self._v2.add_constraint(self)
        self._direction = None

    def remove_from_graph(self):
        if self._v1 is not None:
            self._v1.remove_constraint(self)
        if self._v2 is not None:
            self._v2.remove_constraint(self)
        self._direction = None

    def choose_method(self, mark):
        if self._v1.mark == mark:
            if self._v2.mark != mark and self.strength.stronger(self._v2.walk_strength):
                self._direction = _Direction.FORWARD
                return self._direction
            self._direction = None
            return self._direction
        if self._v2.mark == mark:
            if self._v1.mark != mark and self.strength.stronger(self._v1.walk_strength):
                self._direction = _Direction.BACKWARD
                return self._direction
            self._direction = None
            return self._direction
        if self._v1.walk_strength.weaker(self._v2.walk_strength):
            if self.strength.stronger(self._v1.walk_strength):
                self._direction = _Direction.BACKWARD
                return self._direction
            self._direction = None
            return self._direction
        if self.strength.stronger(self._v2.walk_strength):
            self._direction = _Direction.FORWARD
            return self._direction
        self._direction = None
        return self._direction

    def inputs_do(self, fn):
        if self._direction is _Direction.FORWARD:
            fn(self._v1)
        else:
            fn(self._v2)

    def inputs_has_one(self, fn):
        if self._direction is _Direction.FORWARD:
            return fn(self._v1)
        return fn(self._v2)

    def mark_unsatisfied(self):
        self._direction = None

    def get_output(self):
        return self._v2 if self._direction is _Direction.FORWARD else self._v1

    def recalculate(self):
        if self._direction is _Direction.FORWARD:
            input_ = self._v1
            output = self._v2
        else:
            input_ = self._v2
            output = self._v1
        output.walk_strength = self.strength.weakest(input_.walk_strength)
        output.stay = input_.stay
        if output.stay:
            self.execute()


class _UnaryConstraint(_AbstractConstraint):
    def __init__(self, v, strength, planner):
        super().__init__(strength)
        self._output = v
        self._satisfied = False
        self.add_constraint(planner)

    def is_satisfied(self):
        return self._satisfied

    def add_to_graph(self):
        self._output.add_constraint(self)
        self._satisfied = False

    def remove_from_graph(self):
        if self._output is not None:
            self._output.remove_constraint(self)
        self._satisfied = False

    def choose_method(self, mark):
        self._satisfied = self._output.mark != mark and self.strength.stronger(
            self._output.walk_strength
        )

    @abstractmethod
    def execute(self):
        pass

    def inputs_do(self, fn):
        pass

    def inputs_has_one(self, fn):
        return False

    def mark_unsatisfied(self):
        self._satisfied = False

    def get_output(self):
        return self._output

    def recalculate(self):
        self._output.walk_strength = self.strength
        self._output.stay = not self.is_input()
        if self._output.stay:
            self.execute()


class _EditConstraint(_UnaryConstraint):
    def is_input(self):
        return True

    def execute(self):
        pass


class _EqualityConstraint(_BinaryConstraint):
    def __init__(self, var1, var2, strength, planner):
        super().__init__(var1, var2, strength, planner)
        self.add_constraint(planner)

    def execute(self):
        if self._direction is _Direction.FORWARD:
            self._v2.value = self._v1.value
        else:
            self._v1.value = self._v2.value


class _ScaleConstraint(_BinaryConstraint):
    def __init__(self, src, scale, offset, dest, strength, planner):
        super().__init__(src, dest, strength, planner)
        self._scale = scale
        self._offset = offset
        self.add_constraint(planner)

    def add_to_graph(self):
        self._v1.add_constraint(self)
        self._v2.add_constraint(self)
        self._scale.add_constraint(self)
        self._offset.add_constraint(self)
        self._direction = None

    def remove_from_graph(self):
        if self._v1 is not None:
            self._v1.remove_constraint(self)
        if self._v2 is not None:
            self._v2.remove_constraint(self)
        if self._scale is not None:
            self._scale.remove_constraint(self)
        if self._offset is not None:
            self._offset.remove_constraint(self)
        self._direction = None

    def execute(self):
        if self._direction is _Direction.FORWARD:
            self._v2.value = self._v1.value * self._scale.value + self._offset.value
        else:
            self._v1.value = (self._v2.value - self._offset.value) / self._scale.value

    def inputs_do(self, fn):
        if self._direction is _Direction.FORWARD:
            fn(self._v1)
            fn(self._scale)
            fn(self._offset)
        else:
            fn(self._v2)
            fn(self._scale)
            fn(self._offset)

    def recalculate(self):
        if self._direction is _Direction.FORWARD:
            input_ = self._v1
            output = self._v2
        else:
            output = self._v1
            input_ = self._v2
        output.walk_strength = self.strength.weakest(input_.walk_strength)
        output.stay = input_.stay and self._scale.stay and self._offset.stay
        if output.stay:
            self.execute()


class _StayConstraint(_UnaryConstraint):
    def execute(self):
        pass


# --- Variable ------------------------------------------------------------
class _Variable:
    def __init__(self, value=0):
        self.value = value
        self.constraints = Vector(2)
        self.determined_by = None
        self.mark = 0
        self.walk_strength = _absolute_weakest
        self.stay = True

    def add_constraint(self, c):
        self.constraints.append(c)

    def remove_constraint(self, c):
        self.constraints.remove(c)
        if self.determined_by is c:
            self.determined_by = None


# --- Plan ----------------------------------------------------------------
class _Plan(Vector):
    def __init__(self):
        super().__init__(15)

    def execute(self):
        self.for_each(lambda c: c.execute())


# --- Planner -------------------------------------------------------------
class _Planner:
    def __init__(self):
        self._current_mark = 1

    def incremental_add(self, c):
        mark = self._new_mark()
        overridden = c.satisfy(mark, self)
        while overridden is not None:
            overridden = overridden.satisfy(mark, self)

    def incremental_remove(self, c):
        out = c.get_output()
        c.mark_unsatisfied()
        c.remove_from_graph()
        unsatisfied = self._remove_propagate_from(out)
        unsatisfied.for_each(self.incremental_add)

    def extract_plan_from_constraints(self, constraints):
        sources = Vector()

        def each(c):
            if c.is_input() and c.is_satisfied():
                sources.append(c)

        constraints.for_each(each)
        return self._make_plan(sources)

    def _make_plan(self, sources):
        mark = self._new_mark()
        plan = _Plan()
        todo = sources
        while not todo.is_empty():
            c = todo.remove_first()
            if c.get_output().mark != mark and c.inputs_known(mark):
                plan.append(c)
                c.get_output().mark = mark
                self._add_constraints_consuming_to(c.get_output(), todo)
        return plan

    def propagate_from(self, v):
        todo = Vector()
        self._add_constraints_consuming_to(v, todo)
        while not todo.is_empty():
            c = todo.remove_first()
            c.execute()
            self._add_constraints_consuming_to(c.get_output(), todo)

    @staticmethod
    def _add_constraints_consuming_to(v, coll):
        determining_c = v.determined_by

        def each(c):
            if c is not determining_c and c.is_satisfied():
                coll.append(c)

        v.constraints.for_each(each)

    def add_propagate(self, c, mark):
        todo = vector_with(c)
        while not todo.is_empty():
            d = todo.remove_first()
            if d.get_output().mark == mark:
                self.incremental_remove(c)
                return False
            d.recalculate()
            self._add_constraints_consuming_to(d.get_output(), todo)
        return True

    def change(self, var, new_value):
        edit_c = _EditConstraint(var, _PREFERRED, self)
        edit_v = vector_with(edit_c)
        plan = self.extract_plan_from_constraints(edit_v)
        for _ in range(10):
            var.value = new_value
            plan.execute()
        edit_c.destroy_constraint(self)

    @staticmethod
    def _constraints_consuming(v, fn):
        determining_c = v.determined_by

        def each(c):
            if c is not determining_c and c.is_satisfied():
                fn(c)

        v.constraints.for_each(each)

    def _new_mark(self):
        self._current_mark += 1
        return self._current_mark

    def _remove_propagate_from(self, out):
        unsatisfied = Vector()
        out.determined_by = None
        out.walk_strength = _absolute_weakest
        out.stay = True
        todo = vector_with(out)
        while not todo.is_empty():
            v = todo.remove_first()

            def each(c):
                if not c.is_satisfied():
                    unsatisfied.append(c)

            v.constraints.for_each(each)

            def recalc(c):
                c.recalculate()
                todo.append(c.get_output())

            self._constraints_consuming(v, recalc)

        def comp(c1, c2):
            return -1 if c1.strength.stronger(c2.strength) else 1

        unsatisfied.sort(comp)
        return unsatisfied

    @staticmethod
    def chain_test(n):
        planner = _Planner()
        variables = [None] * (n + 1)
        for i in range(n + 1):
            variables[i] = _Variable()
        for i in range(n):
            v1 = variables[i]
            v2 = variables[i + 1]
            _EqualityConstraint(v1, v2, _REQUIRED, planner)
        _StayConstraint(variables[n], _STRONG_DEFAULT, planner)
        edit_c = _EditConstraint(variables[0], _PREFERRED, planner)
        edit_v = vector_with(edit_c)
        plan = planner.extract_plan_from_constraints(edit_v)
        for i in range(100):
            variables[0].value = i
            plan.execute()
            if variables[n].value != i:
                raise Exception("Chain test failed!")
        edit_c.destroy_constraint(planner)

    @staticmethod
    def projection_test(n):
        planner = _Planner()
        dests = Vector()
        scale = _Variable(10)
        offset = _Variable(1000)
        src = None
        dst = None
        for i in range(1, n + 1):
            src = _Variable(i)
            dst = _Variable(i)
            dests.append(dst)
            _StayConstraint(src, _DEFAULT, planner)
            _ScaleConstraint(src, scale, offset, dst, _REQUIRED, planner)
        planner.change(src, 17)
        if dst.value != 1170:
            raise Exception("Projection test 1 failed!")
        planner.change(dst, 1050)
        if src.value != 5:
            raise Exception("Projection test 2 failed!")
        planner.change(scale, 5)
        for i in range(n - 1):
            if dests.at(i).value != (i + 1) * 5 + 1000:
                raise Exception("Projection test 3 failed!")
        planner.change(offset, 2000)
        for i in range(n - 1):
            if dests.at(i).value != (i + 1) * 5 + 2000:
                raise Exception("Projection test 4 failed!")


_Planner.chain_test(_CHAIN_N)
_Planner.projection_test(_PROJECTION_N)
print("ok")
