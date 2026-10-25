#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>

// PlotRecipes — what a craftsman in a plot can make, read from the game data.
//
// A crafting step names a real thing made by a real recipe, for the reason
// the item pool gives: a step built on something that does not exist can
// never be written into a memory that says anything true. Unlike the pool
// this is not curated. Every recipe the loaded game has is a candidate, so a
// mod's recipes and a mod's craftsmen take part the same as vanilla's.
//
// What counts as a recipe differs by craft, because the game makes things in
// three different ways:
//
//   smith, set, fletch  constructible objects at the forge, the Skyforge or
//                       Dawnguard's workbench, told apart by what they make
//   concoct             any potion or poison two ingredients share an effect
//                       for: vanilla has no recipe records for these at all
//   enchant             the enchanted variants of base items, each a record
//                       naming the item it was made from
//
// The ingredients are never part of a plot. They are how hard the making is,
// which is all a plot needs to know about them.
//
// See docs/implementation/PHASE_17_MILESTONE_2_RIGID_STEPS.md, Steps 29-31.
namespace NarrativeEngine::PlotRecipes
{
    enum class Craft : std::uint8_t
    {
        Smith,
        Concoct,
        Enchant,
        Set,
        Fletch,

        Count
    };

    inline constexpr std::size_t kCraftCount = static_cast<std::size_t>(Craft::Count);

    inline constexpr std::array<Craft, kCraftCount> kCrafts{Craft::Smith,
                                                            Craft::Concoct,
                                                            Craft::Enchant,
                                                            Craft::Set,
                                                            Craft::Fletch};

    // The verb id of a craft: "smith", "concoct", and so on.
    [[nodiscard]] std::string_view CraftId(Craft craft) noexcept;

    // The skill a craft is practised with. `set` and `fletch` are smithing
    // work in Skyrim: both are made at the forge.
    [[nodiscard]] RE::ActorValue SkillOf(Craft craft) noexcept;

    struct Recipe
    {
        // The thing made.
        RE::FormID form = 0;
        // As it is said in a sentence, with its article: "a Steel Sword",
        // "a sheaf of Elven Arrows". The game's own capitalisation is kept,
        // because lowering it would lower "Nordic" and "Skyforge" with it.
        std::string displayName;
        Craft craft = Craft::Smith;
        // The skill, 0 to 100, the recipe asks of whoever makes it.
        int skill = 0;
        // What the ingredients are worth together, and how many there are.
        float ingredientValue = 0.0f;
        int ingredientCount = 0;
    };

    // Load at kDataLoaded, after the data handler is up. Idempotent.
    void Load();

    // Stable for the session once loaded, so the plot worker may read it
    // with no lock.
    [[nodiscard]] const std::vector<Recipe>& Recipes();

    // --- The pure half ----------------------------------------------------

    // What the made thing is, as far as telling crafts apart goes.
    enum class Made : std::uint8_t
    {
        Weapon,
        Bow, // a bow or a crossbow
        Ammo,
        Armor,
        Jewelry, // armor covering nothing but a ring, amulet or circlet slot
        Other
    };

    // Which of the forge's three crafts makes this, or none.
    [[nodiscard]] std::optional<Craft> ForgeCraftOf(Made made) noexcept;

    // One recipe per name within a craft, keeping the one that asks the
    // least skill, then the one whose ingredients are worth least. Order is
    // otherwise kept.
    [[nodiscard]] std::vector<Recipe> Deduplicated(std::vector<Recipe> recipes);

    // How big a job making this is, 0 to 1, from its ingredients: more of
    // them and dearer ones make a bigger one, and nothing makes a smaller.
    // What a crafting step is sized by in place of a target's standing.
    [[nodiscard]] double Importance(float ingredientValue, int ingredientCount) noexcept;
} // namespace NarrativeEngine::PlotRecipes
