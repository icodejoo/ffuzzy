// Real usability tests of the built bundles, run with `node --test`.
// API mirrors ffuzzy.dart (strings / byKey / byKeys / addKey / update /
// removeAt / removeWhere / fuzzy / substring / prefix / postfix / exact).
import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  ffuzzyInitialize, ffuzzyReady, FuzzyCorpus, FuzzyKey, FuzzyKeyKind,
  FuzzyScoring, fuzzyCodepointToUtf16,
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
  assert.equal(hits[0].obj, 'src/main.dart');
  assert.ok(hits[0].score > 0);
  c.dispose();
});

test('full: all five modes', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['alpha', 'alphabet', 'beta', 'al/pha']);
  assert.ok(c.fuzzy('alph').length >= 2);
  assert.ok(c.substring('pha').length >= 1);
  assert.ok(c.prefix('alpha').length >= 2);
  assert.ok(c.postfix('bet').length >= 1);
  assert.equal(c.exact('beta').length, 1);
  c.dispose();
});

test('full: byKey + byKeys (Dart-aligned)', async () => {
  await ffuzzyInitialize();
  const rows = [{ name: 'Acme Inc', city: 'Boston' }, { name: 'Globex', city: 'Acme City' }];
  const byName = FuzzyCorpus.byKey(rows, 'name');
  assert.equal(byName.fuzzy('acme')[0].obj.name, 'Acme Inc');
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
  assert.equal(hits[0].obj, '张三');
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
  assert.equal(c.exact('apple').length, 0);
  c.update(0, 'blueberry');
  assert.equal(c.exact('blueberry').length, 1);
  assert.equal(c.removeWhere((s) => s.startsWith('b')), 1);
  assert.equal(c.length, 1);
  c.dispose();
});

test('full: highlight indices + codepoint→utf16', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['café_münchen']);
  const [hit] = c.fuzzy('cm');
  assert.ok(hit.indices.length >= 1);
  assert.equal(fuzzyCodepointToUtf16('café_münchen', hit.indices).length, hit.indices.length);
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
  assert.deepEqual(c.substring('中文').map((h) => h.obj), ['中文搜索']);
  assert.ok(c.fuzzy('app').length >= 2);
  c.dispose();
});
