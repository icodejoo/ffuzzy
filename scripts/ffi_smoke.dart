// End-to-end smoke test of the Dart FFI binding against a real built library.
// Usage: dart run tool/ffi_smoke.dart [path/to/libffz.so]
// Exits non-zero on any failure (used by CI).
// ignore_for_file: avoid_print
import 'package:ffuzzy/ffuzzy.dart';

Future<void> main(List<String> args) async {
  final libPath = args.isNotEmpty ? args[0] : null;
  final c = FuzzyCorpus.strings(const [], libraryPath: libPath);

  c.addAll(['src/main.rs', 'lib/ffz.dart', '中文搜索引擎', 'README.md', 'café']);
  if (c.length != 5) throw 'length ${c.length} != 5';

  final fuzzy = c.fuzzy('src', limit: 10);
  if (fuzzy.isEmpty) throw 'expected fuzzy hits for "src"';
  if (fuzzy.first.raw.isEmpty) throw 'hit.raw should be the original item text';

  final cjk = c.fuzzy('中文');
  if (cjk.isEmpty) throw 'expected a CJK hit for "中文"';

  final fold = c.fuzzy('cafe'); // diacritic fold café≈cafe
  if (fold.isEmpty) throw 'expected diacritic-folded hit for "cafe"';

  final pref = c.prefix('READ', caseMatching: FuzzyCase.ignore);
  if (pref.isEmpty) throw 'expected prefix hit for "READ"';

  // highlight:false (default) — indices empty; highlight:true — indices populated.
  final noHL = c.fuzzy('src', limit: 1);
  if (noHL.first.indices.isNotEmpty) throw 'highlight:false should yield empty indices';
  final hlHits = c.fuzzy('src', limit: 1, highlight: true);
  if (hlHits.first.indices.isEmpty) throw 'highlight:true should yield indices';
  fuzzyCodepointToUtf16('src/main.rs', hlHits.first.indices);

  // The async twin must agree with the synchronous method, element-by-element.
  final async = await c.fuzzyAsync('src', limit: 10);
  if (async.length != fuzzy.length) {
    throw 'filterAsync len ${async.length} != ${fuzzy.length}';
  }
  for (var i = 0; i < async.length; i++) {
    if (async[i].index != fuzzy[i].index || async[i].score != fuzzy[i].score) {
      throw 'filterAsync mismatch at $i';
    }
  }

  // addKey: a CJK item findable by host-computed pinyin/initials.
  c.addKey('张三', [
    FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
    FuzzyKey.kind('zs', FuzzyKeyKind.initials),
  ]);
  final py = c.fuzzy('zhangsan');
  if (py.isEmpty || py.first.matchedKind != FuzzyKeyKind.pinyin) {
    throw 'addKey pinyin key did not match';
  }

  // Mutation (rebuild path): removeWhere drops items + returns the count.
  final before = c.length;
  final removed = c.removeWhere((s) => s == 'README.md');
  if (removed != 1) throw 'removeWhere should return 1, got $removed';
  if (c.length != before - 1) throw 'removeWhere did not drop one item';
  if (c.fuzzy('README').isNotEmpty) throw 'removed item should not match';
  if (c.fuzzy('src').isEmpty) throw 'survivor should still match after rebuild';

  // single-best view: corpus.one.<mode> returns the top hit (or null), running
  // the same native scan as the list method with limit 1.
  final best = c.one.fuzzy('src');
  if (best == null || best.raw.isEmpty) {
    throw 'one.fuzzy should find the best hit';
  }
  if (c.one.exact('definitely-absent') != null) {
    throw 'one.exact should be null for no match';
  }

  // keyed: a List<Map> searched by a field; hit.raw is the whole map.
  final maps = FuzzyCorpus.byKey([
    {'name': 'Alice', 'id': 1},
    {'name': 'Bob', 'id': 2},
  ], 'name', libraryPath: libPath);
  final ml = maps.prefix('Al');
  if (ml.isEmpty || ml.first.raw['id'] != 1) throw 'keyed map search failed';
  final mo = maps.one.prefix('Al');
  if (mo == null || mo.raw['id'] != 1) throw 'keyed one.prefix failed';
  maps.dispose();

  // buildAsync: populate on a background isolate, search, then disposeAndWait.
  final big = await FuzzyCorpus.buildAsync(
    List.generate(3000, (i) => 'item_$i'),
    stringOf: (s) => s,
    libraryPath: libPath,
  );
  if (big.length != 3000) throw 'buildAsync length ${big.length} != 3000';
  if ((await big.fuzzyAsync('item_42', limit: 1)).isEmpty) {
    throw 'buildAsync search failed';
  }
  await big.disposeAndWait();

  // FuzzyCrash API: install never throws; lastReport on a fresh path is null.
  FuzzyCrash.install();
  if (FuzzyCrash.lastReport(breadcrumbPath: 'no_such_crash.log') != null) {
    throw 'lastReport should be null when no breadcrumb exists';
  }

  c.clear();
  if (c.length != 0) throw 'clear failed';
  c.dispose();
  print('ffi smoke OK: ${fuzzy.length} fuzzy / ${cjk.length} cjk / '
      '${async.length} async hits');
}
