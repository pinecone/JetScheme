const supportRoot = arguments[0];
const benchmarkFile = arguments[1];
const benchmarkName = arguments[2];
const modules = {};

function loadCommonJs(path) {
  if (modules[path]) {
    return modules[path].exports;
  }

  const module = { exports: {} };
  modules[path] = module;
  const source = readFile(path);
  const factory = new Function('require', 'module', 'exports', source);
  const require = request => loadCommonJs(`${supportRoot}/awfy/${request.slice(2)}.js`);
  factory(require, module, module.exports);
  return module.exports;
}

function runAwfy(size) {
  const benchmark = loadCommonJs(benchmarkFile).newInstance();
  if (!benchmark.innerBenchmarkLoop(size)) {
    throw new Error(`${benchmarkName} failed verification`);
  }
}

function runJson() {
  const benchmark = loadCommonJs(benchmarkFile).newInstance();
  for (let i = 0; i < 100; i += 1) {
    benchmark.benchmark();
  }
  if (!benchmark.verifyResult(benchmark.benchmark())) {
    throw new Error('bench-json failed verification');
  }
}

function loadOctane() {
  globalThis.BenchmarkSuite = function BenchmarkSuite() {};
  globalThis.Benchmark = function Benchmark() {};
  load(benchmarkFile);
}

function runAstar() {
  load(benchmarkFile);
  const benchmark = new JetStreamAstarBenchmark();
  const start = benchmark.graph.grid[0][0];
  const end = benchmark.graph.grid[123][321];
  const result = JetStreamAstar.search(benchmark.graph, start, end);
  let pathHash = 2166136261;
  for (const node of result) {
    pathHash = Math.imul(pathHash ^ node.x, 16777619) >>> 0;
    pathHash = Math.imul(pathHash ^ node.y, 16777619) >>> 0;
  }
  if (result.length !== 446 || result[result.length - 1] !== end || pathHash !== 0x5088932e) {
    throw new Error('bench-astar failed verification');
  }
}

switch (benchmarkName) {
  case 'bench-astar':
    runAstar();
    break;
  case 'bench-cdjs':
    runAwfy(1000);
    break;
  case 'bench-deltablue':
    load(benchmarkFile);
    chainTest(12000);
    projectionTest(12000);
    break;
  case 'bench-json':
    runJson();
    break;
  case 'bench-nbody':
    runAwfy(250000);
    break;
  case 'bench-raytrace':
    loadOctane();
    for (let i = 0; i < 60; i += 1) {
      renderScene();
    }
    break;
  case 'bench-richards':
    loadOctane();
    COUNT = 100000;
    EXPECTED_QUEUE_COUNT = 232625;
    EXPECTED_HOLD_COUNT = 93050;
    runRichards();
    break;
  case 'bench-splay':
    load(benchmarkFile);
    runSplayBenchmark();
    break;
  default:
    throw new Error(`unknown benchmark: ${benchmarkName}`);
}

print('ok');
