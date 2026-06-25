// 公开 API 单元测试(自包含):覆盖 FuzzyMatcher / FuzzyStringMatcher / 独立函数 / 配置 /
// 生命周期(显式 buildIndices、freeIndices(purge)、dispose、refresh、single)。
//
// 运行：flutter test test/api_test.dart

import 'package:flutter_test/flutter_test.dart';

import 'package:ffuzzy/ffuzzy.dart';

class _Game {
  const _Game(this.id, this.name);
  final int id;
  final String name;
}

void main() {
  setUpAll(() async => await ffuzzy.ensureInitialized());

  const games = [
    _Game(1, 'Dragon Treasure'),
    _Game(2, 'Golden Fortune'),
    _Game(3, 'Super Gems 1000'),
    _Game(4, 'Lucky Dragon'),
  ];

  group('FuzzyMatcher', () {
    test('函数 key：buildIndices 后 match 返回对象/分数/高亮', () {
      final s = FuzzyMatcher<_Game>(games, (g) => g.name)..buildIndices();
      final hits = s.match('dragon', limit: 10);
      expect(hits, isNotEmpty);
      expect(hits.every((h) => h.obj.name.toLowerCase().contains('dragon')), isTrue);
      expect(hits.first.score, greaterThan(0));
      expect(hits.first.indices, isNotEmpty);
      expect(hits.first.obj, isA<_Game>());
      s.dispose();
    });

    test('字段名 key（Map 数据）', () {
      final maps = [
        {'id': 1, 'name': 'Dragon Treasure'},
        {'id': 2, 'name': 'Golden Fortune'},
      ];
      final s = FuzzyMatcher.key(maps, 'name')..buildIndices();
      expect(s.match('fortune').single.obj['id'], 2);
      s.dispose();
    });

    test('single / singleAsync：无命中返回 null', () async {
      final s = FuzzyMatcher<_Game>(games, (g) => g.name)..buildIndices();
      expect(s.single('dragon')?.obj.name.toLowerCase(), contains('dragon'));
      expect(s.single('zzzzz'), isNull);
      expect((await s.singleAsync('gems'))?.obj.id, 3);
      s.dispose();
    });

    test('未建索引时 match 退化为扫描(不自动建索引)', () {
      final s = FuzzyMatcher<_Game>(games, (g) => g.name);
      expect(s.hasIndices, isFalse);
      expect(s.match('dragon'), isNotEmpty); // 无索引也能搜(慢速整表扫描)
      expect(s.hasIndices, isFalse); // 没有偷偷建索引
      s.buildIndices();
      expect(s.hasIndices, isTrue);
      expect(s.match('dragon'), isNotEmpty);
      s.dispose();
      expect(() => s.match('x'), throwsStateError); // dispose 后才抛
    });

    test('refresh：换源并自动重建', () {
      final s = FuzzyMatcher<_Game>(const <_Game>[], (g) => g.name); // 空占位
      expect(s.length, 0);
      s.refresh(games); // 数据回来后喂入，自动建索引
      expect(s.hasIndices, isTrue);
      expect(s.match('dragon'), isNotEmpty);
      s.dispose();
    });

    test('生命周期：buildIndices / freeIndices / dispose', () {
      final s = FuzzyMatcher<_Game>(games, (g) => g.name);
      expect(s.hasIndices, isFalse); // 未建
      s.buildIndices();
      expect(s.hasIndices, isTrue);

      s.freeIndices(); // 只释放 Rust 索引,Dart 侧对象/投影保留
      expect(s.hasIndices, isFalse);
      expect(s.match('gold'), isNotEmpty); // 退化扫描仍可搜

      s.buildIndices(); // 秒级重建(用保留的投影)
      expect(s.hasIndices, isTrue);

      s.dispose();
      expect(s.isDisposed, isTrue);
      expect(() => s.match('x'), throwsStateError);
    });
  });

  group('FuzzyStringMatcher', () {
    test('buildIndices 后 match 返回 FuzzyHit(下标/分数/高亮)', () {
      final m = FuzzyStringMatcher(const ['alpha', 'beta', 'alphabet'])..buildIndices();
      final hits = m.match('alph', limit: 10);
      expect(hits, isNotEmpty);
      expect(hits.first.index, inInclusiveRange(0, 2));
      expect(hits.first.score, greaterThan(0));
      final best = m.single('bet'); // FuzzyHit?
      expect(best, isNotNull);
      expect(m.items[best!.index], 'beta');
      m.dispose();
    });

    test('indexed=false：无需 buildIndices，每次传整表', () {
      const items = ['alpha', 'beta', 'alphabet'];
      final indexed = FuzzyStringMatcher(items)..buildIndices();
      final plain = FuzzyStringMatcher(items, indexed: false);
      expect(plain.match('alph').map((h) => h.index),
          indexed.match('alph').map((h) => h.index));
      indexed.dispose();
      plain.dispose();
    });

    test('refresh：换源重建', () {
      final m = FuzzyStringMatcher(const <String>[]); // 占位
      m.refresh(const ['gold', 'silver', 'golden']);
      expect(m.hasIndices, isTrue);
      expect(m.match('gold'), isNotEmpty);
      m.dispose();
    });
  });

  group('增量 add', () {
    test('FuzzyMatcher: 建索引后 add,能搜到(不重建)', () {
      final s = FuzzyMatcher<_Game>([games[0]], (g) => g.name)..buildIndices();
      expect(s.single('fortune'), isNull); // 还没加
      s.add(const _Game(99, 'Golden Fortune'));
      expect(s.length, 2);
      expect(s.single('fortune')?.obj.id, 99); // 新项可搜到
      s.dispose();
    });

    test('FuzzyMatcher: 未建索引时 add,buildIndices 后含新项', () {
      final s = FuzzyMatcher<_Game>([games[0]], (g) => g.name);
      s.add(const _Game(99, 'Golden Fortune'));
      s.buildIndices();
      expect(s.single('fortune')?.obj.id, 99);
      s.dispose();
    });

    test('FuzzyMatcher: freeIndices 后 add,重建后含新项', () {
      final s = FuzzyMatcher<_Game>(games, (g) => g.name)..buildIndices();
      s.freeIndices();
      s.add(const _Game(99, 'Brand New Slot'));
      s.buildIndices();
      expect(s.single('brand')?.obj.id, 99);
      s.dispose();
    });

    test('FuzzyStringMatcher: addAll 能搜到,length 增长', () {
      final m = FuzzyStringMatcher(['alpha'])..buildIndices();
      m.addAll(['beta', 'gamma']);
      expect(m.length, 3);
      expect(m.single('gamma'), isNotNull);
      m.dispose();
    });
  });

  group('增删改清(CRUD)', () {
    test('update:替换某条,旧的搜不到、新的能搜到', () {
      final s = FuzzyMatcher<_Game>(
        [const _Game(1, 'Dragon Treasure'), const _Game(2, 'Golden Fortune')],
        (g) => g.name,
      )..buildIndices();
      s.update(0, const _Game(99, 'Phoenix Rising'));
      expect(s.single('dragon'), isNull); // 旧的没了
      expect(s.single('phoenix')?.obj.id, 99); // 新的在,且下标映射正确
      expect(s.length, 2);
      s.dispose();
    });

    test('removeAt / removeWhere:删除后搜不到、下标仍正确', () {
      final s = FuzzyMatcher<_Game>(games, (g) => g.name)..buildIndices();
      final removed = s.removeWhere((g) => g.name.toLowerCase().contains('dragon'));
      expect(removed, 2); // Dragon Treasure + Lucky Dragon
      expect(s.length, 2);
      expect(s.single('dragon'), isNull);
      expect(s.single('golden')?.obj.id, 2); // 剩余项下标映射正确
      s.dispose();
    });

    test('clear:清空后为空,仍可继续 add', () {
      final m = FuzzyStringMatcher(['alpha', 'beta'])..buildIndices();
      m.clear();
      expect(m.length, 0);
      expect(m.match('alpha'), isEmpty);
      m.add('gamma'); // 清空后继续追加
      expect(m.single('gamma'), isNotNull);
      m.dispose();
    });

    test('removeAt 越界抛 RangeError', () {
      final m = FuzzyStringMatcher(['a'])..buildIndices();
      expect(() => m.removeAt(5), throwsRangeError);
      m.dispose();
    });
  });

  group('独立函数与配置', () {
    test('fuzzyMatch / fuzzyMatchIndices', () {
      const cfg = kDefaultFuzzyConfig;
      expect(fuzzyMatch(query: 'dt', haystack: 'Dragon Treasure', config: cfg), isNotNull);
      expect(fuzzyMatch(query: 'zzz', haystack: 'Dragon Treasure', config: cfg), isNull);
      final r = fuzzyMatchIndices(query: 'dt', haystack: 'Dragon Treasure', config: cfg);
      expect(r, isNotNull);
      expect(r!.indices, isNotEmpty);
    });

    test('fuzzyFilter / fuzzyFilterAsync', () async {
      const items = ['Dragon', 'Golden', 'Lucky Dragon'];
      const cfg = kDefaultFuzzyConfig;
      final sync = fuzzyFilter(query: 'dragon', items: items, config: cfg);
      expect(sync, isNotEmpty);
      final async = await fuzzyFilterAsync(query: 'dragon', items: items, config: cfg);
      expect(async.length, sync.length);
    });

    test('FuzzyConfig.ignoreCase 生效', () {
      const respect = FuzzyConfig(ignoreCase: false, normalize: true, preferPrefix: false);
      expect(fuzzyMatch(query: 'rust', haystack: 'RUST', config: respect), isNull);
      expect(fuzzyMatch(query: 'rust', haystack: 'RUST', config: kDefaultFuzzyConfig), isNotNull);
    });
  });
}
