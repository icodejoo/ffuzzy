// Benchmark: corpus vs Dart .where() for prefix / exact / fuzzy.
// Run with: flutter test test/bench_native_test.dart --reporter=expanded
// ignore_for_file: avoid_print
import 'dart:convert';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:ffuzzy/ffuzzy.dart';

double benchUs(void Function() fn, {int warmup = 200, int reps = 2000}) {
  for (var i = 0; i < warmup; i++) fn();
  final t = DateTime.now().microsecondsSinceEpoch;
  for (var i = 0; i < reps; i++) fn();
  return (DateTime.now().microsecondsSinceEpoch - t) / reps;
}

String _row(String lbl, double fUs, double cUs, int fH, int cH) {
  final ratio = (fUs / cUs).toStringAsFixed(2);
  final same  = fH == cH ? '✓' : '✗(f$fH/c$cH)';
  return '  ${lbl.padRight(28)} ${fUs.toStringAsFixed(1).padLeft(9)} µs  '
      '${cUs.toStringAsFixed(1).padLeft(9)} µs  ${ratio.padLeft(6)}x  $same';
}

void main() {
  test('corpus vs Dart filter benchmark', () async {
    // load mock data
    final jsonFile = File('mock.json');
    expect(jsonFile.existsSync(), true,
        reason: 'Run from repo root (where mock.json lives)');
    final List<dynamic> raw =
        jsonDecode(await jsonFile.readAsString()) as List<dynamic>;
    final mock =
        raw.map((e) => Map<String, dynamic>.from(e as Map)).toList();
    print('\nDataset: ${mock.length} items');

    // build corpus
    final t0 = DateTime.now().microsecondsSinceEpoch;
    final corpus = FuzzyCorpus.byKey(mock, 'gameName');
    final buildMs =
        (DateTime.now().microsecondsSinceEpoch - t0) / 1000;
    print('Corpus build: ${buildMs.toStringAsFixed(2)} ms\n');

    const hdr = '  Query                          filter µs    corpus µs    ratio    hits';
    final sep = '  ${'─' * (hdr.length - 2)}';

    // ── prefix ──────────────────────────────────────────────────────────────
    print('── PREFIX: corpus.prefix vs list.where(startsWith) ─────────────────');
    print(hdr); print(sep);
    for (final (lbl, q) in [
      ('1 char  "S"',             'S'),
      ('2 chars "Su"',            'Su'),
      ('4 chars "Supe"',          'Supe'),
      ('7 chars "Super G"',       'Super G'),
      ('full    "Super Gems 1000"', 'Super Gems 1000'),
      ('miss    "ZZNOTFOUND"',    'ZZNOTFOUND'),
    ]) {
      final fUs = benchUs(() => mock
          .where((g) => ((g['gameName'] as String?) ?? '').startsWith(q))
          .toList());
      final cUs = benchUs(() => corpus.prefix(q));
      print(_row(lbl, fUs, cUs,
          mock.where((g) => ((g['gameName'] as String?) ?? '').startsWith(q)).length,
          corpus.prefix(q).length));
    }

    // ── exact ────────────────────────────────────────────────────────────────
    print('\n── EXACT: corpus.exact vs list.where(==) ────────────────────────────');
    print(hdr); print(sep);
    for (final (lbl, field, q) in [
      ('gameId  "101024"',            'gameId',   '101024'),
      ('gameName "Super Gems 1000"',  'gameName', 'Super Gems 1000'),
      ('miss    "ZZNOTFOUND"',        'gameName', 'ZZNOTFOUND'),
    ]) {
      final fUs = benchUs(() => mock
          .where((g) => ((g[field] as String?) ?? '') == q)
          .toList());
      final cUs = benchUs(() => corpus.exact(q));
      print(_row(lbl, fUs, cUs,
          mock.where((g) => ((g[field] as String?) ?? '') == q).length,
          corpus.exact(q).length));
    }

    // ── fuzzy ─────────────────────────────────────────────────────────────────
    print('\n── FUZZY: corpus.fuzzy (no Dart equivalent) ─────────────────────────');
    print('  Query                          corpus µs    hits');
    print('  ${'─' * 45}');
    for (final q in ['gems', 'plinko', 'super', 'sp', 'NOTFOUND']) {
      final cUs = benchUs(() => corpus.fuzzy(q, limit: 50));
      final hits = corpus.fuzzy(q, limit: 50).length;
      print('  ${'"$q"'.padRight(30)} ${cUs.toStringAsFixed(1).padLeft(8)} µs    $hits');
    }

    // ── sustained ─────────────────────────────────────────────────────────────
    print('\n── SUSTAINED prefix "Su" — N queries ───────────────────────────────');
    print('  N        filter total    corpus total    speedup');
    print('  ${'─' * 50}');
    for (final N in [10, 100, 500, 1000, 5000]) {
      for (var i = 0; i < 50; i++) {
        mock.where((g) => ((g['gameName'] as String?) ?? '').startsWith('Su')).toList();
        corpus.prefix('Su');
      }
      var t = DateTime.now().microsecondsSinceEpoch;
      for (var i = 0; i < N; i++) {
        mock.where((g) => ((g['gameName'] as String?) ?? '').startsWith('Su')).toList();
      }
      final ftMs = (DateTime.now().microsecondsSinceEpoch - t) / 1000;
      t = DateTime.now().microsecondsSinceEpoch;
      for (var i = 0; i < N; i++) corpus.prefix('Su');
      final ctMs = (DateTime.now().microsecondsSinceEpoch - t) / 1000;
      final speedup = (ftMs / ctMs).toStringAsFixed(2);
      print('  ${N.toString().padLeft(6)}   '
          '${ftMs.toStringAsFixed(2).padLeft(8)} ms     '
          '${ctMs.toStringAsFixed(2).padLeft(8)} ms     ${speedup}x');
    }

    corpus.dispose();
    print('\nDone.');
  });
}
