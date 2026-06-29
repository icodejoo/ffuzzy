/**
 * Scale benchmark: corpus.fuzzy vs a simple JS fuzzy at different dataset sizes.
 * "JS fuzzy" = naive subsequence check (same algorithm class, pure JS).
 *
 * Usage:  node bench/scale.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';

const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const baseMock = JSON.parse(readFileSync(mockPath, 'utf8'));
await ffuzzyInitialize();

// ── naive JS fuzzy (subsequence, no scoring) ─────────────────────────────────
function jsFuzzy(items, getStr, query, limit) {
  const q = query.toLowerCase();
  const out = [];
  for (const item of items) {
    const s = getStr(item).toLowerCase();
    let qi = 0;
    for (let si = 0; si < s.length && qi < q.length; si++) {
      if (s[si] === q[qi]) qi++;
    }
    if (qi === q.length) out.push(item);
    if (out.length >= limit) break;
  }
  return out;
}

// ── helpers ───────────────────────────────────────────────────────────────────
const WARMUP = 100, REPS = 500;
function bench(fn) {
  for (let i = 0; i < WARMUP; i++) fn();
  const t = performance.now();
  for (let i = 0; i < REPS; i++) fn();
  return (performance.now() - t) / REPS;
}

const ms  = t => t.toFixed(2).padStart(8) + ' ms';
const sep = '─'.repeat(72);

// ── test queries ──────────────────────────────────────────────────────────────
const QUERIES = ['gems', 'plinko', 'sp'];

console.log('='.repeat(72));
console.log(' Scale benchmark: corpus.fuzzy vs plain-JS fuzzy (limit: 50)');
console.log(' JS fuzzy = naive subsequence loop, no scoring, stops at limit');
console.log('='.repeat(72));

for (const query of QUERIES) {
  console.log(`\nQuery: "${query}"`);
  console.log(sep);
  console.log('  Items    Build    JS-fuzzy    corpus.fuzzy   speedup   JS/corpus hits');
  console.log(sep);

  // Sizes: 1× to 10× the base mock
  for (const mult of [1, 2, 5, 10, 20]) {
    const N = baseMock.length * mult;
    const mock = Array.from({ length: mult }, () => baseMock).flat().slice(0, N);

    // build corpus
    const t0 = performance.now();
    const corpus = FuzzyCorpus.byKey(mock, 'gameName');
    const buildMs = performance.now() - t0;

    const jsMs  = bench(() => jsFuzzy(mock, g => g.gameName ?? '', query, 50));
    const cMs   = bench(() => corpus.fuzzy(query, { limit: 50 }));
    const ratio = (jsMs / cMs).toFixed(2) + 'x';

    const jsHits  = jsFuzzy(mock, g => g.gameName ?? '', query, 50).length;
    const cHits   = corpus.fuzzy(query, { limit: 50 }).length;

    console.log(`  ${String(N).padStart(6)}  ${ms(buildMs)}  ${ms(jsMs)}  ${ms(cMs)}  ${ratio.padStart(7)}  ${jsHits}/${cHits}`);
    corpus.dispose();
  }
}

// ── memory estimate ───────────────────────────────────────────────────────────
console.log(`\n${sep}`);
console.log(' Memory estimate (gameName strings only):');
console.log(sep);
for (const mult of [1, 2, 5, 10, 20]) {
  const N = baseMock.length * mult;
  const avgBytes = baseMock.reduce((s, g) => s + (g.gameName?.length ?? 0), 0) / baseMock.length;
  const strMB  = (N * avgBytes / 1024 / 1024).toFixed(1);
  const corpusMB = (N * avgBytes * 2.5 / 1024 / 1024).toFixed(1);  // ~2.5x overhead in WASM
  const jsMB   = (N * 200 / 1024 / 1024).toFixed(1);  // ~200 bytes per JS object
  console.log(`  ${String(N).padStart(6)} items   string data ~${strMB} MB   corpus ~${corpusMB} MB   JS objects ~${jsMB} MB`);
}
