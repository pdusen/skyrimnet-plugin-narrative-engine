#include <CharacterEmbedding.h>

#include <nlohmann/json.hpp>

#include <cctype>

// The pure half of CharacterEmbedding: WordPiece tokenization.
//
// Separate from the runtime half so the tokenization can be driven from
// a probe with a hand-built vocabulary and no ONNX Runtime, no model
// file, and no game. Getting this wrong is the failure mode that would
// be hardest to see from the outside -- a subtly mis-tokenized corpus
// still produces vectors, still ranks, and is simply worse.
namespace NarrativeEngine::CharacterEmbedding
{
    namespace
    {
        // BertNormalizer's word split, for an uncased model.
        //
        // Lowercase, drop anything that is not a letter or digit, and
        // emit punctuation as its own token -- which is what the model
        // was trained against, so departing from it silently degrades
        // every vector.
        //
        // Non-ASCII bytes end the current word and are skipped. The
        // reference normalizer strips accents into their base letters
        // instead; these biographies are English prose where accented
        // characters are rare enough that the difference has not been
        // worth a UTF-8 decoder.
        std::vector<std::string> SplitWords(std::string_view text)
        {
            std::vector<std::string> out;
            std::string current;
            for (const char raw : text) {
                const auto c = static_cast<unsigned char>(raw);
                if (std::isalnum(c) != 0) {
                    current.push_back(static_cast<char>(std::tolower(c)));
                    continue;
                }
                if (!current.empty()) {
                    out.push_back(current);
                    current.clear();
                }
                if (std::ispunct(c) != 0) {
                    out.emplace_back(1, static_cast<char>(c));
                }
            }
            if (!current.empty()) {
                out.push_back(current);
            }
            return out;
        }
    } // namespace

    bool Vocabulary::LoadFromJson(std::string_view json)
    {
        _vocab.clear();

        const auto parsed = nlohmann::json::parse(json, nullptr, false);
        if (parsed.is_discarded()) {
            return false;
        }
        const auto model = parsed.find("model");
        if (model == parsed.end()) {
            return false;
        }
        const auto vocab = model->find("vocab");
        if (vocab == model->end() || !vocab->is_object()) {
            return false;
        }

        _vocab.reserve(vocab->size());
        for (const auto& [token, id] : vocab->items()) {
            if (id.is_number_integer()) {
                _vocab.emplace(token, id.get<std::int64_t>());
            }
        }

        _cls = Id("[CLS]");
        _sep = Id("[SEP]");
        _unk = Id("[UNK]");
        // A vocabulary without the special tokens is not a BERT
        // vocabulary, whatever else it contains.
        if (_cls < 0 || _sep < 0 || _unk < 0) {
            _vocab.clear();
            return false;
        }
        return true;
    }

    std::int64_t Vocabulary::Id(std::string_view token) const
    {
        const auto found = _vocab.find(std::string{token});
        return found == _vocab.end() ? -1 : found->second;
    }

    std::vector<std::int64_t> Vocabulary::Tokenize(std::string_view text) const
    {
        if (_vocab.empty()) {
            return {};
        }

        std::vector<std::int64_t> ids;
        ids.reserve(kMaxTokens);
        ids.push_back(_cls);

        for (const auto& word : SplitWords(text)) {
            // Greedy longest-match: take the longest prefix that is in
            // the vocabulary, then keep going from there with a "##"
            // continuation marker. A word no split covers becomes [UNK]
            // ENTIRELY rather than partially, which is the reference
            // behaviour -- half a word's pieces would be worse evidence
            // than none.
            std::vector<std::int64_t> pieces;
            std::size_t start = 0;
            bool whole = true;
            while (start < word.size()) {
                auto end = word.size();
                std::int64_t found = -1;
                while (start < end) {
                    const auto piece =
                        start == 0 ? word.substr(start, end - start) : "##" + word.substr(start, end - start);
                    const auto id = Id(piece);
                    if (id >= 0) {
                        found = id;
                        break;
                    }
                    --end;
                }
                if (found < 0) {
                    whole = false;
                    break;
                }
                pieces.push_back(found);
                start = end;
            }
            if (!whole) {
                pieces.assign(1, _unk);
            }

            for (const auto piece : pieces) {
                // One slot always reserved for [SEP]; a sequence that
                // ends without it is not what the model was trained on.
                if (ids.size() + 1 >= kMaxTokens) {
                    break;
                }
                ids.push_back(piece);
            }
            if (ids.size() + 1 >= kMaxTokens) {
                break;
            }
        }

        ids.push_back(_sep);
        return ids;
    }

    float Similarity(const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size() || a.empty()) {
            return 0.0f;
        }
        float dot = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
        }
        return dot;
    }
} // namespace NarrativeEngine::CharacterEmbedding
