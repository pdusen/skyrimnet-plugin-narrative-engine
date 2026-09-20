#pragma once

#include <PluginThread.h>

#include <RE/Skyrim.h>

#include <cstdint>
#include <vector>

// VisitArrivalPoint — "where would this person be, having walked here
// from where they live?"
//
// The visit beat warps a sender near the player and lets them walk the
// rest of the way. This module decides where that warp lands. The
// answer has to satisfy two things at once: the player must not see it
// happen, and the direction it happens from must be the direction the
// visitor would actually have come from.
//
// The second half is why this is not `AmbushSpawnPoints`. That module
// searches a ring, because an ambush can come from anywhere and merely
// prefers to be walked into. A visit has exactly one defensible
// direction — the road home — and no freedom to substitute another. The
// two share their placement primitives and nothing else.
//
// == The search ==
//
// Note which end the route is measured from. `FineRoads` covers the
// loaded cell grid and moves with the player, so routing OUTWARD FROM
// THE PLAYER toward the sender puts the high-resolution half of the
// plan exactly where the arrival point has to be. `Plan::finePath` is
// ordered from its start outward, so walking it is walking backward
// along the route the visitor would have taken.
//
//   Tier 1  Route player -> sender. Walk `finePath` outward and take
//   FineRoad     the first node inside the distance band that is still
//                on navmesh and stands behind cover. Reads as "they
//                came up the road", and is the outcome this module
//                exists to produce.
//
//   Tier 2  No fine coverage on that stretch, or the player is indoors
//   Coarse-      and the fine graph is empty. Take a bearing from the
//   Bearing      player toward the first coarse node on the route and
//                sample an arc around it. Reads as "they came from the
//                right direction". Gated on
//                bVisitArrivalAllowCoarseBearing.
//
//   Tier 3  Nothing. A clean failure the caller turns into a declined
//   None         beat. There is deliberately NO any-direction fallback:
//                a visitor arriving opposite their home is the exact
//                tell this module removes, and shipping it as a
//                fallback would make the worst case the thing we set
//                out to fix.
//
// == Why the gates are so few ==
//
// Tier 1's candidates are centroids of `kPreferred`-flagged navmesh
// triangles, so they are on walkable ground by construction. Most of
// what `AmbushSpawnPoints` spends its gates on — ground height
// resolvable, roughly level, not underwater — is already answered. What
// is left is the band, a re-check that the node's cell is still loaded
// and meshed (the fine graph is a snapshot and cells unload), and
// cover.
//
// Tier 2 has none of those guarantees, so it runs the full gate set via
// `StuckRecovery::IsStandable`.
//
// == Distance is straight-line, not along the road ==
//
// The band controls whether the arrival is perceptible and how long the
// approach takes to watch, and both are functions of how far away the
// sender actually is. Road order decides only which candidate is
// considered first. Measured in the XY plane: a fine node's Z is a
// triangle centroid and comparing it against the player's own Z adds
// noise to a question that is really about ground distance.
//
// == Threading ==
//
// Called from the plugin thread. Origin resolution and the cover gate
// touch engine state and marshal through `MainThread::Run`; the routing
// between them is a pure query over both graphs, and Dijkstra has no
// business on the main thread.
namespace NarrativeEngine::VisitArrivalPoint
{
    enum class Tier : std::uint8_t
    {
        None,
        FineRoad,
        CoarseBearing,
        CityApproach, // inside the player's walled worldspace, toward the gate
        CityGate,     // the far side of that gate, in the visitor's worldspace
    };

    const char* TierName(Tier tier);

    struct Result
    {
        // Where the sender is warped to. Already lifted clear of the
        // ground, so it is a standing position rather than a surface
        // point.
        RE::NiPoint3 point{};

        // Points that cleared every gate but lost, in road order —
        // each one further along the same route. `StuckRecovery`
        // escalates through these when the sender cannot travel from
        // where it was placed, which walks them BACK ALONG THEIR OWN
        // PATH rather than sideways onto unrelated terrain. Do not
        // re-sort these by distance to the player; the ordering is the
        // point.
        std::vector<RE::NiPoint3> fallbacks;

        Tier tier = Tier::None;

        // The reference a placement marker must be created from, or 0
        // to create it from the player.
        //
        // PlaceObjectAtMe builds its reference in the CALLER's cell, so
        // placing from the player is only right when `point` is in the
        // cell the player is standing in. The two city tiers are exactly
        // when it is not: CityGate sits in another worldspace, and
        // CityApproach found via a doorstep sits outside an interior the
        // player has not left yet. Both hand back a door reference
        // already standing in the right place to build from.
        RE::FormID placementAnchor = 0;

        bool Ok() const
        {
            return tier != Tier::None;
        }
    };

    // Where should `sender` appear, to read as having walked to
    // `player`? Returns a `Tier::None` result when there is no such
    // place — a normal outcome, not an error, and the caller turns it
    // into a clean COMPOSE failure rather than a wedged beat.
    Result Find(const PluginThread::Token& pt, RE::Actor* sender, RE::Actor* player);

} // namespace NarrativeEngine::VisitArrivalPoint
