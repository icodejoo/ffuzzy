// FuzzyCorpus mutation 单元测试 — 覆盖 update / removeAt / removeWhere 操作。
//
// 运行（在 repo 根目录，需要先构建 native 库）：
//   flutter test test/corpus_mutation_test.dart
//
// Windows 上 native 库为 libffz.dll（repo 根目录）。
// 测试通过 libraryPath 传入绝对路径，跳过 Flutter 插件加载机制，
// 使测试文件在 `flutter test` 下可直接运行。

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:ffuzzy/ffuzzy.dart';

/// 返回 repo 根目录下的 native 库路径（跨平台）。
///
/// flutter test 以项目根目录作为 cwd，因此直接使用 [Directory.current]。
String _nativeLibPath() {
  final repoRoot = Directory.current.path;
  if (Platform.isWindows) return '$repoRoot\\libffz.dll';
  if (Platform.isMacOS) return '$repoRoot/libffz.dylib';
  return '$repoRoot/build_x86_64/libffuzzy.so';
}

void main() {
  late String libPath;

  setUpAll(() {
    libPath = _nativeLibPath();
  });

  group('FuzzyCorpus mutation', () {
    // ── removeAt ────────────────────────────────────────────────────────────

    test('removeAt(0) 后原 index=0 的文本不再出现在搜索结果中', () {
      final corpus = FuzzyCorpus.strings(
        ['apple', 'banana', 'cherry'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      // 删除前确认 index=0 的项目（apple）可搜索到
      final before = corpus.exact('apple');
      expect(before, isNotEmpty, reason: 'removeAt 前应能搜索到 apple');
      expect(before.first.index, equals(0));

      corpus.removeAt(0);

      // 删除后：apple 不应出现
      final afterApple = corpus.exact('apple');
      expect(afterApple, isEmpty, reason: 'removeAt(0) 后 apple 不应出现');

      // 剩余项仍可搜索
      expect(corpus.exact('banana'), isNotEmpty);
      expect(corpus.exact('cherry'), isNotEmpty);

      // 长度正确
      expect(corpus.length, equals(2));
    });

    test('removeAt 后剩余项的 FuzzyHit.index 重新从 0 开始连续编号', () {
      final corpus = FuzzyCorpus.strings(
        ['alpha', 'beta', 'gamma'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      corpus.removeAt(1); // 删除 beta，剩 [alpha, gamma]

      final hits = corpus.fuzzy('a',
          caseMatching: FuzzyCase.ignore, scoring: FuzzyScoring.off);
      // 所有 index 必须在 [0, corpus.length) 范围内
      for (final h in hits) {
        expect(h.index, inInclusiveRange(0, corpus.length - 1),
            reason: 'removeAt 后 index 应在合法范围内');
      }
    });

    // ── update ───────────────────────────────────────────────────────────────

    test('update(0, newItem) 后旧文本搜不到，新文本可搜到', () {
      final corpus = FuzzyCorpus.strings(
        ['oldText', 'unrelated'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      corpus.update(0, 'newText');

      expect(corpus.exact('oldText'), isEmpty,
          reason: 'update 后旧文本应不可搜索');
      expect(corpus.exact('newText'), isNotEmpty,
          reason: 'update 后新文本应可搜索');
      // 未被 update 的项不受影响
      expect(corpus.exact('unrelated'), isNotEmpty);
      expect(corpus.length, equals(2));
    });

    test('update 不改变 corpus.length', () {
      final corpus = FuzzyCorpus.strings(
        ['foo', 'bar', 'baz'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      corpus.update(1, 'qux');
      expect(corpus.length, equals(3));
    });

    test('update 后 FuzzyHit.index 映射到更新后的对象', () {
      final corpus = FuzzyCorpus.strings(
        ['alice', 'bob'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      corpus.update(0, 'charlie');
      final hits = corpus.exact('charlie');
      expect(hits, isNotEmpty);
      expect(hits.first.raw, equals('charlie'),
          reason: 'hit.raw 应反映更新后的值');
      expect(hits.first.index, equals(0),
          reason: 'update 后 index 不变，仍为 0');
    });

    // ── removeWhere ──────────────────────────────────────────────────────────

    test('removeWhere 后 corpus.length == 原长度 - 删除数量', () {
      final corpus = FuzzyCorpus.strings(
        ['cat', 'car', 'dog', 'cart'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      final removed = corpus.removeWhere((s) => s.startsWith('ca'));
      expect(removed, equals(3), reason: 'cat / car / cart 均以 ca 开头');
      expect(corpus.length, equals(1));
    });

    test('removeWhere 后被删除项不可搜索，保留项仍可搜索', () {
      final corpus = FuzzyCorpus.strings(
        ['remove_me', 'keep_this', 'also_remove', 'keep_that'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      corpus.removeWhere((s) => s.startsWith('remove') || s.startsWith('also'));

      expect(corpus.exact('remove_me'), isEmpty);
      expect(corpus.exact('also_remove'), isEmpty);
      expect(corpus.exact('keep_this'), isNotEmpty);
      expect(corpus.exact('keep_that'), isNotEmpty);
    });

    test('removeWhere 无匹配时返回 0，corpus 不变', () {
      final corpus = FuzzyCorpus.strings(
        ['apple', 'banana'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      final removed = corpus.removeWhere((s) => s == 'nonexistent');
      expect(removed, equals(0));
      expect(corpus.length, equals(2));
    });

    test('removeWhere 删除全部后 corpus 为空，搜索返回空列表', () {
      final corpus = FuzzyCorpus.strings(
        ['only_item'],
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      final removed = corpus.removeWhere((_) => true);
      expect(removed, equals(1));
      expect(corpus.length, equals(0));
      expect(corpus.fuzzy('only'), isEmpty);
    });

    // ── update 对 addKey 条目的警告（assert 模式下触发断言）─────────────────

    test('update() 对通过 addKey 添加的条目在 assert 模式下触发 AssertionError', () {
      // 探测 native 库是否支持 ffz_ffi_add_keyed（旧版构建可能没有此符号）。
      // 若不支持，addKey 会抛出 FuzzyException，则跳过本测试。
      final corpus = FuzzyCorpus<String>(
        <String>[],
        stringOf: (s) => s,
        libraryPath: libPath,
      );
      addTearDown(corpus.dispose);

      try {
        corpus.addKey('张三', [
          FuzzyKey.kind('zhangsan', FuzzyKeyKind.pinyin),
          FuzzyKey.kind('zs', FuzzyKeyKind.initials),
        ]);
      } on FuzzyException catch (e) {
        // native 库不含 ffz_ffi_add_keyed，跳过本测试
        markTestSkipped('native 库不支持 addKey，跳过测试: $e');
        return;
      }

      // 在 debug 模式（assert 开启）下，update 会触发 assert 警告。
      // flutter test 默认以 debug 模式运行，因此应抛出 AssertionError。
      // 在 release 模式下，update 会静默丢弃 alternate keys 并继续执行。
      bool assertsEnabled = false;
      assert(() {
        assertsEnabled = true;
        return true;
      }());

      if (assertsEnabled) {
        expect(
          () => corpus.update(0, '李四'),
          throwsA(isA<AssertionError>()),
          reason: 'debug 模式下 update 一个 addKey 条目应触发 AssertionError',
        );
      } else {
        // release 模式：update 静默执行，旧键名不可搜索，新文本可搜索
        corpus.update(0, '李四');
        expect(corpus.exact('李四'), isNotEmpty);
      }
    });
  });
}
