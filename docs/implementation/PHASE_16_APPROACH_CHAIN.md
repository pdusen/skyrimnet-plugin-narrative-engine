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
- **A defined fallback ordering** for `StuckRecovery`: a consumed-once hop to a fine neighbour when the
  visitor stalls inside the loaded grid, and the remaining chain outward of the chosen point when they stall
  beyond it — walking them back along their own path, which is what that module already promises.
- Retiring `BearingHome` and the Phase 14 approach corridor (`CorridorTarget`). The first manufactures a
  bearing where the route had no candidates, the second disambiguates the branch that bearing aimed at; the
  chain has real candidates everywhere and no bearing to aim.

  **`SampleBearingArc` survives**, against the first draft of this doc. It is not only the road path's
  candidate generator — the deferred `Tier::CityApproach` calls it too, to place a visitor between the player
  and the city gate. Retiring it would mean rewriting a tier this phase promises not to touch, so it stays and
  loses only its road-path caller. The same goes for `iVisitMarkerMaxDistanceUnits`, which the chain walk has
  no use for and the city approach still bands against.

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

`max` rather than an average, settled: one expensive node has to be genuinely expensive to pass through
rather than diluted by a cheap neighbour. An average would make a long cheap approach to a single costly node
far more attractive than intended, which is the one behaviour this cost function exists to prevent.

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

The same 512 spacing governs both direct lines, for the same reason and not merely for consistency: the
spacing is what bounds, by construction, how far outside the grid a visitor can be put down. A coarser
spacing on the direct lines would shrink the graph by a hundred-odd nodes and raise that bound in exchange,
which is the wrong trade on the one segment that exists for the cases where nothing else reaches.

One consequence has to be designed around, and one apparent consequence turns out not to be real:

**Bridge points cannot be validated, and are placed anyway.** They are outside the loaded grid by
construction, which is their purpose, and both in-grid gates need loaded cells: `StuckRecovery::IsOnNavmesh`
reads navmesh out of them, and the cover raycast needs geometry to hit. A bridge point is therefore accepted
on distance alone — consistent, since beyond view distance cover is moot — and carries no standability
guarantee. That is accepted rather than mitigated.

For elevation, take the height the chain's own line interpolates at that point, plus a small upward buffer.
`TES::GetLandHeight` is not an option: Step 1 established that it returns `false` for every position outside
the attached cell grid, and writes `-2048.0` into the out parameter when it does — see
[`land-height-outside-the-loaded-grid.md`](../engine-findings/land-height-outside-the-loaded-grid.md).

Precision does not matter: an actor warped outside the loaded cells stays stationary until background travel
carries them into the grid, so a point left well above or below the terrain costs nothing and is grounded the
moment they become a real actor. No special preference for real coarse nodes is needed — every coarse node is
already in the graph at difficulty 1, so the search routes through them wherever they help.

**Bridge length buys no wait.** The selection walk runs outward from the player and takes the *first*
acceptable point, so the point a visitor is actually warped onto is the innermost one that qualifies — at
most one spacing step beyond the furthest loaded cell, whatever the total length of the bridge behind it. The
chain may stretch 100,000 units; the warp target does not move with it. So there is no cap to choose and no
reach limit to tune.

### Off-path candidates

Every fine node within **2 hops** of a node on the chain's fine segment is also a candidate, at lower
priority.

`FineRoads::Graph::adjacency` already holds a neighbour list per node, so this is a bounded breadth-first walk
from each chain node and needs no new graph work.

The reason it earns its place: the fine graph is a **road ribbon**, so a 1-hop neighbour is usually the other
side of the same road, and a 2-hop neighbour is a few metres off it. That is exactly where cheap cover lives —
a spot behind a rock four metres off the road, when the road itself is in plain view. It converts a declined
visit into a placed one without moving the visitor anywhere the player would find strange.

**The one constraint it must inherit.** Hops are undirected, so an expanded node can sit back toward the
player or across them, and the minimum distance is what rules those out: a node on the far side of the player
sits at most one two-hop reach away from them, so a floor above that reach refuses every one of them on
distance before direction is ever a question. Off-path candidates therefore pass exactly the gates chain nodes
pass; hop expansion widens the pool, it does not relax it.

How far two hops reach is a measurement, not an estimate, and an earlier draft of this section guessed it
wrong. Over every exterior cell in Skyrim and all three DLC the median two-hop reach is 226 units and the
**maximum is 1,784** — see
[`fine-road-hop-spans.md`](../engine-findings/fine-road-hop-spans.md). So the shipped 3,000 floor holds with
1.7x to spare, 1,800 would be the least that clears vanilla at all, and 1,600 does not: four measured spans
exceed it.

`iVisitMarkerMinDistanceUnits` is therefore load bearing for direction as well as for pacing. The dependency
is on vanilla's tail, though, which says nothing about mod-added roads — bounding the expansion in units
rather than hops would make the guarantee independent of navmesh geometry and free the floor to go lower.

**The approach corridor is not carried over.** It existed to manufacture a direction from a bearing and a
branch choice, and nothing now needs one: the chain is a route *from* the visitor, so its points are on the
way home by construction, and the floor handles everything hop expansion adds.

### Selection

One walk, outward from the player, along the chain.

1. Take the **first** point at least `iVisitMarkerMinDistanceUnits` from the player that is **obscured** —
   behind cover, or far enough away and outside the player's view that cover is unnecessary
   (`kUnseenDistanceUnits`, 3,000 today).
2. If nothing along the chain is obscured, take the **first point beyond the player's maximum view distance**.
   Bridge, coarse and direct-line points satisfy this by construction, which is what they are for.

   This needs no second pass over the chain. Outward *is* the order, and a point outside the loaded grid is
   beyond any view the player has of real geometry — so once every point inside the grid has been refused, the
   walk arrives at those points on its own. Implementing it as a literal second pass instead, as Step 4 first
   did, accepts visible ground 3,000 units away that the first pass had just rejected.
3. If the chain yields nothing at all, decline — a defensive branch that should be unreachable. The
   visitor-to-player line guarantees a chain, and its far end is by definition as far from the player as the
   visitor is, so some point on it clears the view distance in every case where the visitor was a plausible
   sender at all. The branch stays because declining is a normal outcome elsewhere in the module and costs
   nothing here, but it is not a case the design expects to exercise, and a log line reporting it is a sign
   the graph was built wrong rather than that the world was unhelpful.

At equal standing, the order is:

| Rank | Class | Gates applied |
| --- | --- | --- |
| 1 | Fine, on chain | Navmesh re-check, a walkable corridor to the player, cover or unseen-distance |
| 2 | Fine, off chain (by hop count) | The same, plus hop count as the tiebreak |
| 3 | Synthetic, **inside** the loaded grid | The same — being off-road does not make it unverifiable |
| 4 | Synthetic, **outside** the loaded grid | Distance only — nothing else is answerable there |
| 5 | Coarse | Distance only |

The navmesh gate is two questions, not one: `IsOnNavmesh` says the point is walkable ground, and
`HasNavmeshCorridor` says the visitor can walk FROM it to the player. Phase 14 added the second against a real
failure — a visitor halting at a dead end 550 units below the player — and it is carried over unchanged. It is
not the retired approach corridor, which was about choosing a road branch.

**Rank 3 is not a special case for one segment.** A synthetic point is gated by *where it is*, not by which
line produced it. Both direct lines, and the coarse-to-fine bridge, end inside the loaded grid, so their last
few nodes can be navmesh-checked and cover-tested like any fine node — and should be, because a validated
off-road spot beats an unvalidated one every time. Gating them by segment instead of by position would
throw away the one part of each synthetic run that can actually be verified.

### Stuck recovery

`StuckRecovery::Escort` keeps its contract unchanged. What changes is where its ladder comes from, and the
ladder now has two sources depending on where the visitor got stuck.

**Stuck inside the fine road graph: hop to a neighbouring fine node.** A visitor who stalls within the
loaded grid is caught on local geometry — a fence corner, a doorway, a boulder — and the fix is a short hop
onto validated road nearby, not a long warp back out along the chain that discards the walk they have
already done. A neighbouring node of the fine graph is eligible when it is:

- **More than `iVisitChainUnstuckMinHopUnits` from where they are standing.** The fine graph is dense along a
  road, and the nodes nearest a stalled actor are the ones most likely to be caught on the same obstacle, so
  the nearest neighbour is usually not far enough to be a fix.
- **No more than `iVisitChainUnstuckMaxRetreatUnits` further from the player than they already are.** Hops
  are undirected and the escort's goal is the player's position, so without this a hop is as free to move the
  visitor backwards as forwards — and a backwards hop is not recovery, it is undoing the approach.

The retreat bound is small on purpose, and the geometry is what makes a small number permissive rather than
restrictive. A hop of length `h` taken perpendicular to the player, from a distance `d`, increases the
distance to the player by about `h² / 2d` — for a 300-unit hop at the 1,000 units where the escort is still
running at all, 44 units. Sideways movement is therefore almost free, while a 300-unit hop taken straight
backwards costs the full 300 and is refused. The bound separates the two without having to compute a bearing.

Each node used this way is **out of contention for the rest of the visit**, whether or not the hop worked.
Without that, two nodes either side of one obstacle trade the visitor back and forth, each hop looking like
progress and none of it being any. Consumption is per-visit state, not persisted: the chain and its escort
already live only as long as the beat does.

**Stuck outside it: walk back along the chain.** Here the fallbacks are the remaining chain points **outward
of the chosen one, in chain order**, so each escalation retreats along the path they were supposed to have
travelled. That is what the module's header already asks for and says not to re-sort, and the chain supplies
it directly instead of the current near-player candidate pool.

Its final rung — stepping in toward the goal along a bare line by `closeInStepUnits` — stays as the answer for
a visitor who is stuck with no fine neighbour left unconsumed and no chain left outward of them.

### What this replaces

| Today | After |
| --- | --- |
| `Tier::FineRoad`, `Tier::CoarseBearing` | One selection walk over the chain |
| `WalkFinePath` band with a preference | First-acceptable, outward |
| `SampleBearingArc` on the road path | Hop expansion (the city tier keeps the primitive) |
| `BearingHome` | The graph's own coarse nodes (retired) |
| The Phase 14 approach corridor | The minimum-distance gate (retired) |
| `Route`'s two-container plan | One weighted graph and one search |
| The unfilled fine-to-coarse join | The bridge, at difficulty 3 |
| Declining when nothing is near | A graph that is always connected |

`Tier` keeps `None`, `Doorstep`, `CityApproach` and `CityGate`. The two road tiers are replaced by the
candidate class of whichever point won, which is strictly more informative in the log.

---

## Settled questions

Nothing is held open against the design. `max` for edge cost, synthetic bridge points placed without
validation, no bridge reach cap, 512 spacing on both direct lines, and the approach corridor retired rather
than inherited are each settled in the sections above.

The one question that needed a running game — whether land height is answerable outside the loaded grid — was
answered by Step 1: it is not, and the bridge interpolates its own heights. See
[`land-height-outside-the-loaded-grid.md`](../engine-findings/land-height-outside-the-loaded-grid.md).

---

## Settings

Carried over unchanged: `iVisitMarkerMinDistanceUnits`, `iVisitMarkerMaxDistanceUnits`,
`iVisitArrivalCoverRadiusUnits`.

New:

| Setting | Default | Purpose |
| --- | --- | --- |
| `iVisitChainBridgeSpacingUnits` | `512` | Bridge and direct-line spacing; under one 866-unit background step |
| `iVisitChainHopRadius` | `2` | How far off the fine chain to expand for candidates |
| `iVisitChainUnstuckMinHopUnits` | `300` | How far a fine-neighbour unstuck hop must move a stalled visitor |
| `iVisitChainUnstuckMaxRetreatUnits` | `150` | How much further from the player that hop may leave them |
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
src/VisitArrivalPoint.cpp     selection walk replaces WalkFinePath / CorridorTarget / BearingHome
src/VisitArrivalPoint.engine.test.cpp
include/RoadRoute.h           possibly a chain-shaped query beside Route
src/RoadRoute.cpp
include/Settings.h            seven new keys
src/Settings.cpp
statics/SKSE/Plugins/NarrativeEngine.ini
CMakeLists.txt                ApproachChain in NARRATIVEENGINE_MOCKED_SOURCES
```

---

## Implementation plan

Sequential, and the tree builds and passes at every step boundary. There is no window where the module is
knowingly broken: Step 2 is additive, Step 3 adds a module nothing calls yet, and Steps 4 and 5 each replace
one subsystem's internals while leaving every signature its callers use in place until the step that changes
them.

Only Step 1 and the last two steps need a running game. Step 1 is first because its answer picks the bridge's
elevation source and nothing else in the design waits on it; the validation and tuning steps are last because
both read a log that only exists once visits are being dispatched over real terrain.

Every step is entirely Claude's work or entirely the user's, except Step 1, which is split and says so.
Verification is attributed separately, since a step Claude implements may still need a running game to
confirm.

### Test coverage is part of every step, not a step of its own

**A step that changes C++ lands its tests in the same step.** No catch-up testing step at the end, and no step
is complete with its tests deferred to the next one. Every branch a step adds — including every failure path,
every early return, and every gate that can reject — is covered before its box is checked.

Four things this phase makes easy to get wrong:

- **Deleting code means deleting its tests and checking what that uncovers.** Step 4 retires
  `BearingHome` and the approach corridor, and takes a caller off `SampleBearingArc`. The risk is not
  that their cases fail, it is
  that they quietly disappear and take a branch's coverage with them. Count the cases in
  `src/VisitArrivalPoint.engine.test.cpp` before and after, and account for the difference.
- **An unreachable branch still gets a case.** Selection step 3 and `Chain::valid == false` are both meant to
  be impossible. Each gets a test that constructs the impossible state directly and asserts the module
  degrades rather than crashes, because "unreachable" is a claim about today's callers.
- **A search needs a correctness oracle, not just a smoke test.** The A-star in Step 3 is checked against a
  Dijkstra over the same fixture graph: same cost, same node sequence. A pathfinder that returns *a* path on
  every fixture looks correct and tells you nothing.
- **`pwsh -File build.ps1 test` passing is a floor, not the bar.** It also has to be true that the new
  branches are reached. A suite that builds and passes while never entering the new code is worse than no
  suite, because it looks like coverage.

The `/unit-test` skill is the tool for the new module in Step 3: it writes a Catch2 suite for one C++ module in
four gated stages, which is the shape the three suites this phase touches already have.

---

### Step 1 — Can land height be read outside the loaded grid?

- [X] Complete

**Answer: no.** 76 samples, four rays, standing in cell (8, -5) with a 5x5 attached block: all 18 samples in
an attached cell returned `true`, all 58 outside returned `false`, and the boundary fell exactly on the grid
edge on every ray. Failures write `-2048.0` into the out parameter, which is a plausible Skyrim ground height
rather than an obvious sentinel. The bridge therefore interpolates its own elevation, and the "Plausible is
not the same as correct" check below was moot — nothing outside the grid answered at all. Written up in
[`land-height-outside-the-loaded-grid.md`](../engine-findings/land-height-outside-the-loaded-grid.md).

**[CLAUDE + USER]** — Claude writes the probe and the write-up; the user runs the game and pastes the log.

**Goal:** Settle the bridge's elevation source. `TES::GetLandHeight` is known to answer nothing when the
player is indoors (Phase 14); whether it answers for a point in an *unloaded exterior cell* is unknown, and it
is the one thing in this design that cannot be settled on paper.

**Files:** none committed. The probe is a temporary local edit, reverted before the step's commit; the finding
lands in `docs/engine-findings/land-height-outside-the-loaded-grid.md`.

**Sub-tasks:**

1. Add a one-shot debug dump on the pattern already at `src/GossipHarvest.cpp:218` — `static bool dumped`,
   gated on `Settings::Get().debugMode`, fired from a Tick once the player is outdoors. It samples
   `TES::GetLandHeight` along **four cardinal rays** from the player out to 40,000 units at 2,048-unit steps,
   and logs for each sample: the ray, the offset, the world position, its cell coordinates, whether that cell
   is in the attached grid, whether the call returned true, and the height it gave.

   Four rays rather than one, because a single ray that runs into the sea or off the edge of the worldspace
   answers nothing and costs a second trip through the game to find out. Four cost ten lines of throwaway code
   and make one run conclusive.
2. The user loads an outdoor save, waits for the dump, and pastes the log. 40,000 units crosses well past the
   5x5 loaded grid (~20,480 units edge to edge), so the same run covers both sides of the boundary.
3. Write `docs/engine-findings/land-height-outside-the-loaded-grid.md`: what was asked, what came back, where the
   boundary is, and whether the heights outside it are plausible or garbage. A "no" is as useful as a "yes" and
   is written up the same way.
4. Revert the probe. Update the bridge section of this doc to state the chosen elevation source as a decision
   rather than a branch, and drop "The one engine question left".
5. Run `pwsh -File format.ps1`.

**Specifics:**

- **Plausible is not the same as correct.** A function that returns `true` with a stale or zero height is
  worse than one that returns `false`, because the design would trust it. Compare two or three of the
  outside-grid answers against the height the player actually stands at when they walk there.
- The probe is throwaway and goes nowhere near a commit. Write it as a local edit, keep the diff in the
  scratchpad if it is worth keeping at all, and revert it in this step rather than the next.

**Verify [USER]:** the log shows a sample series crossing the grid boundary, with a clear answer on each side.

**Verify [CLAUDE]:** `git status` is clean of the probe before the step is checked.

---

### Step 2 — Expose the coarse graph's adjacency

- [X] Complete

**[CLAUDE]**

**Goal:** The composite graph needs coarse *edges*. `TravelGraph` holds them in `g_adjacency` and exposes
`FindPath`, `DistanceField`, `GetNode`, `FindNearestNode` and `EdgeCount` — every way of consuming the edges
except reading them. Nothing in Phase 16 can start until that is addressable.

**Files:** `include/TravelGraph.h`, `src/TravelGraph.cpp`, `src/TravelGraph.engine.test.cpp`.

**Sub-tasks:**

1. Add an accessor beside `GetNode`:

   ```cpp
   // Indices of the nodes sharing an edge with `index`, or an empty
   // span when the index is out of range. Undirected: `b` appears in
   // Neighbors(a) exactly when `a` appears in Neighbors(b).
   std::span<const std::size_t> Neighbors(std::size_t index);
   ```

2. Tests: the neighbours of a known node in a fixture graph; symmetry across every edge; an out-of-range
   index; an empty graph; and that the sum of all neighbour counts is exactly twice `EdgeCount()`.

**Specifics:**

- **A span, not a copy.** `FineRoads::Snapshot` copies because its graph is rebuilt under a lock as cells
  stream; the coarse graph is built once at startup and never mutated, so handing out a view is safe and the
  chain builder reads it once per node.
- Purely additive. No existing behaviour changes, and no caller is touched in this step.

**Verify [CLAUDE]:** `pwsh -File build.ps1 test` passes, with the symmetry case failing if the accessor
returns the wrong row.

---

### Step 3 — The `ApproachChain` module

- [X] Complete

**[CLAUDE]**

**Goal:** The composite weighted graph, the search over it, and the chain it returns — standalone, with no
caller. This is the phase's substance; Steps 4 and 5 are consumers.

**Files:** `include/ApproachChain.h`, `src/ApproachChain.cpp`, `src/ApproachChain.engine.test.cpp`,
`include/Settings.h`, `src/Settings.cpp`, `statics/SKSE/Plugins/NarrativeEngine.ini`, `CMakeLists.txt`.

**Sub-tasks:**

1. Settings — five of the seven new keys: `iVisitChainBridgeSpacingUnits` (`512`) and the four difficulties
   (`iVisitChainDifficultyCoarse` `1`, `...Fine` `2`, `...Connector` `3`, `...Direct` `4`). Document each in
   the deployed INI, including that difficulty 1 is the floor and why.
2. The API:

   ```cpp
   enum class PointClass : std::uint8_t { Coarse, Fine, Connector, Direct };

   struct Point
   {
       RE::NiPoint3 position{};
       PointClass cls = PointClass::Direct;
       bool insideLoadedGrid = false;
       // Set only when cls == Fine, so hop expansion can start from it.
       std::size_t fineNode = FineRoads::kInvalidNode;
   };

   struct Chain
   {
       // Ordered visitor -> player. Empty only when the graph could not
       // be built at all, which the direct line is there to prevent.
       std::vector<Point> points;
       bool valid = false;
   };

   Chain Build(RE::FormID worldSpace, const RE::NiPoint3& visitorOrigin, const RE::NiPoint3& playerPos);
   ```

3. Graph assembly, in the order the design's table gives: every coarse node and its edges at difficulty 1;
   every loaded fine node and its edges at difficulty 2; the player and visitor nodes and their connectors at
   3; the two direct lines out of the visitor at 4, one node every `iVisitChainBridgeSpacingUnits`.
4. Fine-network selection: when the loaded grid holds several disconnected pieces, pick the one holding the
   closest node to the player, and attach both bridges to that one — the coarse-to-fine bridge where it is
   cheapest, the visitor line at whichever of its nodes is nearest the visitor. Other networks are left out of
   the graph entirely; an unbridged network is an island the search can never enter.
5. Edge cost `euclidean length x max(difficulty of its two endpoints)`, and A-star with euclidean distance to
   the player as the heuristic.
6. Elevation for synthetic points, per Step 1's finding.
7. One debug-gated log line per build: worldspace, node and edge counts by class, which fine network was
   selected and how many were rejected, the chain's length in points, its total cost, and the class histogram
   of the result. This line is what Steps 6 and 7 read.
8. Add `src/ApproachChain.cpp` to `NARRATIVEENGINE_MOCKED_SOURCES` and write the suite:
   - **Correctness oracle:** A-star's cost and node sequence equal a Dijkstra's over the same fixture.
   - **Connectivity:** both graphs empty still yields a valid chain, on the visitor-to-player line alone.
   - **Road preference:** a fixture where the road is 1.4x the straight-line distance returns the road; the
     worked ratio in the design (3:1 at 100,000 units) is asserted as a cost comparison, not a feel.
   - **`max` not average:** a fixture with one costly node on an otherwise cheap approach, where averaging
     would pick it and `max` does not.
   - **Ordering:** the chain runs visitor to player, not the reverse.
   - **Disconnected fine networks:** the player-adjacent one is used and both bridges attach to it.
   - **Spacing:** direct-line nodes sit one spacing apart, and the innermost point outside the grid is within
     one spacing of the boundary — the bound the whole 512 argument rests on.
   - **`insideLoadedGrid`** is right on both sides of the boundary.
   - **Degenerate inputs:** visitor and player at the same position; a zero worldspace; a spacing setting of 0
     or negative, clamped rather than looping forever.
   - **`valid == false`** constructed directly, asserting a caller reading an invalid chain degrades cleanly.
9. Run `pwsh -File format.ps1`.

**Specifics:**

- **Pure query, no token.** Like `RoadRoute::Route`, graph assembly and search touch no engine state and take
  no `MainThread::Token`. Elevation is the one exception: if Step 1 says `GetLandHeight` answers, that call
  marshals, and it is the only part of this module that does.
- **The heuristic has to stay admissible.** Euclidean distance underestimates only while no difficulty is
  below 1. If a future tuning pass sets a difficulty under 1, A-star stops being optimal — assert the floor at
  settings-load time rather than trusting the INI.
- Difficulty and `PointClass` are different numbers and inversely related. Do not derive one from the other,
  however much the enums tempt it.

**Verify [CLAUDE]:** `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` both pass, and the oracle
case fails if the heuristic is scaled by difficulty.

---

### Step 4 — The selection walk in `VisitArrivalPoint`

- [X] Complete

**[CLAUDE]**

**Goal:** Replace the band and the two road tiers with one outward walk over the chain, add hop expansion, and
retire the three primitives the chain makes unnecessary.

**Files:** `include/VisitArrivalPoint.h`, `src/VisitArrivalPoint.cpp`,
`src/VisitArrivalPoint.engine.test.cpp`, `src/NPCVisitBeat.cpp`, `include/Settings.h`, `src/Settings.cpp`,
`statics/SKSE/Plugins/NarrativeEngine.ini`.

**Sub-tasks:**

1. Settings — `iVisitChainHopRadius` (`2`), documented in the deployed INI.
   `bVisitArrivalAllowCoarseBearing` is repurposed as "may the chain place a visitor outside the loaded grid
   at all", which is the same switch aimed at the new design: false confines every arrival to ground the
   engine has attached, and declines rather than reaching past it.
2. `Tier` loses `FineRoad` and `CoarseBearing` and keeps `None`, `Doorstep`, `CityApproach`, `CityGate`.
   `Result` gains the winning point's `ApproachChain::PointClass`, which is strictly more informative than the
   two tiers it replaces.
3. The walk: outward from the player, first point at least `iVisitMarkerMinDistanceUnits` away that is
   obscured — behind cover, or beyond `kUnseenDistanceUnits` and out of view. Then the first point beyond the
   player's maximum view distance. Then decline.
4. Hop expansion: every fine node within `iVisitChainHopRadius` hops of a fine point on the chain, over
   `FineRoads::Graph::adjacency`, ranked after on-chain fine points with hop count as the tiebreak. No
   directional gate — see the off-path section; the distance floor is what keeps the expansion from reaching
   across the player, provided that floor is the shipped 3,000.
5. The rank table's five classes, each with exactly the gates the design gives it — in particular, synthetic
   points *inside* the grid get the navmesh and cover gates, and are not waved through for being synthetic.
6. Delete `BearingHome` and `CorridorTarget`, with their tests. `SampleBearingArc` keeps its city-tier
   caller and loses only its road-path one.
7. Update the arrival log line at `src/NPCVisitBeat.cpp:1091` to report the class instead of the retired tier.
   This is the only non-test consumer of `Result::tier`.
8. Tests: first-acceptable-outward preferred over a better-hidden point further out; the minimum-distance gate
   rejecting; cover and unseen-distance each rejecting alone; hop expansion finding cover four metres off an
   exposed road; `iVisitChainHopRadius` of 0 collapsing to on-chain only; each of the five ranks winning when
   it is the best available; a synthetic in-grid point refused by the navmesh gate; an out-of-grid point
   accepted on distance alone; the three surviving tiers short-circuiting before any chain is built; and the
   decline branch constructed directly.
9. Run `pwsh -File format.ps1`.

**Specifics:**

- **The city and interior tiers are not touched.** They short-circuit before any chain is built, and their
  existing cases must still pass unchanged — that is the regression test for this step's blast radius.
- **Count the cases before and after.** Removing three primitives removes their tests; the net case count
  should rise. If it falls, something lost coverage rather than gaining it.
- Distance for every gate is straight-line to the player. Chain order decides traversal order only.
- Never return a point in an unloaded cell for the in-grid ranks. The sender is warped there and needs 3D.

**Verify [CLAUDE]:** `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` pass; the retired symbols are
gone from the tree (`grep -rn "BearingHome\|CorridorTarget" src include` is empty).

---

### Step 5 — The stuck-recovery ladder

- [X] Complete

**[CLAUDE]**

**Goal:** Give `Escort` the two-source ladder: a consumed-once hop to a fine neighbour inside the loaded grid,
and the chain outward of the chosen point beyond it.

**Files:** `include/StuckRecovery.h`, `src/StuckRecovery.cpp`, `src/StuckRecovery.engine.test.cpp`,
`src/NPCVisitBeat.cpp`, `include/Settings.h`, `src/Settings.cpp`,
`statics/SKSE/Plugins/NarrativeEngine.ini`.

**Sub-tasks:**

1. Settings — `iVisitChainUnstuckMinHopUnits` (`300`) and `iVisitChainUnstuckMaxRetreatUnits` (`150`),
   documented in the deployed INI with the `h² / 2d` reasoning in one line.
2. Add a ladder-shaped `Begin` beside the existing one:

   ```cpp
   struct Ladder
   {
       // Chain points outward of the chosen arrival, in chain order.
       // Never re-sorted; the ordering is the point.
       std::vector<RE::NiPoint3> chainOutward;
       // Empty when no fine graph is loaded, which sends every
       // escalation to chainOutward.
       FineRoads::Graph fine;
       float minHopUnits = 300.0f;
       float maxRetreatUnits = 150.0f;
   };

   void BeginLadder(Ladder ladder);
   ```

   Named apart from `Begin` rather than overloading it: `Begin({})` is ambiguous between the two, which is a
   call site picking the wrong semantics by accident rather than a compile error worth having.

3. Keep `Begin(std::vector<RE::NiPoint3>)` exactly as it is. `AmbushBeat` uses it at
   `src/AmbushBeat.cpp:904` and is not part of this phase.
4. Escalation order inside `Update`: an eligible unconsumed fine neighbour when the actor is inside the fine
   graph; otherwise the next chain point outward; otherwise the existing `closeInStepUnits` line probe.
5. Eligibility, all three conditions: further than `minHopUnits` from where the actor stands, no more than
   `maxRetreatUnits` further from the goal than the actor already is, and not already consumed. A node is
   consumed when it is used, whether or not the hop worked.
6. Per-visit consumption state, cleared by `Clear()` and by `Begin`, and not persisted — the escort already
   lives only as long as the beat.
7. `NPCVisitBeat` builds the `Ladder` at `src/NPCVisitBeat.cpp:1392` from the chain and the fine snapshot
   instead of passing a bare fallback list.
8. Tests: a neighbour beyond the minimum chosen; a nearer one skipped; one that retreats past the bound
   refused; a lateral one at 1,000 units accepted (the 44-unit case the design computes); consumption blocking
   reuse after a successful hop and after a failed one; both nodes either side of an obstacle consumed, then
   the fall-through to chain order; an actor outside the fine graph going straight to chain order; an empty
   `fine` graph; an empty `chainOutward`; and both exhausted falling through to the close-in probe.
9. Run `pwsh -File format.ps1`.

**Specifics:**

- **The goal is the player, not the arrival point.** `Escort::Update` already takes `goal` and
  `NPCVisitBeat` passes `player->GetPosition()`, which is what makes "further from the goal" the right
  retreat measure.
- **`Options` is shared with `AmbushBeat`.** Put the two new numbers in `Ladder`, not in `Options`, so the
  ambush path cannot pick them up by accident.
- The ladder is a visit concept. `StuckRecovery` stays generic — it is handed a ladder, it does not know a
  chain exists.

**Verify [CLAUDE]:** `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` pass; `AmbushBeat`'s own
escort cases pass untouched.

---

### Step 6 — In-game validation

- [ ] Complete

**[USER]**

**Goal:** Confirm against real terrain that the chain reads correctly, which no fixture can establish. Each
scenario names what the log must show, so a run that disagrees is a finding rather than an impression.

**Files:** none.

**Sub-tasks:** run a visit in each of the following, with debug mode on, and keep the log.

1. **Open country, visitor far away.** Expect: a chain whose middle is coarse, a winning point of class Fine
   or a synthetic one inside the grid, and a visitor who walks in along a road rather than across country.
2. **Visitor a few thousand units off-road.** Expect: the visitor-to-fine-network direct line chosen over a
   detour to the coarse skeleton and back — visible as a Direct-class run handing off to Fine in the class
   histogram.
3. **No fine graph loaded.** Expect: the visitor-to-player line load-bearing, a point outside the grid, and a
   visitor who covers the gap under background travel and then walks.
4. **Nothing near the player is hidden.** Expect: a point just outside the loaded grid, within one spacing of
   the boundary, rather than a declined visit.
5. **A visitor who gets stuck inside the grid.** Expect: a hop to a fine neighbour that keeps their progress,
   no node reused, and no ping-pong between two nodes.
6. **The three untouched tiers.** A doorstep visit, a walled-city visit, and a city-gate visit, each still
   behaving as it did before this phase.

**Verify [USER]:** the visitor arrives from the direction of their home in every scenario, and no visit
declines for want of a point.

**Verify [CLAUDE]:** read the logs and confirm the class histogram matches the scenario in each — the design's
claim that the chain "reads coarse in the middle and dense near the player" is a prediction, and this is where
it is checked.

---

### Step 7 — Tune the ratios and the two unstuck numbers

- [ ] Complete

**[USER + CLAUDE]**

**Goal:** Settle the five numbers that can only be settled against a real log: the four difficulties and the
pair governing the unstuck hop.

**Files:** `statics/SKSE/Plugins/NarrativeEngine.ini`, `src/Settings.cpp` (defaults only).

**Sub-tasks:**

1. From Step 6's logs, check whether the road preference is decisive and not overwhelming — a visitor taking a
   wildly indirect road to avoid a short off-road stretch means the direct difficulty is too high, and one
   cutting across country where a road existed means it is too low.
2. Check the unstuck hop against the stuck cases: hops that fail to clear the obstacle mean
   `iVisitChainUnstuckMinHopUnits` is too low, and hops that visibly teleport the visitor backwards or
   sideways across a road mean it is too high or the retreat bound is too loose.
3. Change defaults, not knobs. Both settings stay independently tunable.
4. Record the final values and the log line that decided each in this doc, the way Phase 14 recorded
   `iVisitArrivalCoverRadiusUnits`.
5. Run `pwsh -File format.ps1`.

**Verify [USER]:** a visit in each of Step 6's first three scenarios behaves the same way twice running.

---

## Done condition

All seven steps checked, and:

1. `pwsh -File build.ps1 build`, `pwsh -File build.ps1 test` and `pwsh -File format.ps1` are all clean.
2. `grep -rn "BearingHome\|CorridorTarget" src include` is empty, and `Tier` holds only `None`,
   `Doorstep`, `CityApproach` and `CityGate`.
3. Every visit log line names a `PointClass` for the winning point, and a declined visit names the reason.
4. The A-star correctness oracle in `src/ApproachChain.engine.test.cpp` passes against Dijkstra on every
   fixture graph in the suite.
5. `docs/engine-findings/land-height-outside-the-loaded-grid.md` exists, and the bridge section of this doc states
   one elevation source rather than a branch.
6. The seven new settings are in the deployed INI with their defaults and a line on what each decides.
7. No probe, harness or captured log from any step is in the repository.

**Explicitly not required:** progressive advancement along the chain, any change to background travel, and any
improvement to the interior or walled-city tiers. All three are deferred by the Scope section and a Phase 16
that touches them has overrun.
