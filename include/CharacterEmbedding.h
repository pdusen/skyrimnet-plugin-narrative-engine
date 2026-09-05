#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// CharacterEmbedding — turning prose into a vector, using the model
// SkyrimNet already ships.
//
// ---------------------------------------------------------------------
// WHY, WHEN BM25 ALREADY WORKS
//
// BM25 matches WORDS. A query whose distinguishing term appears in no
// biography has nothing to match on, and the ranking is then decided by
// whatever else the query happened to say. "Whiterun stablehand who
// works near the city gate" put the stablehand at rank 137 and a
// blacksmith at rank 1, because `stablehand` is in no bio at all and
// `whiterun`, `near`, `city` and `gate` are in hundreds.
//
// An embedding has no such requirement: it puts "stablehand" near
// "stables", "horses" and "grooms" because the model learned they go
// together, not because they co-occur here. On that same query it moved
// the right answer from #137 to #9.
//
// It is NOT better across the board, which is why this augments the
// lexical index rather than replacing it. Measured over twenty queries
// with one known-correct answer each:
//
//   BM25 alone     top-1 11/20   MRR 0.658
//   cosine alone   top-1  9/20   MRR 0.619
//   both, fused    top-1 14/20   MRR 0.783
//
// Cosine alone is WORSE than BM25 alone. It loses on queries whose
// vocabulary is literally present -- "an alchemist who brews potions"
// drops Arcadia, the archetypal alchemist, from #1 to #21. The two fail
// in different places, which is exactly the condition under which fusing
// beats either. See CharacterBios::Rank for the combination.
//
// ---------------------------------------------------------------------
// WHOSE RUNTIME
//
// SkyrimNet ships all-MiniLM-L6-v2 and an ONNX Runtime to execute it,
// and has both loaded before this plugin touches anything. Nothing here
// modifies either: the runtime is reached through GetModuleHandle on the
// module already in the process, and the model and vocabulary are opened
// read-only.
//
// Both can be absent -- they are downloaded at runtime rather than
// shipped, so a fresh install may not have them yet. Everything here
// then reports unavailable and retrieval falls back to BM25 alone, which
// is a real degradation rather than a failure.
namespace NarrativeEngine::CharacterEmbedding
{
    // all-MiniLM-L6-v2's output width, and the tokenizer's own cap.
    //
    // 128 tokens is roughly ninety words, and a character biography runs
    // to several hundred. That is why callers embed a bio BLOCK at a
    // time rather than the whole thing: one vector per person would be
    // one vector of their summary and nothing else.
    inline constexpr std::size_t kDimensions = 384;
    inline constexpr std::size_t kMaxTokens = 128;

    // --- the pure half ----------------------------------------------------

    // WordPiece, over a vocabulary loaded from the model's tokenizer.json.
    //
    // Pure: no engine, no runtime, no files of its own. Given a
    // vocabulary it is a deterministic function from text to token ids,
    // which is what makes the tokenization testable without a model.
    class Vocabulary
    {
    public:
        // Reads the `model.vocab` object out of a HuggingFace
        // tokenizer.json. Returns false on anything it cannot parse,
        // leaving the vocabulary empty.
        bool LoadFromJson(std::string_view json);

        // Text to token ids, wrapped in [CLS] and [SEP], truncated to
        // kMaxTokens.
        //
        // Empty when the vocabulary is empty, which is what makes an
        // absent model degrade rather than crash.
        [[nodiscard]] std::vector<std::int64_t> Tokenize(std::string_view text) const;

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _vocab.size();
        }
        [[nodiscard]] bool Empty() const noexcept
        {
            return _vocab.empty();
        }

    private:
        [[nodiscard]] std::int64_t Id(std::string_view token) const;

        std::unordered_map<std::string, std::int64_t> _vocab;
        std::int64_t _cls = 0;
        std::int64_t _sep = 0;
        std::int64_t _unk = 0;
    };

    // Cosine similarity of two unit vectors. Both are normalised at
    // creation, so this is a plain dot product and needs no division.
    [[nodiscard]] float Similarity(const std::vector<float>& a, const std::vector<float>& b);

    // --- the runtime half -------------------------------------------------

    // Bind to SkyrimNet's ONNX Runtime and open the model. Idempotent.
    //
    // `modelDirectory` holds all-MiniLM-L6-v2.onnx and its
    // tokenizer.json. Returns false when either is missing or the
    // runtime cannot be reached, having logged which.
    bool Initialize(const std::filesystem::path& modelDirectory);

    // Release the session and environment. Safe to call unbound.
    void Shutdown();

    [[nodiscard]] bool IsAvailable();

    // One forward pass. Returns a UNIT vector of kDimensions, or empty
    // when unavailable or the text has no tokens.
    //
    // Measured at about 1.2 ms on CPU, so embedding a whole population
    // block by block is a few seconds rather than a stall -- but it is
    // still far too slow to call per candidate in a ranking loop. Embed
    // the query once and compare against stored vectors.
    [[nodiscard]] std::vector<float> Embed(std::string_view text);
} // namespace NarrativeEngine::CharacterEmbedding
