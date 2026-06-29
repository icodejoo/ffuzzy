// Native Dart FFI benchmark: corpus vs Dart .where() for prefix/exact/fuzzy
// Usage: dart run scripts/bench_native.dart [path/to/libffz.so]
// ignore_for_file: avoid_print
import 'dart:convert';
import 'dart:io';
import 'package:ffuzzy/ffuzzy.dart';

// ── benchmark helpers ─────────────────────────────────────────────────────────
double benchUs(void Function() fn, {int warmup = 200, int reps = 2000}) {
  for (var i = 0; i < warmup; i++) fn();
  final t = DateTime.now().microsecondsSinceEpoch;
  for (var i = 0; i < reps; i++) fn();
  return (DateTime.now().microsecondsSinceEpoch - t) / reps;
}

String padL(Object v, int w) => v.toString().padLeft(w);
String padR(Object v, int w) => v.toString().padRight(w);

// ── table ─────────────────────────────────────────────────────────────────────
void printRow(String label, double filterUs, double corpusUs,
    int fHits, int cHits) {
  final ratio = (filterUs / corpusUs).toStringAsFixed(2);
  final same = fHits == cHits ? '✓' : '✗(f$fHits/c$cHits)';
  print('  ${padR(label, 28)} '
      '${padL(filterUs.toStringAsFixed(1), 9)} µs  '
      '${padL(corpusUs.toStringAsFixed(1), 9)} µs  '
      '${padL(ratio + 'x', 7)}  $same');
}

Future<void> main(List<String> args) async {
  final libPath = args.isNotEmpty ? args[0] : null;

  // ── load data ──────────────────────────────────────────────────────────────
  final jsonFile = File('mock.json');
  if (!jsonFile.existsSync()) {
    print('ERROR: mock.json not found. Run from the repo root.');
    exit(1);
  }
  final List<dynamic> raw =
      jsonDecode(await jsonFile.readAsString()) as List<dynamic>;
  final mock =
      raw.map((e) => Map<String, dynamic>.from(e as Map)).toList();
  print('Dataset: ${mock.length} items\n');

  // ── build corpus ───────────────────────────────────────────────────────────
  final t0 = DateTime.now().microsecondsSinceEpoch;
  final corpus = FuzzyCorpus.byKey(mock, 'gameName',
      libraryPath: libPath);
  final buildMs =
      (DateTime.now().microsecondsSinceEpoch - t0) / 1000;
  print('Corpus build: ${buildMs.toStringAsFixed(2)} ms\n');

  // ── header ─────────────────────────────────────────────────────────────────
  const hdr = '  Query                          filter µs    '
      'corpus µs    ratio    hits';
  final sep = '  ' + '-' * (hdr.length - 2);

  // ── prefix ────────────────────────────────────────────────────────────────
  print('── PREFIX: corpus.prefix vs list.where(startsWith) ─────────────────');
  print(hdr); print(sep);

  for (final entry in [
    ('1 char  "S"', 'S'),
    ('2 chars "Su"', 'Su'),
    ('4 chars "Supe"', 'Supe'),
    ('7 chars "Super G"', 'Super G'),
    ('full    "Super Gems 1000"', 'Super Gems 1000'),
    ('miss    "ZZNOTFOUND"', 'ZZNOTFOUND'),
  ]) {
    final q = entry.$2;
    final fUs = benchUs(
        () => mock.where((g) => (g['gameName'] as String? ?? '').startsWith(q)).toList());
    final cUs = benchUs(() => corpus.prefix(q));
    printRow(entry.$1, fUs, cUs,
        mock.where((g) => (g['gameName'] as String? ?? '').startsWith(q)).length,
        corpus.prefix(q).length);
  }

  // ── exact ─────────────────────────────────────────────────────────────────
  print('\n── EXACT: corpus.exact vs list.where(==) ────────────────────────────');
  print(hdr); print(sep);

  for (final entry in [
    ('gameId  "101024"', 'gameId', '101024'),
    ('gameName "Super Gems 1000"', 'gameName', 'Super Gems 1000'),
    ('miss    "ZZNOTFOUND"', 'gameName', 'ZZNOTFOUND'),
  ]) {
    final field = entry.$2;
    final q = entry.$3;
    final fUs = benchUs(
        () => mock.where((g) => (g[field] as String? ?? '') == q).toList());
    final cUs = benchUs(() => corpus.exact(q));
    printRow(entry.$1, fUs, cUs,
        mock.where((g) => (g[field] as String? ?? '') == q).length,
        corpus.exact(q).length);
  }

  // ── fuzzy ─────────────────────────────────────────────────────────────────
  print('\n── FUZZY: corpus.fuzzy (no Dart equivalent) ─────────────────────────');
  print('  Query                          corpus µs    hits');
  print('  ' + '-' * 45);
  for (final q in ['gems', 'plinko', 'super', 'sp', 'NOTFOUND']) {
    final cUs = benchUs(() => corpus.fuzzy(q, limit: 50));
    final hits = corpus.fuzzy(q, limit: 50).length;
    print('  ${padR('"$q"', 30)} ${padL(cUs.toStringAsFixed(1), 8)} µs    $hits');
  }

  // ── sustained N queries ───────────────────────────────────────────────────
  print('\n── SUSTAINED prefix "Su" — N queries ───────────────────────────────');
  print('  N        filter total    corpus total    speedup');
  print('  ' + '-' * 50);
  for (final N in [10, 100, 500, 1000, 5000]) {
    var ft = 0.0, ct = 0.0;
    // warmup
    for (var i = 0; i < 50; i++) {
      mock.where((g) => (g['gameName'] as String? ?? '').startsWith('Su')).toList();
      corpus.prefix('Su');
    }
    var t = DateTime.now().microsecondsSinceEpoch;
    for (var i = 0; i < N; i++) {
      mock.where((g) => (g['gameName'] as String? ?? '').startsWith('Su')).toList();
    }
    ft = (DateTime.now().microsecondsSinceEpoch - t) / 1000;
    t = DateTime.now().microsecondsSinceEpoch;
    for (var i = 0; i < N; i++) corpus.prefix('Su');
    ct = (DateTime.now().microsecondsSinceEpoch - t) / 1000;
    final speedup = (ft / ct).toStringAsFixed(2);
    print('  ${padL(N, 6)}   ${padL(ft.toStringAsFixed(2) + ' ms', 14)}  '
        '${padL(ct.toStringAsFixed(2) + ' ms', 14)}  ${speedup}x');
  }

  corpus.dispose();
  print('\nDone.');
}
