// Thin JS layer over the ECT-zopfli WASM module (shared by worker.mjs and tests).
//
// Results are reported as passes: a mode K*10000+n runs up to K+1
// split/compress passes, and pass k's blocks are exactly what mode k*10000+n
// would output. Blocks are { start, end, nbits, data } where data holds the
// bits packed LSB-first (DEFLATE bit order) with the BFINAL bit cleared.
import ECTZopfli from './ect-zopfli.mjs';

function extractBits(src, from, to) {
  const n = to - from;
  const out = new Uint8Array((n + 7) >> 3);
  for (let i = 0; i < n; i++) {
    const b = from + i;
    if ((src[b >> 3] >> (b & 7)) & 1) out[i >> 3] |= 1 << (i & 7);
  }
  return out;
}

export async function createEngine() {
  const M = await ECTZopfli();

  // Copies the input into WASM memory (ECT may read up to 8 bytes past its end).
  const withInput = (bytes, fn) => {
    const p = M._malloc(bytes.length + 8);
    M.HEAPU8.fill(0, p, p + bytes.length + 8);
    M.HEAPU8.set(bytes, p);
    try {
      return fn(p);
    } finally {
      M._free(p);
    }
  };

  const copyIn = (typed) => {
    if (!typed || !typed.length) return 0;
    const p = M._malloc(typed.byteLength + 8);
    M.HEAPU8.set(new Uint8Array(typed.buffer, typed.byteOffset, typed.byteLength), p);
    return p;
  };

  // Emitted blocks of the last call, grouped by pass index.
  const readPasses = (size, mode) => {
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
      passes[pass] ||= { pass, mode: pass * 10000 + (mode % 10000), nbits: 0, blocks: [] };
      passes[pass].nbits += bitend - bitstart;
      passes[pass].blocks.push({ start, end, nbits: bitend - bitstart, data });
    }
    return passes.filter(Boolean).map(({ nbits, ...p }) => ({ ...p, size: (nbits + 7) >> 3 }));
  };

  return {
    // A whole mode, all passes in sequence; stops once passes repeat a state.
    // With start/end only bytes[start..end) is compressed, the earlier bytes
    // serving as dictionary (block positions stay absolute).
    deflate(bytes, mode, seed, start = 0, end = bytes.length) {
      end = Math.min(end, bytes.length);
      start = Math.min(start, end);
      return withInput(bytes, p => readPasses(M._ect_deflate(p, end, mode, seed, 1, start), mode));
    },

    // One block of one pass (see ect_pass_task in ectz.cpp). block -2 only
    // computes the pass's block count. Returns { nblocks, blocks, store, costState }:
    // store is the block's LZ77 data (for passes that feed a next pass) and
    // costState the carried cost model after the block.
    passTask(bytes, { mode, seed, pass, twiceMode, prev, costState, block, start = 0, end = bytes.length }) {
      end = Math.min(end, bytes.length);
      start = Math.min(start, end);
      return withInput(bytes, p => {
        const ll = copyIn(prev && prev.litlens), dd = copyIn(prev && prev.dists), st = copyIn(costState);
        try {
          const size = M._ect_pass_task(p, end, start, mode, seed, pass, twiceMode, ll, dd,
            prev ? prev.litlens.length : 0, st, block);
          const passes = readPasses(size, mode);
          const n = M._ect_task_store_size();
          const store = n ? {
            litlens: M.HEAPU16.slice(M._ect_task_litlens() >> 1, (M._ect_task_litlens() >> 1) + n),
            dists: M.HEAPU16.slice(M._ect_task_dists() >> 1, (M._ect_task_dists() >> 1) + n),
          } : null;
          const sp = M._ect_cost_state_ptr();
          return {
            nblocks: M._ect_task_blocks(),
            blocks: passes.length ? passes[0].blocks : [],
            store,
            costState: M.HEAPU8.slice(sp, sp + M._ect_cost_state_size()),
          };
        } finally {
          for (const q of [ll, dd, st]) if (q) M._free(q);
        }
      });
    },
  };
}
