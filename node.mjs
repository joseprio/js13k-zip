// Node API: the search from core.mjs with a ready-made worker pool.
//
//   import { packHtml, recompress, packFile } from 'js13k-zip';
//   const { zip } = await packHtml(fs.readFileSync('dist/index.html', 'utf8'), { preset: 'max' });
//   await packFile('dist/index.html', 'dist/build.zip');   // HTML or .zip input
//
// Options are those of optimize() in core.mjs (preset, timeLimit, target,
// extra, focus, seedBase, workers, onProgress, ...) plus, for packHtml, bom
// and filename.
import fs from 'node:fs/promises';
import os from 'node:os';
import { Worker } from 'node:worker_threads';
import { isZip, optimize, recompressZip, variants } from './core.mjs';

export * from './core.mjs';

const workerUrl = new URL('./ect/worker.mjs', import.meta.url);

// Worker factory for optimize()/recompressZip() in Node.
export function createNodeWorker() {
  const w = new Worker(workerUrl);
  const waiting = new Map();
  w.on('message', r => {
    const resolve = waiting.get(r.id);
    if (resolve) { waiting.delete(r.id); resolve(r); }
  });
  return {
    run: msg => new Promise(resolve => { waiting.set(msg.id, resolve); w.postMessage(msg); }),
    terminate: () => w.terminate(),
  };
}

const defaults = opts => ({
  createWorker: createNodeWorker,
  workers: os.availableParallelism?.() || os.cpus().length,
  ...opts,
});

// Packs one HTML document (string, or UTF-8 bytes) into a ZIP.
// Resolves to optimize()'s result: { zip, deflated, variant, ... }.
export async function packHtml(html, { bom = 'auto', filename = 'index.html', ...opts } = {}) {
  const text = typeof html === 'string' ? html : new TextDecoder().decode(html);
  return optimize(defaults({ ...opts, inputs: variants(text, bom), filename }));
}

// Recompresses an existing ZIP (bytes), keeping every file's name and contents.
// Resolves to { zip, files: [{ name, size, compressed, stored }] }.
export async function recompress(zip, opts = {}) {
  return recompressZip(defaults({ ...opts, zip: new Uint8Array(zip) }));
}

// Reads `input` (HTML, or a ZIP detected by its signature), writes the result
// to `output` (default: input with .zip, or .min.zip for a ZIP input) and
// resolves to { output, zip, result }.
export async function packFile(input, output, opts = {}) {
  const bytes = new Uint8Array(await fs.readFile(input));
  const zipInput = isZip(bytes);
  output ||= input.replace(/\.[^./\\]*$/, '') + (zipInput ? '.min.zip' : '.zip');
  const result = zipInput ? await recompress(bytes, opts) : await packHtml(bytes, opts);
  await fs.writeFile(output, result.zip);
  return { output, zip: result.zip, result };
}
