// ECT-zopfli worker, usable as a browser module Worker or a Node worker_threads Worker.
// In:  { id, bytes: Uint8Array, mode, seed }
// Out: { id, passes: [{ mode, size, blocks: [{ start, end, nbits, data }] }] }
// A mode K*10000+n runs K+1 split/compress passes; pass k's blocks are exactly
// what mode k*10000+n would output, so every pass is reported as its own
// result. Block data holds the bits packed LSB-first (DEFLATE bit order) with
// the BFINAL bit cleared.
import ECTZopfli from './ect-zopfli.mjs';

const ready = ECTZopfli();

function extractBits(src, from, to) {
  const n = to - from;
  const out = new Uint8Array((n + 7) >> 3);
  for (let i = 0; i < n; i++) {
    const b = from + i;
    if ((src[b >> 3] >> (b & 7)) & 1) out[i >> 3] |= 1 << (i & 7);
  }
  return out;
}

async function handle({ id, bytes, mode, seed }) {
  const M = await ready;
  // ECT may read up to 8 bytes past the end of the input.
  const p = M._malloc(bytes.length + 8);
  M.HEAPU8.fill(0, p, p + bytes.length + 8);
  M.HEAPU8.set(bytes, p);
  const size = M._ect_deflate(p, bytes.length, mode, seed, 1);
  M._free(p);
  const outPtr = M._ect_output();
  const out = M.HEAPU8.subarray(outPtr, outPtr + size);
  const count = M._ect_block_count();
  const base = M._ect_blocks() >> 2;
  const rec = M.HEAPU32.subarray(base, base + count * 5);
  const passes = [];
  for (let i = 0; i < count; i++) {
    const [start, end, bitstart, bitend, pass] = rec.subarray(i * 5, i * 5 + 5);
    const data = extractBits(out, bitstart, bitend);
    data[0] &= ~1;
    passes[pass] ||= { mode: pass * 10000 + (mode % 10000), nbits: 0, blocks: [] };
    passes[pass].nbits += bitend - bitstart;
    passes[pass].blocks.push({ start, end, nbits: bitend - bitstart, data });
  }
  return { id, passes: passes.map(({ nbits, ...p }) => ({ ...p, size: (nbits + 7) >> 3 })) };
}

if (typeof process === 'object' && process.versions && process.versions.node) {
  const { parentPort } = await import('node:worker_threads');
  parentPort.on('message', async msg => parentPort.postMessage(await handle(msg)));
} else {
  self.onmessage = async e => self.postMessage(await handle(e.data));
}
