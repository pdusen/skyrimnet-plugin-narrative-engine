#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <RE/Skyrim.h>

#include <FineRoads.h>
#include <MainThread.h>

// ApproachChain — one unbroken run of points from where a visitor is to
// where the player is.
//
// ---------------------------------------------------------------------
// What this is for
//
// A visit has to answer "where along their way here could this person
// plausibly be?". `RoadRoute::Route` answers a different question: it
// plans a journey in two containers of different types, ordered from the
// player, with nothing at all occupying the gap between them. The one
// thing a route is for — an ordered list of places between A and B —
// does not exist in its output.
//
// This module builds that list. The arrival point then becomes a choice
// along the chain rather than a search in a band beside it.
//
// ---------------------------------------------------------------------
// One graph, not four segments
//
// Everything walkable goes into a single weighted graph, each node
// carrying a difficulty, and the route is whatever a search over that
// graph returns:
//
//   difficulty 1  every coarse road node in the worldspace, and its edges
//   difficulty 2  every node of the SELECTED loaded fine network
//   difficulty 3  bridge points, the player, the visitor, their connectors
//   difficulty 4  two direct lines out of the visitor
//
// Edge cost is `length x max(difficulty of its two endpoints)`. `max`
// rather than an average, so one expensive node is genuinely expensive
// to pass through instead of being diluted by a cheap neighbour.
//
// Hand-assembling "coarse, then bridge, then fine" would force a shape
// on the answer. A weighted search follows roads because roads are
// cheap, not because the construction order said so — and the chain
// comes out coarse in the middle and dense near the player without that
// having been imposed.
//
// ---------------------------------------------------------------------
// The two direct lines, and why the graph is always connected
//
// Both run outward FROM THE VISITOR, at 512-unit spacing:
//
//   * To the selected fine network, meeting it at whichever of its nodes
//     is nearest the visitor. This is the useful one — it lets somebody
//     off-road but close cut straight to the road near the player and
//     finish on cheap ground, rather than detouring out to the coarse
//     skeleton and back.
//   * To the player. The connectivity guarantee, and the only segment
//     that exists with no fine network to bridge to: the player indoors,
//     a save just restored, open country away from any road.
//
// With the second line present the search can never fail to produce a
// route, so every caller may assume a chain exists. That does not weaken
// the promise never to place a visitor arriving from the wrong
// direction, because both lines run outward from the visitor — a point
// on either is on the bearing home by construction. They are the worst
// routes available, not wrong ones.
//
// ---------------------------------------------------------------------
// Difficulty and class are different numbers, inversely related
//
// Worth stating plainly, because merging them would look like a
// simplification and would be a bug:
//
//   * DIFFICULTY decides which way the route goes. Coarse is cheapest.
//   * CLASS decides which point along it a visitor may stand on. Coarse
//     is worst — unvalidated ground outside the loaded grid.
//
// A coarse node is the best thing to route through and the last thing to
// stand on. One number cannot carry both meanings.
//
// ---------------------------------------------------------------------
// Threading
//
// `Build` is a pure query over both road graphs and the settings. No
// engine access, so no main-thread token, exactly like
// `RoadRoute::Route`. The one thing it cannot work out for itself is
// which of its points fall inside the attached cell grid, so the caller
// reads that once with `ReadLoadedGrid` — the only function here that
// touches the engine — and hands it in.
namespace NarrativeEngine::ApproachChain
{
    // Which graph a point came from. Ranked by how safe it is to put an
    // actor there, which is the reverse of how cheap it was to route
    // through.
    enum class PointClass : std::uint8_t
    {
        Coarse,    // a coarse road node: outside the grid, unvalidated
        Fine,      // a loaded fine road node: real navmesh under it
        Connector, // bridge fill, and the player's and visitor's own nodes
        Direct,    // a point on one of the two direct lines
    };

    const char* PointClassName(PointClass cls);

    // The attached exterior cell grid, in cell coordinates.
    //
    // `Build` is pure and cannot ask the engine which cells are loaded,
    // but the answer changes how a point may be used: a synthetic point
    // inside the grid can be navmesh-checked and cover-tested, and one
    // outside it can only be accepted on distance. An invalid grid (the
    // player is indoors, or the read happened too early) makes every
    // point read as outside, which costs fidelity near the player and is
    // never unsafe.
    struct LoadedGrid
    {
        RE::FormID worldSpace = 0;
        std::int32_t minCellX = 0;
        std::int32_t maxCellX = 0;
        std::int32_t minCellY = 0;
        std::int32_t maxCellY = 0;
        bool valid = false;

        [[nodiscard]] bool Contains(RE::FormID ws, const RE::NiPoint3& pos) const;
    };

    // MAIN THREAD. Read the bounds of the attached exterior grid. Returns
    // an invalid grid indoors, which is a normal answer rather than a
    // failure.
    LoadedGrid ReadLoadedGrid(const MainThread::Token&);

    struct Point
    {
        RE::NiPoint3 position{};
        PointClass cls = PointClass::Direct;

        // Whether `position` is in a cell the engine currently has
        // attached, per the `LoadedGrid` handed to `Build`.
        bool insideLoadedGrid = false;

        // Set only for `PointClass::Fine`, so hop expansion over
        // `FineRoads::Graph::adjacency` can start from it without
        // searching for the node again.
        std::size_t fineNode = FineRoads::kInvalidNode;
    };

    struct Chain
    {
        // Ordered from the visitor to the player. `points.front()` is the
        // visitor's own origin and `points.back()` is the player.
        std::vector<Point> points;

        // Total scored cost, in the same mixed units the edge cost is in:
        // comparable between chains, not a distance.
        float cost = 0.0f;

        // False only when the graph could not be built at all — no
        // worldspace, or a degenerate request. The visitor-to-player line
        // exists to make this unreachable in practice; callers still have
        // to handle it, because "unreachable" is a claim about today's
        // inputs.
        bool valid = false;
    };

    // Build the chain. `worldSpace` is the one both endpoints are
    // measured in; `visitorOrigin` is `RoadRoute::ResolveOrigin`'s answer
    // for the visitor, not necessarily where they are standing.
    Chain Build(const LoadedGrid& grid,
                RE::FormID worldSpace,
                const RE::NiPoint3& visitorOrigin,
                const RE::NiPoint3& playerPos);
} // namespace NarrativeEngine::ApproachChain

namespace NarrativeEngine::ApproachChain_Testing
{
    // The same graph `Build` assembles, searched twice: once with the
    // A-star the module ships, once with a plain Dijkstra that uses no
    // heuristic at all.
    //
    // A pathfinder that returns *a* path on every fixture looks correct
    // and tells you nothing. Dijkstra is the oracle for whether the route
    // is the CHEAPEST one, and running it over the same graph in the same
    // call is what makes the comparison about the search rather than
    // about two different graphs. Exists for the test suite; production
    // never calls it.
    struct OracleResult
    {
        bool built = false;
        std::size_t nodeCount = 0;
        std::size_t edgeCount = 0;
        float astarCost = 0.0f;
        float dijkstraCost = 0.0f;
        std::vector<RE::NiPoint3> astarPath;
        std::vector<RE::NiPoint3> dijkstraPath;
    };

    OracleResult SearchBothWays(const ApproachChain::LoadedGrid& grid,
                                RE::FormID worldSpace,
                                const RE::NiPoint3& visitorOrigin,
                                const RE::NiPoint3& playerPos);
} // namespace NarrativeEngine::ApproachChain_Testing
