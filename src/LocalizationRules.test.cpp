#include <Localization.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// Unit tests for the two rules Localization applies at startup: which
// language file to load, and whether the game's fonts can show it.

namespace
{
    using namespace NarrativeEngine::Localization;

    const std::vector<std::string> kShipped =
        {"english", "french", "german", "italian", "spanish", "polish", "russian", "japanese", "chinese"};
} // namespace

TEST_CASE("Localization::Resolve", "[Localization]")
{
    SECTION("every token the SE launcher writes, in any case")
    {
        for (const auto& token : kShipped) {
            std::string upper = token;
            for (auto& c : upper) {
                c = static_cast<char>(c - 'a' + 'A');
            }
            CHECK(Resolve(upper, "", kShipped).stem == token);
            CHECK(Resolve(token, "", kShipped).stem == token);
        }
        CHECK(Resolve("Polish", "", kShipped).stem == "polish");
    }
    SECTION("the MCM's stored option names a language, or none")
    {
        CHECK(Resolve("ENGLISH", "$NE_Language_german", kShipped).stem == "german");
        CHECK(Resolve("ENGLISH", "$ne_language_GERMAN", kShipped).stem == "german");
        CHECK(Resolve("RUSSIAN", "$NE_Language_Auto", kShipped).stem == "russian");
        const auto auto_ = Resolve("RUSSIAN", "$NE_Language_Auto", kShipped);
        // Auto is no setting at all, so nothing was skipped.
        CHECK_FALSE(auto_.reason.contains("no file"));
        CHECK(Resolve("RUSSIAN", "$NE_Language_klingon", kShipped).stem == "russian");
    }
    SECTION("the setting wins when it names a file, whatever the game says")
    {
        CHECK(Resolve("ENGLISH", "german", kShipped).stem == "german");
        CHECK(Resolve("RUSSIAN", " Japanese ", kShipped).stem == "japanese");
    }
    SECTION("the setting can name a language Skyrim does not ship")
    {
        auto available = kShipped;
        available.push_back("portuguese");
        const auto resolution = Resolve("ENGLISH", "portuguese", available);
        CHECK(resolution.stem == "portuguese");
        CHECK(resolution.reason.contains("setting"));
    }
    SECTION("a setting naming a missing file falls through to the game's language, and says so")
    {
        const auto resolution = Resolve("GERMAN", "klingon", kShipped);
        CHECK(resolution.stem == "german");
        CHECK(resolution.reason.contains("klingon"));
    }
    SECTION("a game language with no file, or none at all, is English")
    {
        CHECK(Resolve("CZECH", "", kShipped).stem == "english");
        CHECK(Resolve("", "", kShipped).stem == "english");
        CHECK(Resolve("GERMAN", "", std::vector<std::string>{"english"}).stem == "english");
    }
    SECTION("the override's case and the files' case do not matter")
    {
        CHECK(Resolve("ENGLISH", "GERMAN", std::vector<std::string>{"English", "German"}).stem == "german");
    }
}

TEST_CASE("Localization::FontsCover", "[Localization]")
{
    const std::vector<std::string> polish = {"pl", "ru"};
    const std::vector<std::string> german = {"en"};

    SECTION("the five vanilla font configurations")
    {
        CHECK(FontsCover("Interface\\fontconfig.txt", german).fontSet == "en");
        CHECK(FontsCover("Interface\\FontConfig_pl.txt", polish).fontSet == "pl");
        CHECK(FontsCover("Interface\\fontconfig_ru.txt", polish).fontSet == "ru");
        CHECK(FontsCover("Interface\\fontconfig_ja.txt", german).fontSet == "ja");
        CHECK(FontsCover("Interface\\fontconfig_cn.txt", german).fontSet == "cn");
    }
    SECTION("an empty value is the default set, which is en")
    {
        CHECK(FontsCover("", german).verdict == FontVerdict::Covered);
        CHECK(FontsCover("", polish).verdict == FontVerdict::NotCovered);
    }
    SECTION("covered when the language names the active set")
    {
        CHECK(FontsCover("Interface\\FontConfig_pl.txt", polish).verdict == FontVerdict::Covered);
        CHECK(FontsCover("Interface/fontconfig_ru.txt", polish).verdict == FontVerdict::Covered);
    }
    SECTION("Polish on an English install is the case worth warning about")
    {
        const auto check = FontsCover("Interface\\fontconfig.txt", polish);
        CHECK(check.verdict == FontVerdict::NotCovered);
        CHECK(check.fontSet == "en");
    }
    SECTION("a font mod's own configuration cannot be judged")
    {
        const auto check = FontsCover("Interface\\fontconfig_custom.txt", german);
        CHECK(check.verdict == FontVerdict::Unknown);
        CHECK(check.fontSet.empty());
    }
}
