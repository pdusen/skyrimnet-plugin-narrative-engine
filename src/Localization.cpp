#include <Localization.h>

#include <logger.h>
#include <Settings.h>
#include <TextKeys.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace NarrativeEngine::Localization
{
    namespace
    {
        constexpr std::string_view kOverlaySuffix = ".custom.ini";

        // Built once at Load and never touched again; published through the
        // atomics below so a reader on another thread sees a complete table.
        std::optional<LanguageTable::Table> g_session;
        std::optional<LanguageTable::Table> g_english;
        FontCheck g_fonts;
        std::atomic<const LanguageTable::Table*> g_sessionPtr{nullptr};
        std::atomic<const LanguageTable::Table*> g_englishPtr{nullptr};

        const LanguageTable::Table& Empty()
        {
            static const LanguageTable::Table empty;
            return empty;
        }

        [[nodiscard]] std::string GameSetting(const char* name)
        {
            auto* collection = RE::INISettingCollection::GetSingleton();
            auto* setting = collection != nullptr ? collection->GetSetting(name) : nullptr;
            if (setting == nullptr || setting->GetType() != RE::Setting::Type::kString) {
                return {};
            }
            const char* value = setting->GetString();
            return value != nullptr ? std::string{value} : std::string{};
        }

        [[nodiscard]] std::optional<std::string> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::nullopt;
            }
            return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        // The stems of the language files present: `german.ini` is `german`;
        // overlays are not languages of their own.
        [[nodiscard]] std::vector<std::string> AvailableStems()
        {
            std::vector<std::string> out;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path{kFolder}, ec)) {
                const auto name = entry.path().filename().string();
                if (entry.path().extension() == ".ini" && !name.ends_with(kOverlaySuffix)) {
                    out.push_back(entry.path().stem().string());
                }
            }
            return out;
        }

        void LogCensus(const LanguageTable::Table& table, std::string_view stem)
        {
            const auto& header = table.GetHeader();
            const auto& census = table.GetCensus();
            logger::info("Localization: {} ({}, '{}', {}): {} keys, {} from its own file, {} from English, {} from "
                         "{}.custom.ini",
                         header.name,
                         header.code,
                         stem,
                         header.status == LanguageTable::Status::Reviewed ? "reviewed" : "draft",
                         census.keys,
                         census.fromShipped,
                         census.fromEnglish,
                         census.fromOverlay,
                         stem);
            for (const auto& line : census.rejected) {
                logger::warn("Localization: rejected: {}", line);
            }
            for (const auto& line : census.fellBack) {
                logger::info("Localization: fell back: {}", line);
            }
            for (const auto& line : census.overlayUnmatched) {
                logger::warn("Localization: {}.custom.ini overrides nothing with {}", stem, line);
            }
            for (const auto& line : census.unavailable) {
                logger::error("Localization: no text at all for {}; it will render empty", line);
            }
        }
    } // namespace

    std::optional<LanguageTable::Table> LoadShipped(std::string_view stem, const std::filesystem::path& folder)
    {
        const auto english = ReadFile(folder / "english.ini");
        if (!english) {
            return std::nullopt;
        }
        std::optional<std::string> shipped;
        if (stem != "english") {
            shipped = ReadFile(folder / (std::string{stem} + ".ini"));
            if (!shipped) {
                return std::nullopt;
            }
        }
        return LanguageTable::Table::Build(
            {*english,
             shipped ? std::optional<std::string_view>{*shipped} : std::optional<std::string_view>{},
             std::nullopt},
            TextKeys::All());
    }

    void Load()
    {
        const auto detected = GameSetting("sLanguage:General");
        const auto fontConfig = GameSetting("sFontConfigFile:Fonts");
        const auto& override = Settings::Get().localizationLanguage;
        const auto available = AvailableStems();
        const auto resolution = Resolve(detected, override, available);
        logger::info("Localization: game language '{}', setting '{}', {} language file(s): using '{}', because {}",
                     detected,
                     override,
                     available.size(),
                     resolution.stem,
                     resolution.reason);

        const std::filesystem::path folder{kFolder};
        const auto english = ReadFile(folder / "english.ini");
        if (!english) {
            logger::error("Localization: {}/english.ini is missing; every text will render empty", kFolder);
        }
        const std::string_view englishText = english ? std::string_view{*english} : std::string_view{};

        std::optional<std::string> shipped;
        if (resolution.stem != "english") {
            shipped = ReadFile(folder / (resolution.stem + ".ini"));
        }
        const auto overlay = ReadFile(folder / (resolution.stem + std::string{kOverlaySuffix}));

        const auto& keys = TextKeys::All();
        auto englishTable = LanguageTable::Table::Build({englishText, std::nullopt, std::nullopt}, keys);
        auto sessionTable =
            LanguageTable::Table::Build({englishText,
                                         shipped ? std::optional<std::string_view>{*shipped} : std::nullopt,
                                         overlay ? std::optional<std::string_view>{*overlay} : std::nullopt},
                                        keys);
        LogCensus(sessionTable, resolution.stem);

        auto fonts = FontsCover(fontConfig, sessionTable.GetHeader().fonts);
        Publish(std::move(sessionTable), std::move(englishTable), std::move(fonts));
        switch (g_fonts.verdict) {
        case FontVerdict::Covered:
            logger::info(
                "Localization: the game's font set '{}' covers {}", g_fonts.fontSet, g_session->GetHeader().name);
            break;
        case FontVerdict::NotCovered: {
            std::string sets;
            for (const auto& set : g_session->GetHeader().fonts) {
                sets += sets.empty() ? set : ", " + set;
            }
            logger::warn("Localization: the game's font set '{}' (sFontConfigFile '{}') does not have the letters {} "
                         "needs, so some will show as blanks. Point sFontConfigFile at one of: {}",
                         g_fonts.fontSet,
                         fontConfig,
                         g_session->GetHeader().name,
                         sets);
            break;
        }
        case FontVerdict::Unknown:
            logger::info(
                "Localization: sFontConfigFile '{}' is not a vanilla font set; cannot tell whether it covers {}",
                fontConfig,
                g_session->GetHeader().name);
            break;
        }
    }

    void Publish(LanguageTable::Table session, LanguageTable::Table english, FontCheck fonts)
    {
        g_english.emplace(std::move(english));
        g_session.emplace(std::move(session));
        g_fonts = std::move(fonts);
        g_englishPtr.store(&*g_english, std::memory_order_release);
        g_sessionPtr.store(&*g_session, std::memory_order_release);
    }

    const LanguageTable::Table& Get()
    {
        const auto* table = g_sessionPtr.load(std::memory_order_acquire);
        return table != nullptr ? *table : Empty();
    }

    const LanguageTable::Table& English()
    {
        const auto* table = g_englishPtr.load(std::memory_order_acquire);
        return table != nullptr ? *table : Empty();
    }

    FontCheck Fonts()
    {
        return g_sessionPtr.load(std::memory_order_acquire) != nullptr ? g_fonts : FontCheck{};
    }
} // namespace NarrativeEngine::Localization
