#include <LanguageTable.h>
#include <PlotRecipes.h>
#include <PlotText.h>
#include <TextKeys.h>

#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tests for the engine-free half of PlotRecipes: how a recipe read from the
// game data is classed, said and weighed. The reading itself walks the data
// handler's arrays and has nothing to assert here that the load's log line
// does not already show.

namespace
{
    namespace PlotRecipes = NarrativeEngine::PlotRecipes;
    namespace PlotText = NarrativeEngine::PlotText;
    using NarrativeEngine::Testing::ShippedEnglish;
    using PlotRecipes::Craft;
    using PlotRecipes::Made;

    PlotRecipes::Recipe Make(std::string name, Craft craft, int skill, float value)
    {
        PlotRecipes::Recipe r;
        r.displayName = std::move(name);
        r.craft = craft;
        r.skill = skill;
        r.ingredientValue = value;
        r.ingredientCount = 2;
        return r;
    }

    // A shipped language as the plugin loads it, over the shipped English.
    NarrativeEngine::LanguageTable::Table Shipped(std::string_view stem)
    {
        static const std::string english = NarrativeEngine::Testing::ShippedLanguageFile("english");
        const auto file = NarrativeEngine::Testing::ShippedLanguageFile(stem);
        REQUIRE_FALSE(file.empty());
        return NarrativeEngine::LanguageTable::Table::Build({english, std::string_view{file}, std::nullopt},
                                                            NarrativeEngine::TextKeys::All());
    }
} // namespace

TEST_CASE("PlotRecipes classing", "[PlotRecipes][engine]")
{
    SECTION("when something is made at the forge")
    {
        SECTION("should make weapons and armor smith's work")
        {
            REQUIRE(PlotRecipes::ForgeCraftOf(Made::Weapon) == Craft::Smith);
            REQUIRE(PlotRecipes::ForgeCraftOf(Made::Armor) == Craft::Smith);
        }

        SECTION("should make bows and their ammunition a fletcher's")
        {
            REQUIRE(PlotRecipes::ForgeCraftOf(Made::Bow) == Craft::Fletch);
            REQUIRE(PlotRecipes::ForgeCraftOf(Made::Ammo) == Craft::Fletch);
        }

        SECTION("should make rings, amulets and circlets a jeweler's")
        {
            REQUIRE(PlotRecipes::ForgeCraftOf(Made::Jewelry) == Craft::Set);
        }

        SECTION("should leave anything else to nobody")
        {
            REQUIRE_FALSE(PlotRecipes::ForgeCraftOf(Made::Other).has_value());
        }
    }

    SECTION("when a craft's skill is asked for")
    {
        SECTION("should practise everything made at the forge with Smithing")
        {
            REQUIRE(PlotRecipes::SkillOf(Craft::Smith) == RE::ActorValue::kSmithing);
            REQUIRE(PlotRecipes::SkillOf(Craft::Set) == RE::ActorValue::kSmithing);
            REQUIRE(PlotRecipes::SkillOf(Craft::Fletch) == RE::ActorValue::kSmithing);
        }

        SECTION("should practise potions with Alchemy and enchantments with Enchanting")
        {
            REQUIRE(PlotRecipes::SkillOf(Craft::Concoct) == RE::ActorValue::kAlchemy);
            REQUIRE(PlotRecipes::SkillOf(Craft::Enchant) == RE::ActorValue::kEnchanting);
        }
    }

    SECTION("when every craft's id is read")
    {
        SECTION("should be the verb that makes it")
        {
            REQUIRE(PlotRecipes::CraftId(Craft::Smith) == "smith");
            REQUIRE(PlotRecipes::CraftId(Craft::Concoct) == "concoct");
            REQUIRE(PlotRecipes::CraftId(Craft::Enchant) == "enchant");
            REQUIRE(PlotRecipes::CraftId(Craft::Set) == "set");
            REQUIRE(PlotRecipes::CraftId(Craft::Fletch) == "fletch");
        }
    }
}

TEST_CASE("PlotRecipes phrasing", "[PlotRecipes][engine]")
{
    SECTION("when a thing is named")
    {
        SECTION("should give it its article, keeping the game's capitals")
        {
            REQUIRE(PlotText::ItemPhrase(ShippedEnglish(), "Steel Sword", false) == "a Steel Sword");
            REQUIRE(PlotText::ItemPhrase(ShippedEnglish(), "Iron Sword of Burning", false)
                    == "an Iron Sword of Burning");
            REQUIRE(PlotText::ItemPhrase(ShippedEnglish(), "Elven Bow", false) == "an Elven Bow");
        }

        SECTION("should say ammunition as a sheaf")
        {
            REQUIRE(PlotText::ItemPhrase(ShippedEnglish(), "Elven Arrow", true) == "a sheaf of Elven Arrows");
            REQUIRE(PlotText::ItemPhrase(ShippedEnglish(), "Steel Bolt", true) == "a sheaf of Steel Bolts");
        }

        SECTION("should say nothing for a thing with no name")
        {
            REQUIRE(PlotText::ItemPhrase(ShippedEnglish(), "", false).empty());
        }
    }

    // The game's own names in each language, fitted by the shipped [Grammar]
    // Item.* rules. Non-ASCII text is written as escapes, glyphs in comments.
    SECTION("in German")
    {
        const auto german = Shipped("german");

        SECTION("should give a name the accusative article of its head, declining an adjective before it")
        {
            REQUIRE(PlotText::ItemPhrase(german, "Daedrischer Bogen der Angst", false)
                    == "einen Daedrischen Bogen der Angst");
            REQUIRE(PlotText::ItemPhrase(german, "Eisenschwert", false) == "ein Eisenschwert");
            // Elfenrüstung.
            REQUIRE(PlotText::ItemPhrase(german, "Elfenr\xC3\xBCstung", false) == "eine Elfenr\xC3\xBCstung");
        }

        SECTION("should give a plural no article")
        {
            REQUIRE(PlotText::ItemPhrase(german, "Alte Nordstiefel", false) == "Alte Nordstiefel");
        }

        SECTION("should leave a name it does not know bare, as a trial in an English game hands it")
        {
            REQUIRE(PlotText::ItemPhrase(german, "Iron Sword", false) == "Iron Sword");
        }

        SECTION("should put ammunition in the plural")
        {
            // ein Bündel Elfenpfeile.
            REQUIRE(PlotText::ItemPhrase(german, "Elfenpfeil", true) == "ein B\xC3\xBCndel Elfenpfeile");
        }
    }

    SECTION("in Russian")
    {
        const auto russian = Shipped("russian");

        SECTION("should put a feminine name in the accusative, leaving the genitive after it")
        {
            // Стеклянная булава летаргии -> Стеклянную булаву летаргии.
            REQUIRE(
                PlotText::ItemPhrase(russian,
                                     "\xD0\xA1\xD1\x82\xD0\xB5\xD0\xBA\xD0\xBB\xD1\x8F\xD0\xBD\xD0\xBD\xD0\xB0\xD1\x8F "
                                     "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xB0 "
                                     "\xD0\xBB\xD0\xB5\xD1\x82\xD0\xB0\xD1\x80\xD0\xB3\xD0\xB8\xD0\xB8",
                                     false)
                == "\xD0\xA1\xD1\x82\xD0\xB5\xD0\xBA\xD0\xBB\xD1\x8F\xD0\xBD\xD0\xBD\xD1\x83\xD1\x8E "
                   "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xB0\xD0\xB2\xD1\x83 "
                   "\xD0\xBB\xD0\xB5\xD1\x82\xD0\xB0\xD1\x80\xD0\xB3\xD0\xB8\xD0\xB8");
        }

        SECTION("should leave a masculine name and its genitive alone")
        {
            // Яд нетча.
            constexpr std::string_view kNetchPoison = "\xD0\xAF\xD0\xB4 \xD0\xBD\xD0\xB5\xD1\x82\xD1\x87\xD0\xB0";
            REQUIRE(PlotText::ItemPhrase(russian, kNetchPoison, false) == kNetchPoison);
        }

        SECTION("should leave ammunition in the nominative after its quiver")
        {
            // колчан — Стальная стрела.
            REQUIRE(PlotText::ItemPhrase(russian,
                                         "\xD0\xA1\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F "
                                         "\xD1\x81\xD1\x82\xD1\x80\xD0\xB5\xD0\xBB\xD0\xB0",
                                         true)
                    == "\xD0\xBA\xD0\xBE\xD0\xBB\xD1\x87\xD0\xB0\xD0\xBD \xE2\x80\x94 "
                       "\xD0\xA1\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F "
                       "\xD1\x81\xD1\x82\xD1\x80\xD0\xB5\xD0\xBB\xD0\xB0");
        }
    }

    SECTION("in Polish")
    {
        const auto polish = Shipped("polish");

        SECTION("should tell an adjective's -a from a noun's")
        {
            // Słaba trucizna -> Słabą truciznę.
            REQUIRE(PlotText::ItemPhrase(polish,
                                         "S\xC5\x82"
                                         "aba trucizna",
                                         false)
                    == "S\xC5\x82"
                       "ab\xC4\x85 trucizn\xC4\x99");
            // Krasnoludzka kusza -> Krasnoludzką kuszę.
            REQUIRE(PlotText::ItemPhrase(polish, "Krasnoludzka kusza", false) == "Krasnoludzk\xC4\x85 kusz\xC4\x99");
        }

        SECTION("should leave a masculine name and its genitive alone")
        {
            // Dekokt otwierania zamków.
            REQUIRE(PlotText::ItemPhrase(polish, "Dekokt otwierania zamk\xC3\xB3w", false)
                    == "Dekokt otwierania zamk\xC3\xB3w");
        }
    }

    SECTION("in French")
    {
        const auto french = Shipped("french");

        SECTION("should give a name the article of its gender or number")
        {
            // une Épée d'ébonite.
            REQUIRE(PlotText::ItemPhrase(french,
                                         "\xC3\x89p\xC3\xA9"
                                         "e d'\xC3\xA9"
                                         "bonite",
                                         false)
                    == "une \xC3\x89p\xC3\xA9"
                       "e d'\xC3\xA9"
                       "bonite");
            REQUIRE(PlotText::ItemPhrase(french, "Bottes nordiques antiques", false)
                    == "des Bottes nordiques antiques");
        }

        SECTION("should put ammunition and its adjectives in the plural")
        {
            REQUIRE(PlotText::ItemPhrase(french, "Carreau d'acier explosif de glace", true)
                    == "un lot de Carreaux d'acier explosifs de glace");
        }
    }

    SECTION("in Italian")
    {
        const auto italian = Shipped("italian");

        SECTION("should choose the article by the sound the name opens with")
        {
            REQUIRE(PlotText::ItemPhrase(italian, "Ascia d'ebano", false) == "un'Ascia d'ebano");
            REQUIRE(PlotText::ItemPhrase(italian, "Scudo di ferro", false) == "uno Scudo di ferro");
            REQUIRE(PlotText::ItemPhrase(italian, "Stivali di vetro", false) == "degli Stivali di vetro");
        }
    }

    SECTION("in Spanish")
    {
        const auto spanish = Shipped("spanish");

        SECTION("should give hacha the masculine article and a plural its own")
        {
            REQUIRE(PlotText::ItemPhrase(spanish, "Hacha de combate de cristal", false)
                    == "un Hacha de combate de cristal");
            // unas Botas nórdicas antiguas.
            REQUIRE(PlotText::ItemPhrase(spanish, "Botas n\xC3\xB3rdicas antiguas", false)
                    == "unas Botas n\xC3\xB3rdicas antiguas");
        }

        SECTION("should put ammunition in the plural without touching a noun after de")
        {
            // un carcaj de Flechas élficas.
            REQUIRE(PlotText::ItemPhrase(spanish, "Flecha \xC3\xA9lfica", true)
                    == "un carcaj de Flechas \xC3\xA9lficas");
            REQUIRE(PlotText::ItemPhrase(spanish, "Virote de acero", true) == "un carcaj de Virotes de acero");
        }
    }
}

TEST_CASE("PlotRecipes duplicates", "[PlotRecipes][engine]")
{
    SECTION("when two recipes of a craft make things of the same name")
    {
        const auto out = PlotRecipes::Deduplicated({Make("a Steel Sword", Craft::Smith, 30, 20.0f),
                                                    Make("an Iron Dagger", Craft::Smith, 0, 10.0f),
                                                    Make("a Steel Sword", Craft::Smith, 0, 25.0f),
                                                    Make("a Steel Sword", Craft::Smith, 0, 15.0f)});

        SECTION("should keep one, in the first one's place")
        {
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].displayName == "a Steel Sword");
            REQUIRE(out[1].displayName == "an Iron Dagger");
        }

        SECTION("should keep the one asking the least skill, then the cheapest")
        {
            REQUIRE(out[0].skill == 0);
            REQUIRE(out[0].ingredientValue == 15.0f);
        }
    }

    SECTION("when two crafts make things of the same name")
    {
        const auto out = PlotRecipes::Deduplicated(
            {Make("a Gold Ring", Craft::Set, 0, 50.0f), Make("a Gold Ring", Craft::Enchant, 0, 80.0f)});

        SECTION("should keep both")
        {
            REQUIRE(out.size() == 2);
        }
    }
}

TEST_CASE("PlotRecipes importance", "[PlotRecipes][engine]")
{
    SECTION("when a recipe's ingredients are weighed")
    {
        SECTION("should stay between 0 and 1")
        {
            REQUIRE(PlotRecipes::Importance(0.0f, 0) >= 0.0);
            REQUIRE(PlotRecipes::Importance(1.0e6f, 100) <= 1.0);
        }

        SECTION("should never make dearer ingredients an easier job")
        {
            double last = -1.0;
            for (const float value : {0.0f, 5.0f, 10.0f, 100.0f, 1000.0f, 2000.0f, 50000.0f}) {
                const double now = PlotRecipes::Importance(value, 3);
                REQUIRE(now >= last);
                last = now;
            }
        }

        SECTION("should never make more ingredients an easier job")
        {
            double last = -1.0;
            for (int count = 0; count <= 20; ++count) {
                const double now = PlotRecipes::Importance(100.0f, count);
                REQUIRE(now >= last);
                last = now;
            }
        }

        SECTION("should make Daedric armor a far bigger job than an iron dagger")
        {
            // A dagger: an ingot and a strip of leather. Daedric armor: five
            // ebony ingots, a daedra heart and three strips.
            REQUIRE(PlotRecipes::Importance(1000.0f, 9) > PlotRecipes::Importance(10.0f, 2) + 0.5);
        }
    }
}
