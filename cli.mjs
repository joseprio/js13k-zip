#!/usr/bin/env node
// Command-line front end: node cli.mjs index.html -o entry.zip [options]
import fs from 'node:fs';
import os from 'node:os';
import { parseArgs } from 'node:util';
import { Worker } from 'node:worker_threads';
import { PRESETS, optimize, variants } from './core.mjs';

const { values: opts, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    out: { type: 'string', short: 'o' },
    preset: { type: 'string', short: 'p', default: 'normal' },
    time: { type: 'string', short: 't', default: '0' },
    target: { type: 'string', default: '0' },
    focus: { type: 'string', default: '0.5' },
    seed: { type: 'string', default: '0' },
    'fill-cap': { type: 'string' },
    workers: { type: 'string', short: 'w' },
    bom: { type: 'string', default: 'auto' },
    name: { type: 'string', default: 'index.html' },
    quiet: { type: 'boolean', short: 'q', default: false },
    help: { type: 'boolean', short: 'h', default: false },
  },
});

if (opts.help || positionals.length !== 1 || !PRESETS[opts.preset] || !['auto', 'yes', 'no'].includes(opts.bom)) {
  console.error(`Usage: node cli.mjs <input.html> [options]

  -o, --out <file>      output zip (default: <input>.zip)
  -p, --preset <name>   fast | normal | max (default: normal)
  -t, --time <sec>      after the preset, keep trying new seeds for <sec> seconds
      --target <bytes>  stop as soon as the zip is <bytes> or smaller
      --focus <0..1>    share of extra jobs spent re-compressing single blocks
                        of the best result (default: 0.5)
      --seed <n>        offset for the extra jobs' seeds (another random stream)
      --fill-cap <n>    max extra jobs running alongside the preset
                        (default: none, or no limit with --time)
  -w, --workers <n>     parallel workers (default: CPU count)
      --bom <mode>      auto | yes | no (default: auto; ASCII input never gets one)
      --name <file>     file name inside the zip (default: index.html)
  -q, --quiet           only print the final size`);
  process.exit(opts.help ? 0 : 1);
}

const input = positionals[0];
const out = opts.out || input.replace(/\.[^./\\]*$/, '') + '.zip';
const inputs = variants(fs.readFileSync(input, 'utf8'), opts.bom);
const nWorkers = Math.max(1, parseInt(opts.workers, 10) || os.availableParallelism?.() || os.cpus().length);

const workerUrl = new URL('./ect/worker.mjs', import.meta.url);
const createWorker = () => {
  const w = new Worker(workerUrl);
  const waiting = new Map();
  w.on('message', r => {
    const resolve = waiting.get(r.id);
    if (resolve) { waiting.delete(r.id); resolve(r); }
  });
  return { run: msg => new Promise(resolve => { waiting.set(msg.id, resolve); w.postMessage(msg); }), terminate: () => w.terminate() };
};

const t0 = performance.now();
const tty = process.stderr.isTTY && !opts.quiet;
const res = await optimize({
  inputs, createWorker, workers: nWorkers, preset: opts.preset, timeLimit: +opts.time || 0, target: +opts.target || 0,
  focus: +opts.focus, seedBase: parseInt(opts.seed, 10) || 0, fillCap: opts['fill-cap'] === undefined ? undefined : parseInt(opts['fill-cap'], 10) || 0, filename: opts.name,
  onProgress: p => {
    if (!tty) return;
    process.stderr.write(`\r[preset ${p.plannedDone}/${p.planned}, ${p.extraDone} extra] ${p.what} | best zip ${p.bestZipSize} B      `);
  },
});
if (tty) process.stderr.write('\n');

fs.writeFileSync(out, res.zip);
const secs = ((performance.now() - t0) / 1000).toFixed(1);
if (opts.quiet) {
  console.log(res.zip.length);
} else {
  const b = res.bestSingle;
  console.log(`${out}: ${res.zip.length} bytes (${res.deflated.length} deflate, ${res.variant}, ${res.blocks} blocks)`);
  console.log(`  best single run: ${b.size} deflate (-${b.mode} seed ${b.seed}, ${b.variant}); block recombination saved ${b.size - res.deflated.length} B`);
  console.log(`  ${res.runs.length} mode results from ${res.jobs} ECT tasks on ${nWorkers} workers in ${secs}s; ${13312 - res.zip.length} bytes left of 13 KB`);
}
