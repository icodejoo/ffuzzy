/**
 * Sustained-query benchmark: corpus built once, queried N times.
 * Simulates real usage: search box / batch lookup over a long session.
 *
 * Usage:  node bench/sustained.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';

const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const mock = JSON.parse(readFileSync(mockPath, 'utf8'));
await ffuzzyInitialize();

// ── helpers ───────────────────────────────────────────────────────────────────
const ms = (t) => t.toFixed(2) + ' ms';
const us = (t) => (t * 1000).toFixed(1) + ' µs';

function totalMs(fn, n) {
  for (let i = 0; i < 50; i++) fn(i % queries.length);  // warmup
  const t = performance.now();
  for (let i = 0; i < n; i++) fn(i % queries.length);
  return performance.now() - t;
}

// ── query pool ────────────────────────────────────────────────────────────────
// Mix of gameId exact, gameName exact, gameName fuzzy — mirrors real search box
const queries = [
  '101024', 'PSS-ON-00171', '560', '1', 'NOTFOUND',
  'Super Gems 1000', 'Plinko Classic', 'Phoenix Legend', 'NOTFOUND GAME',
  'gems', 'plinko', 'super', 'legend', 'king',
];

// ── build corpus once ─────────────────────────────────────────────────────────
const t0 = performance.now();
const corpus = FuzzyCorpus.byKeys(mock, ['gameName', 'gameId']);
const buildMs = performance.now() - t0;

console.log('='.repeat(60));
console.log(' Sustained-query benchmark  —  4886 items');
console.log('='.repeat(60));
console.log(`Corpus build:  ${ms(buildMs)}`);
console.log(`Query pool:    ${queries.length} distinct queries, cycled round-robin`);
console.log();

// ── scenario 1: exact — corpus vs filter ─────────────────────────────────────
const exactQueries = queries.slice(0, 9);  // gameId + gameName exact queries

function filterExact(i) {
  const q = exactQueries[i % exactQueries.length];
  return mock.filter(g => g.gameId === q || g.gameName === q);
}
function corpusExact(i) {
  return corpus.exact(queries[i % queries.length]);
}

console.log('── exact: corpus vs Array.filter ───────────────────────────────');
console.log('  N queries │ filter total │ corpus total │  speedup │ corpus QPS');
console.log('  ─────────────────────────────────────────────────────────────');

for (const N of [10, 50, 200, 500, 1000, 5000, 10000]) {
  const ft = totalMs(filterExact, N);
  const ct = totalMs(corpusExact, N) + (N === 10 ? buildMs : 0);  // build amortised only at N=10
  const ct_raw = totalMs(corpusExact, N);
  const speedup = (ft / ct_raw).toFixed(2);
  const qps = Math.round(N / (ct_raw / 1000));
  const note = N === 10 ? ' (incl. build)' : '';
  console.log(`  ${String(N).padStart(8)}   ${ms(ft).padStart(11)}   ${ms(ct_raw).padStart(11)}   ${speedup.padStart(7)}x   ${String(qps).padStart(8)} q/s${note}`);
}

console.log();
console.log('── break-even analysis ─────────────────────────────────────────');
const avgFilterUs = totalMs(filterExact, 2000) / 2000 * 1000;
const avgCorpusUs = totalMs(corpusExact, 2000) / 2000 * 1000;
const savingPerQuery = (avgFilterUs - avgCorpusUs) / 1000;  // ms saved per query (negative = corpus slower)
const breakEven = savingPerQuery > 0
  ? Math.ceil(buildMs / savingPerQuery)
  : null;
console.log(`  filter avg:   ${us(avgFilterUs / 1000)} / query`);
console.log(`  corpus avg:   ${us(avgCorpusUs / 1000)} / query`);
console.log(`  build cost:   ${ms(buildMs)}`);
if (breakEven !== null) {
  console.log(`  break-even:   ${breakEven} queries  (corpus faster after this point)`);
} else {
  console.log(`  break-even:   never — corpus exact is slower than filter for this dataset`);
  console.log(`  → use Map<id, item> for O(1) exact lookup; save corpus for fuzzy/prefix`);
}

// ── scenario 2: fuzzy — corpus dominates ─────────────────────────────────────
const fuzzyPool = ['gems', 'plinko', 'super', 'legend', 'king', 'phoen', 'PSS', 'pl'];

console.log();
console.log('── fuzzy search (corpus only — filter has no equivalent) ────────');
console.log('  query        │  avg µs │  hits');
console.log('  ────────────────────────────');
for (const q of fuzzyPool) {
  const t = performance.now();
  const REPS = 500;
  let hits = 0;
  for (let i = 0; i < REPS; i++) hits = corpus.fuzzy(q, { limit: 50 }).length;
  const avgUs = (performance.now() - t) / REPS * 1000;
  console.log(`  ${q.padEnd(12)}   ${String(avgUs.toFixed(1)).padStart(6)}   ${hits}`);
}

// ── scenario 3: mixed workload (exact + fuzzy together) ───────────────────────
console.log();
console.log('── mixed workload (50% exact + 50% fuzzy, N=1000) ──────────────');
const mixedQueries = [...exactQueries, ...fuzzyPool];
function filterMixed(i) {
  const q = exactQueries[i % exactQueries.length];
  return mock.filter(g => g.gameId === q || g.gameName === q);
}
function corpusMixed(i) {
  const q = mixedQueries[i % mixedQueries.length];
  return i % 2 === 0 ? corpus.exact(q) : corpus.fuzzy(q, { limit: 50 });
}
const N = 1000;
const ftMix = totalMs(filterMixed, N);
const ctMix = totalMs(corpusMixed, N);
console.log(`  filter (exact only, can't do fuzzy): ${ms(ftMix)}`);
console.log(`  corpus (exact + fuzzy):              ${ms(ctMix)}`);
console.log(`  corpus handles ${Math.round(N/2)} fuzzy queries filter can't touch`);

console.log();
corpus.dispose();
