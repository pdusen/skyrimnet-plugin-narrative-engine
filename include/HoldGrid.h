#pragma once

#include <cstdint>

#include <RE/Skyrim.h>

// HoldGrid — precomputed exterior-cell → hold FormID lookup.
//
// Built once at kDataLoaded by:
//   1. Enumerating every exterior CELL record in every loaded
//      worldspace (via TESWorldSpace::cellMap).
//   2. For each cell that has a BGSLocation chaining up to
//      LocTypeHold via parentLoc, seeding the
//      (worldspace, X, Y) → hold FormID entry.
//   3. Round-robin (queue-based) BFS from all seeds simultaneously,
//      filling 4-neighbor cells with the same hold. Cells assigned
//      once are never overwritten — first-touch wins, which under
//      Manhattan distance approximates Voronoi.
//
// Result: an in-memory partition where every reachable exterior
// coordinate maps to a hold — including cells whose own BGSLocation
// doesn't happen to chain up to LocTypeHold (many vanilla wilderness
// cells fall into this gap, which is why the parent-walk in `Region`
// alone leaves visible holes).
//
// Used by Region::ForPlayer as a fast-path lookup before falling back
// to the parent-walk on the current location.
//
// Threading: Initialize runs on the main thread at kDataLoaded and is
// mutex-guarded. After Initialize returns, the table is const for the
// rest of the session — readers still take the mutex for correctness
// (the critical section is a single unordered_map lookup).
namespace NarrativeEngine::HoldGrid
{
    // One-shot build. Idempotent — subsequent calls return without
    // work. Logs stats (seed count, filled count, elapsed ms) at
    // completion. On any resolve failure (missing LocTypeHold keyword,
    // no TESDataHandler) the module degrades to an empty table and
    // every lookup returns 0.
    void Initialize();

    // Look up the hold FormID at a specific exterior cell. Returns 0
    // when the cell isn't in the grid (unclassified worldspace, or an
    // isolated cell the BFS couldn't reach from any seed, or the
    // module isn't initialized).
    RE::FormID LookupCell(RE::TESWorldSpace* worldspace, std::int16_t cellX, std::int16_t cellY);

    // Look up the hold FormID at a world-coordinate position. Converts
    // to cell coords via floor(x / 4096) / floor(y / 4096) and calls
    // LookupCell.
    RE::FormID LookupWorldPosition(RE::TESWorldSpace* worldspace, float x, float y);

    // Convenience: look up the player's current exterior cell. Returns
    // 0 for interior cells (they don't have grid coordinates —
    // callers should fall back to the parent-walk on the player's
    // location for those).
    RE::FormID LookupPlayer();
} // namespace NarrativeEngine::HoldGrid
