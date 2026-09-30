# Phase 16 — The approach chain

A rework of how a visit decides where its visitor appears. `VisitArrivalPoint` currently uses the road
graphs to pick a *direction* and then searches a band of ground near the player. This phase makes the
route itself the object: one unbroken chain of points from where the visitor actually is to where the
player actually is, and the arrival point becomes a choice along that chain rather than a search beside it.

---

## Why this phase exists

### The route is computed and then discarded

`RoadRoute::Route` already builds both halves of a journey. `Plan::finePath` holds walkable positions from
the player outward to the frontier; `Plan::coarsePath` holds coarse node indices from the handoff to the
destination. They are never joined:

- They are **different types**. One is world positions, the other is graph indices.
- They are **ordered from the player**, not from the visitor.
- The **join between them emits no point at all.** `Route` measures that gap — the `join` term in its
  frontier scoring is exactly the distance from the last fine node to the first coarse node — and then
  throws the measurement away. Nothing occupies the space between the two halves.

So the one thing a route is for, an ordered list of places between A and B, does not exist in any form. The
coarse half is consumed twice and never as a position: once to score which frontier to hand off at, and once
by `BearingHome`, which walks it only to find the first node 1,000 units clear of the player and return it as
an angle.

### Everything is clipped to the loaded neighbourhood

`kMaxReachUnits` is 8,000 and `WalkFinePath` discards any candidate past it, because past the loaded grid
there is no navmesh to check and no geometry for the cover raycast to hit. The result is a module that models
only the last 8,000 units of a journey that may be 100,000 long, and has nothing to say about the rest.

That is defensible for placing an actor. It is not enough to answer "where along their way here could this
person plausibly be", which is the question a visit is really asking.

### The band is the wrong selection rule

Today: gather every fine node inside `[iVisitMarkerMinDistanceUnits, iVisitMarkerMaxDistanceUnits]`, prefer
in-band, hold beyond-band as a fallback pool, reject past 8,000. The band's ceiling is a preference already,
which is a sign it was never really a band.

What the design wants is simpler and gives different answers: walk outward from the player and take the
**first** point that is far enough away and hidden. Where the nearest acceptable point happens to be 3,000
units out, the band and the walk agree; where it is 6,000, the band has to be overridden to get there.

---

## Scope

### In scope

- **A chain type.** One ordered sequence of points from the visitor's world-map origin to the player's
  position, with each point carrying the class it belongs to.
- **Chain construction** across four segments: coarse run, bridge, fine run, and a straight-line fallback.
- **Off-path candidates** by hop expansion over `FineRoads::Graph::adjacency`.
- **A single selection walk** outward from the player, replacing the band and the two tiers under it.
- **A defined fallback ordering** for `StuckRecovery`, which is the remaining chain outward of the chosen
  point — walking the visitor back along their own path, which is what that module already promises.
- Retiring `SampleBearingArc` and `BearingHome`. Both exist to manufacture candidates where the route had
  none; the chain has candidates everywhere. `SampleBearingArc` is local to `VisitArrivalPoint`
  (`AmbushSpawnPoints` has its own ring search), so nothing else loses a primitive.

### Deferred (explicitly out)

- **The interior and walled-city cases.** `Tier::Doorstep`, `Tier::CityApproach` and `Tier::CityGate` keep
  working exactly as they do now and short-circuit before any chain is built. They are not improved here and
  not regressed.
- **Anything that moves the visitor along the chain.** The chain makes progressive advancement *possible*;
  this phase does not do it. One warp, as today.
- **Changing background travel.** Out of reach — see the note under the bridge.
- **The cover gate's tuning.** `iVisitArrivalCoverRadiusUnits` was calibrated against a real log in Phase 14
  and is carried over unchanged.

---

## Design

The route is not assembled segment by segment. Everything that could be walked on goes into **one weighted
graph**, each node carrying a difficulty, and the route is whatever a pathfind over that graph returns. The
segments below describe what is *put into* the graph, not the order the answer comes out in.

That distinction is the point: hand-assembling "coarse, then bridge, then fine" forces a shape on the answer.
A weighted search finds the cheapest real route and follows roads because roads are cheap, not because the
construction order told it to.

### The graph

| Difficulty | What goes in |
| --- | --- |
| **1** | Every coarse road node, and its edges |
| **2** | Every loaded fine road node, and its edges |
| **3** | Bridge points, the player's world-map position, the visitor's, and any connectors they need |
| **4** | Two direct lines out of the visitor, one node every 512 units — see below |

**Multiple disconnected fine networks.** The loaded grid can hold several unconnected pieces of road. Prefer
the one holding the closest point to the player, and bridge to that one — **both** bridges, the coarse-to-fine
one and the visitor-to-fine one, attach to the same selected network, at whatever point on it each is
cheapest to reach. The rule governs *bridge construction*, not the search: an unbridged network is an island
the search can never enter, so adding the others is cost without reach.

**Two direct lines, both out of the visitor, both difficulty 4 at 512-unit spacing.**

- **Visitor to the selected fine network.** Which network that is does not change — it is still the
  player-adjacent one chosen below, by closest point to the player. What differs is only the *attach point*:
  this line meets that same network at whichever of its nodes is nearest the visitor. Network selection is a
  player-side decision; where a bridge touches it is a per-bridge one.

  This is the useful line. It lets somebody off-road but reasonably close cut straight to the road near the
  player and finish the approach on difficulty 2, instead of detouring out to the coarse skeleton and back.
  For a visitor a few thousand units from the loaded grid it is usually the cheapest route in the graph.
- **Visitor to the player.** The connectivity guarantee.

They are not redundant. The first dominates the second whenever a fine network sits between the two — it is
shorter and hands off to cheap road, where the visitor-to-player line stays at difficulty 4 the whole way. So
in practice the visitor-to-player line wins only when the fine network lies *behind* the visitor, or when
there is no fine graph loaded at all: the player indoors, a save just restored, open country away from any
road. That last case is the one that makes it load-bearing, because it is the only segment that exists
without a fine network to bridge to.

**Connectivity, and the direction guarantee.** With the visitor-to-player line present the graph is always
connected, so the search can never fail to produce a route and every stage downstream may assume a chain
exists. That does not weaken the module's promise never to place a visitor arriving from the wrong direction,
because both lines run *outward from the visitor* — a point on either is on the bearing home by
construction. They are the worst routes available, not wrong ones.

### The cost function

Difficulty is a property of nodes; a search needs a cost per edge. The reading this phase uses:

```text
cost(edge) = euclidean length x max(difficulty of its two endpoints)
```

`max` rather than an average, so that one expensive node is genuinely expensive to pass through rather than
being diluted by a cheap neighbour. The difficulty ratings do not make this decision for us, and it is worth
confirming before implementation — an average would make a long cheap approach to a single costly node far
more attractive than intended.

**The ratios matter, not the labels.** Worked against real numbers, for a visitor 100,000 units away:

- Direct line, difficulty 4: `100,000 x 4 = 400,000`.
- A road route is typically 1.2 to 1.5 times straight-line, so about 130,000 units at difficulty 1.

The road wins by roughly three to one, which is the intended margin: roads are preferred decisively, and the
direct line is reached for only when the road network genuinely cannot get there. The fine and bridge
segments are short enough that their higher difficulties barely register in a cross-map total — they matter
near the player, which is exactly where the design wants fidelity rather than speed.

### The search

A-star over the composite graph, with euclidean distance to the player as the heuristic. That heuristic stays
admissible only while it is not scaled by any difficulty above 1, which is the reason difficulty 1 is the
floor of the scale rather than a middle value.

The graph is small enough that this is not a performance question. Measured on a real session: the Tamriel
coarse graph is **622 nodes / 636 edges**, a loaded fine graph was **110 nodes over 25 cells**, and a direct
line across 100,000 units at 512-unit spacing adds about 196. Under a thousand nodes in total, against a
`RoadRoute::Route` that already runs two Dijkstras over comparable data on the plugin thread.

### The chain

The chain is the search's result: an ordered list of points from the visitor to the player, each carrying the
class of graph it came from. Because difficulty made roads cheap, it reads coarse in the middle and dense
near the player without that having been imposed on it.

**Origin** is `RoadRoute::ResolveOrigin`, unchanged: the visitor's own position outdoors, the far side of
their load door indoors, or their home location's map marker when neither is available.

### Difficulty and class are different numbers, inversely related

Worth stating plainly, because merging them later would look like a simplification and would be a bug.

- **Difficulty** decides *which way the route goes*. Coarse is cheapest.
- **Class** decides *which point along it the visitor is warped onto*. Coarse is worst — it is unvalidated
  ground outside the loaded grid.

A coarse node is the best thing to route through and the last thing to stand on. One number cannot carry both
meanings.

### The bridge, and why 512

The bridge joins the coarse network to the loaded fine network, and it exists for one specific outcome: when
no fine node in the loaded grid is sufficiently hidden, the visitor can still be placed on a **connected**
point just beyond the grid, in the direction they are coming from, rather than the visit declining.

The spacing is 512 units, chosen against a measured figure rather than by feel. Background travel advances an
unloaded actor in a **fixed step of 866 units**, roughly every 1.8 game minutes, and that step is indifferent
to movement speed — `SpeedMult` raised to 500, with the engine's own `GetWalkSpeed`/`GetJogSpeed`/`GetRunSpeed`
reporting 412/1880/1895, changed neither the step nor its cadence. At 512-unit spacing the innermost bridge
point is closer to the loaded grid than one step is long, so a visitor placed there crosses into the grid on a
single background tick and becomes a real, loaded, walking actor immediately.

(That measurement came from an instrumented run in an earlier investigation whose harness was removed; the
number is recorded here because this design depends on it, and it is cheap to re-measure if it is ever in
doubt.)

Two consequences have to be accepted or designed around:

**Bridge points cannot be validated.** They are outside the loaded grid by construction, which is their
purpose, and both in-grid gates need loaded cells: `StuckRecovery::IsOnNavmesh` reads navmesh out of them, and
the cover raycast needs geometry to hit. A bridge point is therefore accepted on distance alone, which is
consistent — beyond view distance, cover is moot — but carries **no standability guarantee**. `kMaxReachUnits`
exists today precisely to avoid placing an actor on ground nobody checked.

*Mitigation to decide:* prefer real **coarse nodes** wherever the bridge line passes near one, and use
synthetic fill only between them. Coarse nodes derive from the game's own preferred-path data, so they are
walkable by construction; the synthetic points are the unvalidated part. This is the open question below.

**Bridge length buys a wait.** A visitor placed beyond the grid is in low process and covers the remaining
distance at the background rate — roughly 25,000 units per game hour gross, and less in net progress because
the simulation wanders. A bridge point 4,000 units out is a few game minutes; one 12,000 units out is closer
to half a game hour before they are even near. The far end of the bridge is where a visit stops feeling like
a response to anything, so the bridge wants a cap rather than reaching as far as the geometry allows.

### Off-path candidates

Every fine node within **2 hops** of a node on the chain's fine segment is also a candidate, at lower
priority.

`FineRoads::Graph::adjacency` already holds a neighbour list per node, so this is a bounded breadth-first walk
from each chain node and needs no new graph work.

The reason it earns its place: the fine graph is a **road ribbon**, so a 1-hop neighbour is usually the other
side of the same road, and a 2-hop neighbour is a few metres off it. That is exactly where cheap cover lives —
a spot behind a rock four metres off the road, when the road itself is in plain view. It converts a declined
visit into a placed one without moving the visitor anywhere the player would find strange.

**Two constraints it must inherit.** Hops are undirected, so an expanded node can sit back toward the player,
or behind them. That is the precise failure the Phase 14 approach corridor was added to prevent, where a
bearing aimed at the nearest scrap of road instead of the road home. Off-path candidates therefore pass the
same distance and corridor constraints as chain nodes; hop expansion widens the pool, it does not relax it.

### Selection

One walk, outward from the player, along the chain.

1. Take the **first** point at least `iVisitMarkerMinDistanceUnits` from the player that is **obscured** —
   behind cover, or far enough away and outside the player's view that cover is unnecessary
   (`kUnseenDistanceUnits`, 3,000 today).
2. If nothing along the chain is obscured, take the **first point beyond the player's maximum view distance**.
   Bridge, coarse and direct-line points satisfy this by construction, which is what they are for.
3. If the chain yields nothing at all, decline. A declined visit is a normal outcome and not an error, and
   there is deliberately no "arrive from anywhere" fallback — a visitor appearing opposite their own home is
   the exact tell this module exists to remove.

At equal standing, the order is:

| Rank | Class | Gates applied |
| --- | --- | --- |
| 1 | Fine, on chain | Navmesh re-check, cover or unseen-distance, band, corridor |
| 2 | Fine, off chain (by hop count) | The same, plus hop count as the tiebreak |
| 3 | Synthetic, **inside** the loaded grid | The same — being off-road does not make it unverifiable |
| 4 | Synthetic, **outside** the loaded grid | Distance only — nothing else is answerable there |
| 5 | Coarse | Distance only |

**Rank 3 is not a special case for one segment.** A synthetic point is gated by *where it is*, not by which
line produced it. Both direct lines, and the coarse-to-fine bridge, end inside the loaded grid, so their last
few nodes can be navmesh-checked and cover-tested like any fine node — and should be, because a validated
off-road spot beats an unvalidated one every time. Gating them by segment instead of by position would
throw away the one part of each synthetic run that can actually be verified.

### Stuck recovery

`StuckRecovery::Escort` keeps its contract unchanged. What changes is where its ladder comes from: the
fallbacks are the remaining chain points **outward of the chosen one, in chain order**, so each escalation
walks the visitor back along the path they were supposed to have travelled. That is what the module's header
already asks for and says not to re-sort, and the chain supplies it directly instead of the current
near-player candidate pool.

Its final rung — stepping in toward the goal along a bare line by `closeInStepUnits` — stays as the answer for
a visitor who is stuck with no chain left outward of them.

### What this replaces

| Today | After |
| --- | --- |
| `Tier::FineRoad`, `Tier::CoarseBearing` | One selection walk over the chain |
| `WalkFinePath` band with a preference | First-acceptable, outward |
| `SampleBearingArc` | Hop expansion (retired) |
| `BearingHome` | The graph's own coarse nodes (retired) |
| `Route`'s two-container plan | One weighted graph and one search |
| The unfilled fine-to-coarse join | The bridge, at difficulty 3 |
| Declining when nothing is near | A graph that is always connected |

`Tier` keeps `None`, `Doorstep`, `CityApproach` and `CityGate`. The two road tiers are replaced by the
candidate class of whichever point won, which is strictly more informative in the log.

---

## Open engine questions

1. **Is `max` the right way to turn node difficulty into edge cost?** The alternative is an average, which
   would make a long cheap approach to one costly node far more attractive than intended. It decides routing
   behaviour everywhere, so it wants settling before any code.
2. **May the bridge place a visitor on unvalidated ground?** Or is it restricted to real coarse nodes, with
   synthetic fill used only to *order* the chain and never as a warp target? This decides whether the bridge
   is a placement tier or only a connectivity device.
3. **How far out may the bridge reach** before the resulting background-travel wait makes the visit
   pointless? A number, in units, informed by the ~25,000 u/game-hour rate.
4. **Is land height answerable outside the loaded grid?** `StuckRecovery::IsStandable` reads
   `TES::GetLandHeight`, which Phase 14 found has no landscape to answer from when the player is indoors. If
   it also answers nothing outside the grid, question 2 resolves itself and the bridge is coarse-only.
5. **Does the corridor constraint have meaning for off-chain nodes at 2 hops?** The corridor was built to
   choose between road branches; 2 hops may be inside its tolerance everywhere, making the check free but
   also useless.
6. **Does the direct line need its own spacing?** It shares 512 with the bridge today, which across 100,000
   units is ~196 nodes for a segment the search should almost never choose. A coarser spacing there would
   cost nothing in quality and shrink the graph.

---

## Settings

Carried over unchanged: `iVisitMarkerMinDistanceUnits`, `iVisitMarkerMaxDistanceUnits`,
`iVisitArrivalCoverRadiusUnits`.

New:

| Setting | Default | Purpose |
| --- | --- | --- |
| `iVisitChainBridgeSpacingUnits` | `512` | Bridge and direct-line spacing; under one 866-unit background step |
| `iVisitChainHopRadius` | `2` | How far off the fine chain to expand for candidates |
| `iVisitChainMaxBridgeUnits` | TBD | How far beyond the grid the bridge may place a visitor |
| `iVisitChainDifficultyCoarse` | `1` | Cost multiplier for coarse road nodes |
| `iVisitChainDifficultyFine` | `2` | Cost multiplier for loaded fine road nodes |
| `iVisitChainDifficultyConnector` | `3` | Bridge points, player and visitor nodes, their connectors |
| `iVisitChainDifficultyDirect` | `4` | Both direct lines out of the visitor |

The four difficulties are settings rather than constants for the same reason `iVisitArrivalCoverRadiusUnits`
became one in Phase 14: the ratios only settle against a real log, and they are the knob that decides whether
a visitor takes the road or cuts across country.

`bVisitArrivalAllowCoarseBearing` is retired with `SampleBearingArc`, or repurposed as "may the chain place
outside the loaded grid at all", which is the same switch aimed at the new design.

---

## File map

```text
include/ApproachChain.h       the weighted graph, the search, and the chain it returns
src/ApproachChain.cpp
src/ApproachChain.engine.test.cpp
include/VisitArrivalPoint.h   Tier loses its two road entries, gains candidate class
src/VisitArrivalPoint.cpp     selection walk replaces WalkFinePath / SampleBearingArc / BearingHome
src/VisitArrivalPoint.engine.test.cpp
include/RoadRoute.h           possibly a chain-shaped query beside Route
src/RoadRoute.cpp
include/Settings.h            three new keys
src/Settings.cpp
statics/SKSE/Plugins/NarrativeEngine.ini
CMakeLists.txt                ApproachChain in NARRATIVEENGINE_MOCKED_SOURCES
```

---

## Implementation plan

To be written.

---

## Done condition

To be written.
