// Real-device (Flutter Windows) comparison: the C engine (clang/, via the
// `ffz` Dart FFI package) vs the Rust engine (ffuzzy / nucleo, via
// flutter_rust_bridge), over the same dataset + queries. Reports correctness
// (identical match sets), throughput, and resident-corpus memory (RSS deltas).
//
// IMPORTANT: run in profile (both engines release-optimized):
//   cd example && flutter drive \
//     --driver=test_driver/integration_test.dart \
//     --target=integration_test/c_vs_rust_test.dart -d windows --profile
import 'dart:io';

import 'package:ffuzzy/ffuzzy.dart'; // C engine (FuzzyCorpus, FuzzyMode) — now the published package
import 'package:ffuzzy_rust_bench/ffuzzy.dart' as rust; // deprecated Rust/nucleo engine, kept for comparison
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

// ── deterministic dataset (same shape as perf/gen_data.py) ───────────────────
const _words = [
  'src','lib','core','util','config','model','view','controller','service',
  'widget','render','parser','lexer','token','stream','buffer','cache','index',
  'query','filter','matcher','engine','module','plugin','handler','adapter',
  'factory','builder','context','session','request','response','client','server',
];
const _ext = ['rs','dart','c','h','py','js','ts','go'];

List<String> genItems(int n) {
  var s = 0x12345678;
  int rnd(int m) {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    return s % m;
  }
  return List<String>.generate(n, (_) {
    final a = _words[rnd(_words.length)], b = _words[rnd(_words.length)];
    final c = _words[rnd(_words.length)], num = rnd(1000), e = _ext[rnd(_ext.length)];
    switch (rnd(4)) {
      case 0: return '$a/${b}_$c/$c$num.$e';
      case 1: return '${a}_${b}_$c$num';
      case 2: return '$a/$b/$c.$e';
      default: return 'get${_cap(b)}${_cap(c)}$num';
    }
  });
}

String _cap(String w) => w[0].toUpperCase() + w.substring(1);
double _mb(int b) => b / (1024 * 1024);

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async => await rust.ffuzzy.ensureInitialized());

  test('C (ffz) vs Rust (nucleo): correctness + speed + memory', () {
    const n = 100000;
    const queries = ['src', 'mod', 'cfg', 'usrsvc', 'rndr'];
    final items = genItems(n);

    final base = ProcessInfo.currentRss;

    // C resident corpus via the bundled plugin native library (no path).
    final c = FuzzyCorpus();
    c.addAll(items);
    final afterC = ProcessInfo.currentRss;
    final memC = afterC - base;

    // Rust resident corpus (ffuzzy / nucleo). Kept alive alongside C so the RSS
    // deltas are additive (no freed-page reuse between the two measurements).
    final r = rust.FuzzyStringMatcher(items)..buildIndices();
    final afterR = ProcessInfo.currentRss;
    final memR = afterR - afterC;

    // correctness: identical match SETS (rank/order ignored) for each query.
    for (final q in queries) {
      final cSet = c.filter(q, mode: FuzzyMode.fuzzy, highlight: false)
          .map((h) => h.index).toSet();
      final rSet = r.match(q).map((h) => h.index).toSet();
      expect(cSet, rSet, reason: 'identical matched items for "$q"');
    }

    // speed: both materialize top-50 (index+score+highlight) into Dart objects.
    var sink = 0;
    const reps = 20;
    final swC = Stopwatch()..start();
    for (var i = 0; i < reps; i++) {
      for (final q in queries) {
        sink += c.filter(q, mode: FuzzyMode.fuzzy, parallel: true, limit: 50).length;
      }
    }
    swC.stop();
    final swR = Stopwatch()..start();
    for (var i = 0; i < reps; i++) {
      for (final q in queries) {
        sink += r.match(q, limit: 50).length;
      }
    }
    swR.stop();

    final filters = reps * queries.length;
    final cMs = swC.elapsedMicroseconds / 1000 / filters;
    final rMs = swR.elapsedMicroseconds / 1000 / filters;

    // ignore: avoid_print
    print('''

══════════ C (ffz) vs Rust (nucleo) — Flutter Windows, N=$n ══════════
 memory (resident corpus):   C = ${_mb(memC).toStringAsFixed(2)} MB    Rust = ${_mb(memR).toStringAsFixed(2)} MB    (C/Rust = ${(memC / memR).toStringAsFixed(2)}x)
 filter (fuzzy, top-50):      C = ${cMs.toStringAsFixed(3)} ms     Rust = ${rMs.toStringAsFixed(3)} ms     (Rust/C = ${(rMs / cMs).toStringAsFixed(2)}x)
 process peak RSS:            ${_mb(ProcessInfo.maxRss).toStringAsFixed(1)} MB     (sink=$sink)
══════════════════════════════════════════════════════════════════════
''');

    c.dispose();
    r.dispose();
    expect(sink, greaterThan(0));
  }, timeout: const Timeout(Duration(minutes: 3)));
}
