// 场景：首页 -> 搜索页（构建带缓存的 fuzzy-rs 语料）-> 搜索进行中突然切回首页
//        -> 搜索页 State.dispose 释放 fuzzy-rs -> 检查内存回落 / 无泄漏。
//
// 生命周期映射（用普通 test 复现，避免 testWidgets 的 FakeAsync 卡住真实 FFI 异步）：
//   进入搜索页 / State.initState  ==  new FuzzyStringMatcher(data)   （Rust 侧建缓存语料）
//   搜索进行中                    ==  filterAsync(...) 未 await（在飞）
//   切回首页 / State.dispose      ==  matcher.dispose()        （停新 + 排空在飞后释放）
//
// 内存判定：单次 RSS 不一定回到基线（分配器常保留已释放内存不还 OS），故用
// 「多轮后 RSS 是否有界（不随轮数线性增长）」判断是否泄漏。我们的 dispose 是确定性释放，
// 不依赖 GC，因此排空在飞搜索后语料应立即归还。
//
// 运行：flutter test test/navigation_dispose_memory_test.dart

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ffuzzy/ffuzzy.dart';

void main() {
  setUpAll(() async => await ffuzzy.ensureInitialized());

  test(
    '搜索中途切回首页 -> dispose fuzzy-rs -> 多轮后内存有界（无泄漏）',
    () async {
      const corpusSize = 300000;
      final data = List<String>.generate(
        corpusSize,
        (i) => 'service_module_$i',
      );

      double rssMb() => ProcessInfo.currentRss / (1024 * 1024);

      const rounds = 10;
      const warmup = 3;
      final samples = <double>[];

      for (var round = 0; round < rounds; round++) {
        // 进入搜索页（initState）：在 Rust 侧建缓存语料。
        final matcher = FuzzyStringMatcher(data)..buildIndices();

        // 搜索进行中：发起但不 await，使其处于「在飞」。
        final pending = matcher.matchAsync('service', limit: 20);

        // 切回首页（State.dispose）：停掉新搜索；有在飞则推迟释放。
        matcher.dispose();
        expect(matcher.isDisposed, isTrue);

        // 等在飞搜索排空——其完成会触发确定性释放语料（_onIdle -> 释放）。
        await pending;

        samples.add(rssMb());
      }

      final stable = samples.sublist(warmup);
      final baseline = stable.reduce((a, b) => a < b ? a : b);
      final peak = stable.reduce((a, b) => a > b ? a : b);
      final growth = peak - baseline;

      // ignore: avoid_print
      print(
        '\n[内存] 每轮 RSS(MB): '
        '${samples.map((e) => e.toStringAsFixed(0)).join(', ')}\n'
        '[内存] 预热后 基线=${baseline.toStringAsFixed(0)}MB '
        '峰值=${peak.toStringAsFixed(0)}MB 波动=${growth.toStringAsFixed(0)}MB '
        '(单个语料约 ${(corpusSize * 116 / 1024 / 1024).toStringAsFixed(0)}MB)\n',
      );

      // 若 dispose 未真正释放，RSS 会随轮数线性增长（每轮数十 MB，10 轮数百 MB）。
      // 正常释放下波动应远小于「一个语料 × 轮数」，取 200MB 作为泄漏阈值（留足余量）。
      expect(
        growth,
        lessThan(200),
        reason: '预热后 RSS 波动过大，疑似 fuzzy-rs 语料未随 dispose 释放（泄漏）',
      );
    },
    timeout: const Timeout(Duration(minutes: 3)),
  );
}
