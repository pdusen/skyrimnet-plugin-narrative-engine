#pragma once

#include <PlotCasting.h>

// PlotPopulation — the engine-bound half of casting.
//
// Builds PlotCasting::Population once from the graph GossipGraph
// already maintains, plus the faction ranks that graph does not carry.
// Everything after that point is pure, which is what makes the casting
// RULES probeable while the data behind them still comes from the real
// game.
//
// There is deliberately no second index of unique NPCs. GossipGraph's
// participant set already resolves residence, hold, personal edges and
// cached display names for exactly the population plots care about, and
// building a parallel one would give two indices that can disagree
// about who exists.
//
// What GossipGraph does NOT carry is faction RANK, which mastermind
// weighting and the subordinate test both need. That is read once here,
// on the main thread, rather than per tick: it is a walk of every
// participant's faction list, and doing it on the plot worker would be
// an engine read from the wrong thread as well as a waste.
namespace NarrativeEngine::PlotPopulation
{
    // Main thread, after GossipGraph::Initialize. Idempotent.
    //
    // A no-op when the gossip graph is unavailable — plots then have no
    // population, every tick finds nobody to cast, and the simulation
    // idles rather than crashing. That is the same degradation gossip
    // itself takes.
    void Build();

    [[nodiscard]] bool IsReady();

    // Stable for the session once built, so the plot worker may read it
    // with no lock. Empty until Build runs.
    [[nodiscard]] const PlotCasting::Population& Get();

    // Alive-and-enabled check for one participant, safe off the main
    // thread: unique NPCs' Actor objects are persistent and always
    // resident, and only their 3D unloads, so this is a plain pointer
    // and bool load. The same property gossip relies on for its own
    // life checks.
    [[nodiscard]] bool IsAlive(RE::FormID npc);

    // Is this NPC's 3D loaded — i.e. could the player plausibly be
    // watching them right now?
    //
    // The presence gate for conspicuous steps. "3D loaded" rather than a
    // distance check because it is both cheaper and more correct: an
    // actor thirty feet away through a wall in an unloaded cell is not
    // being watched. Safe off the main thread for the same reason
    // IsAlive is.
    [[nodiscard]] bool IsNearPlayer(RE::FormID npc);

    // Bound form of the above, for handing to PlotCasting.
    [[nodiscard]] PlotCasting::AlivePredicate AlivePredicate();
} // namespace NarrativeEngine::PlotPopulation
