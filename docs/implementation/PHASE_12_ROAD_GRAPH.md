# Phase 12 — Road Graph

Two complementary road graphs reconstructed from data the engine already ships, plus the query layer over them,
so that future beats can place and move NPCs along Skyrim's actual road network instead of straight lines
between markers.

- **`TravelGraph`** — coarse, global, always resident. Built once at `kDataLoaded` from the `NAVI` record's
  precomputed preferred-path chains. ~620 nodes covering all of Tamriel's major roads.
- **`FineRoads`** — fine, local, follows the player. Built from `kPreferred`-flagged navmesh triangles in the
  loaded cell grid, rebuilt as cells load and unload.
- **`RoadRoute`** — the query layer over both, answering *"what can an NPC walk from here toward there, and
  where does it hand off?"*

No gameplay features. No LLM calls. No new beats. Nothing consumes `RoadRoute` yet — this phase builds the
capability the visit and courier beats will call into.

> **Doc status: retrospective.** This document was written after the implementation, and describes what was
> arrived at rather than a plan that was followed. The engine findings in **Design overview** were all
> established empirically during the work — several contradict what the CommonLibSSE-NG headers suggest, and
> those are called out where they matter.

---

## Why this phase exists

Several planned features need to know where roads are: an NPC dispatched to visit the player should appear to
arrive along the road from their home city, and leave along it. The engine exposes no road data — there is no
"road" record type, and `docs/engine-findings/navmesh-queries-in-commonlibsse-ng.md` established that
CommonLibSSE-NG binds no navmesh query functions at all.

The initial plan was to reconstruct the network by observation: spawn generic NPCs, send them between map
markers on travel packages, and record where they walked. That was abandoned once it became clear the data
already exists in two places, both of which are cheaper and exact:

- Skyrim ships **one `NAVI` record** (`0x00012FB4` in `Skyrim.esm`) whose `PreferredPathing` section holds ~100
  ordered chains of navmeshes. These are precomputed long-distance routes, and the engine uses them to move
  actors travelling outside the loaded cell grid — which is why an NPC on a long travel package still appears
  to follow roads when nobody is watching. Plotted end to end they trace the road network.
- Individual **navmesh triangles carry a `kPreferred` flag**, and the flagged triangles form the actual road
  surface at metre resolution.

The two are not redundant. `NAVI`'s chains are a *routing skeleton*: ~620 nodes for all of Tamriel, spaced
roughly one cell apart, major roads only, and visibly corner-cutting on bends. It contains no spurs at all,
because a track that dead-ends at a barrow is useless for cross-map routing and was never generated into it.
Measured over one cell block, the triangle data holds roughly two orders of magnitude more road detail.

The triangle data cannot replace the skeleton either: triangle geometry lives on `NavMesh` forms, and only
~15% of those are resident at `kDataLoaded`. Fine data is only ever available for loaded cells.

Hence two graphs, with different lifetimes.

---

## Scope

### In scope

- **`TravelGraph`** — the coarse global graph, plus `FindNearestNode` / `FindPath` (Dijkstra) /
  `DistanceField` / `GetNode`.
- **`FineRoads`** — the fine local graph: extraction, per-cell session cache, adjacency, frontier detection,
  and event-driven invalidation.
- **`RoadRoute`** — the query layer spanning both graphs: `Route` (hybrid fine-then-coarse plan) and
  `ResolveOrigin` (interior → exterior position via load door).
- **`BmpWriter`** — a shared 24-bit BMP writer, extracted from the copy `HoldGrid` had grown for its own
  partition dump. Both graphs write debug bitmaps through it.
- Debug bitmaps for both graphs, and a `[TravelGraph]` / `[FineRoads]` INI surface.
- Runtime calibration of the undocumented `BSNavmeshInfo` layout (see below) — measured each session against
  ground truth rather than hardcoded.

### Deferred (explicitly out)

- **Driving movement along the coarse path.** `RoadRoute` returns it, but nothing needs to follow it: the
  engine already moves actors outside the loaded grid along the same preferred-path data, so a travel package
  to the destination follows roads unaided. The coarse path is for estimating progress and direction.
- **Centerline thinning.** Fine nodes are raw triangle centroids, so the graph is a ribbon two or three nodes
  wide rather than a thinned path. Acceptable because the engine paths between successive travel-package
  targets on navmesh anyway; revisit only if routing needs it.
- **Disk persistence of fine data.** The cache is session-scoped. Persisting it keyed by a load-order hash
  would let coverage accumulate permanently across sessions.
- **Mod-added roads absent from `NAVI`.** Regenerating preferred pathing is a manual Creation Kit step that
  essentially nobody runs, so a mod that adds a road adds navmesh but does not extend the coarse chains.
  `FineRoads` picks such roads up automatically once the player is near them; the coarse graph does not.

---

## Design overview

### Coarse graph — `TravelGraph`

Built once at `kDataLoaded`, then const for the session.

1. **Locate `NAVI` and the `NavMesh` forms.** Neither is registered in `TESDataHandler`'s per-type form arrays
   — `FormType::Navigation` and `FormType::NavMesh` both come back **empty** on Skyrim SE, which is the first
   non-obvious finding here. Both are present in the global form table (`TESForm::GetAllForms`), so a single
   pass over it collects whatever the arrays did not provide.
2. **Calibrate `BSNavmeshInfo`.** See below.
3. **Node per navmesh.** `ckNavMeshInfoMap` covers all ~16,600 navmeshes the engine knows about — not just the
   ~2,500 with resident forms — so it, not the form array, is what makes the graph whole. Position comes from
   the calibrated point, falling back to the centre of the cell the engine filed the navmesh under.
4. **Edges from `allPaths`.** Each entry is an ordered chain; consecutive entries become an edge. Chains that
   share road merge into real junctions because deduplication is on node identity.
5. **Compaction.** Navmeshes no chain ever touched are dropped, leaving only the road network. On vanilla plus
   DLC this yields **622 nodes / 636 edges from 100 chains**, with zero unmapped chain entries.

#### The `BSNavmeshInfo` layout problem

CommonLibSSE-NG forward-declares `BSNavmeshInfo` and defines no members, but the position we need is inside it.
Hardcoding offsets would be a guess, and would rot silently across game versions.

Instead the module **measures them at startup**. The minority of navmeshes with a resident `NavMesh` form give
pairs of (`BSNavmeshInfo*`, known position from `meshGrid` bounds, known FormID). Every 4-byte-aligned offset in
the first `0x80` bytes is scored by mean XY error against those samples; an offset matching within one cell for
a supermajority wins. The navmesh FormID offset is located the same way as an independent cross-check — two
fields landing where expected is far stronger evidence than one.

On Skyrim SE 1.6.x this finds **FormID at `0x00`, `NiPoint3` at `0x04`**, with a mean error of ~190 units over
2,059 of 2,060 samples. That residual is expected: `NAVI`'s stored point is a representative point for the
navmesh, not the `meshGrid` midpoint being compared against. Those values are **not relied on** — if a future
runtime moves the fields, calibration finds the new ones; if it finds nothing, every node falls back to a
cell-centre position and the graph still connects at 4096-unit granularity.

Reads use SEH (`SafeReadBytes`) so a speculative offset returns false rather than crashing, and a resolved point
more than two cells from the cell the engine filed it under is rejected in favour of the cell centre.

#### `ckNavMeshInfoMap` key packing

Read directly off live data and confirmed against negative coordinates and a second worldspace:

```text
bits [63..32] = worldspace FormID
bits [31..16] = cellX as int16
bits [15..0]  = cellY as int16
```

**This is not the field order `HoldGrid` uses for its own key** (which packs Y high, X low). Do not copy one to
the other.

### Fine graph — `FineRoads`

Covers the loaded cell grid and moves with the player.

**Nodes** are the centroids of `kPreferred` triangles. **Edges** come from same-mesh triangle adjacency plus
cross-mesh portals. **Frontier nodes** are those whose triangle has a portal into a navmesh we have not
extracted — the precise point at which a traveller passes out of fine coverage and onto the coarse graph. Note
what is *not* a frontier: a flagged triangle whose neighbour is unflagged is the side of the road ribbon, and a
portal into a loaded mesh with no flagged triangle behind it is a genuine dead end.

#### Triangle edge encoding

`BSNavmeshTriangle::triangles[e]` means one of two different things depending on the matching `kEdgeN_Link`
flag: with the flag set it indexes `extraEdgeInfo` (a portal to another navmesh); without it, it is a triangle
index within the same mesh, and `0xFFFF` means no neighbour.

That reading is **inferred from the record format**, not documented in CommonLibSSE-NG. It is corroborated by
the Spriggit export — link-flagged slots hold small values consistent with an index into a short portal array,
unflagged slots hold triangle-range values or `-1`. Every index is bounds-checked, so a misreading costs
adjacency rather than stability. Disconnected fragments where a continuous road is expected would be the first
symptom.

#### Lifecycle and invalidation

Rescans are event-driven. Three sinks — `TESCellFullyLoadedEvent`, `TESLoadGameEvent`, `TESFastTravelEndEvent`
— each do nothing but set an atomic flag, which the next Tick consumes. This coalesces naturally: crossing one
cell boundary fires the cell event once per newly loaded cell, and all of them collapse into a single rescan.

**There is no cell-unload event.** `TESCellFullyLoadedEvent` is load-only and `TESCellAttachDetachEvent` is
per-*reference* (high volume, wrong granularity). So the events cannot describe the active set themselves —
they only say "something probably changed", and a scan of `TES::gridCells` remains the source of truth. That
scan is ~25 `GetCell` calls and a set compare, so it was never the expensive part.

A slow backstop rescan (default 180s) covers the one gap the events leave: an unload unaccompanied by any load,
which would otherwise leave stale cells in the active set indefinitely. It may be dead weight — the grid shifts
as a unit — but the failure it guards against is silent.

Extraction results are **cached per cell for the session**, so crossing back over a boundary costs nothing.
Cells that unload leave the active graph but stay cached. The active graph is rebuilt wholesale rather than
patched incrementally; at ~25 cells and a few dozen flagged triangles each it is small, and a full rebuild
avoids a class of incremental-update bugs for no meaningful cost.

#### Threading

`Poll` runs on the plugin thread. Reading navmesh data touches engine memory, so extraction happens inside a
`MainThread::Run`. The cache mutex is **never** held across that call — the set of already-cached cells is
copied out first, so there is no lock-ordering hazard between plugin and main.

### Query layer — `RoadRoute`

`Route(worldSpace, from, to)` returns an ordered list of fine points to walk, plus the coarse continuation.

The interesting decision is which frontier node to hand off at. Picking by bearing — "which one points toward
the destination?" — gets forks wrong: at a crossroads two branches can leave at almost the same angle and lead
to wildly different journeys. So each frontier `f` is scored on the whole trip:

```text
cost(f) = fine(start -> f)                 walked, true path length
        + join(f -> nearest coarse node)   the ungraphed gap
        + coarse(that node -> destination) the rest of the trip
```

and the cheapest wins, so the branch that *joins the network better* beats the branch that merely points the
right way. The coarse term comes from one `DistanceField` call seeded at the destination, making each frontier
a lookup rather than another search.

Degradation is staged rather than all-or-nothing:

- Destination inside fine coverage → fine-only path, no handoff.
- No fine graph for this worldspace → coarse-only.
- No frontier reachable from the start (the player at the end of a spur) → coarse-only, since a fine path
  would walk the NPC into a dead end.

**Threading.** `Route` is a pure query over our own structures and takes no token. `ResolveOrigin` walks a
cell's references, so it is engine-touching and takes a `MainThread::Token`.

`ResolveOrigin(ref)` places an arbitrary reference on the network. Outdoors that is its own position; indoors it
follows a load door and returns the arrival point on the far side — where the occupant would actually emerge,
which is what makes an NPC arriving there read correctly. Interior-to-interior doors are skipped, and a cell
with no load door at all falls back to its location's map marker.

---

## Settings

`[TravelGraph]`

| Key | Default | Purpose |
| --- | --- | --- |
| `bTravelGraphEnabled` | `true` | Build the coarse graph at all. |
| `bTravelGraphDebugBitmap` | `true` | One BMP per worldspace: nodes black, edges gray, HoldGrid partition as background. |
| `iTravelGraphBitmapUnitsPerPixel` | `256` | Auto-raised if it would exceed 4096px on a side. |
| `bTravelGraphLogCalibration` | `false` | Log runner-up offsets. Calibration itself always runs. |

`[FineRoads]`

| Key | Default | Purpose |
| --- | --- | --- |
| `bFineRoadsEnabled` | `true` | Build the local graph at all. |
| `iFineRoadsBackstopSeconds` | `180` | Backstop rescan only; normal path is event-driven. |
| `bFineRoadsDebugBitmap` | `true` | Dump the active graph on change: nodes red, frontier blue, edges gray. |
| `iFineRoadsBitmapUnitsPerPixel` | `16` | Much finer than the coarse bitmap — this covers ~5x5 cells. |

---

## File map

```text
include/BmpWriter.h      shared 24-bit BMP writer (moved out of HoldGrid)
include/TravelGraph.h    coarse graph API
src/TravelGraph.cpp      NAVI resolution, layout calibration, chain walk, bitmap
include/FineRoads.h      fine graph API
src/FineRoads.cpp        triangle extraction, cell cache, adjacency, sinks, bitmap
include/RoadRoute.h      hybrid query API
src/RoadRoute.cpp        frontier scoring, path reconstruction, interior origin resolution
```

Wiring: `TravelGraph::Initialize()` and `FineRoads::Initialize()` at `kDataLoaded` in `Plugin.cpp` (after
`HoldGrid::Initialize()`, which the coarse bitmap's background depends on); `FineRoads::Poll` in `Tick.cpp`.
`RoadRoute` has no wiring and no state of its own — it is a pure library over the other two, called on demand.

---

## Implementation plan

- [x] **Step 1 — `TravelGraph`: coarse graph from `NAVI`.** Global-form-table resolution, chain walk, edge
      dedup, compaction, per-worldspace bitmap with HoldGrid background.
- [x] **Step 2 — `BSNavmeshInfo` runtime calibration.** Ground-truth sampling from resident forms, offset
      search, FormID cross-check, SEH-guarded reads, cell-centre fallback.
- [x] **Step 3 — `FineRoads`: extraction, cache, adjacency, frontier detection.** Per-cell session cache,
      wholesale rebuild, debug bitmap.
- [x] **Step 4 — Event-driven invalidation.** Three sinks plus the backstop rescan.
- [x] **Step 5 — Hybrid routing query.** `RoadRoute::Route`, frontier scoring on total journey cost, staged
      degradation to coarse-only.
- [x] **Step 6 — Interior origin resolution.** `RoadRoute::ResolveOrigin`, load-door traversal with a
      location-marker fallback.

---

## Open questions

- **Branch selection at forks is the main quality risk, and is untested in play.** Scoring on total journey
  cost should beat a bearing test — the branch that joins the network better wins over the one that merely
  points the right way — but no route has yet been walked by an actual NPC.
- **Whether the backstop rescan is needed at all.** The rebuild log line does not currently record what
  triggered it, so this is not yet observable. Adding a trigger tag would settle it empirically.
- **Whether the coarse graph's cost model biases routing.** Coarse distances underestimate (corner-cutting)
  while fine distances are true path lengths, so a route scored across both mixes the two. The underestimate
  should be roughly proportional and so preserve ranking between candidate frontier nodes, but this has not
  been verified.
