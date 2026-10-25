#pragma once

#include <LanguageTable.h>

#include <string>
#include <string_view>

// The language files statics/ ships, for engine tests that render real text.
namespace NarrativeEngine::Testing
{
    // The shipped file's text, or empty when there is no such file.
    [[nodiscard]] std::string ShippedLanguageFile(std::string_view stem);

    // A prompt file statics/ ships, by its path under the plugin's
    // prompts/ folder without the extension: "components/narrative_engine/
    // beats/ambush". Empty when there is no such file.
    [[nodiscard]] std::string ShippedPrompt(std::string_view path);

    // English as the plugin loads it, built once: the shipped english.ini
    // against every key the plugin declares, with no overlay.
    [[nodiscard]] const LanguageTable::Table& ShippedEnglish();
} // namespace NarrativeEngine::Testing
