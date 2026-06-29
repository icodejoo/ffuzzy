/**
 * Prefix / Postfix benchmark: FuzzyCorpus vs Array.filter + startsWith/endsWith
 * Default FuzzyCase.smart: queries with uppercase = case-sensitive (mirrors filter behaviour).
 *
 * Usage:  node bench/prefix.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';

const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const mock = JSON.parse(readFileSync(mockPath, 'utf8'));
await ffuzzyInitialize();

// ── helpers ───────────────────────────────────────────────────────────────────
const ms = t => t.toFixed(2) + ' ms';
const WARMUP = 300, REPS = 3000;

function bench(fn) {
  for (let i = 0; i < WARMUP; i++) fn();
  const t = performance.now();
  for (let i = 0; i < REPS; i++) fn();
  return (performance.now() - t) / REPS * 1000;   // µs/call
}

// ── build corpus ──────────────────────────────────────────────────────────────
const t0 = performance.now();
// gameName-only corpus for apples-to-apples comparison with filter(gameName)
const corpus = FuzzyCorpus.byKey(mock, 'gameName');
const buildMs = performance.now() - t0;

console.log('='.repeat(64));
console.log(' Prefix / Postfix benchmark  —  4886 items');
console.log(' Corpus: byKey("gameName")   Filter: gameName.startsWith/endsWith');
console.log('='.repeat(64));
console.log(`Corpus build: ${ms(buildMs)}\n`);

// ── table helper ──────────────────────────────────────────────────────────────
const W = [28, 10, 10, 8, 7, 7];
const divider = W.map(n => '─'.repeat(n)).join('─┼─');
const row = cols => cols.map((c, i) => String(c).padEnd(W[i])).join(' │ ');

function table(title, queries, corpusFn, filterFn) {
  console.log(`── ${title} ${'─'.repeat(60 - title.length)}`);
  console.log(row(['Query', 'filter µs', 'corpus µs', 'ratio', 'f-hits', 'c-hits']));
  console.log(divider);
  for (const [label, q] of queries) {
    const fUs  = bench(() => filterFn(q));
    const cUs  = bench(() => corpusFn(q));
    const fN   = filterFn(q).length;
    const cN   = corpusFn(q).length;
    const diff = fN !== cN ? `✗(f${fN}/c${cN})` : '✓';
    const ratio = (fUs / cUs).toFixed(2) + 'x';
    console.log(row([`${label} "${q}"`, fUs.toFixed(1), cUs.toFixed(1), ratio, fN, diff]));
  }
  console.log();
}

// ── prefix ────────────────────────────────────────────────────────────────────
// Filter uses plain startsWith (case-sensitive); corpus uses default FuzzyCase.smart
// — when query has uppercase, smart == case-sensitive, so results match.
const prefixFilter = q => mock.filter(g => g.gameName?.startsWith(q));
const prefixCorpus = q => corpus.prefix(q);

table('PREFIX search', [
  ['1 char,  578 hits', 'S'],
  ['2 chars, 131 hits', 'Su'],
  ['4 chars,  92 hits', 'Supe'],
  ['6 chars,  30 hits', 'Super '],
  ['8 chars,   9 hits', 'Super G'],
  ['full,      1 hit ', 'Super Gems 1000'],
  ['0 hits          ', 'ZZNOTFOUND'],
], prefixCorpus, prefixFilter);

// ── postfix ───────────────────────────────────────────────────────────────────
const postfixFilter = q => mock.filter(g => g.gameName?.endsWith(q));
const postfixCorpus = q => corpus.postfix(q);

table('POSTFIX search', [
  ['1 char,  165 hits', 'e'],
  ['4 chars,  17 hits', '1000'],
  ['7 chars,   4 hits', 'nd Spin'],
  ['full,      1 hit ', 'Super Gems 1000'],
  ['0 hits          ', 'ZZNOTFOUND'],
], postfixCorpus, postfixFilter);

// ── sustained N queries ───────────────────────────────────────────────────────
// Cycle through a realistic query pool (simulates search-box typing)
const prefixPool = ['S','Su','Sup','Supe','Super','Super ','Super G','Super Ge','Super Gem'];

function sustained(N, corpusFn, filterFn) {
  const n = prefixPool.length;
  for (let i = 0; i < 100; i++) { filterFn(prefixPool[i % n]); corpusFn(prefixPool[i % n]); }
  const t1 = performance.now();
  for (let i = 0; i < N; i++) filterFn(prefixPool[i % n]);
  const ft = performance.now() - t1;
  const t2 = performance.now();
  for (let i = 0; i < N; i++) corpusFn(prefixPool[i % n]);
  const ct = performance.now() - t2;
  return [ft, ct];
}

console.log('── sustained prefix (pool: 1-9 char queries for "Super…") ──────');
console.log('  N queries │ filter total │ corpus total │  speedup');
console.log('  ──────────────────────────────────────────────────');
for (const N of [10, 50, 200, 500, 1000, 5000]) {
  const [ft, ct] = sustained(N, prefixCorpus, prefixFilter);
  const speedup = (ft / ct).toFixed(2);
  console.log(`  ${String(N).padStart(8)}   ${ms(ft).padStart(11)}   ${ms(ct).padStart(11)}   ${speedup}x`);
}

console.log();
// ── break-even ────────────────────────────────────────────────────────────────
const avgF = bench(() => prefixFilter('Super G'));
const avgC = bench(() => prefixCorpus('Super G'));
const saving = avgF - avgC;
console.log(`── break-even (query "Super G", 9 hits) ────────────────────────`);
console.log(`  filter avg:  ${avgF.toFixed(1)} µs`);
console.log(`  corpus avg:  ${avgC.toFixed(1)} µs`);
console.log(`  build cost:  ${ms(buildMs)}`);
if (saving > 0) {
  const be = Math.ceil(buildMs * 1000 / saving);
  console.log(`  break-even:  ${be} queries  (corpus faster after this point)`);
} else {
  console.log(`  break-even:  never for this query length — corpus slower than filter`);
}
console.log();

corpus.dispose();
