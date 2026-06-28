#!/usr/bin/env node
// Regenerate the publishable bundles (ffuzzy.js / ffuzzy-lite.js) by appending
// the high-level wrapper (ffuzzy-corpus.mjs) to each committed WASM engine module
// (*.engine.mjs, produced by build-engine.sh). No dependencies.
//
//   npm run build
import { readFileSync, writeFileSync } from 'node:fs';

const here = (f) => new URL('./' + f, import.meta.url);
const wrapper = readFileSync(here('ffuzzy-corpus.mjs'), 'utf8');

const variants = [
  ['ffuzzy.engine.mjs', 'ffuzzy.js'],
  ['ffuzzy-lite.engine.mjs', 'ffuzzy-lite.js'],
];

for (const [engine, out] of variants) {
  const eng = readFileSync(here(engine), 'utf8').replace(/\n*$/, '\n');
  writeFileSync(here(out), eng + wrapper);
  console.log(`built ${out}  (${engine} + ffuzzy-corpus.mjs)`);
}
