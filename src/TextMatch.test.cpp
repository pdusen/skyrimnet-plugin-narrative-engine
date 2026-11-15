#include <LanguageTable.h>
#include <TextMatch.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>

// The [Matching] vocabulary, against the examples the design gives for it:
// the English and German scheme checks, the Japanese one written without
// spaces, the ghost tests of three languages, and the player's German title in
// an inflected form. Non-ASCII text is written as escapes so the file stays
// ASCII.

namespace
{
    using NarrativeEngine::TextMatch::Options;
    using NarrativeEngine::TextMatch::Rules;

    constexpr Options kWords{};
    constexpr Options kCharacters{false};

    Rules With(std::initializer_list<std::pair<const char*, const char*>> keys, Options options = kWords)
    {
        Rules rules;
        rules.options = options;
        for (const auto& [key, value] : keys) {
            rules.Set(key, value);
        }
        return rules;
    }

    // The English list the design quotes. Today's SchemeOpensWithVerb has
    // more words; Step 17 carries its exact list over.
    constexpr const char* kEnglishScheme = "he, she, they, it, i, we, you, his, her, the, a, an, this, that, to, * is, "
                                           "* plans, * wants";
} // namespace

TEST_CASE("TextMatch: the English scheme check", "[TextMatch]")
{
    const auto rules = With({{"Scheme.Reject.Start", kEnglishScheme}});
    const auto* scheme = rules.Find("Scheme");
    REQUIRE(scheme != nullptr);

    SECTION("an instruction passes")
    {
        CHECK(scheme->Passes("Steal the amulet from Hulda.", kWords));
        CHECK(scheme->Passes("plant evidence among Belethor's goods", kWords));
    }

    SECTION("a pronoun, an article or 'to' first fails, whatever its case")
    {
        CHECK_FALSE(scheme->Passes("He steals the amulet.", kWords));
        CHECK_FALSE(scheme->Passes("THE amulet must be stolen", kWords));
        CHECK_FALSE(scheme->Passes("To steal the amulet", kWords));
    }

    SECTION("a name followed by what they mean to do fails, by the second-word patterns")
    {
        CHECK_FALSE(scheme->Passes("Korir plans to steal the amulet", kWords));
        CHECK_FALSE(scheme->Passes("Korir is stealing the amulet", kWords));
    }

    SECTION("a pattern's word is a whole word, not the start of one")
    {
        // "Heal" opens with "he", and "Theft" with "the".
        CHECK(scheme->Passes("Heal the Jarl's son", kWords));
        CHECK(scheme->Passes("Theft of the amulet", kWords));
    }
}

TEST_CASE("TextMatch: the German scheme check", "[TextMatch]")
{
    const auto rules = With({{"Scheme.Require.End", "zu *, *zu*"}});
    const auto* scheme = rules.Find("Scheme");
    REQUIRE(scheme != nullptr);

    SECTION("a zu-clause passes, by 'zu *'")
    {
        CHECK(scheme->Passes("das Amulett zu stehlen", kWords));
    }

    SECTION("zu inside a separable verb passes, by '*zu*'")
    {
        CHECK(scheme->Passes("das Paket abzuholen", kWords));
    }

    SECTION("a sentence with no zu at its end fails")
    {
        CHECK_FALSE(scheme->Passes("Hulda bestiehlt Lars", kWords));
    }

    SECTION("'das Geld zur\\xC3\\xBCck' passes too: the false positive the design admits")
    {
        CHECK(scheme->Passes("das Geld zur\xC3\xBC"
                             "ck",
                             kWords));
    }
}

TEST_CASE("TextMatch: the Japanese scheme check, written without spaces", "[TextMatch]")
{
    // Require.End: the plain-form endings; Reject.End: the polite and past
    // forms; Reject.Start: "he/she/they/I" with the topic particle.
    const auto rules =
        With({{"Scheme.Require.End",
               "\xE3\x81\x86, \xE3\x81\x8F, \xE3\x81\x90, \xE3\x81\x99, \xE3\x81\xA4, \xE3\x81\xAC, \xE3\x81\xB6, "
               "\xE3\x82\x80, \xE3\x82\x8B"},
              {"Scheme.Reject.End", "\xE3\x81\xBE\xE3\x81\x99, \xE3\x81\xA7\xE3\x81\x99, \xE3\x81\x9F, \xE3\x81\xA0"},
              {"Scheme.Reject.Start",
               "\xE5\xBD\xBC\xE3\x81\xAF, \xE5\xBD\xBC\xE5\xA5\xB3\xE3\x81\xAF, \xE5\xBD\xBC\xE3\x82\x89\xE3\x81\xAF, "
               "\xE7\xA7\x81\xE3\x81\xAF"}},
             kCharacters);
    const auto* scheme = rules.Find("Scheme");
    REQUIRE(scheme != nullptr);
    // "アミュレットを盗む": steal the amulet, plain form.
    const std::string steal = "\xE3\x82\xA2\xE3\x83\x9F\xE3\x83\xA5\xE3\x83\xAC\xE3\x83\x83\xE3\x83\x88\xE3\x82\x92"
                              "\xE7\x9B\x97\xE3\x82\x80";

    SECTION("a plain-form verb phrase passes, a closing full stop aside")
    {
        CHECK(scheme->Passes(steal, kCharacters));
        CHECK(scheme->Passes(steal + "\xE3\x80\x82", kCharacters));
    }

    SECTION("the polite form fails, though it ends in an allowed kana")
    {
        // "...盗みます"
        CHECK_FALSE(
            scheme->Passes(steal.substr(0, steal.size() - 3) + "\xE3\x81\xBF\xE3\x81\xBE\xE3\x81\x99", kCharacters));
    }

    SECTION("the past form fails")
    {
        // "...盗んだ"
        CHECK_FALSE(scheme->Passes(steal.substr(0, steal.size() - 3) + "\xE3\x82\x93\xE3\x81\xA0", kCharacters));
    }

    SECTION("a sentence about someone fails")
    {
        // "彼は..."
        CHECK_FALSE(scheme->Passes("\xE5\xBD\xBC\xE3\x81\xAF" + steal, kCharacters));
    }
}

TEST_CASE("TextMatch: the ghost test in three languages", "[TextMatch]")
{
    SECTION("English looks at the last word")
    {
        const auto rules = With({{"Ghost.Match.End", "ghost"}});
        const auto* ghost = rules.Find("Ghost");
        REQUIRE(ghost != nullptr);
        CHECK(ghost->Fires("Dunmer Ghost", kWords));
        CHECK_FALSE(ghost->Fires("Ghost of Somebody", kWords));
        CHECK_FALSE(ghost->Fires("Ghostly Lydia", kWords));
    }

    SECTION("French looks at the first")
    {
        const auto rules = With({{"Ghost.Match.Start", "fant\xC3\xB4me, spectre"}});
        const auto* ghost = rules.Find("Ghost");
        REQUIRE(ghost != nullptr);
        CHECK(ghost->Fires("Fant\xC3\xB4me dunmer", kWords));
        CHECK(ghost->Fires("Spectre de Lydia", kWords));
        CHECK_FALSE(ghost->Fires("Lydia", kWords));
    }

    SECTION("German finds it anywhere, compounded or not")
    {
        const auto rules = With({{"Ghost.Match.Anywhere", "*geist"}});
        const auto* ghost = rules.Find("Ghost");
        REQUIRE(ghost != nullptr);
        CHECK(ghost->Fires("Dunmer-Sturmgeist", kWords));
        CHECK(ghost->Fires("Geist der Hulda", kWords));
        CHECK_FALSE(ghost->Fires("Geistliche Hulda", kWords));
    }
}

TEST_CASE("TextMatch: a stem pattern covers an inflection", "[TextMatch]")
{
    const auto rules = With({{"PlayerTitle.Match.Anywhere", "drachenblut*"}});
    const auto* title = rules.Find("PlayerTitle");
    REQUIRE(title != nullptr);
    CHECK(title->Fires("das Schwert des Drachenbluts", kWords));
    CHECK(title->Fires("Drachenblut", kWords));
    CHECK_FALSE(title->Fires("ein Drache", kWords));
}

TEST_CASE("TextMatch: a disabled check, and plain lists", "[TextMatch]")
{
    const auto rules =
        With({{"Scheme.Disabled", "no list says where this language puts its verb"}, {"Name.Suffixes", "'s, s"}});

    SECTION("a disabled check passes everything and fires on nothing")
    {
        const auto* scheme = rules.Find("Scheme");
        REQUIRE(scheme != nullptr);
        CHECK(scheme->Defined());
        CHECK(scheme->Passes("He is nobody's instruction", kWords));
        CHECK_FALSE(scheme->Fires("anything", kWords));
    }

    SECTION("a plain list reads as its items")
    {
        CHECK(rules.List("Name.Suffixes") == std::vector<std::string>{"'s", "s"});
        CHECK(rules.List("Title.Strip.Start").empty());
        CHECK(rules.Find("Ghost") == nullptr);
    }
}

TEST_CASE("TextMatch: a language table's [Matching]", "[TextMatch]")
{
    using NarrativeEngine::LanguageTable::Sources;
    using NarrativeEngine::LanguageTable::Table;

    constexpr const char* kEnglish = "[Language]\nName = English\nNativeName = English\nCode = en\nStatus = reviewed\n"
                                     "PluralRule = one_other\nFonts = en\n"
                                     "[Matching]\nScheme.Reject.Start = he, the, * is\nGhost.Match.End = ghost\n"
                                     "Title.Strip.After = the\n";
    constexpr const char* kGerman = "[Language]\nName = German\nNativeName = Deutsch\nCode = de\nStatus = draft\n"
                                    "PluralRule = one_other\nFonts = en\n"
                                    "[Matching]\nScheme.Require.End = zu *, *zu*\nGhost.Disabled = not yet drafted\n";
    constexpr const char* kJapanese = "[Language]\nName = Japanese\nNativeName = Nihongo\nCode = ja\n"
                                      "Status = draft\nPluralRule = none\nFonts = ja\n"
                                      "[Grammar]\nWordSeparated = false\n"
                                      "[Matching]\nScheme.Require.End = \xE3\x82\x80, \xE3\x82\x8B\n";

    SECTION("a check the language defines is its own, with none of English's rules mixed in")
    {
        const auto table = Table::Build(Sources{kEnglish, std::string_view{kGerman}, std::nullopt}, {});
        const auto* scheme = table.Matching().Find("Scheme");
        REQUIRE(scheme != nullptr);
        CHECK(scheme->rejectStart.empty());
        CHECK(scheme->Passes("das Amulett zu stehlen", table.Matching().options));
    }

    SECTION("a check the language says nothing of is English's")
    {
        const auto table = Table::Build(Sources{kEnglish, std::string_view{kGerman}, std::nullopt}, {});
        CHECK(table.Matching().List("Title.Strip.After") == std::vector<std::string>{"the"});
    }

    SECTION("a disabled check is named in the census, with its reason")
    {
        const auto table = Table::Build(Sources{kEnglish, std::string_view{kGerman}, std::nullopt}, {});
        const auto& disabled = table.GetCensus().disabled;
        CHECK(std::ranges::count(disabled, std::string{"Ghost: not yet drafted"}) == 1);
        CHECK(table.Matching().Find("Ghost")->Fires("Dunmer Ghost", table.Matching().options) == false);
    }

    SECTION("an overlay changes a check's rules without bringing English's back")
    {
        const auto table = Table::Build(
            Sources{kEnglish, std::string_view{kGerman}, std::string_view{"[Matching]\nScheme.Reject.Start = es\n"}},
            {});
        const auto* scheme = table.Matching().Find("Scheme");
        REQUIRE(scheme != nullptr);
        CHECK(scheme->rejectStart.size() == 1);
        CHECK(scheme->requireEnd.size() == 2);
        CHECK_FALSE(scheme->Passes("es ist zu stehlen", table.Matching().options));
    }

    SECTION("a language written without spaces matches by characters")
    {
        const auto table = Table::Build(Sources{kEnglish, std::string_view{kJapanese}, std::nullopt}, {});
        CHECK_FALSE(table.Matching().options.wordSeparated);
        CHECK(table.Grammar().wordSeparated == false);
        CHECK(table.Matching().Find("Scheme")->Passes("\xE7\x9B\x97\xE3\x82\x80", table.Matching().options));
    }

    SECTION("[Matching] keys are never reported as unknown")
    {
        const auto table = Table::Build(Sources{kEnglish, std::string_view{kGerman}, std::nullopt}, {});
        for (const auto& line : table.GetCensus().rejected) {
            INFO(line);
            CHECK(line.find("Matching") == std::string::npos);
        }
    }
}
