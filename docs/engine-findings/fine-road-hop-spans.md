# Hop count is a poor proxy for distance on Skyrim's road navmesh

## TL;DR

`FineRoads` nodes are the centroids of `kPreferred` navmesh triangles, and its
edges are triangle adjacency — so "two hops along the road" is a distance only
by accident. Measured over every exterior cell in Skyrim plus all three DLC:

| | 1 hop | 2 hops |
| --- | --- | --- |
| median | 139u | 226u |
| p99 | 399u | 665u |
| p99.99 | 721u | 1,217u |
| **max** | **923u** | **1,784u** |

The median two-hop reach is 226 units and the maximum is 1,784 — an eight-fold
spread. Any rule that needs to bound how far a hop walk travels has to say so in
units; a hop count will not do it.

## Why it matters

The visit arrival search expands off the approach chain by hop count to find
cover a few metres off an exposed road. Two things depend on how far that can
reach:

- **Direction.** Hops are undirected, so expansion off a chain node near the
  player can come back out behind them. What stops that being chosen is the
  arrival distance floor (`iVisitMarkerMinDistanceUnits`): a node across the
  player sits at most one two-hop reach away from them, so a floor above 1,784
  refuses every one of them on distance. At the shipped 3,000 that holds with
  1.7x to spare. At 1,600 it does not — four measured spans exceed it.
- **Plausibility.** A "neighbour a few metres off the road" that is actually
  1,784 units away is not the thing the expansion was reaching for.

A units bound on the expansion would make both independent of navmesh geometry,
including mod-added roads this measurement says nothing about.

## The measurement

Taken from the Spriggit export at `C:\Projects\spriggit-output\`, over
`Worldspaces/*/.../RecordData.yaml` in `Skyrim`, `Dawnguard`, `HearthFires` and
`Dragonborn`: 73,550 cell records, 1,912 navmeshes carrying road, 66,632
preferred triangles, 132,978 directed one-hop pairs.

Adjacency came from **shared vertex pairs** — two triangles are edge-adjacent
exactly when they share two vertices. The worst case is `FortAmol04` (cell
3,-1), where a 923-unit single hop and a 1,784-unit two-hop walk both occur.

Cross-mesh portal links are not counted. Triangles joined by a portal abut along
a cell boundary, so their spans are the same order and excluding them cannot
inflate the maximum.

## False lead: the export's EdgeLink fields are not triangle indices

The first attempt derived adjacency from `EdgeLink_0_1` / `_1_2` / `_2_0` and
reported a 4,738-unit single hop — which would have meant no floor could ever
bound the expansion. It was wrong. The giveaway: the two triangles shared **no
vertices**, so they were never edge-adjacent.

`src/FineRoads.cpp` already warns about this. In the export those slots hold a
triangle index in some cases and an index into a short portal array in others,
and the triangle's own `Flags` list is what distinguishes them — a triangle
carrying an `EdgeLink_2_0` *flag* has a portal index in that slot, not a
neighbour. The runtime reads `neighborTri` and `portalMesh`/`portalTri` as
separate fields and has no such ambiguity; the YAML does.

Derive adjacency from shared vertices when working from the export, and sanity
check any surprising span by asking whether the two triangles share an edge at
all.
