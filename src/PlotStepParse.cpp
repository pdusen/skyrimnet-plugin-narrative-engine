#include <PlotStepParse.h>

#include <TextUtf8.h>

namespace NarrativeEngine::PlotStepParse
{
    namespace
    {
        // A name word too short to count: "Al" is a word in too many
        // sentences that are not about anyone.
        constexpr std::size_t kShortestName = 3;

    } // namespace

    bool NamesThePlayer(std::string_view text, std::string_view playerName, const LanguageTable::Table& table)
    {
        const auto& rules = table.Matching();
        if (const auto* title = rules.Find("PlayerTitle"); title != nullptr && title->Fires(text, rules.options)) {
            return true;
        }
        const auto suffixes = rules.List("Name.Suffixes");
        const auto said = TextUtf8::Words(text);
        for (const auto name : TextUtf8::Words(playerName)) {
            if (TextUtf8::CodePointCount(name) < kShortestName) {
                continue;
            }
            if (!rules.options.wordSeparated) {
                if (text.find(name) != std::string_view::npos) {
                    return true;
                }
                continue;
            }
            for (const auto word : said) {
                if (TextMatch::IsNameWord(word, name, suffixes)) {
                    return true;
                }
            }
        }
        return false;
    }
} // namespace NarrativeEngine::PlotStepParse
