#include <LanguageTable.h>
#include <TextKeys.h>

#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

// Every language file statics/ ships, checked against every key the plugin
// declares, so a broken file fails the build's tests rather than a player's
// session. Lives with the engine tests because the declared keys include ones
// generated from engine-coupled tables.

TEST_CASE("Every shipped language file", "[LanguageFiles][engine]")
{
    using namespace NarrativeEngine;
    const auto folder = std::filesystem::path{NARRATIVEENGINE_STATICS_DIR} / "SKSE/Plugins/NarrativeEngine/Languages";
    REQUIRE(std::filesystem::exists(folder / "english.ini"));
    const auto english = Testing::ShippedLanguageFile("english");
    const auto& keys = TextKeys::All();

    SECTION("English defines every declared key, and rejects nothing")
    {
        const auto& table = Testing::ShippedEnglish();
        const auto& census = table.GetCensus();
        for (const auto& line : census.unavailable) {
            UNSCOPED_INFO("unavailable: " << line);
        }
        for (const auto& line : census.rejected) {
            UNSCOPED_INFO("rejected: " << line);
        }
        CHECK(census.unavailable.empty());
        CHECK(census.rejected.empty());
        // The declared keys, and [Dashboard]'s, which English declares itself.
        CHECK_FALSE(table.Strings().empty());
        CHECK(census.keys == keys.size() + table.Strings().size());
        CHECK(table.GetHeader().code == "en");
    }
    SECTION("every other language loads with no rejections")
    {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            const auto name = entry.path().filename().string();
            if (entry.path().extension() != ".ini" || name == "english.ini" || name.ends_with(".custom.ini")) {
                continue;
            }
            const auto shipped = Testing::ShippedLanguageFile(entry.path().stem().string());
            const auto table = LanguageTable::Table::Build({english, std::string_view{shipped}, std::nullopt}, keys);
            INFO(name);
            for (const auto& line : table.GetCensus().rejected) {
                UNSCOPED_INFO("rejected: " << line);
            }
            CHECK(table.GetCensus().rejected.empty());
            CHECK_FALSE(table.GetHeader().code.empty());
        }
    }
    SECTION("every language says something of every content check English has: rules, or why it has none")
    {
        // A check another language leaves out would quietly run on
        // English's rules against that language's text. Saying why it is off
        // (`<Check>.Disabled = <reason>`) is the only way to have no rules.
        const auto& checks = Testing::ShippedEnglish().Matching().checks;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            const auto name = entry.path().filename().string();
            if (entry.path().extension() != ".ini" || name.ends_with(".custom.ini")) {
                continue;
            }
            const auto shipped = Testing::ShippedLanguageFile(entry.path().stem().string());
            // Built from the file alone, so nothing it lacks is filled in.
            const auto own = LanguageTable::Table::Build({shipped, std::nullopt, std::nullopt}, {});
            for (const auto& [check, rules] : checks) {
                INFO(name << ": " << check);
                const auto* theirs = own.Matching().Find(check);
                CHECK((theirs != nullptr && theirs->Defined()));
            }
        }
    }
}
