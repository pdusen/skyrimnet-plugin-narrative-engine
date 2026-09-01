#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <PlotCasting.h>
#include <PlotItemPool.h>

// PlotMenus — every noun a plot can name, resolved before the prompt is
// built.
//
// The model is never asked to invent a person, a place, a faction or an
// object. It is handed four lists of things that already exist and picks
// indices off them. That is the difference between a plot the simulation
// can execute and a plot that reads well and cannot be run: a step whose
// target is "the Jarl's cousin" has nobody to travel to, nothing to put
// in an inventory, and no name to write into a memory.
//
// ---------------------------------------------------------------------
// Why the menus are per-plot rather than global
//
// The population is 881 people. Handed all of them, the model picks
// whoever the prompt happened to mention last, and the plot has no
// reason to involve the people it involves. Handed the ~12 who are
// actually connected to this mastermind -- their faction, their friends,
// their hold -- every choice it makes is one a reader could justify.
//
// Each actor therefore carries the REASON they are on the list. That
// reason is the useful half: "Vilkas, a shield-brother in the
// Companions" tells the model what kind of scheme is available in a way
// that a bare name cannot.
//
// ---------------------------------------------------------------------
// Stable indices
//
// The order is deterministic and depends on nothing but the population
// and the mastermind. That is what lets the prompt say "3" and the
// response mean the same person -- and it is why ties are broken by
// FormID rather than left to hash order.
//
// See docs/implementation/PHASE_14_FACTION_PLOTS.md step 15.
namespace NarrativeEngine::PlotMenus
{
    // Why this person is on the menu, most connected first. Also the
    // ordering key, so the enum's order is the menu's order.
    enum class Relation : std::uint8_t
    {
        // They share a roof, and an organisation as well. In Skyrim's
        // terms this is nearly always family -- the Gray-Manes, the
        // Battle-Borns -- or a hall like Jorrvaskr, and it is the
        // strongest link the graph can express.
        //
        // Its own rung rather than a flag on the ones below, because
        // without it a daughter and a shield-sister came out of the
        // menu described identically, and the model schemed against
        // whichever one the sort happened to put first.
        HouseholdAndColleague,
        Household,
        // A personal tie AND a shared organisation.
        CloseAndColleague,
        Close,     // a personal tie
        Colleague, // a shared organisation
        Neighbour, // the same hold, and nothing else

        Count
    };

    [[nodiscard]] std::string_view RelationId(Relation r) noexcept;

    // A short phrase for the prompt, e.g. "a friend and colleague".
    [[nodiscard]] std::string_view RelationPhrase(Relation r) noexcept;

    struct ActorOption
    {
        RE::FormID npc = 0;
        std::string name;
        Relation relation = Relation::Neighbour;
        // Their standing in the faction they share with the mastermind,
        // 0 when they share none. Lets the prompt say "her steward"
        // rather than "someone she knows".
        double standing = 0.0;
    };

    struct LocationOption
    {
        RE::FormID location = 0;
        std::string name;
    };

    struct FactionOption
    {
        RE::FormID faction = 0;
        std::string name;
    };

    // What the engine-bound caller supplies. Gathered once per session
    // rather than per plot, because none of it depends on who is
    // scheming.
    struct World
    {
        // Every settlement the population lives in, with its display
        // name. The menu picks the subset its actors come from.
        std::vector<LocationOption> settlements;
        // The rostered factions, in roster order.
        std::vector<FactionOption> factions;
        // The curated Acquire pool, in file order.
        std::vector<PlotItemPool::Item> items;
    };

    struct Limits
    {
        // Twelve is a menu a person could read. The number matters less
        // than the fact that there is one: an unbounded list is how a
        // prompt stops fitting and starts costing.
        std::size_t actors = 12;
        std::size_t locations = 8;
        std::size_t factions = 6;
        std::size_t items = 20;
    };

    struct Menus
    {
        std::vector<ActorOption> actors;
        std::vector<LocationOption> locations;
        std::vector<FactionOption> factions;
        std::vector<PlotItemPool::Item> items;

        [[nodiscard]] bool Empty() const
        {
            return actors.empty() && locations.empty() && factions.empty() && items.empty();
        }
    };

    // Pure. Everything engine-bound arrives through `world`, so a probe
    // can drive this against a fabricated population.
    //
    // The mastermind is never on their own actor menu: a plot whose
    // target is its own author is not a plot.
    [[nodiscard]] Menus Build(const PlotCasting::Population& population,
                              RE::FormID mastermind,
                              const World& world,
                              const Limits& limits);

    // --- The engine-bound half -------------------------------------------

    // Gather the world once, on the main thread, after the population
    // and the roster are built. Idempotent.
    void BuildWorld();

    [[nodiscard]] const World& CurrentWorld();
} // namespace NarrativeEngine::PlotMenus
