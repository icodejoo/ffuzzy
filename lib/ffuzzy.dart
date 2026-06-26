/// ffuzzy – C-backed fuzzy string matching for Flutter.
///
/// Usage:
/// ```dart
/// import 'package:ffuzzy/ffuzzy.dart';
///
/// final corpus = FuzzyCorpus(['hello world', 'fuzzy search', 'flutter']);
///
/// // Synchronous search
/// final hits = corpus.filter('flu');
/// for (final h in hits) {
///   print('${h.index}  score=${h.score}  positions=${h.indices}');
/// }
///
/// // Async search (runs in a background Isolate)
/// final hitsAsync = await corpus.filterAsync('fz', limit: 10);
///
/// corpus.dispose();
/// ```
library ffuzzy;

export 'src/matcher.dart' show FuzzyCorpus, FuzzyHit;
