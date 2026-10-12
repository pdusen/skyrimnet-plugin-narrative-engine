#include <PlotStepParse.h>

#include <cctype>

namespace NarrativeEngine::PlotStepParse
{
    namespace
    {
        bool IsWordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
        }

        // `word` stands whole somewhere in `text`. With `foldCase`, letters
        // compare without regard to case.
        bool HasWord(std::string_view text, std::string_view word, bool foldCase)
        {
            if (word.empty()) {
                return false;
            }
            for (std::size_t at = 0; at + word.size() <= text.size(); ++at) {
                bool same = true;
                for (std::size_t i = 0; i < word.size() && same; ++i) {
                    const char a = text[at + i];
                    const char b = word[i];
                    same = foldCase ? std::tolower(static_cast<unsigned char>(a))
                                          == std::tolower(static_cast<unsigned char>(b))
                                    : a == b;
                }
                if (!same) {
                    continue;
                }
                const auto before = at == 0 || !IsWordChar(text[at - 1]);
                const auto after = at + word.size() >= text.size() || !IsWordChar(text[at + word.size()]);
                if (before && after) {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    bool NamesThePlayer(std::string_view text, std::string_view playerName)
    {
        if (HasWord(text, "dragonborn", true)) {
            return true;
        }
        std::size_t start = 0;
        while (start < playerName.size()) {
            auto end = playerName.find(' ', start);
            if (end == std::string_view::npos) {
                end = playerName.size();
            }
            const auto word = playerName.substr(start, end - start);
            if (word.size() >= 3 && HasWord(text, word, false)) {
                return true;
            }
            start = end + 1;
        }
        return false;
    }
} // namespace NarrativeEngine::PlotStepParse
