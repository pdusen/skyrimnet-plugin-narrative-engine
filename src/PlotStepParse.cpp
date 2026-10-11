#include <PlotStepParse.h>

#include <cctype>

namespace NarrativeEngine::PlotStepParse
{
    bool NamesThePlayer(std::string_view text)
    {
        constexpr std::string_view kWord = "dragonborn";
        for (std::size_t at = 0; at + kWord.size() <= text.size(); ++at) {
            bool same = true;
            for (std::size_t i = 0; i < kWord.size() && same; ++i) {
                same = static_cast<char>(std::tolower(static_cast<unsigned char>(text[at + i]))) == kWord[i];
            }
            if (!same) {
                continue;
            }
            const auto before = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
            const auto after =
                at + kWord.size() >= text.size() || !std::isalpha(static_cast<unsigned char>(text[at + kWord.size()]));
            if (before && after) {
                return true;
            }
        }
        return false;
    }
} // namespace NarrativeEngine::PlotStepParse
