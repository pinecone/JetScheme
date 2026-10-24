# SPDX-License-Identifier: Apache-2.0
# Copyright 2011 Google Inc.
# Havlak loop recognition over a synthetic control-flow graph.

UNVISITED = 2_147_483_647
MAX_NON_BACK_PREDS = 32 * 1024
NONHEADER = 1
REDUCIBLE = 2
SELF = 3
IRREDUCIBLE = 4
DEAD = 5


class Block:
    def __init__(self, name):
        self.name = name
        self.in_edges = []
        self.out_edges = []


class CFG:
    def __init__(self):
        self.blocks = []
        self.start = None

    def node(self, name):
        while len(self.blocks) <= name:
            self.blocks.append(None)
        block = self.blocks[name]
        if block is None:
            block = Block(name)
            self.blocks[name] = block
            if self.start is None:
                self.start = block
        return block

    def connect(self, source, target):
        source = self.node(source)
        target = self.node(target)
        source.out_edges.append(target.name)
        target.in_edges.append(source.name)


class Loop:
    def __init__(self, ident, header, reducible):
        self.ident = ident
        self.header = header
        self.reducible = reducible
        self.parent = None
        self.children = []
        self.child_ids = set()
        self.blocks = set()
        self.nesting = 0
        self.depth = 0

    def add_child(self, child):
        if child.ident not in self.child_ids:
            self.child_ids.add(child.ident)
            self.children.append(child)

    def set_parent(self, parent):
        self.parent = parent
        parent.add_child(self)


class LoopGraph:
    def __init__(self):
        self.root = Loop(0, None, True)
        self.loops = [self.root]

    def new_loop(self, header, reducible):
        loop = Loop(len(self.loops), header, reducible)
        self.loops.append(loop)
        return loop

    def calculate_nesting(self):
        for loop in self.loops[1:]:
            if loop.parent is None:
                loop.set_parent(self.root)
        self._calculate(self.root, 0)

    def _calculate(self, loop, depth):
        loop.depth = depth
        for child in loop.children:
            self._calculate(child, depth + 1)
            loop.nesting = max(loop.nesting, child.nesting + 1)


class UnionNode:
    def __init__(self, block, number):
        self.parent = number
        self.block = block
        self.number = number
        self.loop = None


class Finder:
    def __init__(self, cfg, loops):
        self.cfg = cfg
        self.loops = loops
        self.non_back = []
        self.back = []
        self.number = {}
        self.header = []
        self.kind = []
        self.last = []
        self.nodes = []

    def find_set(self, number):
        node = number
        path = []
        while self.nodes[node].parent != node:
            path.append(node)
            node = self.nodes[node].parent
        for item in path:
            self.nodes[item].parent = node
        return node

    def dfs(self, block_id, number):
        self.nodes[number] = UnionNode(block_id, number)
        self.number[block_id] = number
        last = number
        for target in self.cfg.blocks[block_id].out_edges:
            if self.number[target] == UNVISITED:
                last = self.dfs(target, last + 1)
        self.last[number] = last
        return last

    def find_loops(self):
        if self.cfg.start is None:
            return
        size = len(self.cfg.blocks)
        self.non_back = [set() for _ in range(size)]
        self.back = [[] for _ in range(size)]
        self.number = {block.name: UNVISITED for block in self.cfg.blocks}
        self.header = [0] * size
        self.kind = [NONHEADER] * size
        self.last = [0] * size
        self.nodes = [None] * size
        self.dfs(self.cfg.start.name, 0)

        for w in range(size):
            node = self.nodes[w]
            if node is None:
                self.kind[w] = DEAD
                continue
            for source in self.cfg.blocks[node.block].in_edges:
                v = self.number[source]
                if v != UNVISITED:
                    if w <= v <= self.last[w]:
                        self.back[w].append(v)
                    else:
                        self.non_back[w].add(v)

        self.header[0] = 0
        for w in range(size - 1, -1, -1):
            node_w = self.nodes[w]
            pool = []
            pool_ids = set()
            if node_w is not None:
                for v in self.back[w]:
                    if v == w:
                        self.kind[w] = SELF
                    else:
                        root = self.find_set(v)
                        if root not in pool_ids:
                            pool_ids.add(root)
                            pool.append(root)

                at = 0
                if pool:
                    self.kind[w] = REDUCIBLE
                while at < len(pool):
                    x = pool[at]
                    at += 1
                    if len(self.non_back[self.nodes[x].number]) > MAX_NON_BACK_PREDS:
                        return
                    for pred in self.non_back[self.nodes[x].number]:
                        root = self.find_set(pred)
                        if not (w <= self.nodes[root].number <= self.last[w]):
                            self.kind[w] = IRREDUCIBLE
                            self.non_back[w].add(self.nodes[root].number)
                        elif root != w and root not in pool_ids:
                            pool_ids.add(root)
                            pool.append(root)

            if pool or self.kind[w] == SELF:
                loop = self.loops.new_loop(node_w.block, self.kind[w] != IRREDUCIBLE)
                loop.blocks.add(node_w.block)
                self.nodes[w].loop = loop
                for number in pool:
                    node = self.nodes[number]
                    self.header[node.number] = w
                    node.parent = w
                    if node.loop is not None:
                        node.loop.set_parent(loop)
                    else:
                        loop.blocks.add(node.block)


def build_diamond(cfg, start):
    cfg.connect(start, start + 1)
    cfg.connect(start, start + 2)
    cfg.connect(start + 1, start + 3)
    cfg.connect(start + 2, start + 3)
    return start + 3


def build_straight(cfg, start, length):
    for offset in range(length):
        cfg.connect(start + offset, start + offset + 1)
    return start + length


def build_base_loop(cfg, start):
    header = build_straight(cfg, start, 1)
    diamond1 = build_diamond(cfg, header)
    middle = build_straight(cfg, diamond1, 1)
    diamond2 = build_diamond(cfg, middle)
    footer = build_straight(cfg, diamond2, 1)
    cfg.connect(diamond2, middle)
    cfg.connect(diamond1, header)
    cfg.connect(footer, start)
    return build_straight(cfg, footer, 1)


def run():
    cfg = CFG()
    loops = LoopGraph()
    cfg.node(0)
    build_base_loop(cfg, 0)
    cfg.node(1)
    cfg.connect(0, 2)

    Finder(cfg, loops).find_loops()

    n = 2
    for _ in range(10):
        cfg.node(n + 1)
        cfg.connect(2, n + 1)
        n += 1
        for _ in range(10):
            top = n
            n = build_straight(cfg, n, 1)
            for _ in range(5):
                n = build_base_loop(cfg, n)
            bottom = build_straight(cfg, n, 1)
            cfg.connect(n, top)
            n = bottom
        cfg.connect(n, 1)

    Finder(cfg, loops).find_loops()
    for _ in range(50):
        Finder(cfg, LoopGraph()).find_loops()
    loops.calculate_nesting()
    return len(loops.loops), len(cfg.blocks)


result = run()
if result != (1605, 5213):
    raise RuntimeError(f"verify failed: {result}")
print("ok")
