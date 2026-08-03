#pragma once

#include <MainThread.h>

#include <RE/Skyrim.h>

#include <cstddef>
#include <vector>

// RoadRoute — the query layer over the two road graphs.
//
// `TravelGraph` holds a coarse global skeleton; `FineRoads` holds a
// high-resolution graph of whatever cells are loaded. Neither is useful
// on its own for the thing we actually want to ask:
//
//   "Given where I am and where I'm going, what points can an NPC walk
//    until the fine data runs out, and where do they hand off to the
//    coarse network?"
//
// That is what `Route` answers.
//
// ---------------------------------------------------------------------
// How the handoff is chosen
//
// The fine graph's boundary is its frontier nodes — points where the road
// leaves the loaded region. Any of them could be the handoff, and picking
// by bearing ("which one points toward the destination?") gets forks
// wrong: at a crossroads two branches can leave at almost the same angle
// while leading to wildly different journeys.
//
// So the choice is made on total journey cost instead. For every frontier
// node f:
//
//   cost(f) = fine(start -> f)                  walked, true path length
//           + join(f -> nearest coarse node)    the ungraphed gap
//           + coarse(that node -> destination)  the rest of the trip
//
// and the cheapest wins. That makes the branch that *joins the network
// better* beat the branch that merely points the right way.
//
// One known bias: coarse distances underestimate, because the skeleton
// cuts corners, while fine distances are true path lengths. The
// underestimate should be roughly proportional and so preserve the
// ranking between candidates, but it has not been verified.
//
// ---------------------------------------------------------------------
// What the caller is responsible for
//
// Route stops at the frontier. Beyond it, issuing a travel package to the
// destination is usually enough on its own: the engine moves actors
// outside the loaded grid along the same preferred-path data TravelGraph
// is built from, so it already follows roads unaided. The coarse path is
// returned for estimating progress and direction, not because anything
// needs to drive movement along it.
namespace NarrativeEngine::RoadRoute
{
    // A position on the coarse graph's own worldspace, resolved from an
    // arbitrary reference. Interiors resolve through their load door.
    struct Origin
    {
        bool valid = false;
        RE::FormID worldSpace = 0;
        RE::NiPoint3 position{};
        // True when the reference was indoors and this is where its load
        // door comes out, rather than the reference's own position.
        bool viaLoadDoor = false;
    };

    // Where `ref` sits on the exterior road network.
    //
    // Outdoors this is just the reference's own position. Indoors there
    // is no fine graph and the interior has no place on the coarse one,
    // so we follow a load door out and use the arrival point — that is
    // where the occupant would actually emerge, which is what makes an
    // NPC arriving there read correctly. Falls back to the reference's
    // location marker when the cell has no load door (some interiors
    // genuinely don't), and returns invalid if neither resolves.
    Origin ResolveOrigin(const MainThread::Token&, RE::TESObjectREFR* ref);

    struct Plan
    {
        bool valid = false;

        // Ordered walkable points from the start outward. Empty when no
        // fine graph covers the start — the caller then has only the
        // coarse path to work with.
        std::vector<RE::NiPoint3> finePath;

        // Coarse node indices continuing from the handoff to the
        // destination. Empty when the destination lies inside fine
        // coverage, or when the route is fine-only.
        std::vector<std::size_t> coarsePath;

        // The whole journey fit inside the fine graph; `finePath` reaches
        // the destination and there is no handoff.
        bool destinationWithinFine = false;

        // Total scored cost in world units. Mixes true fine lengths with
        // underestimating coarse ones — comparable between plans, not an
        // accurate distance.
        float estimatedCost = 0.0f;
    };

    // Plan a route. Pure query over both graphs — no engine access, so no
    // main-thread token needed.
    //
    // Degrades in steps rather than failing: no fine coverage, or no
    // frontier reachable from the start (the player standing at the end
    // of a spur), falls back to a coarse-only plan. An invalid result
    // means neither graph could place the endpoints at all.
    Plan Route(RE::FormID worldSpace, const RE::NiPoint3& from, const RE::NiPoint3& to);
} // namespace NarrativeEngine::RoadRoute
