// End-to-end smoke test of the Dart FFI binding against a real built library.
// Usage: dart run tool/ffi_smoke.dart [path/to/libffz.so]
// Exits non-zero on any failure (used by CI).
// ignore_for_file: avoid_print
import 'package:ffuzzy/ffuzzy.dart';

Future<void> main(List<String> args) async {
  final libPath = args.isNotEmpty ? args[0] : null;
  final c = FfzCorpus(libraryPath: libPath);

  c.addAll(['src/main.rs', 'lib/ffz.dart', '中文搜索引擎', 'README.md', 'café']);
  if (c.length != 5) throw 'length ${c.length} != 5';

  final fuzzy = c.filter('src', limit: 10);
  if (fuzzy.isEmpty) throw 'expected fuzzy hits for "src"';

  final cjk = c.filter('中文');
  if (cjk.isEmpty) throw 'expected a CJK hit for "中文"';

  final fold = c.filter('cafe'); // diacritic fold café≈cafe
  if (fold.isEmpty) throw 'expected diacritic-folded hit for "cafe"';

  final pref =
      c.filter('READ', mode: FfzMode.prefix, caseMatching: FfzCase.ignore);
  if (pref.isEmpty) throw 'expected prefix hit for "READ"';

  // Highlight conversion must yield in-range UTF-16 offsets.
  final h = fuzzy.first;
  ffzCodepointToUtf16('src/main.rs', h.indices);

  // filterAsync must agree with the synchronous filter, element-by-element.
  final async = await c.filterAsync('src', limit: 10);
  if (async.length != fuzzy.length) {
    throw 'filterAsync len ${async.length} != ${fuzzy.length}';
  }
  for (var i = 0; i < async.length; i++) {
    if (async[i].index != fuzzy[i].index || async[i].score != fuzzy[i].score) {
      throw 'filterAsync mismatch at $i';
    }
  }

  // addKeyed: a CJK item findable by host-computed pinyin/initials.
  c.addKeyed('张三', [
    FfzKey.kind('zhangsan', FfzKeyKind.pinyin),
    FfzKey.kind('zs', FfzKeyKind.initials),
  ]);
  final py = c.filter('zhangsan');
  if (py.isEmpty || py.first.matchedKind != FfzKeyKind.pinyin) {
    throw 'addKeyed pinyin key did not match';
  }

  // FfzCrash API: install never throws; lastReport on a fresh path is null.
  FfzCrash.install();
  if (FfzCrash.lastReport(breadcrumbPath: 'no_such_crash.log') != null) {
    throw 'lastReport should be null when no breadcrumb exists';
  }

  c.clear();
  if (c.length != 0) throw 'clear failed';
  c.dispose();
  print('ffi smoke OK: ${fuzzy.length} fuzzy / ${cjk.length} cjk / '
      '${async.length} async hits');
}
