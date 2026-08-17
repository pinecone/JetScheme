#!/usr/bin/env python3
# Port of the ARES-6 "Basic" benchmark (a BASIC interpreter) to Python.
# Generator-driven evaluator that yields I/O events, hand-scanning lexer,
# recursive-descent parser, case-insensitive variable maps, nested-array DIM.
import math
import random


class BasicError(Exception):
    pass


# ---------------- RNG: Robert Jenkins' 32-bit integer hash (from Octane) ----------------
def create_rng(seed):
    def rng():
        nonlocal seed
        seed = ((seed + 0x7ed55d16) + (seed << 12)) & 0xffffffff
        seed = ((seed ^ 0xc761c23c) ^ (seed >> 19)) & 0xffffffff
        seed = ((seed + 0x165667b1) + (seed << 5)) & 0xffffffff
        seed = ((seed + 0xd3a2646c) ^ (seed << 9)) & 0xffffffff
        seed = ((seed + 0xfd7046c5) + (seed << 3)) & 0xffffffff
        seed = ((seed ^ 0xb55a4f09) ^ (seed >> 16)) & 0xffffffff
        return (seed & 0x0fffffff) / 0x10000000
    return rng


def create_rng_with_fixed_seed():
    return create_rng(49734321)


def create_rng_with_random_seed():
    return create_rng(int(random.random() * 4294967296))


def sign(x):
    return 1.0 if x > 0 else (-1.0 if x < 0 else 0.0)


def num_to_str(x):
    if x != x:
        return "NaN"
    if x == math.floor(x) and abs(x) < 1e21:
        return str(int(x))
    return repr(x)


# ---------------- values ----------------
class CaselessMap:
    def __init__(self, other=None):
        self._map = dict(other._map) if other is not None else {}

    def set(self, key, value):
        self._map[key.lower()] = value

    def has(self, key):
        return key.lower() in self._map

    def get(self, key):
        return self._map.get(key.lower())


class NumberValue:
    def __init__(self, value=0.0):
        self._value = value

    @property
    def value(self):
        return self._value

    def apply(self, state, parameters):
        state.validate(len(parameters) == 0, "Should not pass arguments to simple numeric variables")
        return self._value

    def left_apply(self, state, parameters):
        state.validate(len(parameters) == 0, "Should not pass arguments to simple numeric variables")
        return self

    def assign(self, value):
        self._value = value


class NumberArray:
    def __init__(self, dim=None):
        if dim is None:
            dim = [11]

        def allocate(index):
            count = int(dim[index])
            if index + 1 < len(dim):
                return [allocate(index + 1) for _ in range(count)]
            return [NumberValue() for _ in range(count)]

        self._array = allocate(0)
        self._dim = dim

    def apply(self, state, parameters):
        return self.left_apply(state, parameters).apply(state, [])

    def left_apply(self, state, parameters):
        if len(self._dim) != len(parameters):
            state.abort("Expected " + str(len(self._dim)) + " arguments but " + str(len(parameters)) + " were passed.")
        result = self._array
        for value in parameters:
            index = math.floor(value)
            if not (index >= state.program["base"]) or not (index < len(result)):
                state.abort("Index out of bounds: " + str(index))
            result = result[index]
        return result


class NumberFunction:
    def __init__(self, parameters, code):
        self._parameters = parameters
        self._code = code

    def apply(self, state, parameters):
        if len(self._parameters) != len(parameters):
            state.abort("Wrong number of arguments")
        old_values = state.values
        state.values = CaselessMap(old_values if isinstance(old_values, CaselessMap) else None)
        for i in range(len(parameters)):
            state.values.set(self._parameters[i], parameters[i])
        result = evaluate(self._code, state)
        state.values = old_values
        return result

    def left_apply(self, state, parameters):
        state.abort("Cannot use a function as an lvalue")


class NativeFunction:
    def __init__(self, arity, callback):
        self._arity = arity
        self._callback = callback

    def apply(self, state, parameters):
        if self._arity != len(parameters):
            state.abort("Expected " + str(self._arity) + " arguments but " + str(len(parameters)) + " were passed")
        return self._callback(*parameters)

    def left_apply(self, state, parameters):
        state.abort("Cannot use a native function as an lvalue")


class State:
    def __init__(self, program):
        self.values = CaselessMap()
        self.string_values = CaselessMap()
        self.statement = None
        self.next_line_number = 0
        self.sub_stack = []
        self.data_index = 0
        self.program = program
        self.rng = create_rng_with_fixed_seed()

        def add_native(name, arity, callback):
            self.values.set(name, NativeFunction(arity, callback))

        add_native("abs", 1, lambda x: abs(x))
        add_native("atn", 1, math.atan)
        add_native("cos", 1, math.cos)
        add_native("exp", 1, math.exp)
        add_native("int", 1, lambda x: float(math.floor(x)))
        add_native("log", 1, math.log)
        add_native("rnd", 0, lambda: self.rng())
        add_native("sgn", 1, sign)
        add_native("sin", 1, math.sin)
        add_native("sqr", 1, math.sqrt)
        add_native("tan", 1, math.tan)

    def get_value(self, name, num_parameters):
        if self.values.has(name):
            return self.values.get(name)
        if num_parameters == 0:
            result = NumberValue()
        else:
            result = NumberArray([11] * num_parameters)
        self.values.set(name, result)
        return result

    def abort(self, text):
        if not self.statement:
            raise BasicError("At beginning of execution: " + text)
        raise BasicError("At " + str(self.statement["sourceLineNumber"]) + ": " + text)

    def validate(self, predicate, text):
        if not predicate:
            self.abort(text)


# ---------------- expression evaluators ----------------
def evaluate(node, state):
    return node["ev"](node, state)


def e_number_apply(node, state):
    parameters = [evaluate(p, state) for p in node["parameters"]]
    return state.get_value(node["name"], len(parameters)).apply(state, parameters)


def e_variable(node, state):
    parameters = [evaluate(p, state) for p in node["parameters"]]
    return state.get_value(node["name"], len(parameters)).left_apply(state, parameters)


def e_const(node, state):
    return node["value"]


def e_pow(node, state):
    return math.pow(evaluate(node["left"], state), evaluate(node["right"], state))


def e_mul(node, state):
    return evaluate(node["left"], state) * evaluate(node["right"], state)


def e_div(node, state):
    return evaluate(node["left"], state) / evaluate(node["right"], state)


def e_neg(node, state):
    return -evaluate(node["term"], state)


def e_add(node, state):
    return evaluate(node["left"], state) + evaluate(node["right"], state)


def e_sub(node, state):
    return evaluate(node["left"], state) - evaluate(node["right"], state)


def e_string_var(node, state):
    value = state.string_values.get(node["name"])
    if value is None:
        state.abort("Could not find string variable " + node["name"])
    return value


def e_equals(node, state):
    return evaluate(node["left"], state) == evaluate(node["right"], state)


def e_not_equals(node, state):
    return evaluate(node["left"], state) != evaluate(node["right"], state)


def e_less_than(node, state):
    return evaluate(node["left"], state) < evaluate(node["right"], state)


def e_greater_than(node, state):
    return evaluate(node["left"], state) > evaluate(node["right"], state)


def e_less_equal(node, state):
    return evaluate(node["left"], state) <= evaluate(node["right"], state)


def e_greater_equal(node, state):
    return evaluate(node["left"], state) >= evaluate(node["right"], state)


# ---------------- statement processors ----------------
# Most statements just mutate state and return None; only Print/Input yield events.
def s_goto(node, state):
    state.next_line_number = node["target"]


def s_gosub(node, state):
    state.sub_stack.append(state.next_line_number)
    state.next_line_number = node["target"]


def s_def(node, state):
    state.validate(not state.values.has(node["name"]), "Cannot redefine function")
    state.values.set(node["name"], NumberFunction(node["parameters"], node["expression"]))


def s_let(node, state):
    evaluate(node["variable"], state).assign(evaluate(node["expression"], state))


def s_if(node, state):
    if evaluate(node["condition"], state):
        state.next_line_number = node["target"]


def s_return(node, state):
    state.validate(len(state.sub_stack), "Not in a subroutine")
    state.next_line_number = state.sub_stack.pop()


def s_stop(node, state):
    state.next_line_number = None


def s_on(node, state):
    index = evaluate(node["expression"], state)
    if not (index >= 1) or not (index <= len(node["targets"])):
        state.abort("Index out of bounds: " + num_to_str(index))
    state.next_line_number = node["targets"][math.floor(index) - 1]


def s_for(node, state):
    side = {}
    node["sideState"] = side
    side["variable"] = state.get_value(node["variable"], 0).left_apply(state, [])
    side["initialValue"] = evaluate(node["initial"], state)
    side["limitValue"] = evaluate(node["limit"], state)
    side["stepValue"] = evaluate(node["step"], state)
    side["variable"].assign(side["initialValue"])

    def should_stop():
        return (side["variable"].value - side["limitValue"]) * sign(side["stepValue"]) > 0

    side["shouldStop"] = should_stop
    if should_stop():
        state.next_line_number = node["target"]["lineNumber"] + 1


def s_next(node, state):
    side = node["target"]["sideState"]
    side["variable"].assign(side["variable"].value + side["stepValue"])
    if side["shouldStop"]():
        return
    state.next_line_number = node["target"]["lineNumber"] + 1


def s_print(node, state):
    string = ""
    for item in node["items"]:
        kind = item["kind"]
        if kind == "comma":
            while len(string) % 14:
                string += " "
        elif kind == "tab":
            value = evaluate(item["value"], state)
            value = max(round(value), 1)
            while len(string) % value:
                string += " "
        elif kind == "string" or kind == "number":
            value = evaluate(item["value"], state)
            string += value if isinstance(value, str) else num_to_str(value)
        else:
            raise BasicError("Bad item kind: " + kind)
    yield {"kind": "output", "string": string}


def s_input(node, state):
    results = yield {"kind": "input", "numItems": len(node["items"])}
    state.validate(results is not None and len(results) == len(node["items"]),
                   "Input did not get the right number of items")
    for i in range(len(results)):
        evaluate(node["items"][i], state).assign(results[i])


def s_read(node, state):
    for item in node["items"]:
        state.validate(state.data_index < len(state.program["data"]), "Attempting to read past the end of data")
        evaluate(item, state).assign(state.program["data"][state.data_index])
        state.data_index += 1


def s_restore(node, state):
    state.data_index = 0


def s_dim(node, state):
    for item in node["items"]:
        state.validate(not state.values.has(item["name"]), "Variable " + item["name"] + " already exists")
        state.validate(len(item["bounds"]), "Dim statement is for arrays")
        state.values.set(item["name"], NumberArray([bound + 1 for bound in item["bounds"]]))


def s_randomize(node, state):
    state.rng = create_rng_with_random_seed()


def s_end(node, state):
    state.next_line_number = None


BLOCK_END = None  # set below once s_next / s_end exist


def program_process(program, state):
    statements = program["statements"]
    if not statements:
        return
    max_line = max(statements.keys())
    while state.next_line_number is not None:
        state.validate(state.next_line_number <= max_line, "Went out of bounds of the program")
        statement = statements.get(state.next_line_number)
        state.next_line_number += 1
        if statement is None or statement.get("proc") is None:
            continue
        state.statement = statement
        events = statement["proc"](statement, state)
        if events is not None:
            yield from events


BLOCK_END = {s_next, s_end}


# ---------------- lexer (hand-scanning) ----------------
KEYWORDS = {
    "base", "data", "def", "dim", "end", "for", "go", "gosub", "goto", "if", "input", "let",
    "next", "on", "option", "print", "randomize", "read", "restore", "return", "step", "stop",
    "sub", "then", "to",
}
OPERATOR_CHARS = set("-+*/^(),$;=<>")


def lex(string):
    source_line_number = 0
    for line in string.split("\n"):
        source_line_number += 1
        length = len(line)
        pos = 0
        while pos < length and line[pos].isspace():
            pos += 1
        start = pos
        while pos < length and line[pos].isdigit():
            pos += 1
        if pos == start:
            raise BasicError("At line " + str(source_line_number) + ": Expect line number: " + line[pos:])
        user_line_number = int(line[start:pos])
        yield {"kind": "userLineNumber", "string": line[start:pos],
               "sourceLineNumber": source_line_number, "userLineNumber": user_line_number}
        while pos < length and line[pos].isspace():
            pos += 1
        while pos < length:
            char = line[pos]
            if char.isalpha() or char == "_":
                start = pos
                pos += 1
                while pos < length and (line[pos].isalnum() or line[pos] == "_"):
                    pos += 1
                word = line[start:pos]
                kind = "keyword" if word.lower() in KEYWORDS else "identifier"
                yield {"kind": kind, "string": word,
                       "sourceLineNumber": source_line_number, "userLineNumber": user_line_number}
            elif char.isdigit() or (char == "." and pos + 1 < length and line[pos + 1].isdigit()):
                start = pos
                if char == ".":
                    pos += 1
                    while pos < length and line[pos].isdigit():
                        pos += 1
                else:
                    while pos < length and line[pos].isdigit():
                        pos += 1
                    if pos < length and line[pos] == ".":
                        pos += 1
                        while pos < length and line[pos].isdigit():
                            pos += 1
                text = line[start:pos]
                yield {"kind": "number", "string": text, "value": float(text),
                       "sourceLineNumber": source_line_number, "userLineNumber": user_line_number}
            elif char == '"':
                i = pos + 1
                value = []
                while i < length:
                    if line[i] == '"':
                        if i + 1 < length and line[i + 1] == '"':
                            value.append('"')
                            i += 2
                        else:
                            break
                    else:
                        value.append(line[i])
                        i += 1
                raw = line[pos:i + 1]
                yield {"kind": "string", "string": raw, "value": "".join(value),
                       "sourceLineNumber": source_line_number, "userLineNumber": user_line_number}
                pos = i + 1
            else:
                two = line[pos:pos + 2]
                if two in ("<>", "<=", ">="):
                    op = two
                elif char in OPERATOR_CHARS:
                    op = char
                else:
                    raise BasicError("At line " + str(source_line_number) + ": Cannot lex token: " + line[pos:])
                yield {"kind": "operator", "string": op,
                       "sourceLineNumber": source_line_number, "userLineNumber": user_line_number}
                pos += len(op)
            while pos < length and line[pos].isspace():
                pos += 1
        yield {"kind": "newLine", "string": "\n",
               "sourceLineNumber": source_line_number, "userLineNumber": user_line_number}


# ---------------- parser ----------------
class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.push_back = []
        self.program = None

    def next_token(self):
        if self.push_back:
            return self.push_back.pop()
        try:
            return next(self.tokens)
        except StopIteration:
            return {"kind": "endOfFile", "string": "<end of file>"}

    def push_token(self, token):
        self.push_back.append(token)

    def peek_token(self):
        token = self.next_token()
        self.push_token(token)
        return token

    def consume_kind(self, kind):
        token = self.next_token()
        if token["kind"] != kind:
            raise BasicError("At " + str(token.get("sourceLineNumber")) + ": expected " + kind + " but got: " + token["string"])
        return token

    def consume_token(self, string):
        token = self.next_token()
        if token["string"].lower() != string.lower():
            raise BasicError("At " + str(token.get("sourceLineNumber")) + ": expected " + string + " but got: " + token["string"])
        return token

    def parse_variable(self):
        name = self.consume_kind("identifier")["string"]
        result = {"ev": e_variable, "name": name, "parameters": []}
        if self.peek_token()["string"] == "(":
            while True:
                self.next_token()
                result["parameters"].append(self.parse_numeric_expression())
                if self.peek_token()["string"] != ",":
                    break
            self.consume_token(")")
        return result

    def parse_primary(self):
        token = self.next_token()
        if token["kind"] == "identifier":
            result = {"ev": e_number_apply, "name": token["string"], "parameters": []}
            if self.peek_token()["string"] == "(":
                while True:
                    self.next_token()
                    result["parameters"].append(self.parse_numeric_expression())
                    if self.peek_token()["string"] != ",":
                        break
                self.consume_token(")")
            return result
        if token["kind"] == "number":
            return {"ev": e_const, "value": token["value"]}
        if token["kind"] == "operator" and token["string"] == "(":
            result = self.parse_numeric_expression()
            self.consume_token(")")
            return result
        raise BasicError("At " + str(token.get("sourceLineNumber")) + ": expected identifier, number, or (, but got: " + token["string"])

    def parse_factor(self):
        primary = self.parse_primary()
        while self.peek_token()["string"] == "^":
            self.next_token()
            primary = {"ev": e_pow, "left": primary, "right": self.parse_primary()}
        return primary

    def parse_term(self):
        factor = self.parse_factor()
        while True:
            string = self.peek_token()["string"]
            if string == "*":
                self.next_token()
                factor = {"ev": e_mul, "left": factor, "right": self.parse_factor()}
            elif string == "/":
                self.next_token()
                factor = {"ev": e_div, "left": factor, "right": self.parse_factor()}
            else:
                break
        return factor

    def parse_numeric_expression(self):
        negate = False
        string = self.peek_token()["string"]
        if string == "+":
            self.next_token()
        elif string == "-":
            negate = True
            self.next_token()
        term = self.parse_term()
        if negate:
            term = {"ev": e_neg, "term": term}
        while True:
            string = self.peek_token()["string"]
            if string == "+":
                self.next_token()
                term = {"ev": e_add, "left": term, "right": self.parse_term()}
            elif string == "-":
                self.next_token()
                term = {"ev": e_sub, "left": term, "right": self.parse_term()}
            else:
                break
        return term

    def parse_constant(self):
        string = self.peek_token()["string"]
        if string == "+":
            self.next_token()
            return self.consume_kind("number")["value"]
        if string == "-":
            self.next_token()
            return -self.consume_kind("number")["value"]
        if self.is_string_expression():
            return self.consume_kind("string")["value"]
        return self.consume_kind("number")["value"]

    def parse_string_expression(self):
        token = self.next_token()
        if token["kind"] == "string":
            return {"ev": e_const, "value": token["value"]}
        if token["kind"] == "identifier":
            self.consume_token("$")
            return {"ev": e_string_var, "name": token["string"]}
        raise BasicError("At " + str(token.get("sourceLineNumber")) + ": expected string expression but got " + token["string"])

    def is_string_expression(self):
        token = self.next_token()
        if token["kind"] == "string":
            self.push_token(token)
            return True
        if token["kind"] == "identifier":
            result = self.peek_token()["string"] == "$"
            self.push_token(token)
            return result
        self.push_token(token)
        return False

    def parse_relational_expression(self):
        if self.is_string_expression():
            left = self.parse_string_expression()
            operator = self.next_token()
            if operator["string"] == "=":
                ev = e_equals
            elif operator["string"] == "<>":
                ev = e_not_equals
            else:
                raise BasicError("At " + str(operator.get("sourceLineNumber")) + ": expected a string comparison operator but got: " + operator["string"])
            return {"ev": ev, "left": left, "right": self.parse_string_expression()}
        left = self.parse_numeric_expression()
        operator = self.next_token()
        table = {"=": e_equals, "<>": e_not_equals, "<": e_less_than,
                 ">": e_greater_than, "<=": e_less_equal, ">=": e_greater_equal}
        if operator["string"] not in table:
            raise BasicError("At " + str(operator.get("sourceLineNumber")) + ": expected a numeric comparison operator but got: " + operator["string"])
        return {"ev": table[operator["string"]], "left": left, "right": self.parse_numeric_expression()}

    def parse_non_negative_integer(self):
        token = self.next_token()
        if not (token["string"] and all(c in "0123456789" for c in token["string"])):
            raise BasicError("At " + str(token.get("sourceLineNumber")) + ": expected a line number but got: " + token["string"])
        return token["value"]

    def parse_goto(self, st):
        st["proc"] = s_goto
        st["target"] = self.parse_non_negative_integer()

    def parse_gosub(self, st):
        st["proc"] = s_gosub
        st["target"] = self.parse_non_negative_integer()

    def parse_statement(self):
        st = {}
        st["lineNumber"] = self.consume_kind("userLineNumber")["userLineNumber"]
        self.program["statements"][st["lineNumber"]] = st
        command = self.next_token()
        st["sourceLineNumber"] = command["sourceLineNumber"]
        if command["kind"] == "keyword":
            kw = command["string"].lower()
            if kw == "def":
                st["proc"] = s_def
                st["name"] = self.consume_kind("identifier")["string"]
                st["parameters"] = []
                if self.peek_token()["string"] == "(":
                    while True:
                        self.next_token()
                        st["parameters"].append(self.consume_kind("identifier")["string"])
                        if self.peek_token()["string"] != ",":
                            break
                st["expression"] = self.parse_numeric_expression()
            elif kw == "let":
                st["proc"] = s_let
                st["variable"] = self.parse_variable()
                self.consume_token("=")
                st["expression"] = self.parse_numeric_expression()
            elif kw == "go":
                nxt = self.next_token()
                if nxt["string"] == "to":
                    self.parse_goto(st)
                elif nxt["string"] == "sub":
                    self.parse_gosub(st)
                else:
                    raise BasicError("At " + str(nxt.get("sourceLineNumber")) + ": expected to or sub but got: " + nxt["string"])
            elif kw == "goto":
                self.parse_goto(st)
            elif kw == "gosub":
                self.parse_gosub(st)
            elif kw == "if":
                st["proc"] = s_if
                st["condition"] = self.parse_relational_expression()
                self.consume_token("then")
                st["target"] = self.parse_non_negative_integer()
            elif kw == "return":
                st["proc"] = s_return
            elif kw == "stop":
                st["proc"] = s_stop
            elif kw == "on":
                st["proc"] = s_on
                st["expression"] = self.parse_numeric_expression()
                if self.peek_token()["string"] == "go":
                    self.consume_token("go")
                    self.consume_token("to")
                else:
                    self.consume_token("goto")
                st["targets"] = []
                while True:
                    st["targets"].append(self.parse_non_negative_integer())
                    if self.peek_token()["string"] != ",":
                        break
                    self.next_token()
            elif kw == "for":
                st["proc"] = s_for
                st["variable"] = self.consume_kind("identifier")["string"]
                self.consume_token("=")
                st["initial"] = self.parse_numeric_expression()
                self.consume_token("to")
                st["limit"] = self.parse_numeric_expression()
                if self.peek_token()["string"] == "step":
                    self.next_token()
                    st["step"] = self.parse_numeric_expression()
                else:
                    st["step"] = {"ev": e_const, "value": 1.0}
                self.consume_kind("newLine")
                last = self.parse_statements()
                if last.get("proc") is not s_next:
                    raise BasicError("At " + str(last.get("sourceLineNumber")) + ": expected next statement")
                if last["variable"] != st["variable"]:
                    raise BasicError("At " + str(last.get("sourceLineNumber")) + ": expected next for " + st["variable"] + " but got " + last["variable"])
                last["target"] = st
                st["target"] = last
                return st
            elif kw == "next":
                st["proc"] = s_next
                st["variable"] = self.consume_kind("identifier")["string"]
            elif kw == "print":
                st["proc"] = s_print
                st["items"] = []
                while True:
                    string = self.peek_token()["string"]
                    if string == ",":
                        self.next_token()
                        st["items"].append({"kind": "comma"})
                    elif string == ";":
                        self.next_token()
                    elif string == "tab":
                        self.next_token()
                        self.consume_token("(")
                        st["items"].append({"kind": "tab", "value": self.parse_numeric_expression()})
                    elif string == "\n":
                        break
                    else:
                        if self.is_string_expression():
                            st["items"].append({"kind": "string", "value": self.parse_string_expression()})
                        else:
                            st["items"].append({"kind": "number", "value": self.parse_numeric_expression()})
            elif kw == "input":
                st["proc"] = s_input
                st["items"] = []
                while True:
                    st["items"].append(self.parse_variable())
                    if self.peek_token()["string"] != ",":
                        break
                    self.next_token()
            elif kw == "read":
                st["proc"] = s_read
                st["items"] = []
                while True:
                    st["items"].append(self.parse_variable())
                    if self.peek_token()["string"] != ",":
                        break
                    self.next_token()
            elif kw == "restore":
                st["proc"] = s_restore
            elif kw == "data":
                while True:
                    self.program["data"].append(self.parse_constant())
                    if self.peek_token()["string"] != ",":
                        break
                    self.next_token()
            elif kw == "dim":
                st["proc"] = s_dim
                st["items"] = []
                while True:
                    name = self.consume_kind("identifier")["string"]
                    self.consume_token("(")
                    bounds = [self.parse_non_negative_integer()]
                    if self.peek_token()["string"] == ",":
                        self.next_token()
                        bounds.append(self.parse_non_negative_integer())
                    self.consume_token(")")
                    st["items"].append({"name": name, "bounds": bounds})
                    if self.peek_token()["string"] != ",":
                        break
                    self.consume_token(",")
            elif kw == "option":
                self.consume_token("base")
                base = self.parse_non_negative_integer()
                if base != 0 and base != 1:
                    raise BasicError("At " + str(command["sourceLineNumber"]) + ": unexpected base: " + str(base))
                self.program["base"] = base
            elif kw == "randomize":
                st["proc"] = s_randomize
            elif kw == "end":
                st["proc"] = s_end
            else:
                raise BasicError("At " + str(command["sourceLineNumber"]) + ": unexpected command but got: " + command["string"])
        elif command["kind"] == "remark":
            pass
        else:
            raise BasicError("At " + str(command["sourceLineNumber"]) + ": expected command but got: " + command["string"] + " (of kind " + command["kind"] + ")")
        self.consume_kind("newLine")
        return st

    def parse_statements(self):
        while True:
            st = self.parse_statement()
            proc = st.get("proc")
            if proc is not None and proc in BLOCK_END:
                break
        return st

    def make_program(self):
        self.program = {"process": program_process, "statements": {}, "data": [], "base": 0}
        last = self.parse_statements()
        if last.get("proc") is not s_end:
            raise BasicError("At " + str(last.get("sourceLineNumber")) + ": expected end")
        return self.program


# ---------------- glue ----------------
def prepare(string):
    program = Parser(lex(string)).make_program()
    state = State(program)
    return program_process(program, state)


def simulate(generator, inputs):
    result = ""
    send_value = None
    while True:
        try:
            event = generator.send(send_value)
        except StopIteration:
            break
        send_value = None
        if event["kind"] == "output":
            result += event["string"] + "\n"
        elif event["kind"] == "input":
            count = event["numItems"]
            send_value = inputs[:count]
            del inputs[:count]
    return result


def run_iteration(cases):
    for case in cases:
        result = simulate(prepare(case["src"]), [])
        if result != case["expected"]:
            raise BasicError("Program produced " + repr(result) + " but we expected " + repr(case["expected"]))


ITERS = 200

CASES = [{'src': '10 print "hello, world!"\n20 end', 'expected': 'hello, world!\n'}, {'src': '10 let x = 0\n20 let x = x + 1\n30 print x\n40 if x < 10 then 20\n50 end', 'expected': '1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n'}, {'src': '10 print int(rnd * 100)\n20 end\n', 'expected': '98\n'}, {'src': '10 let value = int(rnd * 2000)\n20 print value\n30 if value <> 100 then 10\n40 end', 'expected': '1974\n697\n1126\n1998\n1658\n264\n1650\n1677\n226\n117\n492\n861\n877\n1969\n38\n1039\n197\n1261\n1102\n1522\n916\n1683\n1943\n1835\n476\n1898\n939\n176\n966\n908\n474\n614\n1326\n564\n1916\n728\n524\n162\n1303\n758\n832\n1279\n1856\n1876\n982\n6\n1613\n1781\n681\n1238\n494\n1583\n1953\n788\n1026\n347\n1116\n1465\n514\n583\n463\n1970\n1573\n412\n1256\n1453\n838\n1538\n1984\n1598\n209\n411\n1700\n546\n861\n91\n132\n884\n378\n693\n11\n433\n1719\n860\n164\n472\n231\n1786\n806\n811\n106\n1697\n118\n980\n890\n1199\n227\n1667\n1933\n1903\n1390\n1595\n923\n1746\n39\n1361\n117\n1297\n923\n901\n1180\n818\n1444\n269\n933\n327\n1744\n1082\n1527\n1260\n622\n528\n318\n856\n296\n1796\n1574\n585\n1871\n111\n827\n1725\n1320\n1868\n1695\n1914\n216\n63\n1847\n156\n671\n893\n127\n1867\n811\n279\n913\n310\n814\n907\n1363\n1624\n1670\n478\n714\n436\n355\n1484\n1628\n1208\n800\n611\n917\n829\n830\n273\n1791\n340\n214\n992\n1444\n442\n1555\n144\n1194\n282\n180\n1228\n1251\n1883\n678\n1555\n347\n72\n1661\n1828\n1090\n1183\n957\n1685\n930\n475\n103\n759\n1725\n1902\n1662\n1587\n61\n614\n863\n1418\n321\n1050\n505\n1622\n1425\n803\n589\n1511\n1098\n1051\n1554\n1898\n27\n747\n813\n1544\n332\n728\n1363\n771\n759\n1145\n1098\n1991\n385\n230\n520\n1369\n1840\n1285\n1562\n1845\n102\n760\n1874\n748\n361\n575\n277\n1661\n1764\n1117\n332\n757\n1766\n1722\n143\n474\n1507\n1294\n1180\n1578\n904\n845\n321\n496\n1911\n1784\n1116\n938\n1591\n1403\n1374\n533\n1085\n452\n708\n1096\n1634\n522\n564\n1397\n1357\n980\n978\n1760\n1088\n1361\n1184\n314\n1242\n217\n133\n1187\n1723\n646\n605\n591\n46\n135\n1420\n1821\n1147\n1211\n61\n244\n1307\n1551\n449\n1122\n1336\n140\n880\n22\n1155\n1326\n590\n1499\n1376\n112\n1771\n1897\n1071\n938\n1685\n1963\n1203\n1296\n804\n1275\n453\n1387\n482\n1262\n1883\n1381\n418\n1417\n1222\n1208\n1263\n632\n450\n1422\n1285\n1408\n644\n665\n275\n363\n1012\n165\n354\n80\n609\n291\n1661\n1724\n117\n407\n59\n906\n1224\n136\n855\n1275\n1468\n482\n1537\n1283\n1784\n1568\n1832\n452\n867\n1546\n1467\n800\n45\n1225\n1890\n465\n1372\n47\n1608\n193\n1345\n1847\n1059\n1788\n518\n52\n1052\n1003\n1210\n1135\n1433\n519\n1558\n39\n1249\n1017\n39\n1713\n1449\n1245\n1354\n82\n1140\n916\n1595\n838\n607\n389\n1270\n821\n247\n1692\n1305\n1211\n1960\n429\n1703\n1635\n575\n1618\n1490\n1495\n682\n1256\n964\n420\n1520\n1429\n1997\n396\n382\n856\n1182\n296\n1295\n298\n1892\n990\n711\n934\n1939\n1339\n682\n1631\n1533\n742\n1520\n1281\n1332\n1042\n656\n1576\n1253\n1608\n375\n169\n14\n414\n1586\n1562\n1508\n1245\n303\n715\n1053\n340\n915\n160\n1796\n111\n925\n1872\n735\n350\n107\n1913\n1653\n987\n825\n1893\n1601\n460\n1228\n1526\n1613\n1359\n1854\n1352\n542\n665\n109\n1874\n467\n533\n1188\n1629\n851\n630\n1060\n1530\n1853\n743\n765\n126\n1540\n1411\n858\n1741\n284\n299\n577\n1848\n1495\n283\n1886\n284\n129\n1077\n1245\n1364\n1505\n176\n1012\n1663\n1306\n1586\n410\n315\n660\n256\n1102\n1289\n1292\n939\n762\n601\n1140\n574\n1851\n44\n560\n1948\n1142\n1787\n947\n948\n280\n1210\n1139\n1072\n1033\n92\n1244\n1589\n1079\n22\n1514\n163\n157\n1742\n1058\n514\n196\n1858\n565\n354\n1413\n792\n183\n526\n1724\n1007\n158\n1229\n1802\n99\n1514\n708\n1276\n1802\n1564\n1387\n1235\n1132\n715\n1584\n617\n1664\n1559\n1625\n1037\n601\n1175\n1713\n107\n88\n384\n1634\n904\n1835\n1472\n212\n1145\n443\n1617\n866\n1963\n937\n1917\n855\n1215\n1867\n520\n892\n1483\n1898\n1747\n1441\n289\n1609\n328\n566\n271\n458\n1616\n843\n1107\n507\n1090\n854\n1094\n806\n166\n408\n661\n334\n230\n1917\n1323\n927\n1912\n673\n311\n952\n1783\n1549\n1714\n1500\n450\n1498\n530\n442\n607\n609\n1226\n370\n1769\n1815\n788\n536\n293\n115\n947\n290\n1764\n243\n1219\n1851\n289\n599\n1528\n150\n1859\n297\n279\n1542\n1719\n1910\n551\n401\n952\n1764\n946\n1835\n647\n1309\n271\n275\n70\n129\n1518\n972\n1164\n816\n1125\n575\n588\n1456\n1154\n290\n1681\n1133\n561\n343\n1360\n1035\n1158\n1365\n744\n781\n58\n531\n271\n1612\n1774\n28\n1480\n1312\n1855\n666\n1574\n613\n42\n456\n351\n727\n1503\n1115\n333\n1972\n822\n1575\n848\n1087\n1262\n1671\n710\n460\n1816\n287\n172\n492\n1079\n582\n1236\n1756\n1792\n1095\n1205\n1894\n22\n1930\n1529\n1547\n1383\n1768\n364\n1108\n1972\n287\n200\n230\n1335\n187\n486\n1722\n20\n963\n792\n1114\n633\n1862\n1433\n829\n737\n215\n1570\n378\n1677\n944\n1301\n1160\n500\n150\n886\n1337\n662\n1062\n290\n460\n592\n1867\n872\n155\n1613\n1913\n1548\n1847\n855\n1702\n952\n1894\n587\n1813\n1021\n21\n654\n254\n910\n1696\n1606\n679\n1222\n696\n1319\n368\n447\n549\n905\n1194\n189\n1766\n616\n278\n1418\n1965\n872\n998\n1268\n1673\n1647\n1163\n533\n1650\n1849\n1124\n1252\n1412\n703\n944\n468\n1485\n1352\n681\n864\n1432\n1771\n497\n956\n1794\n363\n1099\n1804\n457\n1227\n1487\n446\n1993\n1576\n272\n709\n1810\n330\n876\n1107\n1187\n122\n1625\n472\n676\n314\n1257\n1509\n350\n741\n366\n33\n536\n293\n1663\n1039\n1527\n126\n923\n1937\n1767\n1302\n1510\n1518\n1343\n91\n1551\n1614\n1687\n1748\n137\n75\n738\n1977\n751\n237\n313\n566\n24\n202\n889\n1716\n1460\n129\n1760\n1597\n96\n1057\n1323\n1188\n1373\n537\n955\n65\n1679\n1441\n1315\n398\n647\n1470\n1335\n617\n331\n796\n129\n1635\n1497\n836\n855\n1472\n1828\n568\n862\n690\n1370\n1657\n819\n45\n420\n258\n1980\n672\n615\n358\n852\n1148\n1897\n1306\n1092\n1405\n719\n1752\n1456\n1338\n332\n351\n479\n747\n249\n1977\n1671\n1061\n1685\n306\n254\n1060\n764\n420\n1139\n1452\n426\n835\n929\n1424\n1336\n697\n191\n1697\n1897\n644\n546\n982\n359\n1201\n1095\n1623\n1947\n215\n10\n855\n297\n551\n1037\n945\n396\n211\n1059\n423\n1521\n1770\n203\n1828\n879\n1179\n1912\n1028\n1416\n1845\n698\n715\n1857\n817\n50\n473\n1122\n126\n70\n1773\n40\n1970\n1311\n826\n355\n1921\n23\n526\n1717\n1397\n1932\n1075\n1652\n997\n1039\n1481\n779\n415\n49\n1330\n317\n1701\n690\n245\n1824\n639\n799\n1240\n422\n344\n1639\n20\n546\n912\n1930\n1368\n1541\n1109\n369\n66\n1564\n444\n1928\n1963\n1899\n744\n1593\n1702\n100\n'}, {'src': '10 dim a(2000)\n20 for i = 2 to 2000\n30 let a(i) = 1\n40 next i\n50 for i = 2 to sqr(2000)\n60 if a(i) = 0 then 100\n70 for j = i ^ 2 to 2000 step i\n80 let a(j) = 0\n90 next j\n100 next i\n110 for i = 2 to 2000\n120 if a(i) = 0 then 140\n130 print i\n140 next i\n150 end\n', 'expected': '2\n3\n5\n7\n11\n13\n17\n19\n23\n29\n31\n37\n41\n43\n47\n53\n59\n61\n67\n71\n73\n79\n83\n89\n97\n101\n103\n107\n109\n113\n127\n131\n137\n139\n149\n151\n157\n163\n167\n173\n179\n181\n191\n193\n197\n199\n211\n223\n227\n229\n233\n239\n241\n251\n257\n263\n269\n271\n277\n281\n283\n293\n307\n311\n313\n317\n331\n337\n347\n349\n353\n359\n367\n373\n379\n383\n389\n397\n401\n409\n419\n421\n431\n433\n439\n443\n449\n457\n461\n463\n467\n479\n487\n491\n499\n503\n509\n521\n523\n541\n547\n557\n563\n569\n571\n577\n587\n593\n599\n601\n607\n613\n617\n619\n631\n641\n643\n647\n653\n659\n661\n673\n677\n683\n691\n701\n709\n719\n727\n733\n739\n743\n751\n757\n761\n769\n773\n787\n797\n809\n811\n821\n823\n827\n829\n839\n853\n857\n859\n863\n877\n881\n883\n887\n907\n911\n919\n929\n937\n941\n947\n953\n967\n971\n977\n983\n991\n997\n1009\n1013\n1019\n1021\n1031\n1033\n1039\n1049\n1051\n1061\n1063\n1069\n1087\n1091\n1093\n1097\n1103\n1109\n1117\n1123\n1129\n1151\n1153\n1163\n1171\n1181\n1187\n1193\n1201\n1213\n1217\n1223\n1229\n1231\n1237\n1249\n1259\n1277\n1279\n1283\n1289\n1291\n1297\n1301\n1303\n1307\n1319\n1321\n1327\n1361\n1367\n1373\n1381\n1399\n1409\n1423\n1427\n1429\n1433\n1439\n1447\n1451\n1453\n1459\n1471\n1481\n1483\n1487\n1489\n1493\n1499\n1511\n1523\n1531\n1543\n1549\n1553\n1559\n1567\n1571\n1579\n1583\n1597\n1601\n1607\n1609\n1613\n1619\n1621\n1627\n1637\n1657\n1663\n1667\n1669\n1693\n1697\n1699\n1709\n1721\n1723\n1733\n1741\n1747\n1753\n1759\n1777\n1783\n1787\n1789\n1801\n1811\n1823\n1831\n1847\n1861\n1867\n1871\n1873\n1877\n1879\n1889\n1901\n1907\n1913\n1931\n1933\n1949\n1951\n1973\n1979\n1987\n1993\n1997\n1999\n'}]


def main():
    for _ in range(ITERS):
        run_iteration(CASES)
    print("ok")


if __name__ == "__main__":
    main()
