import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:ffuzzy/ffuzzy.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await ffuzzy.ensureInitialized();
  runApp(const FfuzzyExampleApp());
}

/// 演示数据(可替换为你的对象列表)。
const List<String> kItems = <String>[
  'Dragon Treasure',
  'Golden Fortune',
  'Super Gems 1000',
  'Lucky Dragon',
  'Phoenix Rising',
  'Mystic Forest',
  'Ocean Pearls',
  'Wild West Gold',
  'Fruit Party',
  'Sweet Bonanza',
];

class FfuzzyExampleApp extends StatelessWidget {
  const FfuzzyExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ffuzzy demo',
      theme: ThemeData(useMaterial3: true, colorSchemeSeed: Colors.indigo),
      home: const SearchPage(),
    );
  }
}

class SearchPage extends StatefulWidget {
  const SearchPage({super.key});

  @override
  State<SearchPage> createState() => _SearchPageState();
}

class _SearchPageState extends State<SearchPage> {
  late final FuzzyStringMatcher _matcher =
      FuzzyStringMatcher(kItems)..buildIndices();
  List<FuzzyHit> _hits = const <FuzzyHit>[];
  int _token = 0; // 防竞态:只接受最新一次查询结果

  @override
  void initState() {
    super.initState();
    _showAll();
  }

  @override
  void dispose() {
    _matcher.dispose();
    super.dispose();
  }

  void _showAll() {
    _hits = <FuzzyHit>[
      for (int i = 0; i < kItems.length; i++)
        FuzzyHit(index: i, score: 0, indices: Uint32List(0)),
    ];
  }

  Future<void> _onChanged(String query) async {
    final token = ++_token;
    if (query.trim().isEmpty) {
      setState(_showAll);
      return;
    }
    // 异步搜索:后台线程执行,不阻塞 UI。
    final hits = await _matcher.matchAsync(query, limit: 50);
    if (!mounted || token != _token) return; // 丢弃过期结果
    setState(() => _hits = hits);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(
        title: const Text('ffuzzy 模糊搜索'),
        backgroundColor: theme.colorScheme.inversePrimary,
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(12),
            child: TextField(
              key: const Key('search-field'),
              autofocus: true,
              decoration: const InputDecoration(
                border: OutlineInputBorder(),
                prefixIcon: Icon(Icons.search),
                hintText: '输入模糊查询(如 drg、gld、bnz)',
              ),
              onChanged: _onChanged,
            ),
          ),
          const Divider(height: 1),
          Expanded(
            child: _hits.isEmpty
                ? const Center(child: Text('无匹配结果'))
                : ListView.builder(
                    itemCount: _hits.length,
                    itemBuilder: (context, i) {
                      final hit = _hits[i];
                      final text = kItems[hit.index];
                      final matched = hit.indices.toSet();
                      // indices 为字符(rune)下标,高亮按 runes 切分。
                      final runes = text.runes.toList();
                      return ListTile(
                        dense: true,
                        title: Text.rich(TextSpan(children: [
                          for (int c = 0; c < runes.length; c++)
                            TextSpan(
                              text: String.fromCharCode(runes[c]),
                              style: matched.contains(c)
                                  ? TextStyle(
                                      fontWeight: FontWeight.bold,
                                      color: theme.colorScheme.primary,
                                    )
                                  : null,
                            ),
                        ])),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
