// SPDX-License-Identifier: Apache-2.0
// Copyright 2011 Google Inc.
// Havlak loop recognition over a synthetic control-flow graph.

'use strict';

const UNVISITED = 2147483647;
const MAX_NON_BACK_PREDS = 32 * 1024;
const NONHEADER = 1;
const REDUCIBLE = 2;
const SELF = 3;
const IRREDUCIBLE = 4;
const DEAD = 5;

class Block {
  constructor(name) {
    this.name = name;
    this.inEdges = [];
    this.outEdges = [];
  }
}

class CFG {
  constructor() {
    this.blocks = [];
    this.start = null;
  }

  node(name) {
    let block = this.blocks[name];
    if (block === undefined) {
      block = new Block(name);
      this.blocks[name] = block;
      if (this.start === null) this.start = block;
    }
    return block;
  }

  connect(source, target) {
    source = this.node(source);
    target = this.node(target);
    source.outEdges.push(target.name);
    target.inEdges.push(source.name);
  }
}

class Loop {
  constructor(id, header, reducible) {
    this.id = id;
    this.header = header;
    this.reducible = reducible;
    this.parent = null;
    this.children = [];
    this.childIds = new Set();
    this.blocks = new Set();
    this.nesting = 0;
    this.depth = 0;
  }

  addChild(child) {
    if (!this.childIds.has(child.id)) {
      this.childIds.add(child.id);
      this.children.push(child);
    }
  }

  setParent(parent) {
    this.parent = parent;
    parent.addChild(this);
  }
}

class LoopGraph {
  constructor() {
    this.root = new Loop(0, null, true);
    this.loops = [this.root];
  }

  newLoop(header, reducible) {
    const loop = new Loop(this.loops.length, header, reducible);
    this.loops.push(loop);
    return loop;
  }

  calculateNesting() {
    for (let i = 1; i < this.loops.length; i += 1) {
      const loop = this.loops[i];
      if (loop.parent === null) loop.setParent(this.root);
    }
    this.calculate(this.root, 0);
  }

  calculate(loop, depth) {
    loop.depth = depth;
    for (const child of loop.children) {
      this.calculate(child, depth + 1);
      loop.nesting = Math.max(loop.nesting, child.nesting + 1);
    }
  }
}

class UnionNode {
  constructor(block, number) {
    this.parent = number;
    this.block = block;
    this.number = number;
    this.loop = null;
  }
}

class Finder {
  constructor(cfg, loops) {
    this.cfg = cfg;
    this.loops = loops;
  }

  findSet(number) {
    let node = number;
    const path = [];
    while (this.nodes[node].parent !== node) {
      path.push(node);
      node = this.nodes[node].parent;
    }
    for (const item of path) this.nodes[item].parent = node;
    return node;
  }

  dfs(blockId, number) {
    this.nodes[number] = new UnionNode(blockId, number);
    this.number.set(blockId, number);
    let last = number;
    for (const target of this.cfg.blocks[blockId].outEdges) {
      if (this.number.get(target) === UNVISITED) last = this.dfs(target, last + 1);
    }
    this.last[number] = last;
    return last;
  }

  findLoops() {
    if (this.cfg.start === null) return;
    const size = this.cfg.blocks.length;
    this.nonBack = Array.from({ length: size }, () => new Set());
    this.back = Array.from({ length: size }, () => []);
    this.number = new Map(this.cfg.blocks.map((block) => [block.name, UNVISITED]));
    this.header = new Array(size).fill(0);
    this.kind = new Array(size).fill(NONHEADER);
    this.last = new Array(size).fill(0);
    this.nodes = new Array(size).fill(null);
    this.dfs(this.cfg.start.name, 0);

    for (let w = 0; w < size; w += 1) {
      const node = this.nodes[w];
      if (node === null) {
        this.kind[w] = DEAD;
        continue;
      }
      for (const source of this.cfg.blocks[node.block].inEdges) {
        const v = this.number.get(source);
        if (v !== UNVISITED) {
          if (w <= v && v <= this.last[w]) this.back[w].push(v);
          else this.nonBack[w].add(v);
        }
      }
    }

    this.header[0] = 0;
    for (let w = size - 1; w >= 0; w -= 1) {
      const nodeW = this.nodes[w];
      const pool = [];
      const poolIds = new Set();
      if (nodeW !== null) {
        for (const v of this.back[w]) {
          if (v === w) this.kind[w] = SELF;
          else {
            const root = this.findSet(v);
            if (!poolIds.has(root)) {
              poolIds.add(root);
              pool.push(root);
            }
          }
        }

        if (pool.length !== 0) this.kind[w] = REDUCIBLE;
        for (let at = 0; at < pool.length; at += 1) {
          const x = pool[at];
          if (this.nonBack[this.nodes[x].number].size > MAX_NON_BACK_PREDS) return;
          for (const pred of this.nonBack[this.nodes[x].number]) {
            const root = this.findSet(pred);
            if (!(w <= this.nodes[root].number && this.nodes[root].number <= this.last[w])) {
              this.kind[w] = IRREDUCIBLE;
              this.nonBack[w].add(this.nodes[root].number);
            } else if (root !== w && !poolIds.has(root)) {
              poolIds.add(root);
              pool.push(root);
            }
          }
        }
      }

      if (pool.length !== 0 || this.kind[w] === SELF) {
        const loop = this.loops.newLoop(nodeW.block, this.kind[w] !== IRREDUCIBLE);
        loop.blocks.add(nodeW.block);
        this.nodes[w].loop = loop;
        for (const number of pool) {
          const node = this.nodes[number];
          this.header[node.number] = w;
          node.parent = w;
          if (node.loop !== null) node.loop.setParent(loop);
          else loop.blocks.add(node.block);
        }
      }
    }
  }
}

function buildDiamond(cfg, start) {
  cfg.connect(start, start + 1);
  cfg.connect(start, start + 2);
  cfg.connect(start + 1, start + 3);
  cfg.connect(start + 2, start + 3);
  return start + 3;
}

function buildStraight(cfg, start, length) {
  for (let offset = 0; offset < length; offset += 1) cfg.connect(start + offset, start + offset + 1);
  return start + length;
}

function buildBaseLoop(cfg, start) {
  const header = buildStraight(cfg, start, 1);
  const diamond1 = buildDiamond(cfg, header);
  const middle = buildStraight(cfg, diamond1, 1);
  const diamond2 = buildDiamond(cfg, middle);
  const footer = buildStraight(cfg, diamond2, 1);
  cfg.connect(diamond2, middle);
  cfg.connect(diamond1, header);
  cfg.connect(footer, start);
  return buildStraight(cfg, footer, 1);
}

function run() {
  const cfg = new CFG();
  const loops = new LoopGraph();
  cfg.node(0);
  buildBaseLoop(cfg, 0);
  cfg.node(1);
  cfg.connect(0, 2);
  new Finder(cfg, loops).findLoops();

  let n = 2;
  for (let i = 0; i < 10; i += 1) {
    cfg.node(n + 1);
    cfg.connect(2, n + 1);
    n += 1;
    for (let j = 0; j < 10; j += 1) {
      const top = n;
      n = buildStraight(cfg, n, 1);
      for (let k = 0; k < 5; k += 1) n = buildBaseLoop(cfg, n);
      const bottom = buildStraight(cfg, n, 1);
      cfg.connect(n, top);
      n = bottom;
    }
    cfg.connect(n, 1);
  }

  new Finder(cfg, loops).findLoops();
  for (let i = 0; i < 50; i += 1) new Finder(cfg, new LoopGraph()).findLoops();
  loops.calculateNesting();
  return [loops.loops.length, cfg.blocks.length];
}

const result = run();
if (result[0] !== 1605 || result[1] !== 5213) throw new Error(`verify failed: ${result}`);
