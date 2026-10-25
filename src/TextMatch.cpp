#include <TextMatch.h>

#include <TextUtf8.h>

#include <array>
#include <cstddef>
#include <utility>

namespace NarrativeEngine::TextMatch
{
    namespace
    {
        constexpr std::array<std::pair<std::string_view, List Check::*>, 7> kLists{{
            {".Reject.Start", &Check::rejectStart},
            {".Reject.End", &Check::rejectEnd},
            {".Require.Start", &Check::requireStart},
            {".Require.End", &Check::requireEnd},
            {".Match.Start", &Check::matchStart},
            {".Match.End", &Check::matchEnd},
            {".Match.Anywhere", &Check::matchAnywhere},
        }};
        constexpr std::string_view kDisabled = ".Disabled";

        std::string_view Trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
                text.remove_suffix(1);
            }
            return text;
        }

        bool EndsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
        }

        std::u32string CodePoints(std::string_view text)
        {
            std::u32string out;
            for (std::size_t at = 0; at < text.size();) {
                const auto decoded = TextUtf8::DecodeAt(text, at);
                out.push_back(decoded.codePoint);
                at += decoded.length;
            }
            return out;
        }

        // Whether `pattern` matches all of `text`, `*` standing for any run of
        // code points, the empty one included.
        bool Glob(std::u32string_view pattern, std::u32string_view text)
        {
            std::size_t p = 0;
            std::size_t t = 0;
            std::size_t star = std::u32string_view::npos;
            std::size_t resume = 0;
            while (t < text.size()) {
                if (p < pattern.size() && pattern[p] == U'*') {
                    star = p++;
                    resume = t;
                } else if (p < pattern.size() && pattern[p] == text[t]) {
                    ++p;
                    ++t;
                } else if (star != std::u32string_view::npos) {
                    p = star + 1;
                    t = ++resume;
                } else {
                    return false;
                }
            }
            while (p < pattern.size() && pattern[p] == U'*') {
                ++p;
            }
            return p == pattern.size();
        }

        bool WordsMatch(const std::vector<std::u32string>& words, std::size_t at, const Entry& entry)
        {
            for (std::size_t i = 0; i < entry.patterns.size(); ++i) {
                if (!Glob(CodePoints(entry.patterns[i]), words[at + i])) {
                    return false;
                }
            }
            return true;
        }

        bool MatchesWords(std::string_view folded, const Entry& entry, Position position)
        {
            std::vector<std::u32string> words;
            for (const auto word : TextUtf8::Words(folded)) {
                words.push_back(CodePoints(word));
            }
            const auto n = entry.patterns.size();
            if (n == 0 || words.size() < n) {
                return false;
            }
            switch (position) {
            case Position::Start:
                return WordsMatch(words, 0, entry);
            case Position::End:
                return WordsMatch(words, words.size() - n, entry);
            case Position::Anywhere:
                for (std::size_t at = 0; at + n <= words.size(); ++at) {
                    if (WordsMatch(words, at, entry)) {
                        return true;
                    }
                }
                return false;
            }
            return false;
        }

        // The text between its first and last word character: what a run of
        // characters is matched against when words are not separated.
        std::u32string Core(std::string_view folded)
        {
            auto text = CodePoints(folded);
            std::size_t first = 0;
            while (first < text.size() && !TextUtf8::IsWordChar(text[first])) {
                ++first;
            }
            std::size_t last = text.size();
            while (last > first && !TextUtf8::IsWordChar(text[last - 1])) {
                --last;
            }
            return text.substr(first, last - first);
        }

        bool MatchesCharacters(std::string_view folded, const Entry& entry, Position position)
        {
            if (entry.patterns.empty()) {
                return false;
            }
            const auto pattern = CodePoints(entry.patterns.front());
            if (pattern.empty()) {
                return false;
            }
            std::u32string anchored;
            switch (position) {
            case Position::Start:
                anchored = pattern + U"*";
                break;
            case Position::End:
                anchored = U"*" + pattern;
                break;
            case Position::Anywhere:
                anchored = U"*" + pattern + U"*";
                break;
            }
            return Glob(anchored, Core(folded));
        }

        bool AnyMatch(std::string_view folded, const List& list, Position position, const Options& options)
        {
            for (const auto& entry : list) {
                if (options.wordSeparated ? MatchesWords(folded, entry, position)
                                          : MatchesCharacters(folded, entry, position)) {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    std::vector<std::string> Items(std::string_view value)
    {
        std::vector<std::string> out;
        while (!value.empty()) {
            const auto comma = value.find(',');
            const auto item = Trim(value.substr(0, comma));
            if (!item.empty()) {
                out.emplace_back(item);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            value.remove_prefix(comma + 1);
        }
        return out;
    }

    List Parse(std::string_view value, const Options& options)
    {
        List out;
        for (const auto& item : Items(value)) {
            const auto folded = TextUtf8::Fold(item);
            Entry entry;
            if (!options.wordSeparated) {
                entry.patterns.push_back(folded);
            } else {
                std::string_view rest{folded};
                while (!rest.empty()) {
                    const auto space = rest.find(' ');
                    if (const auto word = rest.substr(0, space); !word.empty()) {
                        entry.patterns.emplace_back(word);
                    }
                    if (space == std::string_view::npos) {
                        break;
                    }
                    rest.remove_prefix(space + 1);
                }
            }
            if (!entry.patterns.empty()) {
                out.push_back(std::move(entry));
            }
        }
        return out;
    }

    bool Matches(std::string_view text, const List& list, Position position, const Options& options)
    {
        return AnyMatch(TextUtf8::Fold(text), list, position, options);
    }

    bool Check::Defined() const noexcept
    {
        if (!disabled.empty()) {
            return true;
        }
        for (const auto& [suffix, member] : kLists) {
            if (!(this->*member).empty()) {
                return true;
            }
        }
        return false;
    }

    bool Check::Passes(std::string_view text, const Options& options) const
    {
        if (!disabled.empty()) {
            return true;
        }
        const auto folded = TextUtf8::Fold(text);
        if (AnyMatch(folded, rejectStart, Position::Start, options)
            || AnyMatch(folded, rejectEnd, Position::End, options)) {
            return false;
        }
        if (!requireStart.empty() && !AnyMatch(folded, requireStart, Position::Start, options)) {
            return false;
        }
        if (!requireEnd.empty() && !AnyMatch(folded, requireEnd, Position::End, options)) {
            return false;
        }
        return true;
    }

    bool Check::Fires(std::string_view text, const Options& options) const
    {
        if (!disabled.empty()) {
            return false;
        }
        const auto folded = TextUtf8::Fold(text);
        return AnyMatch(folded, matchStart, Position::Start, options)
               || AnyMatch(folded, matchEnd, Position::End, options)
               || AnyMatch(folded, matchAnywhere, Position::Anywhere, options);
    }

    bool IsNameWord(std::string_view word,
                    std::string_view name,
                    const std::vector<std::string>& suffixes,
                    bool foldCase)
    {
        const auto same = [foldCase](std::string_view a, std::string_view b) {
            return foldCase ? TextUtf8::EqualsFolded(a, b) : a == b;
        };
        if (name.empty()) {
            return false;
        }
        if (same(word, name)) {
            return true;
        }
        for (const auto& suffix : suffixes) {
            if (!suffix.empty() && word.size() > suffix.size() && same(word.substr(word.size() - suffix.size()), suffix)
                && same(word.substr(0, word.size() - suffix.size()), name)) {
                return true;
            }
        }
        return false;
    }

    bool StartsName(std::string_view word)
    {
        if (word.empty()) {
            return false;
        }
        const auto first = TextUtf8::DecodeAt(word, 0).codePoint;
        return TextUtf8::IsUpper(first) || (TextUtf8::IsLetter(first) && !TextUtf8::IsLower(first));
    }

    std::string_view CheckName(std::string_view key) noexcept
    {
        for (const auto& [suffix, member] : kLists) {
            if (EndsWith(key, suffix) && key.size() > suffix.size()) {
                return key.substr(0, key.size() - suffix.size());
            }
        }
        if (EndsWith(key, kDisabled) && key.size() > kDisabled.size()) {
            return key.substr(0, key.size() - kDisabled.size());
        }
        return {};
    }

    const Check* Rules::Find(std::string_view name) const
    {
        const auto it = checks.find(name);
        return it != checks.end() ? &it->second : nullptr;
    }

    std::vector<std::string> Rules::List(std::string_view key) const
    {
        const auto it = lists.find(key);
        return it != lists.end() ? Items(it->second) : std::vector<std::string>{};
    }

    void Rules::Set(std::string_view key, std::string_view value)
    {
        const auto name = CheckName(key);
        if (name.empty()) {
            lists.insert_or_assign(std::string{key}, std::string{Trim(value)});
            return;
        }
        auto& check = checks[std::string{name}];
        if (EndsWith(key, kDisabled)) {
            check.disabled = std::string{Trim(value)};
            return;
        }
        for (const auto& [suffix, member] : kLists) {
            if (EndsWith(key, suffix)) {
                check.*member = Parse(value, options);
                return;
            }
        }
    }
} // namespace NarrativeEngine::TextMatch
