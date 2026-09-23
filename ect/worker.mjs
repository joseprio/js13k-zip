// ECT worker, usable as a browser module Worker or a Node worker_threads Worker.
// In:  { id, kind: 'deflate', bytes, mode, seed, start?, end? }
//        -> { id, passes }         (see engine.mjs deflate)
//      { id, kind: 'task', bytes, ...passTask fields }
//        -> { id, task }           (see engine.mjs passTask)
// Posts { ready: true } once the WASM module is instantiated.
import { createEngine } from './engine.mjs';

const ready = createEngine();

async function handle({ id, kind, bytes, ...args }) {
  const E = await ready;
  if (kind === 'task') return { id, task: E.passTask(bytes, args) };
  return { id, passes: E.deflate(bytes, args.mode, args.seed, args.start, args.end) };
}

if (typeof process === 'object' && process.versions && process.versions.node) {
  const { parentPort } = await import('node:worker_threads');
  parentPort.on('message', async msg => parentPort.postMessage(await handle(msg)));
  ready.then(() => parentPort.postMessage({ ready: true }));
} else {
  self.onmessage = async e => self.postMessage(await handle(e.data));
  ready.then(() => self.postMessage({ ready: true }));
}
