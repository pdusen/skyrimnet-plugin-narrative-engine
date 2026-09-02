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

    // Main thread, at kNewGame / kPostLoadGame. Places every settlement
    // on the road graph and measures the distances between them.
    //
    // SEPARATE FROM Build, and late, because it is the one thing here
    // that needs a running game rather than loaded plugin data. A
    // settlement is placed by its map marker, which BGSLocation holds as
    // an ObjectRefHandle -- and at kDataLoaded, where Build runs, there
    // is no game yet for a reference handle to point into. The first
    // in-game run of this feature resolved ZERO of sixty settlements and
    // fell back to the hold proxy for every step, silently, which is
    // exactly what an offline harness cannot catch.
    //
    // Idempotent, and retried on each load until it succeeds: a session
    // that placed nobody costs one more attempt next time rather than
    // wedging the fallback in for the life of the playthrough.
    void OnSessionStart();

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

    // Alive, and in a state to START a scheme right now: unique, not
    // mid-fight, not trailing the player, and somewhere the world can
    // act on. The same four checks the visit beat makes before warping
    // somebody in.
    //
    // MASTERMIND selection only. An agent is cast for a step that plays
    // out over in-world days, so their state this second says nothing
    // about it; a mastermind is being chosen to have an idea now.
    [[nodiscard]] bool IsViableMastermind(RE::FormID npc);

    // Bound forms of the above, for handing to PlotCasting.
    [[nodiscard]] PlotCasting::AlivePredicate AlivePredicate();
    [[nodiscard]] PlotCasting::AlivePredicate ViablePredicate();

    // --- Tenure: the one part of a member's standing that MOVES --------
    //
    // Everything else here is authored data read once. Who currently
    // holds a hold's offices is not: CWGovernmentScript installs and
    // exiles courts with AddToFaction and RemoveFromFaction as the war
    // is fought, so a court read at load says who governed the province
    // when Bethesda shipped it and nothing about this save.
    //
    // So it is not read at load. This binds a predicate that asks the
    // question LIVE, per membership, at the moment casting reads it --
    // which is both simpler than a snapshot and strictly more current
    // than one, since no window exists between taking the answer and
    // using it.
    //
    // Safe from the plot worker; see PlotFactionRoster::IsSeated for
    // why. Returns an EMPTY predicate when no rostered section declares
    // a tenure gate, which casting reads as "everything counts" and
    // skips entirely.
    [[nodiscard]] PlotCasting::SeatedPredicate SeatedPredicate();

    // How far apart two members are along the road network, normalised
    // to 0 (the same settlement) .. 1 (as far apart as the province
    // meaningfully gets). Returns a NEGATIVE value when the graph cannot
    // answer -- either node missing, no route between them, or the graph
    // switched off -- so the caller can fall back rather than being
    // handed a plausible-looking zero.
    //
    // Safe off the main thread: the table is built during Build and is
    // const for the session, exactly like the population itself.
    [[nodiscard]] double RoadDistanceNorm(std::size_t nodeA, std::size_t nodeB);
} // namespace NarrativeEngine::PlotPopulation
