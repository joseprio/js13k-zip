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
// already been in (usually after 3-5 passes; see worker.mjs), so jobs are
// [n, K] pairs and K is cheap to raise.
export const PRESETS = {
  fast: { jobs: [[9, 2], [100, 2], [300, 2], [1000, 0]], seeds: 0 },
  normal: { jobs: [9, 60, 100, 300, 500, 1000].map(n => [n, 10]), seeds: 2 },
  max: { jobs: [9, 30, 60, 100, 150, 200, 300, 500, 1000].map(n => [n, 30]), seeds: 4 },
};

// Jobs re-run with extra seeds. Seeds only matter when ECT runs enough
// iterations to hit its randomization step (level >= 7 or explicit iterations).
const SEED_JOBS = [[100, 10], [300, 10], [1000, 10]];

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

// Runs the search.
//   inputs:    [{ label, bytes }]
//   workers:   [{ run(msg) -> Promise<result> }] (see worker.mjs)
//   preset:    'fast' | 'normal' | 'max'
//   timeLimit: seconds; after the planned jobs, keep trying new seeds until it
//              elapses (0 = planned jobs only)
//   onProgress({ done, total, extra, bestZipSize, job, size, variant })
//              (size: best of the job's passes; runs lists every pass)
//              (extra: past the planned jobs, running time-budget seeds)
// Returns { zip, deflated, variant, nbits, blocks, runs, jobs, bestSingle }
// (runs: one entry per mode result, jobs: ECT invocations).
export async function optimize({ inputs, workers, preset = 'normal', timeLimit = 0, filename = 'index.html', onProgress = () => {} }) {
  const planned = planJobs(preset);
  const queue = inputs.flatMap(v => planned.map(j => ({ ...j, v }))).sort((a, b) => jobCost(b) - jobCost(a));
  const total = queue.length;
  const extra = extraJobs(preset);
  const pools = new Map(inputs.map(v => [v, new BlockPool(v.bytes.length)]));
  const deadline = timeLimit > 0 ? Date.now() + timeLimit * 1000 : 0;
  const runs = [];
  let bestSingle = null;
  let best = null;
  let extraPending = [];
  let done = 0;

  const refresh = v => {
    const b = pools.get(v).best();
    const zipSize = 98 + 2 * filename.length + ((b.nbits + 7) >> 3);
    if (!best || zipSize < best.zipSize || (zipSize === best.zipSize && b.nbits < best.nbits)) best = { ...b, v, zipSize };
  };

  const next = () => {
    if (queue.length) return queue.shift();
    if (!deadline || Date.now() >= deadline) return null;
    if (!extraPending.length) {
      const j = extra.next().value;
      extraPending = inputs.map(v => ({ ...j, v }));
    }
    return extraPending.shift();
  };

  let id = 0;
  await Promise.all(workers.map(async w => {
    for (let job; (job = next()); ) {
      const r = await w.run({ id: id++, bytes: job.v.bytes, mode: job.mode, seed: job.seed });
      done++;
      let improved = false;
      for (const pass of r.passes) {
        runs.push({ mode: pass.mode, seed: job.seed, variant: job.v.label, size: pass.size });
        if (!bestSingle || pass.size < bestSingle.size) bestSingle = { mode: pass.mode, seed: job.seed, variant: job.v.label, size: pass.size };
        improved = pools.get(job.v).add(pass.blocks) || improved;
      }
      if (improved) refresh(job.v);
      const size = Math.min(...r.passes.map(pass => pass.size));
      onProgress({
        done, total, extra: done > total,
        bestZipSize: best.zipSize, job, size, variant: job.v.label,
      });
    }
  }));

  const deflated = assemble(best.blocks);
  const check = await inflateRaw(deflated);
  const src = best.v.bytes;
  if (check.length !== src.length || check.some((b, i) => b !== src[i])) throw new Error('Assembled stream failed to round-trip');
  return { zip: makeZip(filename, src, deflated), deflated, variant: best.v.label, nbits: best.nbits, blocks: best.blocks.length, runs, jobs: done, bestSingle };
}
