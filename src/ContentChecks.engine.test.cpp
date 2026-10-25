#include <EngineMock.h>
#include <EngineUtils.h>
#include <LetterComposer.h>
#include <PlotBirth.h>
#include <PlotStepParse.h>

#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

// Characterization of the five checks that read what a model or the game
// wrote, with English's rules: whether a scheme reads as an instruction,
// whether text names the player, whether a record is a ghost, the title a
// letter's fallback label strips, and whether two lines are the same line.
//
// Phase 18 Step 17 moved their word lists into english.ini's [Matching]. These
// cases were pinned first against the checks as C++ wrote them, edge cases
// included, and the move had to leave every one unchanged.

namespace
{
    using NarrativeEngine::PlotBirth::SchemeOpensWithVerb;
    using NarrativeEngine::PlotStepParse::NamesThePlayer;
} // namespace

TEST_CASE("Content checks: a scheme that reads as an instruction", "[ContentChecks][engine]")
{
    SECTION("an instruction passes")
    {
        CHECK(SchemeOpensWithVerb("Steal the amulet from Hulda."));
        CHECK(SchemeOpensWithVerb("steal"));
        CHECK(SchemeOpensWithVerb("Re-forge the blade"));
        CHECK(SchemeOpensWithVerb("Make sure that Hulda is ruined"));
        CHECK(SchemeOpensWithVerb("2 swords for Lars"));
    }

    SECTION("a word that begins like a refused one is not it")
    {
        CHECK(SchemeOpensWithVerb("Heal the Jarl's son"));
        CHECK(SchemeOpensWithVerb("Theft of the amulet"));
        CHECK(SchemeOpensWithVerb("Tolfdir's notes, burned"));
    }

    SECTION("a pronoun, an article or 'to' first fails, in any case")
    {
        CHECK_FALSE(SchemeOpensWithVerb("He steals"));
        CHECK_FALSE(SchemeOpensWithVerb("THE amulet"));
        CHECK_FALSE(SchemeOpensWithVerb("To steal"));
        CHECK_FALSE(SchemeOpensWithVerb("Its keeper sleeps"));
        CHECK_FALSE(SchemeOpensWithVerb("Those who wait"));
    }

    SECTION("'It's' reads as 'its', as letters alone")
    {
        CHECK_FALSE(SchemeOpensWithVerb("It's time to strike"));
    }

    SECTION("a contraction that spells no refused word passes")
    {
        CHECK(SchemeOpensWithVerb("I'll steal it"));
    }

    SECTION("a name followed by what they mean to do fails, however they are separated")
    {
        CHECK_FALSE(SchemeOpensWithVerb("Korir plans to steal"));
        CHECK_FALSE(SchemeOpensWithVerb("Korir is stealing"));
        CHECK_FALSE(SchemeOpensWithVerb("Hulda, plans to strike"));
        CHECK_FALSE(SchemeOpensWithVerb("Brelyna seeks the post"));
    }

    SECTION("a second word that only begins like a modal passes")
    {
        CHECK(SchemeOpensWithVerb("Korir plans-to"));
        CHECK(SchemeOpensWithVerb("Korir's plan unravels"));
    }

    SECTION("text with no words fails")
    {
        CHECK_FALSE(SchemeOpensWithVerb(""));
        CHECK_FALSE(SchemeOpensWithVerb("..."));
    }
}

TEST_CASE("Content checks: text that names the player", "[ContentChecks][engine]")
{
    SECTION("by title: Dragonborn, whole and in any case")
    {
        CHECK(NamesThePlayer("Ruin the Dragonborn."));
        CHECK(NamesThePlayer("the dragonborn's sword"));
        CHECK(NamesThePlayer("_dragonborn"));
        CHECK_FALSE(NamesThePlayer("the Dragonborne"));
        CHECK_FALSE(NamesThePlayer("Dragonborn-blooded"));
        CHECK_FALSE(NamesThePlayer("dragonborns"));
    }

    SECTION("by name: each word of three letters or more, whole and with its case")
    {
        CHECK(NamesThePlayer("Ruin Varian's trade", "Varian Stormblade"));
        CHECK(NamesThePlayer("Shame Stormblade", "Varian Stormblade"));
        CHECK(NamesThePlayer("(Varian) knows", "Varian Stormblade"));
        CHECK_FALSE(NamesThePlayer("the variance", "Varian Stormblade"));
        CHECK_FALSE(NamesThePlayer("ruin varian", "Varian Stormblade"));
        CHECK_FALSE(NamesThePlayer("Varian-led raid", "Varian Stormblade"));
        CHECK_FALSE(NamesThePlayer("Varians", "Varian Stormblade"));
    }

    SECTION("a short word of the name is too common to count")
    {
        CHECK_FALSE(NamesThePlayer("Al went home", "Al Varian"));
        CHECK(NamesThePlayer("Varian went home", "Al Varian"));
    }

    SECTION("a name with an apostrophe in it")
    {
        CHECK(NamesThePlayer("Shame J'zargo's master", "J'zargo"));
        CHECK_FALSE(NamesThePlayer("zargo", "J'zargo"));
    }

    SECTION("no name, no title")
    {
        CHECK_FALSE(NamesThePlayer("Ruin Hulda"));
        CHECK_FALSE(NamesThePlayer("Ruin Hulda", ""));
    }
}

TEST_CASE("Content checks: a record whose name says ghost", "[ContentChecks][engine]")
{
    using NarrativeEngine::EngineUtils::IsGhostRecord;
    NarrativeEngine::Testing::EngineMock engine;

    SECTION("the last word is Ghost, alone or after a hyphen")
    {
        CHECK(IsGhostRecord(engine.AddNPC(0x01u, "Dunmer Ghost")));
        CHECK(IsGhostRecord(engine.AddNPC(0x02u, "Snow-Ghost")));
        CHECK(IsGhostRecord(engine.AddNPC(0x03u, "ghost")));
    }

    SECTION("Ghost elsewhere, or as part of a longer word, is not")
    {
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x11u, "Ghost of Somebody")));
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x12u, "Ghostly Lydia")));
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x13u, "Ghost's Bane")));
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x14u, "Ghostghost")));
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x15u, "Ghosts")));
    }
}

TEST_CASE("Content checks: the title a fallback label strips", "[ContentChecks][engine]")
{
    const auto label = [](const char* name) {
        return NarrativeEngine::LetterComposer::SynthesizeFallbackLabel(NarrativeEngine::Testing::ShippedEnglish(),
                                                                        name);
    };

    SECTION("an epithet after 'the' goes first, then a title, whatever their case")
    {
        CHECK(label("JARL Balgruuf the Greater") == "Note from JARL Balgruuf");
        CHECK(label("HOUSECARL Lydia the Faithful") == "Note from Lydia");
        CHECK(label("Housecarl Lydia THE Faithful") == "Note from Lydia");
    }

    SECTION("a title is stripped only as a whole word")
    {
        CHECK(label("Jarlsberg Ulfricssen-Stormcloak").starts_with("Note from Jarlsberg"));
    }
}

TEST_CASE("Content checks: a scheme already accomplished is the same line", "[ContentChecks][engine]")
{
    using NarrativeEngine::PlotBirth::ParseAmbition;
    using NarrativeEngine::PlotBirth::TextLimits;
    nlohmann::json j;
    j["ambition"] = "Own the Bannered Mare.";
    j["goal"] = "denounce";
    j["finish"] = "denounce Hulda to Balgruuf";
    j["roles"] = {{"mark", "Hulda"}, {"authority", "Balgruuf"}};

    SECTION("whatever its case or closing period")
    {
        j["scheme"] = "expose hulda's skooma trade";
        CHECK_FALSE(ParseAmbition(j.dump(), TextLimits{}, {}, {}, {"Expose Hulda's skooma trade."}).ok);
    }

    SECTION("but not when the words differ")
    {
        j["scheme"] = "Expose Hulda's mead trade";
        CHECK(ParseAmbition(j.dump(), TextLimits{}, {}, {}, {"Expose Hulda's skooma trade."}).ok);
    }
}

// The same checks under other languages' rules, from synthetic language files
// written the way the design describes. Non-ASCII text is written as escapes.
namespace
{
    std::string Header(const char* name, const char* code, const char* rule)
    {
        return std::string{"[Language]\nName = "} + name + "\nNativeName = " + name + "\nCode = " + code
               + "\nStatus = draft\nPluralRule = " + rule + "\nFonts = en, ru, ja\n";
    }

    NarrativeEngine::LanguageTable::Table Language(const std::string& file)
    {
        static const std::string english = NarrativeEngine::Testing::ShippedLanguageFile("english");
        return NarrativeEngine::LanguageTable::Table::Build({english, std::string_view{file}, std::nullopt},
                                                            NarrativeEngine::TextKeys::All());
    }

    const auto& German()
    {
        static const auto table = Language(Header("German", "de", "one_other")
                                           + "[Letter]\nfallback_title = Brief von {sender}\n"
                                             "[Matching]\nScheme.Require.End = zu *, *zu*\n"
                                             "PlayerTitle.Match.Anywhere = drachenblut*\n"
                                             "Ghost.Match.Anywhere = *geist\nName.Suffixes = s\n"
                                             "Title.Strip.Start = Jarl\nTitle.Strip.After = der, die, das\n");
        return table;
    }

    const auto& Russian()
    {
        // "Ulfric" and "Ian" in Cyrillic, with the genitive, dative and
        // instrumental endings.
        static const auto table = Language(Header("Russian", "ru", "russian")
                                           + "[Matching]\nScheme.Disabled = drafted later\n"
                                             "PlayerTitle.Match.Anywhere = \xD0\xB4\xD1\x80\xD0\xB0\xD0\xBA\xD0\xBE"
                                             "\xD0\xBD\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB6\xD0\xB4\xD0\xB5\xD0\xBD*\n"
                                             "Ghost.Disabled = drafted later\n"
                                             "Name.Suffixes = \xD0\xB0, \xD1\x83, \xD0\xBE\xD0\xBC\n");
        return table;
    }

    const auto& Japanese()
    {
        static const auto table = Language(Header("Japanese", "ja", "none") + "[Grammar]\nWordSeparated = false\n"
                                           + "[Matching]\nScheme.Require.End = \xE3\x82\x80, \xE3\x82\x8B\n"
                                             "Scheme.Reject.End = \xE3\x81\xBE\xE3\x81\x99\n"
                                             "PlayerTitle.Match.Anywhere = \xE3\x83\x89\xE3\x83\xA9\xE3\x82\xB4"
                                             "\xE3\x83\xB3\xE3\x83\x9C\xE3\x83\xBC\xE3\x83\xB3\n"
                                             "Ghost.Disabled = drafted later\n");
        return table;
    }
} // namespace

TEST_CASE("Content checks: under other languages' rules", "[ContentChecks][engine]")
{
    using NarrativeEngine::EngineUtils::IsGhostRecord;

    SECTION("a German scheme ends in its verb")
    {
        CHECK(SchemeOpensWithVerb("das Amulett zu stehlen", German()));
        CHECK_FALSE(SchemeOpensWithVerb("Hulda bestiehlt Lars", German()));
        // English's rules are not mixed in: "Das" would be refused by them.
        CHECK(SchemeOpensWithVerb("Das Paket abzuholen", German()));
    }

    SECTION("a Japanese scheme ends in a plain-form verb")
    {
        // "アミュレットを盗む" and "...盗みます".
        const std::string steal = "\xE3\x82\xA2\xE3\x83\x9F\xE3\x83\xA5\xE3\x83\xAC\xE3\x83\x83\xE3\x83\x88"
                                  "\xE3\x82\x92\xE7\x9B\x97";
        CHECK(SchemeOpensWithVerb(steal + "\xE3\x82\x80", Japanese()));
        CHECK_FALSE(SchemeOpensWithVerb(steal + "\xE3\x81\xBF\xE3\x81\xBE\xE3\x81\x99", Japanese()));
    }

    SECTION("a disabled scheme check passes any text with words")
    {
        CHECK(SchemeOpensWithVerb("\xD0\x9E\xD0\xBD \xD1\x83\xD0\xBA\xD1\x80\xD0\xB0\xD0\xBB", Russian()));
        CHECK_FALSE(SchemeOpensWithVerb("...", Russian()));
    }

    SECTION("the player's title, inflected")
    {
        CHECK(NamesThePlayer("das Schwert des Drachenbluts", {}, German()));
        // "драконорожденного": the genitive.
        CHECK(NamesThePlayer("\xD0\xB4\xD1\x80\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB6\xD0"
                             "\xB4\xD0\xB5\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE",
                             {},
                             Russian()));
        // "ドラゴンボーンの剣", unseparated.
        CHECK(NamesThePlayer("\xE3\x83\x89\xE3\x83\xA9\xE3\x82\xB4\xE3\x83\xB3\xE3\x83\x9C\xE3\x83\xBC\xE3\x83\xB3"
                             "\xE3\x81\xAE\xE5\x89\xA3",
                             {},
                             Japanese()));
    }

    SECTION("the player's name with the language's endings")
    {
        CHECK(NamesThePlayer("Varians Plan durchkreuzen", "Varian", German()));
        // "Ульфрика": Ulfric, genitive.
        CHECK(NamesThePlayer("\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA\xD0\xB0",
                             "\xD0\xA3\xD0\xBB\xD1\x8C\xD1\x84\xD1\x80\xD0\xB8\xD0\xBA",
                             Russian()));
        // "リディア" inside "リディアの剣を盗む".
        CHECK(NamesThePlayer("\xE3\x83\xAA\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xA2\xE3\x81\xAE\xE5\x89\xA3\xE3\x82\x92"
                             "\xE7\x9B\x97\xE3\x82\x80",
                             "\xE3\x83\xAA\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xA2",
                             Japanese()));
    }

    SECTION("a two-letter Cyrillic name is too short to count, as two letters are anywhere")
    {
        // "Ян" is four bytes and two letters. Counting bytes, the check used to
        // take it as long enough.
        CHECK_FALSE(NamesThePlayer(
            "\xD0\xAF\xD0\xBD \xD0\xBF\xD1\x80\xD0\xB8\xD1\x88\xD1\x91\xD0\xBB", "\xD0\xAF\xD0\xBD", Russian()));
    }

    SECTION("a German ghost is found compounded")
    {
        NarrativeEngine::Testing::EngineMock engine;
        CHECK(IsGhostRecord(engine.AddNPC(0x21u, "Sturmgeist"), German()));
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x22u, "Dunmer Ghost"), German()));
        CHECK_FALSE(IsGhostRecord(engine.AddNPC(0x23u, "Dunmer Ghost"), Russian()));
    }

    SECTION("a German fallback label strips an epithet after 'der'")
    {
        CHECK(NarrativeEngine::LetterComposer::SynthesizeFallbackLabel(German(),
                                                                       "Jarl Balgruuf der Gro\xC3\x9F"
                                                                       "e")
              == "Brief von Jarl Balgruuf");
    }

    SECTION("two lines that differ only in the case of an accented letter are the same line")
    {
        using NarrativeEngine::PlotBirth::ParseAmbition;
        using NarrativeEngine::PlotBirth::TextLimits;
        nlohmann::json j;
        j["ambition"] = "Own the Bannered Mare.";
        j["goal"] = "denounce";
        j["finish"] = "denounce Hulda to Balgruuf";
        j["roles"] = {{"mark", "Hulda"}, {"authority", "Balgruuf"}};
        j["scheme"] = "ruin \xC3\x96lfr\xC3\xBAn's trade";
        CHECK_FALSE(ParseAmbition(j.dump(), TextLimits{}, {}, {}, {"Ruin \xC3\xB6lfr\xC3\xBAn's trade."}).ok);
    }
}
