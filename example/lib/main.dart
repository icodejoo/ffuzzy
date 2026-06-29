// Minimal demo of the ffz fuzzy matcher: type in the box to filter a list,
// with matched characters highlighted (highlight: true → FuzzyHit.indices).
import 'package:flutter/material.dart';
import 'package:ffuzzy/ffuzzy.dart';

void main() => runApp(const FuzzyDemoApp());

const _items = <String>[
  'lib/src/widgets/scaffold.dart',
  'lib/src/material/app_bar.dart',
  'packages/ffz/lib/ffz.dart',
  'README.md',
  'CHANGELOG.md',
  '中文搜索引擎', // findable by pinyin via addKey below
  'café_menu.json',
  'src/main.rs',
];

class FuzzyDemoApp extends StatelessWidget {
  const FuzzyDemoApp({super.key});
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
  late final FuzzyCorpus<String> _corpus;
  List<FuzzyHit<String>> _hits = const [];
  int _searchGen = 0; // bumped per keystroke so only the latest result is shown

  @override
  void initState() {
    super.initState();
    _corpus = FuzzyCorpus.strings(_items, matchPaths: true);
    // Index the CJK item by host-computed pinyin/initials so latin typing finds it.
    _corpus.addKey('中文搜索引擎', [
      FuzzyKey.kind('zhongwensousuoyinqing', FuzzyKeyKind.pinyin),
      FuzzyKey.kind('zwssyq', FuzzyKeyKind.initials),
    ]);
    _search('');
  }

  Future<void> _search(String q) async {
    // fuzzyAsync keeps the UI smooth even for a large corpus. Because searches
    // can finish out of order under fast typing, tag each with a generation and
    // apply only the latest — so the displayed results always match the newest
    // query (never a stale one). (For a small corpus, a synchronous `_corpus
    // .fuzzy(q)` is simpler and inherently latest-wins.)
    final gen = ++_searchGen;
    final hits = await _corpus.fuzzyAsync(q, limit: 50, highlight: true);
    // Ignore if a newer keystroke superseded this, or the widget is gone.
    if (!mounted || gen != _searchGen) return;
    setState(() => _hits = hits);
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
                final text = hit.raw;
                final positions = hit.matchedKey == 0
                    ? fuzzyCodepointToUtf16(text, hit.indices).toSet()
                    : const <int>{};
                return ListTile(
                  dense: true,
                  title: _Highlighted(text, positions),
                  trailing: Text('${hit.score}'),
                  subtitle: hit.matchedKind == FuzzyKeyKind.original
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
