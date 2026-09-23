#!/usr/bin/env node
// Command-line front end: node cli.mjs index.html -o entry.zip [options]
// (a .zip input is recompressed file by file instead)
import fs from 'node:fs';
import os from 'node:os';
import { parseArgs } from 'node:util';
import { PRESETS, isZip, packHtml, recompress } from './node.mjs';

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
    extra: { type: 'string' },
    workers: { type: 'string', short: 'w' },
    bom: { type: 'string', default: 'auto' },
    name: { type: 'string', default: 'index.html' },
    quiet: { type: 'boolean', short: 'q', default: false },
    help: { type: 'boolean', short: 'h', default: false },
  },
});

if (opts.help || positionals.length !== 1 || !PRESETS[opts.preset] || !['auto', 'yes', 'no'].includes(opts.bom)) {
  console.error(`Usage: js13k-zip <input.html | input.zip> [options]   (or: node cli.mjs ...)

A .zip input is recompressed: every file is extracted and packed again with
the same name and exact contents (--bom and --name don't apply).

  -o, --out <file>      output zip (default: <input>.zip, or <input>.min.zip
                        for a .zip input)
  -p, --preset <name>   fast | normal | max (default: normal)
  -t, --time <sec>      after the preset, keep trying new seeds for <sec> seconds
      --target <bytes>  stop as soon as the zip is <bytes> or smaller
      --focus <0..1>    share of extra jobs spent re-compressing single blocks
                        of the best result (default: 0.5)
      --seed <n>        offset for the extra jobs' seeds (another random stream)
      --extra <n>       also complete <n> extra jobs (default: the preset's)
      --fill-cap <n>    max extra jobs running alongside the preset
                        (default: none, or no limit with --time)
  -w, --workers <n>     parallel workers (default: CPU count)
      --bom <mode>      auto | yes | no (default: auto; ASCII input never gets one)
      --name <file>     file name inside the zip (default: index.html)
  -q, --quiet           only print the final size`);
  process.exit(opts.help ? 0 : 1);
}

const input = positionals[0];
const bytes = new Uint8Array(fs.readFileSync(input));
const zipInput = isZip(bytes);
const out = opts.out || input.replace(/\.[^./\\]*$/, '') + (zipInput ? '.min.zip' : '.zip');
const nWorkers = Math.max(1, parseInt(opts.workers, 10) || os.availableParallelism?.() || os.cpus().length);

const t0 = performance.now();
const secs = () => ((performance.now() - t0) / 1000).toFixed(1);
const tty = process.stderr.isTTY && !opts.quiet;
let prefix = '';
const search = {
  workers: nWorkers, preset: opts.preset, timeLimit: +opts.time || 0, target: +opts.target || 0,
  focus: +opts.focus, seedBase: parseInt(opts.seed, 10) || 0,
  fillCap: opts['fill-cap'] === undefined ? undefined : parseInt(opts['fill-cap'], 10) || 0,
  extra: opts.extra === undefined ? undefined : parseInt(opts.extra, 10) || 0,
  onProgress: p => {
    if (!tty) return;
    process.stderr.write(`\r${prefix}[preset ${p.plannedDone}/${p.planned}, ${p.extraDone} extra] ${p.what} | best zip ${p.bestZipSize} B      `);
  },
};

try {
  if (zipInput) {
    const res = await recompress(bytes, {
      ...search,
      onFile: f => {
        if (tty && f.index) process.stderr.write('\n');
        prefix = f.count > 1 ? `${f.name} (${f.index + 1}/${f.count}) ` : '';
      },
    });
    if (tty) process.stderr.write('\n');
    fs.writeFileSync(out, res.zip);
    if (opts.quiet) {
      console.log(res.zip.length);
    } else {
      const diff = res.zip.length - bytes.length;
      console.log(`${out}: ${bytes.length} -> ${res.zip.length} bytes (${diff > 0 ? '+' : ''}${diff})`);
      for (const f of res.files) console.log(`  ${f.name}: ${f.size} -> ${f.compressed} bytes${f.stored ? ' (stored)' : ''}`);
      console.log(`  ${secs()}s on ${nWorkers} workers; ${13312 - res.zip.length} bytes left of 13 KB`);
    }
  } else {
    const res = await packHtml(bytes, { ...search, bom: opts.bom, filename: opts.name });
    if (tty) process.stderr.write('\n');
    fs.writeFileSync(out, res.zip);
    if (opts.quiet) {
      console.log(res.zip.length);
    } else {
      const b = res.bestSingle;
      console.log(`${out}: ${res.zip.length} bytes (${res.deflated.length} deflate, ${res.variant}, ${res.blocks} blocks)`);
      console.log(`  best single run: ${b.size} deflate (-${b.mode} seed ${b.seed}, ${b.variant}); block recombination saved ${b.size - res.deflated.length} B`);
      console.log(`  ${res.runs.length} mode results from ${res.jobs} ECT tasks on ${nWorkers} workers in ${secs()}s; ${13312 - res.zip.length} bytes left of 13 KB`);
    }
  }
} catch (e) {
  if (tty) process.stderr.write('\n');
  console.error(`${input}: ${e.message}`);
  process.exit(1);
}
