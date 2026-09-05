#include <CharacterIndex.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace NarrativeEngine::CharacterIndex
{
    namespace
    {
        // Terms carrying no discriminating weight over English prose.
        //
        // Deliberately short. A long stoplist is a way of guessing which
        // words do not matter, and BM25 already answers that
        // quantitatively: a term in most documents gets an inverse
        // document frequency near zero and stops affecting the ranking
        // on its own. These are here to keep the postings from being
        // dominated by entries that can never change an outcome, not to
        // improve the ranking.
        const std::unordered_set<std::string_view>& Stopwords()
        {
            static const std::unordered_set<std::string_view> words{
                "and",     "are",     "was",   "were",  "been",    "being",   "its",    "for",    "with",    "from",
                "that",    "this",    "these", "those", "the",     "not",     "nor",    "but",    "any",     "all",
                "both",    "each",    "few",   "more",  "most",    "other",   "some",   "only",   "own",     "same",
                "too",     "very",    "can",   "will",  "would",   "shall",   "should", "may",    "might",   "must",
                "does",    "did",     "done",  "have",  "has",     "had",     "having", "there",  "here",    "who",
                "whom",    "which",   "what",  "when",  "where",   "why",     "how",    "into",   "over",    "under",
                "again",   "further", "one",   "two",   "three",   "about",   "after",  "before", "between", "during",
                "without", "within",  "upon",  "while", "because", "through", "than",   "then",   "such",    "also",
                "just",    "them",    "their", "they",  "his",     "her",     "she",    "him",    "you",     "your",
                "our"};
            return words;
        }

        // Fold a handful of English suffixes.
        //
        // Not a stemmer. A real one (Porter, Snowball) is a few hundred
        // lines of rules for a corpus this size, and the cases that
        // actually matter over occupational prose are the plural and the
        // participle: "recruits" / "recruiting" / "recruited" all
        // reaching "recruit" is most of the benefit, and the rest is
        // long-tail. The length guard is what keeps it from mangling
        // short words — "ash" must not become "a".
        void FoldSuffix(std::string& token)
        {
            static constexpr std::string_view kSuffixes[]{"ing", "edly", "ed", "es", "s"};
            for (const auto suffix : kSuffixes) {
                if (token.size() > suffix.size() + 3
                    && token.compare(token.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    token.erase(token.size() - suffix.size());
                    return;
                }
            }
        }

        [[nodiscard]] constexpr bool IsAsciiLetter(unsigned char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        [[nodiscard]] constexpr char ToLower(unsigned char c) noexcept
        {
            return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
    } // namespace

    std::vector<std::string> Tokenize(std::string_view text)
    {
        std::vector<std::string> out;
        std::string current;
        current.reserve(24);

        const auto flush = [&]() {
            if (current.size() >= 3 && !Stopwords().contains(current)) {
                FoldSuffix(current);
                out.push_back(current);
            }
            current.clear();
        };

        for (const char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (IsAsciiLetter(c)) {
                current.push_back(ToLower(c));
                continue;
            }
            // An apostrophe inside a word joins it rather than breaking
            // it, so "Mogrul's" indexes as one term.
            //
            // Nearly a no-op in practice, and kept for the cases where
            // it is not. Almost every apostrophe in these bios is a
            // possessive 's, and suffix folding strips that trailing s
            // again a moment later -- so "Mogrul's" reaches "mogrul"
            // whether this joins or breaks. It earns its place on
            // O'Ryan-shaped names, where breaking would leave "ryan"
            // as a separate term and lose the "ryan"/"oryan" match.
            //
            // Bios use the typographic apostrophe too; that one is
            // multi-byte, so it terminates the token below. Same
            // outcome for a possessive, for the same reason.
            if (c == '\'' && !current.empty()) {
                continue;
            }
            flush();
        }
        flush();
        return out;
    }

    void Index::Add(RE::FormID character, std::string_view text)
    {
        if (_built || character == 0) {
            return;
        }

        auto tokens = Tokenize(text);
        if (tokens.empty()) {
            return;
        }

        const auto existing = _byCharacter.find(character);
        if (existing != _byCharacter.end()) {
            auto& doc = _docs[existing->second];
            doc.terms.insert(
                doc.terms.end(), std::make_move_iterator(tokens.begin()), std::make_move_iterator(tokens.end()));
            return;
        }

        _byCharacter.emplace(character, static_cast<std::uint32_t>(_docs.size()));
        _docs.push_back(Document{character, 0, std::move(tokens)});
    }

    void Index::Build()
    {
        if (_built) {
            return;
        }
        _built = true;

        _terms.clear();
        _postings.clear();
        _idf.clear();

        // One pass per document, counting term frequencies locally
        // before they reach the postings. Appending straight into the
        // postings would need a linear scan of each list to find whether
        // this document is already on it.
        std::unordered_map<std::uint32_t, std::uint32_t> frequencies;
        std::uint64_t totalLength = 0;

        for (std::uint32_t docIndex = 0; docIndex < _docs.size(); ++docIndex) {
            auto& doc = _docs[docIndex];
            doc.length = static_cast<std::uint32_t>(doc.terms.size());
            totalLength += doc.length;

            frequencies.clear();
            for (const auto& term : doc.terms) {
                const auto known = _terms.find(term);
                std::uint32_t termId = 0;
                if (known != _terms.end()) {
                    termId = known->second;
                } else {
                    termId = static_cast<std::uint32_t>(_postings.size());
                    _terms.emplace(term, termId);
                    _postings.emplace_back();
                }
                ++frequencies[termId];
            }

            for (const auto& [termId, frequency] : frequencies) {
                _postings[termId].push_back(Posting{docIndex, frequency});
            }

            // The tokens themselves are dead once they are postings, and
            // they are the bulk of what this holds: several hundred
            // std::strings per character across the whole population.
            doc.terms.clear();
            doc.terms.shrink_to_fit();
        }

        _averageLength = _docs.empty() ? 0.0f : static_cast<float>(totalLength) / static_cast<float>(_docs.size());

        // The smoothed probabilistic IDF, which unlike the textbook
        // form cannot go negative for a term that appears in more than
        // half the corpus. Without the +1 those terms would SUBTRACT
        // from a document's score, so a bio that happened to mention
        // "guard" would rank below one that never said it, for a query
        // asking for a guard.
        const auto total = static_cast<float>(_docs.size());
        _idf.reserve(_postings.size());
        for (const auto& postings : _postings) {
            const auto containing = static_cast<float>(postings.size());
            _idf.push_back(std::log(1.0f + (total - containing + 0.5f) / (containing + 0.5f)));
        }
    }

    void Index::Accumulate(const std::vector<std::string>& query,
                           std::unordered_map<std::uint32_t, float>& out,
                           std::unordered_map<std::uint32_t, std::uint32_t>* hits,
                           std::size_t* asked) const
    {
        // Distinct terms only. A query that says the same word twice is
        // asking for one thing, and counting it twice would let
        // repetition inflate coverage.
        std::unordered_set<std::string_view> seen;
        for (const auto& term : query) {
            if (!seen.emplace(term).second) {
                continue;
            }
            // Counted BEFORE the vocabulary lookup, so a term nobody has
            // still lands in the denominator. See Match::coverage.
            if (asked != nullptr) {
                ++*asked;
            }
            const auto found = _terms.find(term);
            if (found == _terms.end()) {
                continue;
            }
            const auto termId = found->second;
            const auto idf = _idf[termId];
            for (const auto& posting : _postings[termId]) {
                const auto frequency = static_cast<float>(posting.frequency);
                const auto length = static_cast<float>(_docs[posting.doc].length);
                const auto norm = _params.k1 * (1.0f - _params.b + _params.b * length / _averageLength);
                out[posting.doc] += idf * (frequency * (_params.k1 + 1.0f)) / (frequency + norm);
                if (hits != nullptr) {
                    ++(*hits)[posting.doc];
                }
            }
        }
    }

    std::vector<Match> Index::Query(std::string_view text, std::size_t limit, std::string_view suppress) const
    {
        if (!_built || _docs.empty() || limit == 0 || _averageLength <= 0.0f) {
            return {};
        }

        auto terms = Tokenize(text);
        if (!suppress.empty()) {
            const auto dropped = Tokenize(suppress);
            const std::unordered_set<std::string> drop(dropped.begin(), dropped.end());
            std::erase_if(terms, [&](const std::string& term) { return drop.contains(term); });
        }
        if (terms.empty()) {
            return {};
        }

        std::unordered_map<std::uint32_t, float> scores;
        std::unordered_map<std::uint32_t, std::uint32_t> hits;
        std::size_t asked = 0;
        Accumulate(terms, scores, &hits, &asked);
        if (scores.empty() || asked == 0) {
            return {};
        }

        std::vector<Match> matches;
        matches.reserve(scores.size());
        const auto denominator = static_cast<float>(asked);
        for (const auto& [docIndex, score] : scores) {
            if (score > 0.0f) {
                const auto hit = hits.find(docIndex);
                const auto count = hit == hits.end() ? 0u : hit->second;
                const auto covered = static_cast<float>(count) / denominator;
                matches.push_back(
                    Match{_docs[docIndex].character, score, 0.0f, covered, count, static_cast<std::uint32_t>(asked)});
            }
        }
        if (matches.empty()) {
            return {};
        }

        // Partial sort: the caller wants a handful out of what can be
        // thousands of touched documents, and ordering the tail is work
        // nobody reads.
        const auto keep = std::min(limit, matches.size());
        std::partial_sort(matches.begin(),
                          matches.begin() + static_cast<std::ptrdiff_t>(keep),
                          matches.end(),
                          [](const Match& lhs, const Match& rhs) { return lhs.score > rhs.score; });
        matches.resize(keep);

        const auto best = matches.front().score;
        for (auto& match : matches) {
            match.relative = best > 0.0f ? match.score / best : 0.0f;
        }
        return matches;
    }

    float Index::ScoreFor(RE::FormID character, std::string_view text) const
    {
        if (!_built || _averageLength <= 0.0f) {
            return 0.0f;
        }
        const auto found = _byCharacter.find(character);
        if (found == _byCharacter.end()) {
            return 0.0f;
        }

        const auto terms = Tokenize(text);
        if (terms.empty()) {
            return 0.0f;
        }

        // Deliberately NOT Accumulate. That scores every document a
        // query term touches, which for a common term is most of the
        // corpus, and this is the function a casting ladder calls once
        // per surviving candidate -- doing the whole corpus per
        // candidate would be quadratic in the population for an answer
        // about one person.
        //
        // Build fills each posting list in ascending document order, so
        // the one row wanted is a binary search rather than a scan.
        const auto docIndex = found->second;
        const auto length = static_cast<float>(_docs[docIndex].length);
        const auto norm = _params.k1 * (1.0f - _params.b + _params.b * length / _averageLength);

        auto total = 0.0f;
        for (const auto& term : terms) {
            const auto known = _terms.find(term);
            if (known == _terms.end()) {
                continue;
            }
            const auto& postings = _postings[known->second];
            const auto at =
                std::lower_bound(postings.begin(), postings.end(), docIndex, [](const Posting& lhs, std::uint32_t rhs) {
                    return lhs.doc < rhs;
                });
            if (at == postings.end() || at->doc != docIndex) {
                continue;
            }
            const auto frequency = static_cast<float>(at->frequency);
            total += _idf[known->second] * (frequency * (_params.k1 + 1.0f)) / (frequency + norm);
        }
        return total;
    }

    void Index::Clear() noexcept
    {
        _built = false;
        _averageLength = 0.0f;
        _docs.clear();
        _byCharacter.clear();
        _terms.clear();
        _postings.clear();
        _idf.clear();
    }
} // namespace NarrativeEngine::CharacterIndex
