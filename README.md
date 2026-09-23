# js13k-zip

Squeezes the last bytes out of a [js13kGames](https://js13kgames.com) entry: it
packs your `index.html` into the smallest ZIP it can find, in the browser or
from the command line.

**Try it: [joseprio.github.io/js13k-zip](https://joseprio.github.io/js13k-zip/)**

On a real roadrolled js13k bundle (14,911 bytes of HTML):

| Tool | ZIP size | Time |
|---|---|---|
| `ect -100500` + `advzip -i 100` | 11,114 B | 2.4 s |
| js13k-zip `-p fast` | 11,111 B | 0.5 s |
| js13k-zip `-p normal` | 11,111 B | 1.4 s |
| js13k-zip `-p max` | **11,110 B** | ~13 s |

Times on an 8-core / 16-thread laptop CPU.

## How it works

- **ECT's deflate engine in WebAssembly.** The compressor is the Zopfli fork
  from [ECT](https://github.com/fhanau/Efficient-Compression-Tool) (the engine
  behind `ect -zip`), compiled to a single-file WASM module that runs in
  browsers and Node.
- **A search instead of a single run.** ECT's results aren't monotonic in its
  settings, so many modes (iteration counts × split/compress cycles) and
  random seeds are tried in parallel on all CPU cores.
- **Block recombination.** Every DEFLATE block only references the original
  input, never earlier blocks' encoding, so blocks found by *different* runs
  can be mixed. The smallest encoding of every input range is kept, and the
  cheapest combination covering the file is chosen (a shortest path). This is
  often where the last byte comes from.
- **Less wasted work.** One run of a multi-cycle mode reports every
  intermediate cycle; runs stop once their passes repeat a state; each pass's
  blocks run on separate workers; extra time is spent re-compressing single
  blocks of the best result. None of these change ECT's output — each is
  checked bit-for-bit against plain runs.
- **Minimal ZIP.** A single-entry archive with 118 bytes of overhead for
  `index.html`, a fixed timestamp for reproducible output, and a round-trip
  check before anything is written.

## Web

Open the [page](https://joseprio.github.io/js13k-zip/), paste your HTML or
pick a file, choose a preset and press **PACK ZIP**. Everything runs locally in
Web Workers; nothing is uploaded. The page must be served over HTTP (it uses
ES modules), so to run it locally use any static server, e.g.
`npx serve` or `python -m http.server`.

## Command line

Needs a recent Node.js (tested with 24); no install step or dependencies.

```sh
node cli.mjs dist/index.html -o dist/entry.zip            # normal preset
node cli.mjs dist/index.html -o dist/entry.zip -p max     # best result
node cli.mjs dist/index.html -o dist/entry.zip -t 30      # search 30 more seconds
node cli.mjs dist/index.html -o dist/entry.zip -t 300 --target 11110  # stop early
```

| Option | |
|---|---|
| `-o, --out <file>` | output zip (default: `<input>.zip`) |
| `-p, --preset <name>` | `fast`, `normal` (default) or `max` |
| `-t, --time <sec>` | after the preset, keep searching for `<sec>` seconds |
| `--target <bytes>` | stop as soon as the zip is this size or smaller |
| `--extra <n>` | also complete `<n>` extra jobs (`max` uses 800) |
| `--seed <n>` | use a different random stream for the extra jobs |
| `--focus <0..1>` | share of extra jobs spent on single blocks (default 0.5) |
| `-w, --workers <n>` | parallel workers (default: CPU count) |
| `--bom auto\|yes\|no` | UTF-8 BOM handling (ASCII input never gets one) |
| `--name <file>` | file name inside the zip (default `index.html`) |
| `-q, --quiet` | only print the final size |

In a build script:

```js
import { execFileSync } from 'node:child_process';
execFileSync('node', ['../js13k-zip/cli.mjs', 'dist/index.html', '-o', 'dist/entry.zip', '-p', 'max'], { stdio: 'inherit' });
```

## Presets

- **fast** — a handful of modes; good for watch mode.
- **normal** — 6 iteration counts × up to 4 cycles plus a few seeds.
- **max** — 9 iteration counts × up to 6 cycles, plus 800 extra seed and
  single-block jobs. Slower on large inputs (~70 s for a 37 KB bundle).

The last byte or two depend on lucky random runs, so no preset can promise
the true minimum. Use `-t` or `--extra` to search longer, and `--target` to
stop as soon as you reach the size you're after.

## Building the WASM module

`ect/ect-zopfli.mjs` is prebuilt. To rebuild it, activate an
[Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
and run:

```sh
sh ect/build.sh
```

The ECT sources in `ect/src/` come from ECT at commit `f0b38f7` (the revision
bundled with `ect-bin` 1.4.1), with small hooks marked `js13k-zip`.

## Credits

- Originally created by [xem](https://github.com/xem) as a simplified
  [js13k-pack](https://xem.github.io/js13k-pack).
- [ECT](https://github.com/fhanau/Efficient-Compression-Tool) by Felix Hanau,
  based on Google's [Zopfli](https://github.com/google/zopfli); both Apache
  License 2.0 (see `ect/LICENSE`).
