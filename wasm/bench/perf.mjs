/**
 * Performance benchmark: FuzzyCorpus.byKeys.exact() vs Array.filter()
 * Dataset: mock.json (4886 game records)
 *
 * Usage:  node bench/perf.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';

// ── data ─────────────────────────────────────────────────────────────────────
const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const mock = JSON.parse(readFileSync(mockPath, 'utf8'));
console.log(`Dataset: ${mock.length} items\n`);

await ffuzzyInitialize();

// ── build corpus ──────────────────────────────────────────────────────────────
const t0 = performance.now();
const corpus = FuzzyCorpus.byKeys(mock, ['gameName', 'gameId']);
const buildMs = (performance.now() - t0).toFixed(2);
console.log(`Corpus build: ${buildMs} ms`);
console.log(`(amortised over N queries: ${(parseFloat(buildMs)/1000*1e6).toFixed(0)} µs / query for N=1000)\n`);

// ── queries ───────────────────────────────────────────────────────────────────
const QUERIES = [
  // [label,           field,      value]
  ['gameId  hit  ',   'gameId',   '101024'],
  ['gameId  hit  ',   'gameId',   'PSS-ON-00171'],
  ['gameId  hit  ',   'gameId',   '560'],
  ['gameId  miss ',   'gameId',   'NOTFOUND-XYZ'],
  ['gameName hit ',   'gameName', 'Super Gems 1000'],
  ['gameName hit ',   'gameName', 'Plinko Classic'],
  ['gameName miss',   'gameName', 'NOTFOUND GAME'],
];

// ── benchmark helper ─────────────────────────────────────────────────────────
const WARMUP = 300;
const RUNS   = 3000;

function bench(fn) {
  for (let i = 0; i < WARMUP; i++) fn();
  const t = performance.now();
  for (let i = 0; i < RUNS; i++) fn();
  return (performance.now() - t) / RUNS;   // ms per call
}

// ── table ─────────────────────────────────────────────────────────────────────
const COL = [30, 11, 11, 9, 8, 7];
const hdr = ['Query', 'filter µs', 'corpus µs', 'ratio', 'hits', 'same?'];
const sep  = COL.map(n => '-'.repeat(n)).join('  ');
const row  = cols => cols.map((c, i) => String(c).padEnd(COL[i])).join('  ');

console.log(row(hdr));
console.log(sep);

for (const [label, field, value] of QUERIES) {
  const filterFn = field === 'gameId'
    ? () => mock.filter(g => g.gameId   === value)
    : () => mock.filter(g => g.gameName === value);
  const corpusFn = () => corpus.exact(value);

  const filterUs = bench(filterFn) * 1000;
  const corpusUs = bench(corpusFn) * 1000;

  const fHits = filterFn().length;
  const cHits = corpusFn().length;
  const same  = fHits === cHits ? '✓' : `✗(f${fHits}/c${cHits})`;
  const ratio = (filterUs / corpusUs).toFixed(2) + 'x';

  console.log(row([
    `${label} "${value.slice(0, 20)}"`,
    filterUs.toFixed(1),
    corpusUs.toFixed(1),
    ratio,
    fHits,
    same,
  ]));
}

console.log(sep);
console.log('\nratio = filter_time / corpus_time  (>1 = corpus faster; <1 = filter faster)');
console.log(`build cost spreads across all queries — break-even vs filter: ` +
  `${(parseFloat(buildMs) * 1000 / 47).toFixed(0)} queries\n`);

// ── fuzzy bonus ───────────────────────────────────────────────────────────────
console.log('── Fuzzy search (corpus only, no filter equivalent) ────────────────');
const fuzzyQueries = ['gems', 'plinko', 'super 1000', 'PSS-ON'];
for (const q of fuzzyQueries) {
  const us = bench(() => corpus.fuzzy(q)) * 1000;
  const hits = corpus.fuzzy(q).length;
  console.log(`  fuzzy("${q.padEnd(12)}")  ${us.toFixed(1)} µs  →  ${hits} hits`);
}

corpus.dispose();
