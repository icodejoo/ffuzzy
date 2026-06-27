// Minimal demo of the ffz fuzzy matcher: type in the box to filter a list,
// with matched characters highlighted. Shows the codepoint->UTF-16 conversion
// needed for correct highlighting of Unicode (incl. CJK) text.
import 'package:flutter/material.dart';
import 'package:ffuzzy/ffuzzy.dart';

void main() => runApp(const FfzDemoApp());

const _items = <String>[
  'lib/src/widgets/scaffold.dart',
  'lib/src/material/app_bar.dart',
  'packages/ffz/lib/ffz.dart',
  'README.md',
  'CHANGELOG.md',
  '中文搜索引擎', // findable by pinyin via addKeyed below
  'café_menu.json',
  'src/main.rs',
];

class FfzDemoApp extends StatelessWidget {
  const FfzDemoApp({super.key});
  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'ffz demo',
        theme: ThemeData(useMaterial3: true, colorSchemeSeed: Colors.indigo),
        home: const SearchPage(),
      );
}

class SearchPage extends StatefulWidget {
  const SearchPage({super.key});
  @override
  State<SearchPage> createState() => _SearchPageState();
}

class _SearchPageState extends State<SearchPage> {
  late final FfzCorpus _corpus;
  List<FfzHit> _hits = const [];

  @override
  void initState() {
    super.initState();
    _corpus = FfzCorpus(matchPaths: true);
    _corpus.addAll(_items);
    // Index the CJK item by host-computed pinyin/initials so latin typing finds it.
    _corpus.addKeyed('中文搜索引擎', [
      FfzKey.kind('zhongwensousuoyinqing', FfzKeyKind.pinyin),
      FfzKey.kind('zwssyq', FfzKeyKind.initials),
    ]);
    _search('');
  }

  Future<void> _search(String q) async {
    // filterAsync keeps the UI smooth even for a large corpus.
    final hits = await _corpus.filterAsync(q, limit: 50);
    if (mounted) setState(() => _hits = hits);
  }

  @override
  void dispose() {
    _corpus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('ffz fuzzy search')),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(12),
            child: TextField(
              autofocus: true,
              decoration: const InputDecoration(
                hintText: 'Type to fuzzy-search (try "appbar", "中文", "zwssyq")',
                border: OutlineInputBorder(),
              ),
              onChanged: _search,
            ),
          ),
          Expanded(
            child: ListView.builder(
              itemCount: _hits.length,
              itemBuilder: (context, i) {
                final hit = _hits[i];
                // matchedKey 0 == the original item; only highlight then.
                final text = _items[hit.index];
                final highlight = hit.matchedKey == 0
                    ? ffzCodepointToUtf16(text, hit.indices).toSet()
                    : const <int>{};
                return ListTile(
                  dense: true,
                  title: _Highlighted(text, highlight),
                  trailing: Text('${hit.score}'),
                  subtitle: hit.matchedKind == FfzKeyKind.original
                      ? null
                      : Text('via ${hit.matchedKind.name}'),
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}

class _Highlighted extends StatelessWidget {
  const _Highlighted(this.text, this.positions);
  final String text;
  final Set<int> positions;

  @override
  Widget build(BuildContext context) {
    final base = DefaultTextStyle.of(context).style;
    final hi = base.copyWith(
        fontWeight: FontWeight.bold,
        color: Theme.of(context).colorScheme.primary);
    final spans = <TextSpan>[];
    final units = text.codeUnits;
    for (var i = 0; i < units.length; i++) {
      spans.add(TextSpan(
          text: String.fromCharCode(units[i]),
          style: positions.contains(i) ? hi : base));
    }
    return Text.rich(TextSpan(children: spans));
  }
}
