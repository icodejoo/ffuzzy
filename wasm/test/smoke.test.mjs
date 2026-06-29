// Real usability tests of the built bundles, run with `node --test`.
// High-level API: fuzzy() and fuzzyRaws() only.
// For prefix/postfix/exact/substring use native Array.filter.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  ffuzzyInitialize, ffuzzyReady, FuzzyCorpus, FuzzyKey, FuzzyKeyKind,
  FuzzyScoring, fuzzyCodepointToUtf16, highlightHtml,
} from '../ffuzzy.js';

test('full: init idempotent + ffuzzyReady', async () => {
  assert.equal(ffuzzyReady(), false);
  await ffuzzyInitialize();
  assert.equal(ffuzzyReady(), true);
  await ffuzzyInitialize();
});

test('full: basic fuzzy search', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['src/main.dart', 'lib/widget.dart', 'README.md', '中文搜索']);
  assert.equal(c.length, 4);
  const hits = c.fuzzy('main');
  assert.equal(hits[0].raw, 'src/main.dart');
  assert.ok(hits[0].score > 0);
  c.dispose();
});

test('full: fuzzy search + fuzzyRaws', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['alpha', 'alphabet', 'beta', 'al/pha']);
  assert.ok(c.fuzzy('alph').length >= 2);
  assert.ok(c.fuzzy('bet').length >= 1);
  // fuzzyRaws returns raw items directly
  const raws = c.fuzzyRaws('alph');
  assert.ok(raws.length >= 2);
  assert.ok(typeof raws[0] === 'string');
  c.dispose();
});

test('full: byKey + byKeys (Dart-aligned)', async () => {
  await ffuzzyInitialize();
  const rows = [{ name: 'Acme Inc', city: 'Boston' }, { name: 'Globex', city: 'Acme City' }];
  const byName = FuzzyCorpus.byKey(rows, 'name');
  assert.equal(byName.fuzzy('acme')[0].raw.name, 'Acme Inc');
  byName.dispose();

  const byBoth = FuzzyCorpus.byKeys(rows, ['name', 'city']);
  assert.ok(byBoth.fuzzy('acme').length >= 1); // matches name or city
  byBoth.dispose();
});

test('full: addKey multi-key + matchedKind', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings([]);
  c.addKey('张三', [
    FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
    FuzzyKey.kind('zs', FuzzyKeyKind.initials),
  ]);
  const hits = c.fuzzy('zs');
  assert.equal(hits[0].raw, '张三');
  assert.equal(hits[0].matchedKind, FuzzyKeyKind.initials);
  c.dispose();
});

test('full: scoring modes (off → score 0, insertion order)', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['cfg_helper', 'configure', 'my_cfg']);
  const off = c.fuzzy('cfg', { scoring: FuzzyScoring.off });
  assert.ok(off.length >= 1 && off.every((h) => h.score === 0));
  const fast = c.fuzzy('cfg', { scoring: FuzzyScoring.fast });
  assert.ok(fast.some((h) => h.score > 0));
  c.dispose();
});

test('full: mutation (removeAt / update / removeWhere)', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['apple', 'banana', 'cherry']);
  c.removeAt(0);
  assert.equal(c.length, 2);
  assert.equal(c.fuzzy('apple').length, 0);        // 'apple' gone; 'banana'/'cherry' don't match
  c.update(0, 'blueberry');
  assert.equal(c.fuzzy('blueberry')[0]?.raw, 'blueberry');
  assert.equal(c.removeWhere((s) => s.startsWith('b')), 1);
  assert.equal(c.length, 1);
  c.dispose();
});

test('full: highlight:true populates indices + highlightHtml', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['café_münchen']);
  // highlight:false (default) — indices is empty
  const [fast] = c.fuzzy('cm');
  assert.deepEqual(fast.indices, []);
  // highlight:true — indices populated, fuzzyCodepointToUtf16 converts them
  const [hit] = c.fuzzy('cm', { highlight: true });
  assert.ok(hit.indices.length >= 1);
  const u16 = fuzzyCodepointToUtf16('café_münchen', hit.indices);
  assert.equal(u16.length, hit.indices.length);
  // highlightHtml wraps matched chars, merges adjacent, escapes HTML
  const html = highlightHtml('src/main.dart', [0, 1, 2]);
  assert.equal(html, '<mark>src</mark>/main.dart');
  const safeHtml = highlightHtml('<b>item</b>', [0]);
  assert.ok(safeHtml.startsWith('<mark>&lt;</mark>') || safeHtml.includes('&lt;'));
  c.dispose();
});

test('full: dispose idempotent + use-after-dispose throws', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['x']);
  c.dispose();
  c.dispose();
  assert.throws(() => c.fuzzy('x'), /after dispose/);
});

test('lite: separate bundle, ASCII + CJK', async () => {
  const lite = await import('../ffuzzy-lite.js');
  await lite.ffuzzyInitialize();
  const c = lite.FuzzyCorpus.strings(['中文搜索', 'apple', 'app store']);
  const hits = c.fuzzy('中文');
  assert.deepEqual(hits.map((h) => h.raw), ['中文搜索']);
  assert.ok(c.fuzzy('app').length >= 2);
  c.dispose();
});
