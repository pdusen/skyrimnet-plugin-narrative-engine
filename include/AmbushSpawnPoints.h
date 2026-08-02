#pragma once

#include <vector>

#include <MainThread.h>

#include <RE/Skyrim.h>

// AmbushSpawnPoints — "where should the attackers come from?"
//
// Given the player and a requested distance, produce world positions
// that are behind them, out of view, on ground an actor can stand on,
// and close to the requested range. Returns empty when no such place
// exists; that is a normal outcome, not an error, and the caller turns
// it into a clean COMPOSE failure rather than a wedged beat.
//
// == Why this module exists ==
//
// The previous ambush implementation hung everything off a
// Find-Matching-Reference alias gated on five stacked conditions against
// a FormList of approved marker types. When no approved marker happened
// to be in the loaded area — which is most of Skyrim's open
// wilderness — the alias failed to fill, and because every attacker
// alias was "Create Reference at: SpawnMarker", the whole beat collapsed
// with it. Searching geometry directly has no such dependency on what
// the level designers happened to place nearby.
//
// == The search ==
//
//   1. Sample   — 16 azimuths evenly spaced around the player at the
//                 requested radius. If nothing survives, retry wider
//                 (+25%, +50%) and then narrower (-25%), never below
//                 the configured floor.
//   2. Validate — cheapest gate first: exterior; cell loaded; ground
//                 height resolvable; standable (navmesh, see below);
//                 not visible from the camera.
//   3. Rank     — behind the player's facing first, then by how close
//                 the actual distance lands to the requested one.
//   4. Cluster  — jitter `count` positions around the winner so
//                 attackers don't stack on a single point.
//
// == The navmesh gate ==
//
// This is the highest-risk piece of the beat: it is the difference
// between "attackers converge on the player" and "attackers stand in a
// rock". CommonLibSSE-NG exposes NO callable navmesh query — see
// docs/engine-findings/navmesh-reachability-in-commonlibsse-ng.md. It
// does expose the raw triangle data on each cell's NavMesh records, so
// this module answers CONTAINMENT ("is there navmesh under this point?")
// by testing the point against those triangles directly.
//
// What that does NOT answer is CONNECTIVITY — whether the navmesh under
// the candidate is the same island the player stands on. An attacker
// spawned on an unreachable ledge is on navmesh and still can't path to
// you. The backstops for that are the elevation gate below, the
// post-spawn settle check in the beat itself, StuckRecovery's polling,
// and the abandon-by-timeout route — not anything this module can see.
//
// The containment test itself, along with the water and ground-height
// primitives, lives in StuckRecovery: the same "can an actor stand
// here" question is asked of spawn candidates and of rescue candidates,
// and one copy of the navmesh triangle walk is enough.
//
// Threading: main thread only. Every call reads engine state.
namespace NarrativeEngine::AmbushSpawnPoints
{
    // Nominal humanoid height in game units, used for the visibility
    // probe column and for lifting spawn points clear of the ground.
    inline constexpr float kActorHeightUnits = 128.0f;

    struct Result
    {
        // `count` positions clustered around the winning candidate.
        // Order is not meaningful — callers assign them to actors
        // arbitrarily. Empty when the search failed at every radius.
        std::vector<RE::NiPoint3> spawnPoints;

        // Runner-up winners: candidates that cleared every gate but lost
        // the ranking, best-first, each well separated from the winner
        // and from each other.
        //
        // These exist for StuckRecovery. An actor that can't travel from
        // where it was placed needs somewhere GENUINELY DIFFERENT to go,
        // and hunting for valid ground near a stuck actor mostly finds
        // more of the terrain that trapped it. The runner-ups are
        // already vetted by the same gates as the winner and are far
        // enough away to be a real alternative, so recovery works down
        // this list instead of searching.
        std::vector<RE::NiPoint3> fallbacks;

        bool Ok() const
        {
            return !spawnPoints.empty();
        }
    };

    // Search near `distanceUnits` from `player`. That is the requested
    // radius; the search may return points meaningfully nearer or
    // farther if the requested band is unusable, but never nearer than
    // Settings' configured floor.
    Result Find(RE::Actor* player, int distanceUnits, int count);
    Result Find(const MainThread::Token&, RE::Actor* player, int distanceUnits, int count);

} // namespace NarrativeEngine::AmbushSpawnPoints
