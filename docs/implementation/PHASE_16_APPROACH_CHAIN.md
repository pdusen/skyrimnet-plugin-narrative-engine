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

### The chain

One ordered list, from the visitor to the player. Each entry carries a position and a class, and the class
decides which gates apply and where it sits in the ordering.

```text
visitor origin
  │
  ├─ Coarse        nearest coarse node to the origin, then connected coarse nodes
  │                toward the player, sparse, as far as the coarse node nearest
  │                the player
  │
  ├─ Bridge        straight line from that last coarse node to the nearest loaded
  │                fine node, one point every 512 units
  │
  ├─ Fine          connected fine nodes from there in to the player, dense
  │                (navmesh triangle centroids)
  │
  └─ player
```

Where there is no fine graph at all — the player indoors, a save just loaded, open country away from a road —
the fine segment is replaced by the same straight-line fill at the same 512-unit spacing, run from the last
bridge or coarse point to the player. Either way the chain is unbroken and gets denser as it approaches the
player, which is the property the whole design rests on.

**Origin** is `RoadRoute::ResolveOrigin`, unchanged: the visitor's own position outdoors, the far side of
their load door indoors, or their home location's map marker when neither is available.

**Ordering note.** The chain is *ordered* origin → player, but the fine segment can only be *discovered*
from the player outward, because `FineRoads` covers the loaded grid and nothing else. Construction therefore
runs the existing `Route` query and reverses its fine half before appending. Worth stating because the
direction reversal is the single easiest thing to get backwards here, and getting it backwards yields a
chain that looks right and selects from the wrong end.

### The bridge, and why 512

The bridge is what makes the chain continuous, and it exists for one specific outcome: when no fine node in
the loaded grid is sufficiently hidden, the visitor can still be placed on a **connected** point just beyond
the grid, in the direction they are coming from, rather than the visit declining.

The spacing is 512 units, chosen against a measured figure rather than by feel. Background travel advances an
unloaded actor in a **fixed step of 866 units**, roughly every 1.8 game minutes, and that step is indifferent
to movement speed — `SpeedMult` raised to 500 with the engine's own `GetWalkSpeed`/`GetJogSpeed`/`GetRunSpeed`
reporting 412/1880/1895 changed neither the step nor its cadence. At 512-unit spacing the innermost bridge
point is closer to the loaded grid than one step is long, so a visitor placed there crosses into the grid on
a single background tick and becomes a real, loaded, walking actor immediately.

(That measurement came from an instrumented run in an earlier investigation whose harness was removed; the
number is recorded here because this design depends on it, and it is cheap to re-measure if it is ever in
doubt.)

Two consequences that have to be accepted or designed around:

**Bridge points cannot be validated.** They are outside the loaded grid by construction, which is their
purpose, and both in-grid gates need loaded cells: `StuckRecovery::IsOnNavmesh` reads navmesh out of them,
and the cover raycast needs geometry to hit. A bridge point is therefore accepted on distance alone, which is
consistent — beyond view distance, cover is moot — but carries **no standability guarantee**. `kMaxReachUnits`
exists today precisely to avoid placing an actor on ground nobody checked.

*Mitigation to decide:* prefer real **coarse nodes** wherever the bridge line passes near one, and use
synthetic fill only between them. Coarse nodes derive from the game's own preferred-path data, so they are
walkable by construction; the synthetic points are the unvalidated part. This is the open question below.

**Bridge length buys a wait.** A visitor placed beyond the grid is in low process and covers the remaining
distance at the background rate — roughly 25,000 units per game hour, gross, and less than that in net
progress because the simulation wanders. A bridge point 4,000 units out is a few game minutes; one 12,000
units out is closer to half a game hour before they are even near. The far end of the bridge is where a visit
stops feeling like a response to anything, so the bridge wants a cap rather than reaching as far as the
geometry allows.

### Off-path candidates

Every fine node within **2 hops** of a node on the fine segment is also a candidate, at lower priority.

`FineRoads::Graph::adjacency` already holds a neighbour list per node, so this is a bounded breadth-first
walk from each path node and needs no new graph work.

The reason it earns its place: the fine graph is a **road ribbon**, so a 1-hop neighbour is usually the other
side of the same road, and a 2-hop neighbour is a few metres off it. That is exactly where cheap cover lives —
a spot behind a rock four metres off the road, when the road itself is in plain view. It converts a declined
visit into a placed one without moving the visitor anywhere the player would find strange.

**Two constraints it must inherit.** Hops are undirected, so an expanded node can sit back toward the player,
or behind them. That is the precise failure the Phase 14 approach corridor was added to prevent, where a
bearing aimed at the nearest scrap of road instead of the road home. Off-path candidates therefore pass the
same distance and corridor constraints as path nodes; hop expansion widens the pool, it does not relax it.

### Selection

One walk, outward from the player.

1. Take the **first** point at least `iVisitMarkerMinDistanceUnits` from the player that is **obscured** —
   behind cover, or far enough away and outside the player's view that cover is unnecessary
   (`kUnseenDistanceUnits`, 3,000 today).
2. If nothing along the chain is obscured, take the **first point beyond the player's maximum view
   distance**. Bridge and coarse points satisfy this by construction, which is what they are for.
3. If the chain yields nothing at all, decline. As today, a declined visit is a normal outcome and not an
   error, and there is deliberately no "arrive from anywhere" fallback — a visitor appearing opposite their
   own home is the exact tell this module exists to remove.

At equal standing, the order is:

| Rank | Class | Gates applied |
| --- | --- | --- |
| 1 | Fine, on path | Navmesh re-check, cover or unseen-distance, band, corridor |
| 2 | Fine, off path (by hop count) | The same, plus hop count as the tiebreak |
| 3 | Bridge | Distance only — outside the grid, nothing else is answerable |
| 4 | Coarse | Distance only |

### Stuck recovery

`StuckRecovery::Escort` keeps its contract unchanged. What changes is where its ladder comes from: the
fallbacks are the remaining chain points **outward of the chosen one, in chain order**, so each escalation
walks the visitor back along the path they were supposed to have travelled. That is what the module's header
already asks for and says not to re-sort, and the chain supplies it directly instead of the current
near-player candidate pool.

Its final rung — stepping in toward the goal along a bare line by `closeInStepUnits` — stays as the answer
for a visitor who is stuck with no chain left outward of them.

### What this replaces

| Today | After |
| --- | --- |
| `Tier::FineRoad`, `Tier::CoarseBearing` | One selection walk over the chain |
| `WalkFinePath` band with a preference | First-acceptable, outward |
| `SampleBearingArc` | Hop expansion (retired) |
| `BearingHome` | The chain's own coarse segment (retired) |
| `Plan::coarsePath` as a bearing | Coarse points as real candidates |
| The unfilled fine→coarse join | The bridge |

`Tier` keeps `None`, `Doorstep`, `CityApproach` and `CityGate`. The two road tiers are replaced by the
candidate class of whichever point won, which is strictly more informative in the log.

---

## Open engine questions

1. **May the bridge place a visitor on unvalidated ground?** Or is it restricted to real coarse nodes, with
   synthetic fill used only to *order* the chain and never as a warp target? This is the one design decision
   that cannot be settled from the docs, and it decides whether the bridge is a placement tier or only a
   connectivity device.
2. **How far out may the bridge reach** before the resulting background-travel wait makes the visit
   pointless? A number, in units, informed by the ~25,000 u/game-hour rate.
3. **Is land height answerable outside the loaded grid?** `StuckRecovery::IsStandable` reads
   `TES::GetLandHeight`, which Phase 14 found has no landscape to answer from when the player is indoors. If
   it also answers nothing outside the grid, question 1 resolves itself and the bridge is coarse-only.
4. **Does the corridor constraint have meaning for off-path nodes at 2 hops?** The corridor was built to
   choose between road branches; 2 hops may be inside its tolerance everywhere, making the check free but
   also useless.

---

## Settings

Carried over unchanged: `iVisitMarkerMinDistanceUnits`, `iVisitMarkerMaxDistanceUnits`,
`iVisitArrivalCoverRadiusUnits`.

New:

| Setting | Default | Purpose |
| --- | --- | --- |
| `iVisitChainBridgeSpacingUnits` | `512` | Bridge point spacing; under one 866-unit background step |
| `iVisitChainHopRadius` | `2` | How far off the fine path to expand for candidates |
| `iVisitChainMaxBridgeUnits` | TBD | How far beyond the grid the bridge may place a visitor |

`bVisitArrivalAllowCoarseBearing` is retired with `SampleBearingArc`, or repurposed as "may the chain place
outside the loaded grid at all", which is the same switch aimed at the new design.

---

## File map

```text
include/ApproachChain.h       the chain type and its construction
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
