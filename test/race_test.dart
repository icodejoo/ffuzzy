// P0/P1 回归:异步搜索与 dispose/refresh 的竞态(BUG-A/B)、limit 校验、copyWith。
//
// 运行：flutter test test/race_test.dart

import 'package:flutter_test/flutter_test.dart';

import 'package:ffuzzy/ffuzzy.dart';

void main() {
  setUpAll(() async => await ffuzzy.ensureInitialized());

  group('竞态修复(快照映射)', () {
    test('FuzzyMatcher: 在飞 matchAsync 期间 dispose,不崩、结果被丢弃', () async {
      final data = List<int>.generate(20000, (i) => i);
      final m = FuzzyMatcher<int>(data, (i) => 'item_$i')..buildIndices();

      final pending = m.matchAsync('item', limit: 10); // 在飞
      m.dispose(); // 边搜边销毁(旧路径会 Null check 崩)

      final hits = await pending; // 不应抛异常
      expect(hits, isEmpty); // 版本变化 → 丢弃
      expect(m.isDisposed, isTrue);
    });

    test('FuzzyMatcher: 在飞 matchAsync 期间 refresh 成更短源,不崩、不越界,结果丢弃', () async {
      final big = List<int>.generate(20000, (i) => i);
      final m = FuzzyMatcher<int>(big, (i) => 'item_$i')..buildIndices();

      final pending = m.matchAsync('item', limit: 50); // 在飞
      m.refresh(List<int>.generate(3, (i) => 1000 + i)); // 换成 3 条(旧路径可能 RangeError)

      final hits = await pending; // 不应抛 RangeError;版本变化 → 丢弃
      expect(hits, isEmpty);

      // refresh 后新查询作用于新数据。
      final after = m.match('1000');
      expect(after, isNotEmpty);
      m.dispose();
    });

    test('FuzzyStringMatcher: 在飞 matchAsync 期间 dispose,结果被丢弃', () async {
      final data = List<String>.generate(20000, (i) => 'service_$i');
      final m = FuzzyStringMatcher(data)..buildIndices();
      final pending = m.matchAsync('service', limit: 10);
      m.dispose();
      final hits = await pending;
      expect(hits, isEmpty); // 终止旧任务语义
    });
  });

  group('健壮性 / 易用性', () {
    test('limit 负数抛 ArgumentError', () {
      final m = FuzzyMatcher<int>(const [1, 2, 3], (i) => '$i')..buildIndices();
      expect(() => m.match('1', limit: -1), throwsArgumentError);
      m.dispose();
    });

    test('FuzzyConfig.copyWith', () {
      const base = kDefaultFuzzyConfig;
      final c = base.copyWith(ignoreCase: false);
      expect(c.ignoreCase, isFalse);
      expect(c.normalize, base.normalize);
      expect(c.preferPrefix, base.preferPrefix);
    });

    test('isInitialized 反映初始化已完成', () {
      expect(ffuzzy.isInitialized, isTrue);
    });
  });
}
