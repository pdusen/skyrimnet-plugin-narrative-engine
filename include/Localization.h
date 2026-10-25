#pragma once

#include <LanguageTable.h>

#include <span>
#include <string>
#include <string_view>

// Which language this session plays in, and its table.
//
// The language is the game's own (`sLanguage` in Skyrim.ini) unless the
// `[Localization] sLanguage` setting names a language file, and it is chosen
// once, at kDataLoaded. Nothing changes it afterwards: history written in one
// language stays in that language anyway, so switching mid-session would
// only split one session's text between two.
//
// Resolve and FontsCover are the rules, pure and unit-tested. Load and the
// accessors are the engine glue around them.
namespace NarrativeEngine::Localization
{
    struct Resolution
    {
        // The language file to load: a lower-cased stem such as "german".
        std::string stem;
        // Why, for the log.
        std::string reason;
    };

    // The override when it names an available file, else the game's
    // language when that has one, else English. `available` is the stems of
    // the language files present, overlays excluded; comparisons ignore case.
    [[nodiscard]] Resolution Resolve(std::string_view detected,
                                     std::string_view override,
                                     std::span<const std::string> available);

    enum class FontVerdict
    {
        // The active font set is one the language file names.
        Covered,
        // It is a vanilla set the language file does not name: letters will
        // render as blanks.
        NotCovered,
        // `sFontConfigFile` names no vanilla font set (a font mod, usually),
        // so nothing can be said.
        Unknown,
    };

    struct FontCheck
    {
        FontVerdict verdict = FontVerdict::Unknown;
        // The vanilla set in use, "en" "pl" "ru" "ja" or "cn"; empty when
        // the verdict is Unknown.
        std::string fontSet;
    };

    // `fontConfigFile` is the raw `sFontConfigFile:Fonts` value
    // (`Interface\FontConfig_pl.txt`); empty means the default set, which is
    // `en`. `fonts` is the language header's list.
    [[nodiscard]] FontCheck FontsCover(std::string_view fontConfigFile, std::span<const std::string> fonts);

    // Choose the language, read its files, build and publish its table, and
    // log what happened. Called once from kDataLoaded, on the main thread,
    // after Settings::Load and before anything renders text.
    void Load();

    // Make `session` and `english` the tables every reader sees. Load does
    // this once, from the files it chose. The mocked-engine tests do it at
    // start-up with the shipped English, so production code under test
    // renders real text without a game; nothing else should.
    void Publish(LanguageTable::Table session, LanguageTable::Table english, FontCheck fonts = {});

    // The session's table. Before Load it is empty, and every key renders
    // empty. Immutable once published, so any thread may read it without a
    // lock.
    [[nodiscard]] const LanguageTable::Table& Get();

    // English alone, with no overlay: for the few outputs that stay English
    // whatever the session's language, such as our own log files.
    [[nodiscard]] const LanguageTable::Table& English();

    // What `sFontConfigFile` said at Load, judged against the session's
    // language, for the dashboard to report.
    [[nodiscard]] FontCheck Fonts();
} // namespace NarrativeEngine::Localization
