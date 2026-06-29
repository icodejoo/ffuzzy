#!/usr/bin/env node
// Regenerate publishable bundles from source files:
//   ffuzzy.js / ffuzzy-lite.js   — engine + ffuzzy-corpus.mjs wrapper
//   ffuzzy.d.ts / ffuzzy-lite.d.ts — copied from *.d.ts.src (the editable source)
//
// Edit *.d.ts.src to update types, then run `npm run build`.
//
//   npm run build
import { readFileSync, writeFileSync } from 'node:fs';

const here = (f) => new URL('./' + f, import.meta.url);
const wrapper = readFileSync(here('ffuzzy-corpus.mjs'), 'utf8');

// JS bundles: engine + wrapper
const variants = [
  ['ffuzzy.engine.mjs', 'ffuzzy.js'],
  ['ffuzzy-lite.engine.mjs', 'ffuzzy-lite.js'],
];

for (const [engine, out] of variants) {
  const eng = readFileSync(here(engine), 'utf8').replace(/\n*$/, '\n');
  writeFileSync(here(out), eng + wrapper);
  console.log(`built ${out}  (${engine} + ffuzzy-corpus.mjs)`);
}

// TypeScript declarations: copy from *.d.ts.src
const dtsVariants = [
  ['ffuzzy.d.ts.src', 'ffuzzy.d.ts'],
  ['ffuzzy-lite.d.ts.src', 'ffuzzy-lite.d.ts'],
];

for (const [src, out] of dtsVariants) {
  writeFileSync(here(out), readFileSync(here(src), 'utf8'));
  console.log(`built ${out}  (from ${src})`);
}
