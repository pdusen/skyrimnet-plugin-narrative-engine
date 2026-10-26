#include <TextTemplate.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The formatter against testsupport/fixtures/text_template.json, which the
// dashboard's TypeScript port (dashboard/src/i18n/textTemplate.ts) runs too,
// so that the two cannot drift. Its plural categories are CLDR's, so this also
// checks the hand-written families against CLDR.

namespace
{
    using namespace NarrativeEngine::TextTemplate;

    const nlohmann::json& Fixture()
    {
        static const nlohmann::json fixture = [] {
            std::ifstream in(std::filesystem::path{NARRATIVEENGINE_FIXTURES_DIR} / "text_template.json",
                             std::ios::binary);
            return nlohmann::json::parse(in);
        }();
        return fixture;
    }

    std::optional<PluralRule> Family(std::string_view name)
    {
        static const std::map<std::string, PluralRule, std::less<>> families = {
            {"none", PluralRule::None},
            {"one_other", PluralRule::OneOther},
            {"spanish", PluralRule::Spanish},
            {"french", PluralRule::French},
            {"russian", PluralRule::Russian},
            {"polish", PluralRule::Polish},
        };
        const auto it = families.find(name);
        return it != families.end() ? std::optional{it->second} : std::nullopt;
    }

    std::string_view CategoryName(PluralCategory category)
    {
        switch (category) {
        case PluralCategory::Zero:
            return "zero";
        case PluralCategory::One:
            return "one";
        case PluralCategory::Two:
            return "two";
        case PluralCategory::Few:
            return "few";
        case PluralCategory::Many:
            return "many";
        case PluralCategory::Other:
            break;
        }
        return "other";
    }

    PluralRule FamilyOf(std::string_view language)
    {
        for (const auto& entry : Fixture()["plurals"]) {
            if (entry["language"].get<std::string>() == language) {
                return Family(entry["family"].get<std::string>()).value_or(PluralRule::OneOther);
            }
        }
        FAIL("no plural entry for " << language);
        return PluralRule::OneOther;
    }

    Value ToValue(const nlohmann::json& value)
    {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_number_integer()) {
            return value.get<std::int64_t>();
        }
        if (value.is_array()) {
            return value.get<List>();
        }
        const auto sex = value["sex"].get<std::string>();
        return Person{value["name"].get<std::string>(),
                      sex == "female" ? Sex::Female
                      : sex == "male" ? Sex::Male
                                      : Sex::Other};
    }

    void ApplyGrammar(Grammar& grammar, const nlohmann::json& overrides)
    {
        for (const auto& [key, value] : overrides.items()) {
            if (key == "listSeparator") {
                grammar.listSeparator = value.get<std::string>();
            } else if (key == "listPairSeparator") {
                grammar.listPairSeparator = value.get<std::string>();
            } else if (key == "listFinalSeparator") {
                grammar.listFinalSeparator = value.get<std::string>();
            } else if (key == "capitalizeSentenceStart") {
                grammar.capitalizeSentenceStart = value.get<bool>();
            } else if (key == "phraseTrailingPunctuation") {
                grammar.phraseTrailingPunctuation = value.get<std::string>();
            } else if (key == "phraseLowercaseFirst") {
                grammar.phraseLowercaseFirst = value.get<bool>();
            } else if (key == "elideBeforeVowel") {
                grammar.elideBeforeVowel = value.get<std::vector<std::string>>();
            } else {
                FAIL("the fixture sets a grammar field the formatter does not have: " << key);
            }
        }
    }
} // namespace

TEST_CASE("TextTemplate fixture: every family agrees with CLDR", "[TextTemplate][fixture]")
{
    REQUIRE(Fixture()["plurals"].size() >= 6);
    for (const auto& entry : Fixture()["plurals"]) {
        const auto language = entry["language"].get<std::string>();
        const auto family = Family(entry["family"].get<std::string>());
        INFO(language);
        REQUIRE(family.has_value());
        for (const auto& [number, category] : entry["categories"].items()) {
            INFO(number);
            CHECK(CategoryName(Plural(*family, std::stoll(number))) == category.get<std::string>());
        }
    }
}

TEST_CASE("TextTemplate fixture: renders", "[TextTemplate][fixture]")
{
    REQUIRE(!Fixture()["renders"].empty());
    for (const auto& c : Fixture()["renders"]) {
        const auto name = c["name"].get<std::string>();
        INFO(name);
        const auto parsed = Parse(c["template"].get<std::string>());
        REQUIRE(parsed.Ok());

        Arguments arguments;
        for (const auto& [key, value] : c.value("arguments", nlohmann::json::object()).items()) {
            arguments.Set(key, ToValue(value));
        }
        RenderOptions options;
        options.grammar.pluralRule = FamilyOf(c.value("language", std::string{"en"}));
        ApplyGrammar(options.grammar, c.value("grammar", nlohmann::json::object()));
        options.sentence = c.value("sentence", false);
        const auto missing = c.value("missing", std::map<std::string, std::string>{});
        options.missing = [missing](std::string_view key) {
            const auto it = missing.find(std::string{key});
            return it != missing.end() ? it->second : std::string{};
        };
        CHECK(Render(parsed.message, arguments, options) == c["expected"].get<std::string>());
    }
}

TEST_CASE("TextTemplate fixture: rejects", "[TextTemplate][fixture]")
{
    REQUIRE(!Fixture()["rejects"].empty());
    for (const auto& c : Fixture()["rejects"]) {
        INFO(c["name"].get<std::string>());
        CHECK_FALSE(Parse(c["template"].get<std::string>()).Ok());
    }
}
