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

const workers = Array.from({ length: nWorkers }, () => {
  const w = new Worker(new URL('./ect/worker.mjs', import.meta.url));
  const waiting = new Map();
  w.on('message', r => { waiting.get(r.id)(r); waiting.delete(r.id); });
  return { w, run: msg => new Promise(resolve => { waiting.set(msg.id, resolve); w.postMessage(msg); }) };
});

const t0 = performance.now();
const tty = process.stderr.isTTY && !opts.quiet;
const res = await optimize({
  inputs, workers, preset: opts.preset, timeLimit: +opts.time || 0, target: +opts.target || 0, filename: opts.name,
  onProgress: p => {
    if (!tty) return;
    const where = p.extra ? `+${p.done - p.total} extra` : `${p.done}/${p.total}`;
    process.stderr.write(`\r[${where}] -${p.job.mode} seed ${p.job.seed} (${p.variant}): ${p.size} B deflate | best zip ${p.bestZipSize} B   `);
  },
});
workers.forEach(({ w }) => w.terminate());
if (tty) process.stderr.write('\n');

fs.writeFileSync(out, res.zip);
const secs = ((performance.now() - t0) / 1000).toFixed(1);
if (opts.quiet) {
  console.log(res.zip.length);
} else {
  const b = res.bestSingle;
  console.log(`${out}: ${res.zip.length} bytes (${res.deflated.length} deflate, ${res.variant}, ${res.blocks} blocks)`);
  console.log(`  best single run: ${b.size} deflate (-${b.mode} seed ${b.seed}, ${b.variant}); block recombination saved ${b.size - res.deflated.length} B`);
  console.log(`  ${res.runs.length} mode results from ${res.jobs} ECT runs on ${nWorkers} workers in ${secs}s; ${13312 - res.zip.length} bytes left of 13 KB`);
}
