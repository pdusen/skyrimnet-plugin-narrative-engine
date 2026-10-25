#include <PlotRecipes.h>
#include <PlotText.h>

#include <ShippedLanguage.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
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
