#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>

// CharacterIndex — free prose in, ranked characters out.
//
// A BM25 retrieval index over one text document per character. The
// caller feeds it whatever prose it has about each person, calls Build
// once, and can then ask "who best matches this description" with an
// ordinary English phrase.
//
// ---------------------------------------------------------------------
// WHY THIS EXISTS
//
// Steps need people, and the people they need are described rather than
// named: "a courier who carries letters between holds", "a servant in a
// wealthy household". The previous attempt at answering that was a
// mechanically-derived taxonomy of place / organisation / occupation /
// aptitude terms, and it did not work — the terms were coarser than the
// descriptions, so a query either matched hundreds of people or nobody,
// and mod-added NPCs fell outside the vocabulary entirely.
//
// Prose has neither problem. SkyrimNet's character bios are several
// hundred words of specific, written detail per NPC, they already cover
// mod-added characters, and matching against them is a solved retrieval
// problem rather than an ontology problem.
//
// ---------------------------------------------------------------------
// WHAT IT IS NOT
//
// It is not a filter and it does not decide anything. Query returns
// SCORES, and the casting ladder remains the thing that knows who is
// occupied, who is cooling down, who outranks the schemer and who is
// their enemy. That split is deliberate and it is why the return type
// is a ranked list rather than a set: a set either contains the person
// the ladder wanted or vetoes them, and the taxonomy's worst failures
// were all vetoes. A score composes with the ladder's own weighting; a
// set fights it.
//
// It is also NOT a semantic matcher. BM25 matches words, so a query
// that shares no vocabulary with the bio scores zero however well it
// describes the person. Measured on 3,061 real bios against 94 real
// logged steps, the sharpest instance was "a mage of the College of
// Winterhold", which returned no College members at all: `college` and
// `winterhold` each appear in roughly one bio in six, because every bio
// mentions places in passing, so both carry almost no discriminating
// weight. Rewritten with the jargon the members' own bios use —
// `arcanaeum`, `instructor`, `apprentice` — four of the top six were
// correct. Queries phrased in an institution's NAME will underperform
// queries phrased in its VOCABULARY, and no amount of tuning here fixes
// that; it is what an embedding model would be for.
//
// ---------------------------------------------------------------------
// PURE
//
// Nothing here reads the engine, holds a token or knows a thread
// exists — PlotModel's precedent. RE::FormID appears only as an opaque
// key the caller hands in and gets back. Loading bio text from disk,
// and mapping a character to their bio file, both belong to the
// engine-bound caller.
namespace NarrativeEngine::CharacterIndex
{
    // One result. `character` is whatever key the caller added.
    struct Match
    {
        RE::FormID character = 0;

        // Raw BM25. Unbounded above, and NOT comparable between
        // queries: a six-word query scores lower than a twelve-word one
        // against the same document simply because there are fewer
        // terms to accumulate. Compare within one result set only.
        float score = 0.0f;

        // `score` over the best score in this result set, so 1.0 for the
        // top hit and downward from there. This is the number that is
        // safe to threshold on, and the one to weight a ladder with.
        //
        // Expect it to be HIGH for everything. Across the measured runs
        // the fifth hit averaged 0.79 of the first, because a few
        // hundred words of prose about any Skyrim NPC share a great deal
        // of vocabulary with a few hundred words about any other. This
        // index shortlists well and picks badly; treat a top-five that
        // spans 1.00 to 0.95 as five candidates, not as a winner and
        // four losers.
        float relative = 0.0f;

        // The fraction of the query's terms this document actually
        // contains, 0..1.
        //
        // THE ONLY FIELD HERE THAT MEANS ANYTHING ACROSS QUERIES, and
        // the one to gate on when deciding whether a match is good
        // enough to use at all. `score` is unbounded and scales with
        // query length; `relative` is normalised against the result set,
        // so the top hit is 1.0 by construction even when it is a bad
        // match and the runner-up is worse. Neither can answer "is this
        // person actually what was asked for" -- only "is this person
        // the best of what turned up".
        //
        // EVERY term counts toward the denominator, including ones no
        // document in the corpus contains. That is not the obvious
        // choice and the obvious choice was wrong: excluding unmatchable
        // terms was tried first, on the reasoning that a word nobody has
        // cannot be evidence against anybody, and it inverted the
        // measure. The more alien a query was, the fewer terms survived
        // into the denominator, so the EASIER a perfect score became --
        // "an airline pilot who flies passenger jets between continents"
        // scored 1.00 against a ferryman, and a stockbroker query scored
        // 1.00 against a general-goods merchant.
        //
        // Counting every term makes an unanswerable requirement cost the
        // candidate, which is right: if the corpus cannot confirm a
        // requirement, confidence in the match should fall. Those same
        // two queries now score 0.17 and 0.40.
        float coverage = 0.0f;
    };

    struct Params
    {
        // Okapi BM25's term-frequency saturation. Higher means repeating
        // a term in a document keeps mattering for longer.
        float k1 = 1.2f;

        // Length normalisation, 0..1. At the 0.75 default a long bio is
        // penalised for its length, which is wanted here: the bios run
        // from about 150 words to well over 800, and without it the
        // longest documents win every query on sheer surface area.
        float b = 0.75f;
    };

    // Split prose into indexable terms: lowercased, ASCII-only, minimum
    // three characters, stopwords dropped, a few suffixes folded so that
    // "recruits" and "recruiting" both reach "recruit".
    //
    // Exposed because it is the one part of this whose behaviour is
    // worth asserting directly, and because a caller that wants to know
    // whether a query has any usable terms in it at all should not have
    // to build an index to find out.
    [[nodiscard]] std::vector<std::string> Tokenize(std::string_view text);

    // The index itself.
    //
    // Lifecycle is Add* then Build then Query*. Query on an unbuilt
    // index returns nothing rather than asserting: the caller that
    // forgot to build gets no candidates and falls back, which is the
    // same degradation an empty corpus produces.
    class Index
    {
    public:
        Index() = default;
        explicit Index(Params params) noexcept : _params(params) {}

        // Add text for one character. Calling it more than once for the
        // same character APPENDS — a caller holding a bio in separate
        // blocks can add them one at a time without stitching them
        // together first.
        //
        // Ignored after Build, because the derived weights would be
        // stale. Build again from scratch to change the corpus.
        void Add(RE::FormID character, std::string_view text);

        // Finalise. Computes document lengths, the inverted index and
        // every term's inverse document frequency. Idempotent.
        void Build();

        // Rank characters against a description.
        //
        // `suppress` is prose whose terms are struck from the query
        // before scoring — pass the schemer's name to stop "Bergritte
        // Battle-Born bribes an acolyte" retrieving the Battle-Borns.
        // Names are the single largest source of the wrong answer here,
        // because a step's own text is ABOUT its subject: queried with
        // whole step descriptions, this index returns the step's target
        // far more often than anyone who could carry it out. Query it
        // with a description of the PERSON WANTED, not with the step.
        //
        // At most `limit` results, best first, and never a character
        // that scored zero.
        [[nodiscard]] std::vector<Match> Query(std::string_view text,
                                               std::size_t limit,
                                               std::string_view suppress = {}) const;

        // What one specific character scores against a description.
        //
        // The composition point: a ladder that has already decided who
        // is eligible can weight each survivor by this without ever
        // seeing a ranked list. Zero for an unknown character, an
        // unbuilt index, or a query with no term in common.
        [[nodiscard]] float ScoreFor(RE::FormID character, std::string_view text) const;

        [[nodiscard]] bool IsBuilt() const noexcept
        {
            return _built;
        }
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _docs.size();
        }
        [[nodiscard]] std::size_t VocabularySize() const noexcept
        {
            return _terms.size();
        }

        // Mean document length in terms. Diagnostic — a corpus whose
        // average has collapsed is usually one whose text never arrived.
        [[nodiscard]] float AverageLength() const noexcept
        {
            return _averageLength;
        }

        void Clear() noexcept;

    private:
        struct Posting
        {
            std::uint32_t doc = 0;
            std::uint32_t frequency = 0;
        };

        struct Document
        {
            RE::FormID character = 0;
            std::uint32_t length = 0;
            // Held only until Build, which folds it into the postings.
            std::vector<std::string> terms;
        };

        // Accumulate scores for one already-tokenised query. Shared by
        // Query and ScoreFor so the two can never disagree about what a
        // document is worth.
        //
        // `hits` counts, per document, how many DISTINCT query terms it
        // carried, and `asked` how many distinct terms the query had at
        // all. Together they are the coverage a Match reports.
        void Accumulate(const std::vector<std::string>& query,
                        std::unordered_map<std::uint32_t, float>& out,
                        std::unordered_map<std::uint32_t, std::uint32_t>* hits,
                        std::size_t* asked) const;

        Params _params{};
        bool _built = false;
        float _averageLength = 0.0f;

        std::vector<Document> _docs;
        std::unordered_map<RE::FormID, std::uint32_t> _byCharacter;

        // term -> index into _postings / _idf.
        std::unordered_map<std::string, std::uint32_t> _terms;
        std::vector<std::vector<Posting>> _postings;
        std::vector<float> _idf;
    };
} // namespace NarrativeEngine::CharacterIndex
