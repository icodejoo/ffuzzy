// 验证资源释放语义：FuzzyCorpus / FuzzyStringMatcher 的 dispose 正确、幂等，
// 且大量创建+释放不残留（不抛错、可正常回收）。
//
// 运行：flutter test test/dispose_test.dart
// （需先 (cd rust && cargo build --release) 生成宿主 dll）

import 'package:flutter_test/flutter_test.dart';

import 'package:ffuzzy/ffuzzy.dart';
import 'package:ffuzzy/src/rust/api/fuzzy.dart' show FuzzyCorpus;

void main() {
  setUpAll(() async => await ffuzzy.ensureInitialized());

  group('FuzzyCorpus（底层 opaque）释放', () {
    test('dispose 后 isDisposed 为真，再使用抛异常', () {
      final corpus = FuzzyCorpus(items: const ['service42', 'widget7']);
      expect(corpus.isDisposed, isFalse);
      expect(corpus.len(), 2);

      corpus.dispose();
      expect(corpus.isDisposed, isTrue);

      // dispose 后再调用应抛异常（不会静默读已释放内存）。
      expect(() => corpus.len(), throwsA(anything));
    });
  });

  group('FuzzyStringMatcher（高层封装）释放', () {
    test('缓存模式：dispose 幂等，dispose 后调用抛 StateError', () {
      final m = FuzzyStringMatcher(const ['service42', 'widget7', 'controller9'])
        ..buildIndices();
      expect(m.isDisposed, isFalse);
      expect(m.match('srvc'), isNotEmpty);

      m.dispose();
      expect(m.isDisposed, isTrue);
      m.dispose(); // 幂等：再次调用安全。
      expect(m.isDisposed, isTrue);

      expect(() => m.match('srvc'), throwsStateError);
    });

    test('非缓存模式：无 Rust 侧语料，dispose 仅置位', () {
      final m = FuzzyStringMatcher(const ['abc', 'abd'], indexed: false);
      expect(m.match('ab'), isNotEmpty);
      m.dispose();
      expect(m.isDisposed, isTrue);
      expect(() => m.match('ab'), throwsStateError);
    });
  });

  test('压力：大量创建并释放缓存语料，不抛错、不残留', () {
    // 若 dispose 未真正释放 Arc / 句柄耗尽，循环会出错或显著变慢。
    const rounds = 300;
    const items = 5000;
    final data = List<String>.generate(items, (i) => 'item_$i');
    for (var r = 0; r < rounds; r++) {
      final m = FuzzyStringMatcher(data)..buildIndices();
      final hits = m.match('item', limit: 10);
      expect(hits.length, 10);
      m.dispose();
      expect(m.isDisposed, isTrue);
    }
  });

  group('dispose 与在飞异步搜索的交互', () {
    test('dispose 时有在飞搜索：推迟释放、拒绝新搜索、在飞结果被丢弃', () async {
      final data = List<String>.generate(20000, (i) => 'service_$i');
      final m = FuzzyStringMatcher(data)..buildIndices();

      // 发起但不等待，使其处于「在飞」。
      final pending = m.matchAsync('service', limit: 10);

      // 此刻 dispose：立即置位、拒绝新任务，推迟释放语料。
      m.dispose();
      expect(m.isDisposed, isTrue);
      expect(() => m.match('service'), throwsStateError);
      expect(() => m.matchAsync('service'), throwsStateError);

      // 在飞任务不再抛错,但结果被丢弃(终止旧任务语义)。
      final hits = await pending;
      expect(hits, isEmpty);
    });

    test('disposeAndWait 等到在飞任务排空后才完成', () async {
      final data = List<String>.generate(20000, (i) => 'widget_$i');
      final m = FuzzyStringMatcher(data)..buildIndices();

      final pending = m.matchAsync('widget', limit: 5);
      var drained = false;
      // ignore: unawaited_futures
      pending.then((_) => drained = true);

      await m.disposeAndWait();
      // disposeAndWait 完成时，在飞任务必已结束。
      expect(drained, isTrue);
      expect(m.isDisposed, isTrue);
    });
  });

  test('异步在两种缓存模式下都可用', () async {
    final cached = FuzzyStringMatcher(const ['service42', 'widget7'])
      ..buildIndices();
    final uncached = FuzzyStringMatcher(
      const ['service42', 'widget7'],
      indexed: false,
    );
    expect(await cached.matchAsync('srvc'), isNotEmpty);
    expect(await uncached.matchAsync('srvc'), isNotEmpty);
    cached.dispose();
    uncached.dispose();
  });
}
