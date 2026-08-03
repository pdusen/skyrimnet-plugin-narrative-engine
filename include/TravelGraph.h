#pragma once

#include <cstddef>
#include <vector>

#include <RE/Skyrim.h>

// TravelGraph — a queryable road graph reconstructed from the engine's
// own precomputed long-distance pathing data.
//
// EXPERIMENTAL / DIAGNOSTIC. This module exists to answer one question:
// is the NAVI record's preferred-path data a usable road network? It is
// not yet consumed by any beat or action. Treat the API as provisional.
//
// ---------------------------------------------------------------------
// Where the data comes from
//
// Skyrim ships a single NAVI record (Navigation Mesh Info Map, 0x012FB4
// in Skyrim.esm) whose in-memory form is `RE::NavMeshInfoMap`. Two parts
// of it matter here:
//
//   * `BSNavmeshInfoMap::GetNavmeshInfo(id)` — resolves a navmesh to its
//     `BSNavmeshInfo*`.
//   * `BSPrecomputedNavmeshInfoPathMap::allPaths` — an array of ORDERED
//     chains of `BSNavmeshInfo*`. Each chain is a precomputed
//     long-distance route. In vanilla Skyrim there are ~100 such chains
//     and, plotted end to end, they trace the road network.
//
// The engine uses this to move actors that are travelling while outside
// the loaded cell grid, which is why NPCs on a long travel package
// appear to follow roads even when the player isn't there to see it.
//
// ---------------------------------------------------------------------
// Why the nodes are navmeshes and not NAVI's own points
//
// CommonLibSSE-NG only forward-declares `BSNavmeshInfo` — there is no
// member layout, so the representative point NAVI stores per navmesh is
// not readable through the bindings, and inventing offsets for an
// undocumented engine struct is not something we want load-bearing.
//
// So this module never dereferences a `BSNavmeshInfo`. It uses the
// pointer purely as an opaque identity key: build FormID ->
// `BSNavmeshInfo*` by calling `GetNavmeshInfo` for every `NavMesh` form,
// invert it, and translate each `allPaths` chain back into a sequence of
// navmesh FormIDs. Positions then come from the `NavMesh` forms
// themselves (`BSNavmesh::meshGrid` bounds, falling back to the vertex
// average, falling back to the parent cell's centre).
//
// The upshot is that node spacing is navmesh-sized — roughly one
// exterior cell, ~4096 units — so this is a road SKELETON, not a
// unit-accurate polyline.
//
// ---------------------------------------------------------------------
// Threading
//
// `Initialize` runs on the main thread at kDataLoaded, matching HoldGrid.
// After it returns the graph is const for the session; readers take the
// mutex anyway since the critical sections are tiny.
namespace NarrativeEngine::TravelGraph
{
    // One node per navmesh that participates in at least one preferred
    // path. Position is that navmesh's centre in world units.
    struct Node
    {
        RE::FormID navMesh = 0;
        RE::FormID worldSpace = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Sentinel for "no node" returned by FindNearestNode.
    inline constexpr std::size_t kInvalidNode = static_cast<std::size_t>(-1);

    // One-shot build. Idempotent — subsequent calls return without work.
    // Degrades to an empty graph (and logs why) if the NAVI record or
    // the navmesh form array is unavailable. Gated on
    // Settings::travelGraphEnabled; when disabled this is a no-op.
    void Initialize();

    std::size_t NodeCount();

    // Total number of undirected edges.
    std::size_t EdgeCount();

    // Returns nullptr when `index` is out of range.
    const Node* GetNode(std::size_t index);

    // Nearest node to a world position, restricted to `worldSpace`.
    // Distance is measured in the XY plane — elevation is deliberately
    // ignored, since a node's Z is a navmesh-wide average and comparing
    // it against a specific actor's Z is noise. Returns kInvalidNode if
    // the graph is empty or the worldspace has no nodes.
    std::size_t FindNearestNode(RE::FormID worldSpace, float x, float y);

    // Distance from `source` to every node, indexed by node. Unreachable
    // nodes hold infinity; an invalid source returns an empty vector.
    //
    // Exists so a caller scoring many candidates against one destination
    // pays for a single Dijkstra rather than one per candidate — which is
    // exactly what RoadRoute does when ranking frontier nodes.
    std::vector<float> DistanceField(std::size_t source);

    // Dijkstra over the graph, edge weight = XY distance between node
    // centres. Returns the node indices from `from` to `to` inclusive,
    // or an empty vector if either index is invalid or no route exists
    // (the graph is not fully connected — separate road systems and
    // separate worldspaces form separate components).
    std::vector<std::size_t> FindPath(std::size_t from, std::size_t to);
} // namespace NarrativeEngine::TravelGraph
