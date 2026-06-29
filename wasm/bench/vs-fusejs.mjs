/**
 * corpus.fuzzy vs fuse.js — ranked fuzzy search, apples-to-apples.
 * Both return ranked results (fuse.js uses its default scoring).
 *
 * Usage:  node bench/vs-fusejs.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';
import Fuse from 'fuse.js';

const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const baseMock = JSON.parse(readFileSync(mockPath, 'utf8'));
await ffuzzyInitialize();

const WARMUP = 50, REPS = 200;
function bench(fn) {
  for (let i = 0; i < WARMUP; i++) fn();
  const t = performance.now();
  for (let i = 0; i < REPS; i++) fn();
  return (performance.now() - t) / REPS;
}
const ms = t => (t >= 1 ? t.toFixed(2) + ' ms' : (t*1000).toFixed(0) + ' µs').padStart(10);

const QUERIES = ['gems', 'plinko', 'super', 'sp', 'NOTFOUND'];

console.log('='.repeat(74));
console.log(' corpus.fuzzy vs fuse.js  —  ranked results, limit 50');
console.log('='.repeat(74));

for (const mult of [1, 2, 5, 10]) {
  const N = baseMock.length * mult;
  const mock = Array.from({ length: mult }, () => baseMock).flat().slice(0, N);

  // build corpus
  const t0 = performance.now();
  const corpus = FuzzyCorpus.byKey(mock, 'gameName');
  const corpusBuild = performance.now() - t0;

  // build fuse
  const t1 = performance.now();
  const fuse = new Fuse(mock, { keys: ['gameName'], limit: 50, threshold: 0.4 });
  const fuseBuild = performance.now() - t1;

  console.log(`\n── ${N} items  (corpus build: ${corpusBuild.toFixed(1)} ms  │  fuse build: ${fuseBuild.toFixed(1)} ms) ─`);
  console.log('  Query        fuse.js       corpus.fuzzy   speedup   fuse/corpus hits');
  console.log('  ' + '─'.repeat(67));

  for (const q of QUERIES) {
    const fuseMs   = bench(() => fuse.search(q).slice(0, 50));
    const corpusMs = bench(() => corpus.fuzzy(q, { limit: 50 }));
    const speedup  = (fuseMs / corpusMs).toFixed(1) + 'x';
    const fHits    = fuse.search(q).slice(0, 50).length;
    const cHits    = corpus.fuzzy(q, { limit: 50 }).length;
    console.log(`  ${q.padEnd(12)} ${ms(fuseMs)}   ${ms(corpusMs)}   ${speedup.padStart(6)}    ${fHits}/${cHits}`);
  }

  corpus.dispose();
}

// ── memory reality check ──────────────────────────────────────────────────────
console.log('\n' + '='.repeat(74));
console.log(' Practical browser limits');
console.log('='.repeat(74));
// Each game object in mock has many fields — estimate real object size
const avgObjBytes = JSON.stringify(baseMock[0]).length;
console.log(`  Avg object JSON size: ${avgObjBytes} bytes`);
console.log();
for (const mult of [1, 2, 5, 10, 20, 50]) {
  const N = baseMock.length * mult;
  const objMB    = (N * avgObjBytes / 1024 / 1024).toFixed(0);
  const corpusMB = (N * 15 / 1024 / 1024).toFixed(0);  // ~15 bytes/item for gameName strings
  const fuseMB   = (N * avgObjBytes * 1.5 / 1024 / 1024).toFixed(0);  // fuse indexes internally
  const feasible = N * avgObjBytes < 200 * 1024 * 1024 ? '✓' : '⚠ heavy';
  console.log(`  ${String(N).padStart(7)} items   JS objects ~${objMB.padStart(4)} MB   corpus +${corpusMB.padStart(3)} MB   fuse +${fuseMB.padStart(4)} MB   ${feasible}`);
}
