#include <ItemGrammar.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Unit tests for fitting a made thing's name to its slot. The rules here are
// small ones written for each case; the shipped languages' rules, checked
// against every name the game lets a craftsman make, are exercised through
// PlotText in PlotRecipes.engine.test.cpp. Non-ASCII text is written as
// escapes, with the glyphs in a comment.

namespace
{
    using namespace NarrativeEngine::ItemGrammar;

    // Стеклянная булава летаргии, "Glass Mace of Lethargy".
    constexpr std::string_view kGlassMace =
        "\xD0\xA1\xD1\x82\xD0\xB5\xD0\xBA\xD0\xBB\xD1\x8F\xD0\xBD\xD0\xBD\xD0\xB0\xD1\x8F "
        "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xB0 "
        "\xD0\xBB\xD0\xB5\xD1\x82\xD0\xB0\xD1\x80\xD0\xB3\xD0\xB8\xD0\xB8";
    // Стеклянную булаву летаргии: the accusative.
    constexpr std::string_view kGlassMaceAccusative =
        "\xD0\xA1\xD1\x82\xD0\xB5\xD0\xBA\xD0\xBB\xD1\x8F\xD0\xBD\xD0\xBD\xD1\x83\xD1\x8E "
        "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xB0\xD0\xB2\xD1\x83 "
        "\xD0\xBB\xD0\xB5\xD1\x82\xD0\xB0\xD1\x80\xD0\xB3\xD0\xB8\xD0\xB8";
    // Яд нетча, "Netch Poison": a masculine head and a genitive ending in -а.
    constexpr std::string_view kNetchPoison = "\xD0\xAF\xD0\xB4 \xD0\xBD\xD0\xB5\xD1\x82\xD1\x87\xD0\xB0";
    // *ая > *ую, *а > *у: Russian's feminine accusative.
    constexpr std::string_view kRussianModifier = "*\xD0\xB0\xD1\x8F > *\xD1\x83\xD1\x8E";
    constexpr std::string_view kRussianHead = "*\xD0\xB0 > *\xD1\x83";

    std::vector<Rewrite> Rewrites(std::string_view value)
    {
        auto parsed = ParseRewrites(value);
        REQUIRE(parsed.has_value());
        return std::move(*parsed);
    }

    void AddClass(Rules& rules, std::string_view value, const std::string& label)
    {
        const auto patterns = ParsePatterns(value);
        REQUIRE(patterns.has_value());
        for (const auto& pattern : *patterns) {
            rules.classes.emplace_back(pattern, label);
        }
    }

    Rules German()
    {
        Rules rules;
        AddClass(rules, "*bogen, *dolch, *pfeil", "masculine");
        AddClass(rules, "*schwert", "neuter");
        AddClass(rules, "*stiefel", "plural");
        rules.caseModifier = Rewrites("*ischer > *ischen, *ter > *ten");
        rules.plural = Rewrites("*pfeil > *pfeile");
        return rules;
    }

    Rules Russian()
    {
        Rules rules;
        rules.caseModifier = Rewrites(kRussianModifier);
        rules.caseHead = Rewrites(kRussianHead);
        return rules;
    }
} // namespace

TEST_CASE("ItemGrammar parsing", "[ItemGrammar]")
{
    SECTION("should read words and endings")
    {
        const auto patterns = ParsePatterns("*axt, hacha");
        REQUIRE(patterns.has_value());
        REQUIRE(patterns->size() == 2);
        REQUIRE((*patterns)[0].ending);
        REQUIRE((*patterns)[0].folded == "axt");
        REQUIRE_FALSE((*patterns)[1].ending);
    }

    SECTION("should refuse an entry that is not a word or an ending")
    {
        REQUIRE_FALSE(ParsePatterns("*").has_value());
        REQUIRE_FALSE(ParsePatterns("a*b").has_value());
        REQUIRE_FALSE(ParsePatterns("*a*").has_value());
    }

    SECTION("should read rewrites with a star on both sides or neither")
    {
        REQUIRE(ParseRewrites("*a > *u, trucizna > trucizne").has_value());
        REQUIRE(ParseRewrites("*bolzen > *bolzen").has_value());
    }

    SECTION("should refuse a rewrite whose sides disagree or that is not a pair")
    {
        REQUIRE_FALSE(ParseRewrites("*a > u").has_value());
        REQUIRE_FALSE(ParseRewrites("a > *u").has_value());
        REQUIRE_FALSE(ParseRewrites("a").has_value());
        REQUIRE_FALSE(ParseRewrites("a > b > c").has_value());
    }
}

TEST_CASE("ItemGrammar fitting", "[ItemGrammar]")
{
    SECTION("with Item.Gender lists")
    {
        SECTION("should take the class of the first word a list knows, past an adjective")
        {
            REQUIRE(Fit(German(), "Daedrischer Bogen der Angst", false).gender == "masculine");
            REQUIRE(Fit(German(), "Eisenschwert", false).gender == "neuter");
            REQUIRE(Fit(German(), "Alte Nordstiefel", false).gender == "plural");
        }

        SECTION("should match an ending against the whole word as well")
        {
            REQUIRE(Fit(German(), "Bogen", false).gender == "masculine");
        }

        SECTION("should leave a name no list knows as it is, its case rules unread")
        {
            const auto fitted = Fit(German(), "Daedrischer Zauberstab", false);
            REQUIRE(fitted.name == "Daedrischer Zauberstab");
            REQUIRE(fitted.gender == "other");
        }

        SECTION("should decline the adjectives before the head and stop there")
        {
            REQUIRE(Fit(German(), "Daedrischer Bogen der Angst", false).name == "Daedrischen Bogen der Angst");
            REQUIRE(Fit(German(), "Gewetzter Alter Dolch", false).name == "Gewetzten Alten Dolch");
        }
    }

    SECTION("without Item.Gender lists")
    {
        SECTION("should apply the case rules, as other")
        {
            const auto fitted = Fit(Russian(), kGlassMace, false);
            REQUIRE(fitted.name == kGlassMaceAccusative);
            REQUIRE(fitted.gender == "other");
        }

        SECTION("should stop at a first word no rule matches, leaving a genitive after it alone")
        {
            REQUIRE(Fit(Russian(), kNetchPoison, false).name == kNetchPoison);
        }
    }

    SECTION("when a word matches both a modifier and a head rule")
    {
        Rules rules;
        rules.caseModifier = Rewrites("*na > *nu");
        rules.caseHead = Rewrites("*a > *e, trucizna > truciznx");

        SECTION("should take the longer ending, and read on after a modifier")
        {
            REQUIRE(Fit(rules, "Szklana bulawa", false).name == "Szklanu bulawe");
        }

        SECTION("should take a whole word over any ending")
        {
            REQUIRE(Fit(rules, "Mocna trucizna", false).name == "Mocnu truciznx");
        }
    }

    SECTION("should set a word's punctuation aside and keep its capital")
    {
        Rules rules;
        rules.caseHead = Rewrites("*a > *e, trucizna > truciznx");
        REQUIRE(Fit(rules, "Mikstura: Chodzenie po wodzie", false).name == "Miksture: Chodzenie po wodzie");
        REQUIRE(Fit(rules, "Trucizna", false).name == "Truciznx");
    }

    SECTION("for ammunition")
    {
        SECTION("should rewrite every word by Item.Plural, and read no case rule")
        {
            Rules rules;
            rules.plural = Rewrites("carreau > carreaux, *if > *ifs");
            rules.caseHead = Rewrites("*u > *x");
            REQUIRE(Fit(rules, "Carreau d'acier explosif de glace", true).name
                    == "Carreaux d'acier explosifs de glace");
        }

        SECTION("should keep the capital of a word an ending wholly replaces")
        {
            REQUIRE(Fit(German(), "Daedrischer Pfeil", true).name == "Daedrischer Pfeile");
            REQUIRE(Fit(German(), "Eisenpfeil", true).name == "Eisenpfeile");
        }
    }

    SECTION("should give an empty name back empty")
    {
        REQUIRE(Fit(Russian(), "", false).name.empty());
    }
}
