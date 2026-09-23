// Shared search engine for the web page (index.html) and the CLI (cli.mjs).
//
// Every ECT run emits DEFLATE blocks that are self-contained given the input:
// back-references point into the uncompressed data, which is the same no matter
// how earlier blocks were encoded. So blocks found by *different* runs (other
// modes, seeds or split points) can be freely concatenated. BlockPool keeps the
// smallest encoding of every [start, end) input range seen and picks the
// cheapest chain from 0 to the input length (shortest path over a DAG).

// ECT modes use ECT's `-N` semantics: N % 10000 is the level (<=9) or the
// iteration count (>9); N / 10000 is the number of extra block-split/compress
// cycles. Results are not monotonic in N, so many modes are searched.
// A run of mode K*10000+n reports the results of every k*10000+n (k <= K) for
// the price of one, and stops early once its passes reach a state they have
// already been in (see engine.mjs), so jobs are [n, K] pairs. Inputs that
// don't converge drift instead: on a 37 KB bundle the best pass was always
// k <= 4, so K stays small.
export const PRESETS = {
  fast: { jobs: [[9, 2], [100, 2], [300, 2], [1000, 0]], seeds: 0 },
  normal: { jobs: [9, 60, 100, 300, 500, 1000].map(n => [n, 4]), seeds: 2 },
  // max also completes 800 extra seed/range jobs: enough for every one of 6
  // seed streams to reach the best size found for galaxy-raid (11110 B).
  max: { jobs: [9, 30, 60, 100, 150, 200, 300, 500, 1000].map(n => [n, 6]), seeds: 4, extra: 800 },
};

// Jobs re-run with extra seeds. Seeds only matter when ECT runs enough
// iterations to hit its randomization step (level >= 7 or explicit iterations).
// Picked by measured near-best results per CPU second on a roadrolled 15 KB
// bundle; long jobs such as [1000, 10] were 5-10x less efficient.
const SEED_JOBS = [[300, 2], [200, 1], [300, 1], [100, 4]];

// Iterations per pass for a level or explicit count (ECT's util.c table).
const iterations = n => (n > 9 ? n : [1, 1, 1, 2, 3, 8, 13, 60, 60][Math.max(n, 2) - 1]);
// Expected cost, for scheduling: runs rarely need more than ~5 passes.
const jobCost = ({ mode }) => iterations(mode % 10000) * Math.min(5, 1 + Math.floor(mode / 10000));

// Planned jobs for a preset: every [n, K] with the default seed, then `seeds`
// extra seeds over SEED_JOBS; longest first so the workers finish together.
export function planJobs(preset) {
  const { jobs, seeds } = PRESETS[preset];
  const planned = jobs.map(([n, k]) => ({ mode: k * 10000 + n, seed: 0 }));
  for (let s = 1; s <= seeds; s++) for (const [n, k] of SEED_JOBS) planned.push({ mode: k * 10000 + n, seed: s });
  return planned.sort((a, b) => jobCost(b) - jobCost(a));
}

// Endless stream of additional seed jobs, used while a time budget remains.
export function* extraJobs(preset) {
  for (let s = PRESETS[preset].seeds + 1; ; s++) for (const [n, k] of SEED_JOBS) yield { mode: k * 10000 + n, seed: s };
}

// Runs mode K*10000+n with one task per block per pass, so a pass's blocks can
// run on different workers. Each block task replays the blocks before it just
// far enough to reproduce the state they hand on, so the passes are exactly
// those of a sequential run, including stopping once the state between passes
// repeats. runTask(msg) runs engine.passTask(bytes, msg); onPass(pass) gets
// each finished pass ({ pass, mode, size, blocks }).
export async function runChain({ mode, seed, start = 0, end, runTask, onPass, guess = 3 }) {
  const K = Math.floor(mode / 10000);
  const seen = [];
  let prev = null;
  let costState = null;
  for (let pass = 0; pass <= K; pass++) {
    const twiceMode = K === 0 ? 0 : pass === 0 ? 1 : pass < K ? 3 : 2;
    const msg = block => ({ mode, seed, pass, twiceMode, prev, costState, block, start, end });
    const results = [];
    let launched = 0;
    let nblocks = guess;
    const launch = async block => {
      const r = await runTask(msg(block));
      results[block] = r;
      if (r.nblocks > launched) {
        nblocks = r.nblocks;
        const more = [];
        while (launched < nblocks) more.push(launch(launched++));
        await Promise.all(more);
      }
    };
    const first = [];
    while (launched < nblocks) first.push(launch(launched++));
    await Promise.all(first);
    nblocks = results[0].nblocks;
    const blocks = results.slice(0, nblocks).flatMap(r => r.blocks);
    const nbits = blocks.reduce((a, b) => a + b.nbits, 0);
    onPass({ pass, mode: pass * 10000 + (mode % 10000), size: (nbits + 7) >> 3, blocks });
    guess = nblocks;
    if (pass === K) break;
    // State handed to the next pass: carried cost model + this pass's LZ77 data.
    const stores = results.slice(0, nblocks).map(r => r.store);
    const total = stores.reduce((a, st) => a + st.litlens.length, 0);
    prev = { litlens: new Uint16Array(total), dists: new Uint16Array(total) };
    let o = 0;
    for (const st of stores) {
      prev.litlens.set(st.litlens, o);
      prev.dists.set(st.dists, o);
      o += st.litlens.length;
    }
    costState = results[nblocks - 1].costState;
    const key = new Uint8Array(costState.length + 4 * total);
    key.set(costState, 0);
    key.set(new Uint8Array(prev.litlens.buffer), costState.length);
    key.set(new Uint8Array(prev.dists.buffer), costState.length + 2 * total);
    if (seen.some(k => k.length === key.length && k.every((b, i) => b === key[i]))) break;
    seen.push(key);
  }
}

export class BlockPool {
  constructor(length) {
    this.length = length;
    this.ranges = new Map(); // "start-end" -> smallest block
  }

  // Returns true if any range got a smaller encoding.
  add(blocks) {
    let improved = false;
    for (const b of blocks) {
      const key = b.start + '-' + b.end;
      const cur = this.ranges.get(key);
      if (!cur || b.nbits < cur.nbits) {
        this.ranges.set(key, b);
        improved = true;
      }
    }
    return improved;
  }

  // Cheapest chain of blocks covering [0, length): { nbits, blocks }.
  best() {
    const byEnd = new Map();
    const points = new Set([0]);
    for (const b of this.ranges.values()) {
      if (!byEnd.has(b.end)) byEnd.set(b.end, []);
      byEnd.get(b.end).push(b);
      points.add(b.end);
    }
    const cost = new Map([[0, 0]]);
    const via = new Map();
    for (const p of [...points].sort((a, b) => a - b)) {
      for (const b of byEnd.get(p) || []) {
        if (!cost.has(b.start)) continue;
        const c = cost.get(b.start) + b.nbits;
        if (!cost.has(p) || c < cost.get(p)) {
          cost.set(p, c);
          via.set(p, b);
        }
      }
    }
    if (!cost.has(this.length)) return null;
    const blocks = [];
    for (let p = this.length; p > 0; p = via.get(p).start) blocks.unshift(via.get(p));
    return { nbits: cost.get(this.length), blocks };
  }
}

// Concatenates blocks into a DEFLATE stream, setting BFINAL on the last one.
export function assemble(blocks) {
  const nbits = blocks.reduce((a, b) => a + b.nbits, 0);
  const out = new Uint8Array((nbits + 7) >> 3);
  let pos = 0;
  blocks.forEach((b, k) => {
    for (let i = 0; i < b.nbits; i++, pos++) {
      const bit = i === 0 ? +(k === blocks.length - 1) : (b.data[i >> 3] >> (i & 7)) & 1;
      if (bit) out[pos >> 3] |= 1 << (pos & 7);
    }
  });
  return out;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

export function crc32(bytes) {
  let crc = -1;
  for (let i = 0; i < bytes.length; i++) crc = (crc >>> 8) ^ CRC_TABLE[(crc ^ bytes[i]) & 0xff];
  return (crc ^ -1) >>> 0;
}

// Minimal single-file ZIP (118 bytes of overhead for "index.html").
// Timestamp is fixed at the DOS epoch so output is deterministic.
export function makeZip(filename, inflated, deflated) {
  const name = new TextEncoder().encode(filename);
  const two = v => [v & 255, (v >>> 8) & 255];
  const four = v => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  const datetime = four(0x00210000); // 1980-01-01 00:00:00
  const crc = four(crc32(inflated));
  const csize = four(deflated.length);
  const usize = four(inflated.length);
  const nlen = two(name.length);
  const local = [0x50, 0x4b, 0x03, 0x04, 20, 0, 0, 0, 8, 0, ...datetime, ...crc, ...csize, ...usize, ...nlen, 0, 0, ...name];
  const central = [0x50, 0x4b, 0x01, 0x02, 20, 0, 20, 0, 0, 0, 8, 0, ...datetime, ...crc, ...csize, ...usize, ...nlen,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...name];
  const end = [0x50, 0x4b, 0x05, 0x06, 0, 0, 0, 0, 1, 0, 1, 0, ...four(central.length),
    ...four(local.length + deflated.length), 0, 0];
  const zip = new Uint8Array(local.length + deflated.length + central.length + end.length);
  zip.set(local, 0);
  zip.set(deflated, local.length);
  zip.set(central, local.length + deflated.length);
  zip.set(end, local.length + deflated.length + central.length);
  return zip;
}

export async function inflateRaw(deflated) {
  const stream = new Blob([deflated]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Input variants to try: the UTF-8 bytes with and/or without a BOM.
export function variants(text, bom = 'auto') {
  const enc = new TextEncoder();
  const plain = { label: 'no BOM', bytes: enc.encode(text) };
  const withBom = { label: 'BOM', bytes: enc.encode('\ufeff' + text) };
  if (bom === 'yes') return [withBom];
  if (bom === 'no' || /^[\x00-\x7f]*$/.test(text)) return [plain];
  return [withBom, plain];
}

// Worker pool with priorities. Higher-priority tasks go first; when one is
// waiting and no worker is free, a running speculative (priority 0) task is
// dropped (its worker terminated and replaced from a pre-started spare) so the
// critical path never waits behind speculative work. Idle workers ask
// `filler()` for speculative tasks ({ msg, onResult }, priority 0).
//   createWorker() -> { run(msg) -> Promise, terminate() }
class Pool {
  constructor(createWorker, size) {
    this.createWorker = createWorker;
    this.idle = Array.from({ length: size }, createWorker);
    this.spare = createWorker();
    this.running = new Set();
    this.queue = [];
    this.filler = null;
    this.fillCap = Infinity;
    this.closed = false;
    this.seq = 0;
    this.ids = 0;
  }

  submit(msg, prio) {
    return new Promise((resolve, reject) => {
      this.queue.push({ msg, prio, resolve, reject, seq: this.seq++ });
      this.pump();
    });
  }

  start(worker, task) {
    const slot = { worker, task, started: this.seq++ };
    this.running.add(slot);
    const settle = (fn, value) => {
      if (!this.running.delete(slot) || this.closed) return;
      this.idle.push(worker);
      fn(value);
      this.pump();
    };
    worker.run({ ...task.msg, id: this.ids++ }).then(r => settle(task.resolve, r), e => settle(task.reject, e));
  }

  pump() {
    if (this.closed) return;
    this.queue.sort((a, b) => b.prio - a.prio || a.seq - b.seq);
    while (this.queue.length) {
      let worker = this.idle.pop();
      if (!worker) {
        let victim = null;
        for (const slot of this.running) {
          if (slot.task.prio === 0 && (!victim || slot.started > victim.started)) victim = slot;
        }
        if (!victim) break;
        this.running.delete(victim);
        victim.worker.terminate();
        victim.task.reject(new Error('preempted'));
        worker = this.spare;
        this.spare = this.createWorker();
      }
      this.start(worker, this.queue.shift());
    }
    while (this.idle.length && this.filler && this.speculative() < this.fillCap) {
      const job = this.filler();
      if (!job) break;
      this.start(this.idle.pop(), { prio: 0, msg: job.msg, resolve: job.onResult, reject: job.onDropped || (() => {}) });
    }
  }

  speculative() {
    let n = 0;
    for (const slot of this.running) if (slot.task.prio === 0) n++;
    return n;
  }

  close() {
    this.closed = true;
    for (const w of [...this.idle, this.spare, ...[...this.running].map(s => s.worker)]) w.terminate();
  }
}

// Runs the search.
//   inputs:       [{ label, bytes }]
//   createWorker: () -> { run(msg) -> Promise<result>, terminate() } around ect/worker.mjs
//   workers:      number of workers (one spare is started on top)
//   preset:       'fast' | 'normal' | 'max'
//   timeLimit:    seconds; keep searching with extra jobs until it elapses
//                 (0 = stop when the preset is done). The extra jobs start on
//                 idle workers while the preset is still running.
//   target:       stop as soon as the zip is this many bytes or smaller (0 = off)
//   focus:        share of the extra jobs that re-compress single block ranges
//                 of the current best chain instead of the whole input
//   seedBase:     offset for the extra jobs' seeds (another random stream)
//   extra:        number of extra jobs that must complete besides the preset
//                 (default: the preset's own `extra`, 0 for fast/normal)
//   fillCap:      max extra jobs running at once. Default: none without a
//                 time limit (on an 8-core SMT CPU even 4 extra jobs slowed
//                 the preset's critical path by 13-20%); no limit with one,
//                 where total throughput is what counts
//   onProgress({ planned, plannedDone, extraDone, bestZipSize, what })
// Returns { zip, deflated, variant, nbits, blocks, runs, jobs, bestSingle }
// (runs: one entry per whole-input mode result, jobs: finished tasks).
export async function optimize({ inputs, createWorker, workers = 4, preset = 'normal', timeLimit = 0, target = 0,
  focus = 0.5, seedBase = 0, fillCap, extra: extraBudget, filename = 'index.html', onProgress = () => {} }) {
  const pools = new Map(inputs.map(v => [v, new BlockPool(v.bytes.length)]));
  const deadline = timeLimit > 0 ? Date.now() + timeLimit * 1000 : 0;
  const runs = [];
  const extra = extraJobs(preset);
  let bestSingle = null;
  let best = null;
  let extraPending = [];
  let extraCount = 0;
  let rangeCount = 0;
  let plannedDone = 0;
  let extraDone = 0;
  let extraInFlight = 0;
  let jobs = 0;
  let stopped = false;
  let stop;
  const finished = new Promise(resolve => { stop = () => { stopped = true; resolve(); }; });

  const planned = planJobs(preset);
  extraBudget = extraBudget ?? PRESETS[preset].extra ?? 0;
  const maxCost = Math.max(...planned.map(jobCost));
  const chains = inputs.flatMap(v => planned.map(job => ({ ...job, v })));

  const report = what => onProgress({ planned: chains.length, plannedDone, extraDone, bestZipSize: best && best.zipSize, what });

  const addPass = (v, pass, seed, whole) => {
    if (stopped) return;
    if (whole) {
      runs.push({ mode: pass.mode, seed, variant: v.label, size: pass.size });
      if (!bestSingle || pass.size < bestSingle.size) bestSingle = { mode: pass.mode, seed, variant: v.label, size: pass.size };
    }
    if (!pools.get(v).add(pass.blocks)) return;
    const b = pools.get(v).best();
    const zipSize = 98 + 2 * filename.length + ((b.nbits + 7) >> 3);
    if (!best || zipSize < best.zipSize || (zipSize === best.zipSize && b.nbits < best.nbits)) best = { ...b, v, zipSize };
    if (target && best.zipSize <= target) stop();
  };

  // Speculative work for idle workers: range jobs (re-compress one block range
  // of the current best chain; each range improves on its own at a fraction of
  // a whole run's cost) mixed with whole-input seed jobs.
  const nextExtra = () => {
    if (best && best.blocks.length > 1 && ++extraCount * focus >= rangeCount + 1) {
      const k = rangeCount++;
      const b = best.blocks[k % best.blocks.length];
      const [n, K] = SEED_JOBS[Math.floor(k / best.blocks.length) % SEED_JOBS.length];
      const seed = 1 + seedBase + Math.floor(k / (best.blocks.length * SEED_JOBS.length));
      return { mode: K * 10000 + n, seed, v: best.v, start: b.start, end: b.end };
    }
    if (!extraPending.length) {
      const j = extra.next().value;
      extraPending = inputs.map(v => ({ ...j, seed: j.seed + seedBase, v }));
    }
    return extraPending.shift();
  };

  const pool = new Pool(createWorker, workers);
  pool.fillCap = fillCap ?? (deadline || extraBudget ? Infinity : 0);
  const presetDone = () => plannedDone === chains.length;
  const allDone = () => presetDone() && extraDone >= extraBudget && (!deadline || Date.now() >= deadline);
  pool.filler = () => {
    const wanted = extraDone + extraInFlight < extraBudget || !presetDone() || (deadline && Date.now() < deadline);
    if (stopped || !wanted) return null;
    const job = nextExtra();
    const whole = job.end === undefined;
    extraInFlight++;
    return {
      msg: { kind: 'deflate', bytes: job.v.bytes, mode: job.mode, seed: job.seed, start: job.start, end: job.end },
      onResult: r => {
        extraInFlight--;
        jobs++;
        extraDone++;
        for (const pass of r.passes) addPass(job.v, pass, job.seed, whole);
        report(`-${job.mode} seed ${job.seed}` + (whole ? '' : ` bytes ${job.start}-${job.end}`));
        if (allDone()) stop();
      },
      onDropped: () => { extraInFlight--; },
    };
  };

  // The preset: every job as a chain of per-block tasks, longest chains first.
  const chainRuns = chains.map(job => runChain({
    mode: job.mode, seed: job.seed, end: job.v.bytes.length,
    runTask: msg => pool.submit({ kind: 'task', bytes: job.v.bytes, ...msg }, 1 + jobCost(job) / maxCost)
      .then(r => { jobs++; return r.task; }),
    onPass: pass => { addPass(job.v, pass, job.seed, true); report(`-${pass.mode} seed ${job.seed}`); },
  }).then(() => { plannedDone++; }));

  // The preset always completes; a time limit only bounds the extra search.
  if (deadline) setTimeout(() => { if (allDone()) stop(); }, Math.max(0, deadline - Date.now()));
  let failure = null;
  Promise.all(chainRuns).then(() => { if (allDone()) stop(); },
    e => { failure = e; stop(); });
  pool.pump();
  await finished;
  pool.close();
  if (failure) throw failure;

  const deflated = assemble(best.blocks);
  const check = await inflateRaw(deflated);
  const src = best.v.bytes;
  if (check.length !== src.length || check.some((b, i) => b !== src[i])) throw new Error('Assembled stream failed to round-trip');
  return { zip: makeZip(filename, src, deflated), deflated, variant: best.v.label, nbits: best.nbits, blocks: best.blocks.length, runs, jobs, bestSingle };
}
