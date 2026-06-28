// Real usability tests of the built bundles, run with `node --test`.
// Exercises the published entry points (ffuzzy.js + ffuzzy-lite.js) end-to-end:
// init, every search mode, multi-key, keyed maps, highlight, dispose.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  ffuzzyInitialize, ffuzzyReady, FuzzyCorpus, FuzzyKey, FuzzyKeyKind,
  fuzzyCodepointToUtf16,
} from '../ffuzzy.js';

test('full: init is idempotent + ffuzzyReady reflects state', async () => {
  assert.equal(ffuzzyReady(), false);
  await ffuzzyInitialize();
  assert.equal(ffuzzyReady(), true);
  await ffuzzyInitialize(); // idempotent, no throw
});

test('full: basic fuzzy search', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['src/main.dart', 'lib/widget.dart', 'README.md', '中文搜索']);
  assert.equal(c.length, 4);
  const hits = c.fuzzy('main');
  assert.ok(hits.length >= 1);
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

test('full: keyed map corpus searched by a field', async () => {
  await ffuzzyInitialize();
  const rows = [{ name: 'Acme Inc' }, { name: 'Globex' }];
  const c = FuzzyCorpus.keyed(rows, 'name');
  const hits = c.fuzzy('acme');
  assert.equal(hits[0].obj.name, 'Acme Inc');
  c.dispose();
});

test('full: multi-key (addKeyed) + matchedKind', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings([]);
  c.addKeyed('张三', [
    FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
    FuzzyKey.kind('zs', FuzzyKeyKind.initials),
  ]);
  const hits = c.fuzzy('zs');
  assert.equal(hits.length, 1);
  assert.equal(hits[0].obj, '张三');
  assert.equal(hits[0].matchedKind, FuzzyKeyKind.initials);
  c.dispose();
});

test('full: highlight indices + codepoint→utf16', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['café_münchen']);
  const [hit] = c.fuzzy('cm');
  assert.ok(hit.indices.length >= 1);
  const u16 = fuzzyCodepointToUtf16('café_münchen', hit.indices);
  assert.equal(u16.length, hit.indices.length);
  c.dispose();
});

test('full: clear empties the corpus', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['apple', 'banana']);
  c.clear();
  assert.equal(c.length, 0);
  assert.equal(c.fuzzy('apple').length, 0);
  c.dispose();
});

test('full: dispose is idempotent + use-after-dispose throws', async () => {
  await ffuzzyInitialize();
  const c = FuzzyCorpus.strings(['x']);
  c.dispose();
  c.dispose(); // idempotent
  assert.throws(() => c.fuzzy('x'), /after dispose/);
});

test('lite: separate bundle initializes and searches (ASCII + CJK)', async () => {
  const lite = await import('../ffuzzy-lite.js');
  await lite.ffuzzyInitialize();
  const c = lite.FuzzyCorpus.strings(['中文搜索', 'apple', 'app store']);
  assert.deepEqual(c.substring('中文').map((h) => h.obj), ['中文搜索']);
  assert.ok(c.fuzzy('app').length >= 2);
  c.dispose();
});
