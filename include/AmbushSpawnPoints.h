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
// == The search ==
//
//   1. Sample   — a ring of azimuths around the player, at successively
//                 wider radii, stopping once the forward arc has yielded
//                 a hidden spot AND enough separated runner-ups behind
//                 it. Azimuth count scales with radius so the arc
//                 between samples stays near-constant in world units.
//   2. Validate — cheapest gate first: cell loaded; exterior; ground
//                 height resolvable; roughly level with the player; not
//                 underwater; on navmesh (see below); behind cover.
//   3. Rank     — forward arc before rear as a hard tier, then by a
//                 combined angle-and-distance score.
//   4. Cluster  — jitter `count` positions around the winner so
//                 attackers don't stack on a single point.
//
// == The navmesh gate ==
//
// This is the highest-risk piece of the beat: it is the difference
// between "attackers converge on the player" and "attackers stand in a
// rock". CommonLibSSE-NG exposes NO callable navmesh query — see
// docs/engine-findings/navmesh-queries-in-commonlibsse-ng.md. It does
// expose the raw triangle data on each cell's NavMesh records, so this
// module answers CONTAINMENT ("is there navmesh under this point?") by
// testing the point against those triangles directly.
//
// What that does NOT answer is CONNECTIVITY — whether the navmesh under
// the candidate is the same island the player stands on. An attacker
// spawned on an unreachable ledge is on navmesh and still can't path to
// you. The backstops for that are the elevation gate, the post-spawn
// settle check in the beat itself, StuckRecovery's polling, and the
// abandon-by-timeout route — not anything this module can see.
//
// The containment test itself, along with the water and ground-height
// primitives, lives in StuckRecovery.
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

        // Candidates that cleared every gate but lost the ranking,
        // best-first, each well separated from the winner and from each
        // other. StuckRecovery works down this list when an actor can't
        // travel from where it was placed — the separation is what makes
        // them genuinely different terrain rather than more of the same.
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
