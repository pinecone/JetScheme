function splayNode(key, left, right) {
  return [key, left, right];
}

function splay(tree, key) {
  if (tree === null || key === tree[0]) {
    return tree;
  }
  const rootKey = tree[0];
  const left = tree[1];
  const right = tree[2];
  if (key < rootKey) {
    if (left === null) {
      return tree;
    }
    const leftKey = left[0];
    const leftLeft = left[1];
    const leftRight = left[2];
    if (key === leftKey) {
      return splayNode(leftKey, leftLeft, splayNode(rootKey, leftRight, right));
    }
    if (key < leftKey) {
      if (leftLeft === null) {
        return splayNode(leftKey, leftLeft, splayNode(rootKey, leftRight, right));
      }
      const result = splay(leftLeft, key);
      return splayNode(result[0], result[1],
                       splayNode(leftKey, result[2], splayNode(rootKey, leftRight, right)));
    }
    if (leftRight === null) {
      return splayNode(leftKey, leftLeft, splayNode(rootKey, leftRight, right));
    }
    const result = splay(leftRight, key);
    return splayNode(result[0], splayNode(leftKey, leftLeft, result[1]),
                     splayNode(rootKey, result[2], right));
  }
  if (right === null) {
    return tree;
  }
  const rightKey = right[0];
  const rightLeft = right[1];
  const rightRight = right[2];
  if (key === rightKey) {
    return splayNode(rightKey, splayNode(rootKey, left, rightLeft), rightRight);
  }
  if (key > rightKey) {
    if (rightRight === null) {
      return splayNode(rightKey, splayNode(rootKey, left, rightLeft), rightRight);
    }
    const result = splay(rightRight, key);
    return splayNode(result[0], splayNode(rightKey, splayNode(rootKey, left, rightLeft), result[1]),
                     result[2]);
  }
  if (rightLeft === null) {
    return splayNode(rightKey, splayNode(rootKey, left, rightLeft), rightRight);
  }
  const result = splay(rightLeft, key);
  return splayNode(result[0], splayNode(rootKey, left, result[1]),
                   splayNode(rightKey, result[2], rightRight));
}

function splayInsert(tree, key) {
  if (tree === null) {
    return splayNode(key, null, null);
  }
  const result = splay(tree, key);
  if (result[0] === key) {
    return result;
  }
  if (key < result[0]) {
    return splayNode(key, result[1], splayNode(result[0], null, result[2]));
  }
  return splayNode(key, splayNode(result[0], result[1], null), result[2]);
}

function runSplayBenchmark() {
  const size = 10000;
  let tree = null;
  for (let i = 1; i <= size; i += 1) {
    tree = splayInsert(tree, (i * 31) % size);
  }
  let hits = 0;
  for (let i = 1; i <= size; i += 1) {
    const key = (i * 31) % size;
    tree = splay(tree, key);
    if (tree !== null && tree[0] === key) {
      hits += 1;
    }
  }
  if (hits !== size) {
    throw new Error(`bench-splay produced ${hits} hits, expected ${size}`);
  }
}
