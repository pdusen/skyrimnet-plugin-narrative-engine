// The engine-free half of Localization: which language to load, and whether
// the game's fonts can show it. Localization.cpp holds the engine half.
#include <Localization.h>

#include <algorithm>
#include <array>
#include <format>

namespace NarrativeEngine::Localization
{
    namespace
    {
        [[nodiscard]] std::string Lowered(std::string_view text)
        {
            std::string out{text};
            std::ranges::transform(
                out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
            return out;
        }

        [[nodiscard]] std::string_view Trimmed(std::string_view text) noexcept
        {
            constexpr std::string_view kSpace = " \t\r\n";
            const auto first = text.find_first_not_of(kSpace);
            if (first == std::string_view::npos) {
                return {};
            }
            return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
        }

        // The MCM's Language menu stores its option, which is a translation
        // key: `$NE_Language_german`, or `$NE_Language_Auto` for the game's
        // own language. Anything else is a stem as a player would write it.
        constexpr std::string_view kMcmPrefix = "$ne_language_";

        [[nodiscard]] std::string Wanted(std::string_view override)
        {
            auto wanted = Lowered(Trimmed(override));
            if (wanted.starts_with(kMcmPrefix)) {
                wanted.erase(0, kMcmPrefix.size());
                if (wanted == "auto") {
                    wanted.clear();
                }
            }
            return wanted;
        }

        [[nodiscard]] bool Has(std::span<const std::string> stems, std::string_view stem)
        {
            return std::ranges::any_of(stems, [stem](const std::string& s) { return Lowered(s) == stem; });
        }
    } // namespace

    Resolution Resolve(std::string_view detected, std::string_view override, std::span<const std::string> available)
    {
        const auto wanted = Wanted(override);
        const auto game = Lowered(Trimmed(detected));
        std::string skipped;
        if (!wanted.empty()) {
            if (Has(available, wanted)) {
                return {wanted, std::format("the [Localization] sLanguage setting names '{}'", wanted)};
            }
            skipped = std::format("the setting names '{}', which has no file; ", wanted);
        }
        if (!game.empty() && Has(available, game)) {
            return {game, std::format("{}the game's language is '{}'", skipped, game)};
        }
        if (game.empty()) {
            return {"english", std::format("{}the game reports no language; English is the fallback", skipped)};
        }
        return {"english",
                std::format("{}the game's language '{}' has no file; English is the fallback", skipped, game)};
    }

    FontCheck FontsCover(std::string_view fontConfigFile, std::span<const std::string> fonts)
    {
        // `Interface\FontConfig_pl.txt` -> `fontconfig_pl`.
        auto name = Lowered(Trimmed(fontConfigFile));
        if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
            name.erase(0, slash + 1);
        }
        if (name.ends_with(".txt")) {
            name.resize(name.size() - 4);
        }

        std::string fontSet;
        if (name.empty() || name == "fontconfig") {
            fontSet = "en";
        } else if (name.starts_with("fontconfig_")) {
            fontSet = name.substr(11);
        }
        constexpr std::array<std::string_view, 5> kVanilla = {"en", "pl", "ru", "ja", "cn"};
        if (std::ranges::find(kVanilla, fontSet) == kVanilla.end()) {
            return {FontVerdict::Unknown, {}};
        }
        const bool covered = std::ranges::any_of(fonts, [&](const std::string& f) { return Lowered(f) == fontSet; });
        return {covered ? FontVerdict::Covered : FontVerdict::NotCovered, fontSet};
    }
} // namespace NarrativeEngine::Localization
