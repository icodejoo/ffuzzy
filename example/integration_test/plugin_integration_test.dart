// 真机集成测试:验证 Dart → flutter_rust_bridge → nucleo 整条链路在设备上可用。
//
// 运行(需连接设备):
//   cd example && flutter test integration_test/plugin_integration_test.dart -d <device-id>

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:ffuzzy/ffuzzy.dart';
import 'package:ffuzzy_example/main.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async => await ffuzzy.ensureInitialized());

  group('ffuzzy 真机 API', () {
    test('独立函数 fuzzyMatch / fuzzyFilter', () {
      expect(
        fuzzyMatch(
          query: 'dt',
          haystack: 'Dragon Treasure',
          config: kDefaultFuzzyConfig,
        ),
        isNotNull,
      );
      final hits = fuzzyFilter(
        query: 'drg',
        items: const ['Dragon', 'Golden', 'Lucky Dragon'],
        config: kDefaultFuzzyConfig,
      );
      expect(hits, isNotEmpty);
    });

    test('FuzzyMatcher 对象搜索:返回对象、分数、高亮', () {
      final m = FuzzyMatcher<String>(
        const ['Dragon Treasure', 'Golden Fortune', 'Lucky Dragon'],
        (s) => s,
      )..buildIndices();
      final hits = m.match('dragon', limit: 10);
      expect(hits, isNotEmpty);
      expect(hits.first.score, greaterThan(0));
      expect(hits.first.indices, isNotEmpty);
      expect(m.single('fortune')?.obj, 'Golden Fortune');
      m.dispose();
    });

    test('matchAsync 异步搜索', () async {
      final m = FuzzyStringMatcher(const ['alpha', 'beta', 'alphabet'])
        ..buildIndices();
      final hits = await m.matchAsync('alph', limit: 10);
      expect(hits, isNotEmpty);
      m.dispose();
    });
  });

  group('演示 App 端到端', () {
    testWidgets('输入查询实时筛选', (tester) async {
      await tester.pumpWidget(const FfuzzyExampleApp());
      await tester.pumpAndSettle();

      expect(find.text('Dragon Treasure'), findsOneWidget);

      await tester.enterText(find.byKey(const Key('search-field')), 'bnz');
      await tester.pumpAndSettle();

      // 'Sweet Bonanza' 命中(高亮后文本被拆成多个 span,用 RichText 查找)。
      expect(
        find.byWidgetPredicate(
          (w) => w is RichText && w.text.toPlainText().contains('Bonanza'),
        ),
        findsWidgets,
      );
    });
  });
}
