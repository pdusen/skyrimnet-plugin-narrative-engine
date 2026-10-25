#include "ShippedLanguage.h"

#include <TextKeys.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace NarrativeEngine::Testing
{
    std::string ShippedLanguageFile(std::string_view stem)
    {
        const auto path = std::filesystem::path{NARRATIVEENGINE_STATICS_DIR} / "SKSE/Plugins/NarrativeEngine/Languages"
                          / (std::string{stem} + ".ini");
        std::ifstream in(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    const LanguageTable::Table& ShippedEnglish()
    {
        static const std::string text = ShippedLanguageFile("english");
        static const auto table = LanguageTable::Table::Build({text, std::nullopt, std::nullopt}, TextKeys::All());
        return table;
    }
} // namespace NarrativeEngine::Testing
