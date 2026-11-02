#include <ItemGrammar.h>

#include <TextUtf8.h>

namespace NarrativeEngine::ItemGrammar
{
    namespace
    {
        constexpr std::string_view kOther = "other";

        // More than any ending can score, so a whole word always wins.
        constexpr std::size_t kWholeWord = static_cast<std::size_t>(-1);

        [[nodiscard]] std::string_view Trimmed(std::string_view text) noexcept
        {
            constexpr std::string_view kSpace = " \t\r\n";
            const auto first = text.find_first_not_of(kSpace);
            if (first == std::string_view::npos) {
                return {};
            }
            return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
        }

        [[nodiscard]] std::vector<std::string_view> Split(std::string_view value, char separator)
        {
            std::vector<std::string_view> out;
            std::size_t start = 0;
            while (start <= value.size()) {
                auto end = value.find(separator, start);
                if (end == std::string_view::npos) {
                    end = value.size();
                }
                if (const auto item = Trimmed(value.substr(start, end - start)); !item.empty()) {
                    out.push_back(item);
                }
                start = end + 1;
            }
            return out;
        }

        [[nodiscard]] std::optional<Pattern> ParsePattern(std::string_view text)
        {
            const bool ending = text.starts_with('*');
            if (ending) {
                text.remove_prefix(1);
            }
            if (text.empty() || text.find('*') != std::string_view::npos) {
                return std::nullopt;
            }
            return Pattern{TextUtf8::Fold(text), TextUtf8::CodePointCount(text), ending};
        }

        // The byte offset `count` code points from the end of `text`, or 0
        // when it is no longer than that.
        [[nodiscard]] std::size_t FromEnd(std::string_view text, std::size_t count) noexcept
        {
            std::size_t at = text.size();
            while (count > 0 && at > 0) {
                --at;
                // Step back over continuation bytes to the start of a code point.
                while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
                    --at;
                }
                --count;
            }
            return at;
        }

        // One word of a name: what is matched, and the punctuation after it,
        // which is put back after any rewrite.
        struct Word
        {
            std::string_view core;
            std::string_view tail;
            std::string folded;
        };

        [[nodiscard]] Word Read(std::string_view word)
        {
            std::size_t end = word.size();
            while (end > 0) {
                const auto at = FromEnd(word.substr(0, end), 1);
                if (TextUtf8::IsLetter(TextUtf8::DecodeAt(word, at).codePoint)) {
                    break;
                }
                end = at;
            }
            const auto core = word.substr(0, end);
            return {core, word.substr(end), TextUtf8::Fold(core)};
        }

        // How well `pattern` matches a word: 0 for not at all, the ending's
        // length for an ending, kWholeWord for the whole word.
        [[nodiscard]] std::size_t Score(const Pattern& pattern, const Word& word)
        {
            if (!pattern.ending) {
                return word.folded == pattern.folded ? kWholeWord : 0;
            }
            return word.folded.ends_with(pattern.folded) ? pattern.length : 0;
        }

        template <typename T, typename PatternOf>
        [[nodiscard]] std::pair<const T*, std::size_t> Best(const std::vector<T>& entries,
                                                            const Word& word,
                                                            PatternOf patternOf)
        {
            const T* best = nullptr;
            std::size_t top = 0;
            for (const auto& entry : entries) {
                if (const auto score = Score(patternOf(entry), word); score > top) {
                    best = &entry;
                    top = score;
                }
            }
            return {best, top};
        }

        const Pattern& FromOf(const Rewrite& rewrite)
        {
            return rewrite.from;
        }

        [[nodiscard]] std::string Rewritten(const Word& word, const Rewrite& rewrite)
        {
            auto out = rewrite.from.ending
                           ? std::string{word.core.substr(0, FromEnd(word.core, rewrite.from.length))} + rewrite.to
                           : rewrite.to;
            if (!word.core.empty() && TextUtf8::IsUpper(TextUtf8::DecodeAt(word.core, 0).codePoint)) {
                out = TextUtf8::UppercaseFirst(out);
            }
            return out + std::string{word.tail};
        }

        [[nodiscard]] std::string Joined(const std::vector<std::string>& words)
        {
            std::string out;
            for (const auto& word : words) {
                if (!out.empty()) {
                    out += ' ';
                }
                out += word;
            }
            return out;
        }
    } // namespace

    std::optional<std::vector<Pattern>> ParsePatterns(std::string_view value)
    {
        std::vector<Pattern> out;
        for (const auto item : Split(value, ',')) {
            auto pattern = ParsePattern(item);
            if (!pattern) {
                return std::nullopt;
            }
            out.push_back(std::move(*pattern));
        }
        return out;
    }

    std::optional<std::vector<Rewrite>> ParseRewrites(std::string_view value)
    {
        std::vector<Rewrite> out;
        for (const auto item : Split(value, ',')) {
            const auto sides = Split(item, '>');
            if (sides.size() != 2) {
                return std::nullopt;
            }
            auto from = ParsePattern(sides[0]);
            const bool toEnding = sides[1].starts_with('*');
            const auto to = toEnding ? sides[1].substr(1) : sides[1];
            if (!from || from->ending != toEnding || to.find('*') != std::string_view::npos) {
                return std::nullopt;
            }
            out.push_back({std::move(*from), std::string{to}});
        }
        return out;
    }

    Fitted Fit(const Rules& rules, std::string_view name, bool ammo)
    {
        std::vector<std::string> out;
        std::vector<Word> words;
        for (const auto word : Split(name, ' ')) {
            out.emplace_back(word);
            words.push_back(Read(word));
        }

        std::string gender{kOther};
        if (!rules.classes.empty()) {
            const std::pair<Pattern, std::string>* decided = nullptr;
            for (const auto& word : words) {
                decided =
                    Best(rules.classes, word, [](const auto& entry) -> const Pattern& { return entry.first; }).first;
                if (decided != nullptr) {
                    break;
                }
            }
            if (decided == nullptr) {
                return {std::string{name}, gender};
            }
            gender = decided->second;
        }

        if (ammo) {
            for (std::size_t i = 0; i < words.size(); ++i) {
                if (const auto* rewrite = Best(rules.plural, words[i], FromOf).first) {
                    out[i] = Rewritten(words[i], *rewrite);
                }
            }
            return {Joined(out), gender};
        }

        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto [modifier, asModifier] = Best(rules.caseModifier, words[i], FromOf);
            const auto [head, asHead] = Best(rules.caseHead, words[i], FromOf);
            // A word both lists match is whichever matches it more closely.
            const bool isHead = head != nullptr && asHead > asModifier;
            const auto* rewrite = isHead ? head : modifier;
            if (rewrite == nullptr) {
                break;
            }
            out[i] = Rewritten(words[i], *rewrite);
            if (isHead) {
                break;
            }
        }
        return {Joined(out), gender};
    }
} // namespace NarrativeEngine::ItemGrammar
