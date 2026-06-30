/**
 * @codejoo/ffuzzy vs fuzzysort vs fuse.js — ranked fuzzy search benchmark.
 *
 * Usage:  node bench/vs-fuzzysort.mjs
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { ffuzzyInitialize, FuzzyCorpus } from '../ffuzzy.js';
import fuzzysort from '../node_modules/fuzzysort/fuzzysort.js';
import Fuse from '../node_modules/fuse.js/dist/fuse.mjs';

const mockPath = fileURLToPath(new URL('../../mock.json', import.meta.url));
const baseMock = JSON.parse(readFileSync(mockPath, 'utf8'));
await ffuzzyInitialize();

// ── helpers ───────────────────────────────────────────────────────────────────
const WARMUP = 100, REPS = 300;
function bench(fn) {
  for (let i = 0; i < WARMUP; i++) fn();
  const t = performance.now();
  for (let i = 0; i < REPS; i++) fn();
  return (performance.now() - t) / REPS;
}
const ms = (t, w = 10) => (t >= 1 ? t.toFixed(2) + ' ms' : (t * 1000).toFixed(0) + ' µs').padStart(w);
const sep = (n = 78) => '─'.repeat(n);

const LIMIT = 50;
const QUERIES = ['gems', 'plinko', 'super', 'king', 'sp', 'phoen', 'NOTFOUND'];

// ── print table ───────────────────────────────────────────────────────────────
function printTable(N, mock, gameNames) {
  // Build each engine's corpus/index
  const t0 = performance.now();
  const ffuzzyCorpus = FuzzyCorpus.byKey(mock, 'gameName');
  const ffuzzyBuild = performance.now() - t0;

  const t1 = performance.now();
  const fsortKeys = fuzzysort.prepare ? gameNames.map(s => fuzzysort.prepare(s)) : gameNames;
  const fuzzysortBuild = performance.now() - t1;

  const t2 = performance.now();
  const fuse = new Fuse(mock, { keys: ['gameName'], limit: LIMIT, threshold: 0.4 });
  const fuseBuild = performance.now() - t2;

  console.log(`\n${'═'.repeat(78)}`);
  console.log(` ${N} items   │  build: ffuzzy ${ffuzzyBuild.toFixed(1)} ms  │  fuzzysort ${fuzzysortBuild.toFixed(1)} ms  │  fuse.js ${fuseBuild.toFixed(1)} ms`);
  console.log('═'.repeat(78));
  console.log(`  ${'Query'.padEnd(12)}  ${'ffuzzy'.padStart(10)}  ${'fuzzysort'.padStart(10)}  ${'fuse.js'.padStart(10)}  ${'ff-ratio'.padStart(9)}  ${'fs-ratio'.padStart(9)}`);
  console.log('  ' + sep(73));

  for (const q of QUERIES) {
    const ffuzzyMs  = bench(() => ffuzzyCorpus.fuzzy(q, { limit: LIMIT }));
    const fsortMs   = bench(() => fuzzysort.go(q, fsortKeys, { limit: LIMIT }));
    const fuseMs    = bench(() => fuse.search(q).slice(0, LIMIT));

    const ffuzzyHits  = ffuzzyCorpus.fuzzy(q, { limit: LIMIT }).length;
    const fsortHits   = fuzzysort.go(q, fsortKeys, { limit: LIMIT }).length;
    const fuseHits    = fuse.search(q).slice(0, LIMIT).length;

    // ratio: how many × faster is ffuzzy vs each competitor
    const vsFs  = (fsortMs  / ffuzzyMs).toFixed(2) + 'x';
    const vsFu  = (fuseMs   / ffuzzyMs).toFixed(2) + 'x';

    console.log(
      `  ${('"' + q + '"').padEnd(12)}` +
      `  ${ms(ffuzzyMs)}` +
      `  ${ms(fsortMs)}` +
      `  ${ms(fuseMs)}` +
      `  ${vsFs.padStart(9)}` +
      `  ${vsFu.padStart(9)}` +
      `   [${ffuzzyHits}/${fsortHits}/${fuseHits}]`
    );
  }

  ffuzzyCorpus.dispose();
}

// ── result quality check ──────────────────────────────────────────────────────
function qualityCheck(mock, gameNames) {
  console.log(`\n${'═'.repeat(78)}`);
  console.log(' Result quality: top-5 for "super" (same 4886-item corpus)');
  console.log('═'.repeat(78));

  const corpus  = FuzzyCorpus.byKey(mock, 'gameName');
  const fsortKs = gameNames.map(s => fuzzysort.prepare(s));
  const fuse    = new Fuse(mock, { keys: ['gameName'], threshold: 0.4 });

  const ffHits = corpus.fuzzy('super', { limit: 5, highlight: true });
  const fsHits = fuzzysort.go('super', fsortKs, { limit: 5 });
  const fuHits = fuse.search('super').slice(0, 5);

  const maxLen = Math.max(ffHits.length, fsHits.length, fuHits.length);
  console.log(`  ${'ffuzzy'.padEnd(30)}  ${'fuzzysort'.padEnd(30)}  ${'fuse.js'.padEnd(28)}`);
  console.log('  ' + sep(93));
  for (let i = 0; i < maxLen; i++) {
    const a = (ffHits[i]?.raw?.gameName ?? '').slice(0, 28).padEnd(30);
    const b = (fsHits[i]?.target ?? '').slice(0, 28).padEnd(30);
    const c = (fuHits[i]?.item?.gameName ?? '').slice(0, 26).padEnd(28);
    console.log(`  ${a}  ${b}  ${c}`);
  }

  corpus.dispose();
}

// ── scoring algorithm note ────────────────────────────────────────────────────
function algorithmNote() {
  console.log(`\n${'═'.repeat(78)}`);
  console.log(' Algorithm summary');
  console.log('═'.repeat(78));
  console.log('  ffuzzy    nucleo-class DP (subsequence + positional bonuses)  C / WASM');
  console.log('  fuzzysort Smith–Waterman-style DP, prefix/consecutive bonuses  pure JS');
  console.log('  fuse.js   Bitap (shift-or) algorithm, Levenshtein-based        pure JS');
  console.log();
  console.log('  ratio columns: fuzzysort_time / ffuzzy_time  and  fusejs_time / ffuzzy_time');
  console.log('  >1 = ffuzzy faster;  [a/b/c] = hit counts per engine');
}

// ── run ───────────────────────────────────────────────────────────────────────
const gameNames4886 = baseMock.map(g => g.gameName ?? '');
qualityCheck(baseMock, gameNames4886);

for (const mult of [1, 2, 5, 10]) {
  const N = baseMock.length * mult;
  const mock = Array.from({ length: mult }, () => baseMock).flat().slice(0, N);
  const names = mock.map(g => g.gameName ?? '');
  printTable(N, mock, names);
}

algorithmNote();
