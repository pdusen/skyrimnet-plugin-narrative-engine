#pragma once

#include <PluginThread.h>

#include <RE/Skyrim.h>

#include <cstddef>
#include <vector>

// FineRoads — high-resolution road graph for the currently loaded cells.
//
// EXPERIMENTAL. The routing query that consumes this is not written yet;
// right now the module builds and reports the graph so its accuracy can
// be checked against the game world.
//
// ---------------------------------------------------------------------
// Why this exists
//
// `TravelGraph` reconstructs Skyrim's road network from the NAVI record's
// precomputed preferred-path chains. That data is a long-distance routing
// skeleton: ~620 nodes for all of Tamriel, spaced roughly one cell apart,
// covering major roads only. It has no spurs — a track that dead-ends at
// a barrow is useless for cross-map routing, so Bethesda never generated
// one into it.
//
// The detail is in the navmesh itself. Individual triangles carry a
// `kPreferred` flag, and the flagged triangles form the actual road
// surface: side paths, dead ends, and the true curve of the road rather
// than the skeleton's corner-cutting polyline. Measured against one cell
// block, the triangle data holds roughly two orders of magnitude more
// road detail than the skeleton does over the same ground.
//
// The catch is availability. Triangle geometry lives on `NavMesh` forms,
// and only ~15% of them are resident at kDataLoaded — the rest appear
// only as their cells load. So this graph is necessarily local: it covers
// the loaded cell grid and moves with the player.
//
// ---------------------------------------------------------------------
// Lifecycle
//
// A Tick-driven poll samples `TES::gridCells`, diffs it against the
// active set, and extracts any newly loaded cell. Extraction results are
// cached per cell for the session, so revisiting costs nothing —
// backtracking across a boundary is common. Cells that unload leave the
// active graph but stay in the cache.
//
// The active graph is rebuilt lazily when the active set changes rather
// than patched incrementally. At ~25 loaded cells and a few dozen flagged
// triangles each it is a small structure, and a full rebuild avoids a
// class of incremental-update bugs for no meaningful cost.
//
// ---------------------------------------------------------------------
// Frontier nodes
//
// A node is a frontier when the road leaves the loaded region: its
// triangle has a cross-mesh portal whose target navmesh we have not
// extracted. That is a precise, data-driven boundary rather than "nodes
// near the edge of the grid" — it marks exactly the points where a
// traveller would pass out of fine coverage and onto the coarse graph.
//
// Note the distinction from a road simply ending. A flagged triangle
// whose neighbour is unflagged is the *side* of the road ribbon, and a
// portal into a loaded mesh with no flagged triangle behind it is a
// genuine dead end. Neither is a frontier.
//
// ---------------------------------------------------------------------
// Threading
//
// `Poll` runs on the plugin thread. Reading navmesh data touches engine
// memory, so extraction happens inside a `MainThread::Run`; the module's
// own state is mutated afterwards on the plugin thread. The cache mutex
// is never held across that call.
namespace NarrativeEngine::FineRoads
{
    struct Node
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        // The road continues past here into cells we have not loaded.
        // These are the handoff points to TravelGraph's coarse network.
        bool frontier = false;
    };

    inline constexpr std::size_t kInvalidNode = static_cast<std::size_t>(-1);

    struct Graph
    {
        std::vector<Node> nodes;
        std::vector<std::vector<std::size_t>> adjacency;
        RE::FormID worldSpace = 0;

        [[nodiscard]] bool empty() const
        {
            return nodes.empty();
        }
    };

    // Idempotent. Registers the module; the poll does the work.
    void Initialize();

    // Samples the loaded cell grid, extracts newly loaded cells, and
    // drops unloaded ones from the active set. No-op while the game is
    // paused or the player is in an interior (no exterior grid to read).
    void Poll(const PluginThread::Token&, double unpausedElapsedSeconds);

    // A copy of the current active graph. Empty indoors, or where no
    // loaded cell carries flagged road triangles. Copying is deliberate:
    // callers route over this occasionally, not per frame, and a
    // snapshot frees them from holding the module's lock.
    Graph Snapshot();

    // Cheap enough to poll for logging without taking a snapshot.
    std::size_t NodeCount();
} // namespace NarrativeEngine::FineRoads
