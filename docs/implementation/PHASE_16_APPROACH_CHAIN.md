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
nearest point that is far enough away and hidden. Where the nearest acceptable point happens to be 3,000
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

- **Rebuilding the walled-city or doorstep tiers.** They keep their shape: both short-circuit before any
  chain is built, and neither gets a chain. What Step 6 found broken in them is repaired in place by Steps 10
  to 12 — ground that can be validated inside the walls, a distance floor on the gate arrival, the right load
  door out of an interior. Those are defects in machinery every tier shares, not new tiers, and they are
  fixed here rather than deferred.
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

**Multiple disconnected fine networks.** The loaded grid can hold several unconnected pieces of road — two
valleys either side of a ridge, a bridgeless river. **Every one of them goes in, and every one gets its own
bridge to the coarse skeleton** at whichever pair of nodes is cheapest to join.

This started out the other way: only the network holding the node closest to the player went in, on the
grounds that an unbridged network is an island the search can never enter, so the rest would be cost without
reach. The reasoning was sound and the premise was the mistake — the islands were islands *because only one
network was bridged*. Bridging each removes the objection by construction, and the heuristic it replaces was
measured picking wrong: a player standing in open country had **249 fine nodes loaded in two networks and 81
selected**, so two thirds of the road around them was discarded for being on the wrong side of whichever
single node happened to be nearest, and the chain used no fine road at all.

**The player attaches to each of them**, at that network's nearest node, rather than to the nearest node
overall. Bridging alone is not enough: one attachment leaves every other network enterable only through its
own bridge and exitable only the same way, so a dead-end stub nearer the player than the road makes the road
reachable in principle and useless in fact. Measured in the fixture that reproduces it — two stub nodes twenty
units from the player against a ribbon starting a hundred — the chain used no fine road until the player had an
attachment to each.

The cost of carrying the rest is small and bounded. Node counts run to a few hundred over a loaded grid, each
extra bridge is one `LayLine` whose length cannot exceed the grid's own diagonal, and a bridge too long to be
worth walking prices itself out at connector difficulty without any rule having to exclude it. Which is the
right division of labour: the graph says what exists and the cost function decides what is worth using.

**Two direct lines, both out of the visitor, both difficulty 4 at 512-unit spacing.**

- **Visitor to the nearest fine node.** Whichever node that is, in whichever network — the same rule as
  every other attachment, now that every network is in the graph and reachable.

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
direct line is reached for only when the road network genuinely cannot get there.

**That worked example is the wrong comparison, and Step 6 proved it.** It weighs the direct line against the
road, which the ratios do settle. What actually decides the shape of every chain is a comparison it never
makes: the **connector** against the road. At the player's end there are two ways onto the skeleton, and
their costs are

```text
straight connector to the nearest coarse node :  3 x L_straight     (max(connector 3, coarse 1) = 3)
onto the fine road and out over the bridge    :  2 x L_road + 3 x B  (plus a short hop on, at 3)
```

so, setting the bridge aside, the fine road is chosen only while `2 x L_road < 3 x L_straight` — that is,
only while the real road is **less than 1.5 times** its own straight line. Over the six to twenty thousand
units that separate a player from their nearest coarse node, a Skyrim road is routinely worse than that, and
the bridge's own `x 3` is charged on top. A straight line across open country priced at 1.5 times a fine road
is a straight line that wins, and in Step 6's session it won six times out of six.

The arithmetic that makes raising the connector difficulty the fix rather than a blunt instrument: the leg
being punished (player to coarse, measured at 6,381 to 20,960 units) and the leg being paid (the bridge,
a few hundred units, because fine and coarse describe the same roads) are both connectors. Multiplying both
by a larger number costs the shortcut far more than the entry fee. Step 8 settles the number; the
requirement is that a road up to about 2.5 times its straight line still beats cutting across country, which
puts the connector above 5.

The fine and bridge segments are short enough that their higher difficulties barely register in a cross-map
total — they matter near the player, which is exactly where the design wants fidelity rather than speed.

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
class of graph it came from. The intent is that it reads coarse in the middle and dense near the player
without that having been imposed on it.

**Conditional on the ratios, and false at the ones first shipped.** Step 6 measured `fine=0` in all six
chains it produced: the near end was not dense, it was a single straight connector to the first coarse node.
"Because difficulty made roads cheap" is only true of the middle of the chain, where coarse at 1 competes
with a direct line at 4. Near the player it competes with a connector at 3, and loses. See the cost function
above.

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
[`fine-road-hop-spans.md`](../engine-findings/fine-road-hop-spans.md). The shipped floor of 2,000 clears that
maximum by 216 units; 1,800 would be the least that clears it at all, and 1,600 does not — four measured spans
exceed it.

`iVisitMarkerMinDistanceUnits` is therefore load bearing for direction as well as for pacing. The dependency
is on vanilla's tail, though, which says nothing about mod-added roads — bounding the expansion in units
rather than hops would make the guarantee independent of navmesh geometry and free the floor to go lower.

**The approach corridor is not carried over.** It existed to manufacture a direction from a bearing and a
branch choice, and nothing now needs one: the chain is a route *from* the visitor, so its points are on the
way home by construction, and the floor handles everything hop expansion adds.

### Selection

One walk outward from the player, along the chain. Every point at least `iVisitMarkerMinDistanceUnits` away
is **graded**, and the winner is the nearest point of the best grade anything reached:

1. **Cover.** Real geometry between the player and the arrival, so the instant somebody appears is hidden
   whatever the player is doing.
2. **Unseen.** No cover, but at least `kUnseenDistanceUnits` (5,000) away *and* outside the arc the player is
   facing. Players are not watching the direction a visitor comes from most of the time, and on open ground
   this is the difference between a visit and no visit. It is a fallback rather than an equal, because it
   depends on where somebody happens to be looking and a player who turns is a player who watched the
   arrival.
3. **Unverifiable.** Outside the loaded grid, where neither question can be asked: no navmesh to read, no
   geometry for a ray to hit. Accepted on distance alone — consistent rather than lax, since out there cover
   is moot — and last, because a verified spot beats an unverifiable one. Bridge, coarse and direct-line
   points are what this grade is for.

The grades are preferences, not alternatives: the whole of a better grade is considered before any of a worse
one, so cover 6,000 units out beats open ground at 5,000 even though the walk reaches the open ground first.
Within a grade the order is the outward walk, so the nearest acceptable point wins.

Two things this shape gets right that a cheaper one does not. Grading each point **once** matters because the
cover test is a raycast, and deciding preference by re-walking the chain per grade would pay for it twice. And
the distances are deliberately far apart: at a 2,000-unit floor against a 5,000-unit open-ground distance,
there is a wide band where only real cover will do, which is where the ordinary visit lands.

**Fallbacks may come from any grade**, not just the winner's. They are reached only once the visitor is stuck,
every grade that got this far is one the player is not watching, and starving the escort to keep the ladder
tidy helps nobody.
3. If the chain yields nothing at all, decline — a defensive branch that should be unreachable. The
   visitor-to-player line guarantees a chain, and its far end is by definition as far from the player as the
   visitor is, so some point on it clears the view distance in every case where the visitor was a plausible
   sender at all. The branch stays because declining is a normal outcome elsewhere in the module and costs
   nothing here, but it is not a case the design expects to exercise, and a log line reporting it is a sign
   the graph was built wrong rather than that the world was unhelpful.

At equal standing, the order is:

| Rank | Class | Gates applied |
| --- | --- | --- |
| 1 | Fine, on chain | Navmesh re-check, a walkable corridor to the player, then graded for cover |
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

**More than one bridge per fine network: considered and rejected.** A loaded network is joined to the coarse
skeleton at a single point, the cheapest pair, and at Riverwood that point was 7,000 units from a player
standing on road the route then ignored. Adding a junction wherever the skeleton passes near the network
sounds like the fix and is not, because a player standing on a road is already as close to the skeleton as
the road is:

```text
player -> coarse, straight across country :  3,485u x8 = 27,878
player -> fine -> coarse, over a junction :  92u + 3,409u x8 = 28,010   (+132, loses)
```

Stepping onto the road costs 92 units and buys nothing, since the junction crosses the same ground the
connector did. The near end of the route is unchanged and so is the arrival. It would shorten routes for a
player **off** the road, which is not the case it was proposed for, so it is not worth the graph it adds.
What that case needs is the straight-line connector taken away from a player who is standing on a road.

---

## Settings

Carried over unchanged: `iVisitMarkerMinDistanceUnits`, `iVisitMarkerMaxDistanceUnits`,
`iVisitArrivalCoverRadiusUnits`.

New:

| Setting | Default | Purpose |
| --- | --- | --- |
| `iVisitChainBridgeSpacingUnits` | `512` | Bridge and direct-line spacing; under one 866-unit background step |
| `iVisitChainCoarseDetailUnits` | `16384` | How near the player a coarse edge is laid rather than linked |
| `iVisitArrivalCoverProximityUnits` | `512` | How near the arrival a blocker must be to count as its cover |
| `iVisitChainHopRadius` | `2` | How many hops off the fine chain to expand for candidates |
| `iVisitChainHopReachUnits` | `400` | How far from its chain point an expanded candidate may sit |
| `iVisitChainUnstuckMinHopUnits` | `300` | How far a fine-neighbour unstuck hop must move a stalled visitor |
| `iVisitChainUnstuckMaxRetreatUnits` | `150` | How much further from the player that hop may leave them |
| `iVisitChainDifficultyCoarse` | `1` | Cost multiplier for coarse road nodes |
| `iVisitChainDifficultyFine` | `2` | Cost multiplier for loaded fine road nodes |
| `iVisitChainDifficultyConnector` | `8` | Bridge points, player and visitor nodes, their connectors |
| `iVisitChainDifficultyDirect` | `12` | Both direct lines out of the visitor |

The connector and direct defaults were `3` and `4` through Step 6, which measured what that costs: a straight
connector at 3 undercuts a fine road at 2 on any road more than 1.5 times its own straight line, so the fine
network never appeared on a single route. Step 8 raises them and Step 17 settles them.

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
4. Fine networks: every disconnected piece in the loaded grid goes in, and each gets its own bridge to the
   coarse skeleton at whichever pair of nodes is cheapest to join. The visitor's direct line attaches at the
   nearest fine node overall. Nothing is excluded — a bridge too long to be worth walking prices itself out
   at connector difficulty, which is the cost function's job rather than a selection rule's.
5. Edge cost `euclidean length x max(difficulty of its two endpoints)`, and A-star with euclidean distance to
   the player as the heuristic.
6. Elevation for synthetic points, per Step 1's finding.
7. One debug-gated log line per build: worldspace, node and edge counts by class, how many fine networks
   were bridged, the chain's length in points, its total cost, and the class histogram of the result. This
   line is what Steps 6 and 7 read.
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

- [X] Complete

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

#### What it found

Run 2026-10-02, nine dispatches across Nightgate Inn, the Redoran's Retreat flats, Swindler's Den and
Whiterun. Every visit completed and every visitor arrived from the direction of their home, so both of the
user-facing criteria passed. The prediction did not.

**The fine network was never on a route.** All six chain-tier dispatches:

```text
chain=37 points [coarse=35 fine=0 connector=2 direct=0] inGrid=4
chain=74 points [coarse=72 fine=0 connector=2 direct=0] inGrid=4
chain=74 points [coarse=72 fine=0 connector=2 direct=0] inGrid=4
chain=74 points [coarse=72 fine=0 connector=2 direct=0] inGrid=4
chain=73 points [coarse=71 fine=0 connector=2 direct=0] inGrid=1
chain=98 points [coarse=96 fine=0 connector=2 direct=0] inGrid=1
```

`fine=0` every time, against 81 / 144 / 225 / 608 / 831 fine nodes built and selected. The two connectors
are the player and the visitor themselves. Cause is the cost function, worked out in that section above: a
straight connector at difficulty 3 undercuts a winding road at 2. Not a search defect — `EdgeCost` and the
A-star agree with the Dijkstra oracle, and the route returned is genuinely the cheapest one the published
ratios allow.

**Everything downstream of that went unused.** All 42 kept candidates were `class=coarse`, `hops=0`, and
graded:

| Grade | Candidates |
| --- | --- |
| `outside-grid` (Unverifiable) | 40 |
| `unseen` | 2 |
| `cover` | 0 |

So Step 4's cover preference and the 5,000-unit open-ground rule decided nothing, and Step 5's ladder
reported "1,435 road node(s) to hop between" without ever having a reason to hop. `bVisitArrivalAllowCoarseBearing`
in its repurposed sense — may the chain place outside the grid — is the only thing that kept five of the six
from declining.

**The arrival landed wherever the first coarse node was**, because `playerCoarseAttach` is a plain
`graph.Link` with no points along it: 11,402u, 6,381u, 9,609u, 6,381u, 14,167u and 20,960u, four of the six
outside the loaded grid. The 20,960 is Swindler's Den, where the cell scan and then
`FineRoads: 25 cell(s), 0 node(s)` both confirmed no fine graph exists — so there the long connector is the
only leg available, and no difficulty spread changes that it has no candidates on it. Steps 8 and 9 take
these two.

**Background travel covers those distances far faster than a walk**, which is why the arrivals read correctly
in play: 20,960u at placement, 9,169u 1.3 seconds later, 2,030u at 21 seconds, greeting at 980u.

**The doorstep tier placed a visitor at the wrong door.** `WhiterunBanneredMare` has two load doors, and
`RoadRoute::ResolveOrigin` took the one flagged `MinimalUse` — the back door, 767 units from the entrance the
player used. Step 7 takes it.

**The city-approach tier cannot fire, and the city-gate tier has no floor.** Inside Whiterun, 38 of 45 and
then 45 of 45 arc samples were rejected off-navmesh and `survived=0` both times, and the escort's close-in
probe found nowhere standable three steps running on ordinary city street. Separately, a player standing
1,168 units inside the gate got a visitor placed at the gate and greeted them 789 units away 1.3 seconds
after arming. Both are written up in
[`standable-ground-inside-walled-cities.md`](../engine-findings/standable-ground-inside-walled-cities.md),
and Steps 10 and 11 take them.

Scenarios 2 and 5 were not run; the off-road staging and the forced stall are still unmeasured, and Step 17
carries them.

---

### Step 7 — Pick the door a visitor would actually use

- [X] Complete

**[CLAUDE]**

**Goal:** Stop the doorstep tier putting a visitor at a door the player never uses.

**Files:** `src/RoadRoute.cpp`, `src/RoadRoute.engine.test.cpp`, `testsupport/EngineMock.h`,
`testsupport/EngineMock.cpp`.

**Sub-tasks:**

1. In `ResolveOrigin`'s interior walk, collect every load door with an exterior landing instead of stopping
   at the first one.
2. Drop any whose base `RE::TESObjectDOOR` carries `Flag::kMinimalUse`, unless that would leave nothing — in
   which case take them, because a back door beats a map marker.
3. Among what remains, take the one nearest the actor inside the cell. That is the door they would walk to,
   and in a one-door cell it is the only door, so the common case is unchanged.
4. Mock support: a door base needs a flag field the test can set, and `ForEachReference` needs to be able to
   return two load doors in a chosen order.
5. Tests: one door unchanged; a normal door and a `MinimalUse` door in each order, both resolving to the
   normal one; two normal doors resolving to the nearer; two `MinimalUse` doors resolving to the nearer
   rather than failing; an interior-to-interior door still skipped; and the map-marker fallback still reached
   when no door has an exterior landing.
6. Run `pwsh -File format.ps1`.

**Specifics:**

- **`MinimalUse` is the engine's own answer, not a heuristic.** Bethesda sets it to mean "NPCs should not
  route through this door". `WRDragonDoor01MinUse` (`0x08648C`) carries it and `WRDragonDoor01` (`0x0252C7`)
  does not, which is exactly the pair in `WhiterunBanneredMare`.
- **The blast radius, measured over the Spriggit export.** 544 vanilla interiors have a load door; **279**
  have more than one; **60** of those mix a normal door with a `MinimalUse` one, so reference order alone
  decides the answer today. The remaining 219 hold two normal doors, where "first wins" is arbitrary rather
  than wrong — nearest is a better answer there too.
- The comment being deleted is as wrong as the code: "cells with several exits are rare, and any of them is a
  defensible answer" is false twice over, and should not survive as a justification.

**Verify [CLAUDE]:** `build.ps1 test` clean, and the mutation check is the ordering one — make
`ForEachReference` hand back the `MinimalUse` door first and confirm the resolution does not change.

**Done.** Both doors of a two-door cell are now collected and chosen between. The mutation check ran: dropping
the Minimal Use comparison and leaving pure nearest fails this case and nothing else, because the fixture
stands its two doors equidistant from the occupant, which is what makes the walk's order the deciding factor
when the flag is ignored.

---

### Step 8 — Price cross-country at what it costs

- [X] Complete

**[CLAUDE + USER]**

**Goal:** Make the fine network reachable by preference rather than by accident, by widening the gap between
the road difficulties and the connector difficulties until a straight line across open country stops
undercutting a road.

**Files:** `statics/SKSE/Plugins/NarrativeEngine.ini`, `src/Settings.cpp` (defaults only),
`src/ApproachChain.engine.test.cpp`.

**Sub-tasks:**

1. Raise `iVisitChainDifficultyConnector` and `iVisitChainDifficultyDirect`, leaving coarse at 1 and fine at
   2. Starting point `8` and `12`: the requirement is that a road up to about 2.5 times its own straight line
   still beats cutting across country, which from the break-even in the cost function needs the connector
   above 5. The direct line keeps its margin over the connector, so cutting to the road near the player stays
   cheaper than cutting the whole way.
2. Re-work the cross-map comparison in the cost function section at the new numbers. A 100,000-unit direct
   line at 12 is 1,200,000 against a 130,000-unit road at 1: the margin grows, which is the intended
   direction.
3. Add an `ApproachChain` test that pins the preference rather than the numbers: a fine ribbon between the
   player and a coarse node, laid out so the road is twice its straight line, must still produce a chain with
   `fine > 0`. At the ratios shipped before this step that test fails, which is the regression it exists to
   prevent.
4. Keep both as settings. Change defaults, not knobs.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- **Why raising the connector helps rather than cutting both ways.** Entering the fine network and leaving it
  over the bridge are both connector-priced, so a larger multiplier raises the entry fee too. It still wins,
  because the legs are nothing like the same length: the connector being punished measured 6,381 to 20,960
  units in Step 6, and the bridge is a few hundred, since fine and coarse describe the same roads and the
  bridge is laid between the closest pair.
- **The A-star heuristic stays admissible.** It is euclidean distance, and admissibility needs only that no
  edge is cheaper than its own length — coarse at 1 is the floor and does not move. Raising the expensive end
  of the scale is free in that respect; lowering coarse below 1 would not be, which is why the floor is
  clamped.
- **This is the step that makes Steps 4 and 5 live.** Until a fine node is on the chain there is nothing
  in-grid to cover-test and no `fineNode` for the ladder to hop from. Both were written, tested against
  fixtures, and have never run against real terrain.

**Verify [CLAUDE]:** the oracle still agrees with Dijkstra on every fixture graph, and the new preference test
fails when the connector difficulty is put back to 3.

**Done.** Connector `8`, direct `12`, in the INI and in `Settings.h`. The new case,
"ApproachChain walks a winding road rather than cutting across it", builds one fixture and runs it at both
ratios: at 3 the chain holds no fine points at all, at 8 it walks all five. The same world, the same geometry,
one number different — which is the whole claim, and it needs no mutation because the failing configuration is
one of its own sections.

**Verify [USER]:** Step 17.

---

### Step 9 — Lay the endpoint connectors instead of linking them

- [X] Complete

**[CLAUDE]**

**Goal:** Put candidates along the player's and the visitor's own connectors, so the nearest acceptable point
is not whatever the first coarse node happens to be.

**Files:** `src/ApproachChain.cpp`, `src/ApproachChain.engine.test.cpp`.

**Sub-tasks:**

1. Replace the `graph.Link` calls that attach the player and the visitor to the coarse skeleton and the fine
   network with `LayLine` at `visitChainBridgeSpacingUnits` and connector difficulty — the same treatment the
   bridge and the two direct lines already get.
2. Leave the cost unchanged in kind: `LayLine` over a straight segment totals the same length at the same
   difficulty, so this adds candidates without re-pricing anything. Say so in a comment, because a reader
   will otherwise assume subdividing changed the route.
3. Tests: a player 20,000 units from the only coarse node produces candidates between the two rather than one
   edge; the laid segment steps one `visitChainBridgeSpacingUnits` in 2D; and the total chain cost is
   unchanged from the linked version to within float tolerance.
4. Run `pwsh -File format.ps1`.

**Specifics:**

- **This is the only fix for the no-fine-network case.** At Swindler's Den there is no fine graph anywhere in
  the 5x5, so the chain's near end is a long connector or nothing, whatever the difficulties say. Step 6 put
  the arrival 20,960 units out there, and subdividing is what makes a 2,000-unit arrival available at all.
- Points laid outside the loaded grid keep the synthetic lift and stay unvalidated, exactly as the bridge's
  do — that rule is about where a point is, not which segment laid it.

**Verify [CLAUDE]:** `build.ps1 test` clean, and the cost-unchanged assertion is what distinguishes this step
from a re-pricing.

**Done.** All four endpoint attachments are laid. The new case,
"ApproachChain lays its endpoint connectors rather than linking them", stands a player 14,624 units from the
only coarse node with no fine graph at all: over twenty points now sit along that segment at one spacing each,
one of them inside the 2,000-to-5,000 band the arrival search wants, and the segment's summed length still
equals the straight line it replaced to within a unit.

---

### Step 10 — Ground that answers inside the walls

- [X] Complete

**[CLAUDE]**

**Goal:** Let a standability question asked inside a walled city have an answer, so the city-approach tier can
choose a point at all.

**Files:** `include/StuckRecovery.h`, `src/StuckRecovery.cpp`, `src/StuckRecovery.engine.test.cpp`.

**Sub-tasks:**

1. `NavmeshSurfaceZ` reads the navmesh's own surface height at a position's XY, from the triangle nearest that
   position's own height, with no reference to landscape.
2. `IsStandable` tries terrain first and falls through to it. Terrain-first is what keeps outdoor behaviour
   where it was; the fall-through is reached both when the terrain query fails and when it succeeds but
   disagrees with the navmesh, which is why it did not matter which of those a city hits.
3. `kNavmeshSurfaceWindowUnits`, 512. Bounded because navmesh under a position is not always the ground
   there: a path at the foot of a cliff is navmesh under whoever stands on top of it. Wider than a city's
   own tiers, far narrower than a ravine, and the arrival search's elevation gate is 400 anyway.
4. `HasNavmeshCorridor` resolves ground the same way. Without it the corridor test rejects points
   `IsStandable` has just accepted, which is exactly what the re-run measured: `offNavmesh` inside Whiterun
   fell from 39 to 30 while `noCorridor` rose from 4 to 10.
5. Tests: a street with no landscape at all; the same street with landscape 400 units below the paving; a
   position off the edge of the navmesh, still refused; a position 2,000 units above it, refused for the
   window; and a corridor across navmesh-only ground.

**Specifics:**

- **Why terrain cannot answer in there.** `WhiterunWorld` carries a landscape record in 7 of its 113 cells
  and `WindhelmWorld` in 2 of 57 — the ground somebody walks on inside the walls is authored static geometry,
  and what makes it walkable is the navmesh. `MarkarthWorld` is the opposite at 140 of 146, so this is not a
  property of cities in general, which is why the fix reads both sources rather than switching on one.
- Written up in
  [`standable-ground-inside-walled-cities.md`](../engine-findings/standable-ground-inside-walled-cities.md).

**Open against the city tiers, and not settled by this step [USER]:**

- **`kMaxElevationDeltaUnits`, 400.** Five of 45 samples in the Gildergreen run were standable and rejected
  for elevation. Whiterun is built in tiers and 400 units is less than one of them.
- **The arc's distances inside a city.** Whiterun's navmesh covers 13 cells spanning about 11,600 x 7,400
  units, clipped to them — no mesh crosses its own cell boundary. Sampling 2,000 to 8,000 units from a player
  anywhere but the far end therefore lands outside the walls, in filler cells with no navmesh: 45 of 45
  off-navmesh from a player standing 1,360 units inside the gate. A floor sized for open country may simply
  be wrong in a space this small.

**Verify [CLAUDE]:** every pre-existing `IsStandable`, `GroundPoint` and `Escort` case passes untouched.

**Done.** The two-source resolution is `WalkableGround`, which `IsStandable` wraps with the water test and
`HasNavmeshCorridor` uses directly. The corridor takes ground rather than standability on purpose: a road that
fords a stream is a road, and an actor wades. Four cases cover a street with no landscape, a street with
landscape 400 units beneath it, a position off the mesh and a position 2,000 units above it.

---

### Step 11 — A floor on the gate arrival, and a ladder for the city tiers

- [X] Complete

**[CLAUDE]**

**Goal:** Stop a visitor materialising in front of a player standing at a gate, and stop the escort beginning
a city visit with nothing to work with.

**Files:** `src/VisitArrivalPoint.cpp`, `src/VisitArrivalPoint.engine.test.cpp`.

**Sub-tasks:**

1. When the gate's landing is nearer the player than `iVisitMarkerMinDistanceUnits`, walk the arrival out
   along the player-to-gate line until the floor is satisfied, trying `1`, `0.75`, `0.5` and `0.25` of the
   shortfall and taking the first that holds up.
2. **A pushed point in the other worldspace is accepted on distance alone**, because nothing there can be
   asked. The gate's far side is in Tamriel and Tamriel is not loaded while the player is inside the city, so
   requiring `IsStandable` of it can only ever fail — which is what the re-run measured: `gate was 1484u,
   floor 2000u, 0 fallback(s)`, the push rejected four times and the arrival left at the gate. This is the
   same rule the chain already applies to a point outside the loaded grid, for the same reason.
3. The probes not taken, and the gate's own landing, become the tier's fallbacks. The landing goes last: it
   is the one position in there the engine vouches for, being where it puts anybody walking through.
4. Tests: a player deep in the city, unchanged; a player inside the floor, pushed out and still on the
   bearing through the gate; the gate surviving as the last fallback; the anchor still the far door.
5. The log line names the arrival's distance, the gate's own, and the floor, because that is the number the
   next session is read against.

**Specifics:**

- **Declining was not a candidate.** A player standing at a city gate is an ordinary place to stand, and the
  alternative the user has now watched twice is a visitor appearing inside conversation range.

**Verify [CLAUDE]:** `escort begun with 0 chain fallback(s)` no longer appears on a city visit.

**Done.** The push keeps all four probes and the gate behind them, so a pushed arrival always carries four
fallbacks — which is what the test asserts, because that count only holds if the probes are not being
validated. The earlier version asked `IsStandable` of them and a measured visit rejected all four.

---

### Step 12 — Subdivide the coarse skeleton near the player

- [X] Complete

**[CLAUDE]**

**Goal:** Stop the chain jumping ten thousand units between consecutive coarse nodes, which is what puts an
arrival that far out when the player happens to be standing beside a road node.

**Files:** `src/ApproachChain.cpp`, `src/ApproachChain.engine.test.cpp`.

**Sub-tasks:**

1. Lay the coarse-to-coarse edges that fall near the player at `visitChainBridgeSpacingUnits`, and link the
   rest as now.
2. Bound it by distance from the player, not by hop count. `iVisitChainCoarseDetailUnits`, default 16,384 —
   four cells, comfortably past the loaded grid's corner, which is as far as any arrival is ever placed.
3. Tests: two coarse nodes 11,000 units apart near the player produce candidates between them; the same pair
   far from the player stay one edge; the chain's cost is unchanged either way.
4. Run `pwsh -File format.ps1`.

**Specifics:**

- **Why this was invisible until now.** Step 9 laid the endpoint connectors, which is what the near end of a
  chain is made of when the player is out in open country. A player standing *beside* a coarse node has a
  short connector and joins the skeleton immediately — and the skeleton is one node per exterior navmesh,
  which over Tamriel's 622 nodes averages about 8,000 units. The re-run measured `inGrid=6`, `tooNear=3` and
  an arrival at 11,582 units: three candidates inside the floor, then nothing at all until the next coarse
  node.
- **Why bounded.** Laying every coarse edge would take the graph from about 1,000 nodes to ten thousand for
  points no arrival search will ever look at — it only ever walks outward from the player until it finds
  something, and gives up long before the far end of the province.

**Verify [CLAUDE]:** the oracle still agrees with Dijkstra, and a cost assertion either side of the bound
shows that subdividing changed what is available rather than what anything costs.

**Done.** `iVisitChainCoarseDetailUnits`, 16,384, and the laid points carry `PointClass::Coarse` rather than
Connector — priced as the road they subdivide, not as cutting across it. The new case stands a player 624
units from a skeleton node and reads the nearest chain point past the 2,000 floor: 4,096-plus with the pass
off, under 2,600 with it on, and the route costing the same within 2% either way.

---

### Step 13 — Make the cover gate measure cover

- [X] Complete

**[CLAUDE]**

**Goal:** Stop a hill somewhere between the player and the arrival being read as something the visitor is
standing behind.

**Files:** `include/CameraVisibility.h`, `src/CameraVisibility.cpp`, `src/CameraVisibility.engine.test.cpp`,
`src/VisitArrivalPoint.cpp`, `src/AmbushSpawnPoints.cpp`, `include/Settings.h`, `src/Settings.cpp`,
`statics/SKSE/Plugins/NarrativeEngine.ini`.

**Sub-tasks:**

1. A ray counts toward cover only when what stopped it is **near the arrival**: blocked, and the remaining
   distance behind the blocker no more than `iVisitArrivalCoverProximityUnits`. All nine still have to pass.
2. The setting, default `512` — a rock, a tree, a wall, a building beside the spot. Tunable because the right
   number is a judgement about how far the player may walk before the arrival stops being hidden, and that
   settles against a log rather than an argument.
3. `AmbushSpawnPoints` passes `kNoCoverProximityLimit` and keeps its measured behaviour. Its probes sit in a
   ring close to the player, where rays are short and this failure does not arise; changing it would be a
   second subsystem altered on evidence that is about visits.
4. Tests: a blocker near the spot is cover; the same fraction-0.4 blocker that the old gate accepted is not;
   the silhouette, too-close, overhead and no-camera refusals all unchanged.

**Specifics:**

- **This is the failure Phase 14's Step 6 measured and deliberately did not act on.** Its table, over 514
  road candidates, is the whole argument:

  | Distances looked at | Passed as covered | Candidates |
  | ------------------- | ----------------- | ---------- |
  | up to 999 | 0.0% | 22 |
  | 1000 – 1999 | 2.8% | 106 |
  | 2000 – 2999 | 74.9% | 203 |
  | 4000 – 4999 | 74.4% | 82 |
  | 9000 – 9999 | 100.0% | 101 |

  A gate that passes 100% of candidates at nine thousand units is not measuring cover; it is measuring
  whether four kilometres of Skyrim contains anything at all, and it always does. Phase 14 wrote the
  diagnosis down exactly: "cover proved that way is a hill somewhere in between rather than something the
  visitor is standing behind, and it stops being true the moment either of them moves."

- **Why it got worse after 0.6.0, which is how the user described it.** Nothing about the gate changed. The
  arrivals moved into the range where it fails: the band was 800 to 2,500 units, `b7f9340` widened it to
  5,000, `15333d8` removed the ceiling, and this phase set the floor to 2,000 and took the ceiling off the
  chain tier. Pre-0.6.0 arrivals sat almost entirely in the two buckets where the gate passes 0.0% and 2.8%;
  they now sit in the 74.9%-to-100% ones. The same defect, asked far more often.

- **This is not the change `3d6f870` reverted.** That one scoped the raycast by *range* — trust it under
  2,000 units, let the facing arc decide past that — and cost arrivals on open road that a run had just
  confirmed working. With the floor now at 2,000 it would retire the cover grade altogether. This fixes what
  the ray measures instead, so it works at every range and the grade survives.

- **It will pass fewer candidates, and that is the point.** Expect `survived` to fall and more arrivals to
  come from the unseen grade or from outside the grid. Step 17's run is where that gets read.

**Verify [CLAUDE]:** the old gate accepts the fraction-0.4 blocker and the new one does not, with every other
refusal unchanged.

**Done.** Both tolerances are distances now, which they had to be: `kReachedFractionThreshold` is 5% of the
ray, so on a 10,000-unit one it reads a boulder 400 units in front of the arrival as "reached the endpoint",
clear — the exact cover being looked for, discarded for being far from the camera. The cover gate therefore
takes any hit as a blocker and asks how many units short of the spot it sits.

Mutating the proximity test out fails `CameraVisibility::IsPositionBehindCover` and
"VisitArrivalPoint refuses cover the raycast cannot support", and nothing else. The visit case is a pair on
one world: nine blocked rays at 60% of the way out decline, the same nine at 99% accept.

**Verify [USER]:** a visitor at the `cow Tamriel -7 1` site is hidden when they appear, or does not appear
there at all.

---

### Step 14 — Bound hop expansion in units, not hops

- [X] Complete

**[CLAUDE]**

**Goal:** Stop an off-route point beating every on-route one, which is how a visitor from the north-east
arrived from due west at Nightgate Inn.

**Files:** `src/VisitArrivalPoint.cpp`, `src/VisitArrivalPoint.engine.test.cpp`, `include/Settings.h`,
`src/Settings.cpp`, `statics/SKSE/Plugins/NarrativeEngine.ini`.

**Sub-tasks:**

1. `iVisitChainHopReachUnits`, default `400`: an expanded node is a candidate only while it is within that
   many units of the fine chain point it hangs off. `GatherCandidates` already computes that distance to
   order fallbacks, so the check costs nothing.
2. Keep `iVisitChainHopRadius` as well. Hops bound how far the search walks the ribbon; units bound where it
   may end up. Dropping either leaves one of the two failure modes it was built against.
3. Rewrite the comment that claims the distance floor makes a directional test unnecessary. It is wrong, and
   this step is why.
4. Tests: a node inside the reach is still offered; one past it is not; and the Nightgate shape — every
   on-chain point far out and out of grid, one hop point near and on the far side — picks nothing on the far
   side.

**Specifics:**

- **The measurement, from `NarrativeEngine.2.log`.** Player at `(73021,50240)` at Nightgate Inn, visitor from
  Winterhold, `home=(115577,114656)`, `bearing_home=57deg`. The kept candidates:

  ```text
  (71022,50109)  2003u out  class=fine    hops=2  in_grid=true   passed by cover
  (84098,51036) 11106u out  class=coarse  hops=0  in_grid=false  passed by outside-grid
  (84968,47141) 12343u out  class=coarse  hops=0  in_grid=false  passed by outside-grid
  ...
  ```

  **Every on-chain point is east**, 11,106 units out and further, which is the road sweeping around the east
  side of the mountain exactly as it should. The route was right. What won was the one off-route point: a fine
  node two hops to the side, 2,003 units **west**, `bearing_arrival=-176deg`. It won because grade beats
  distance — it was in-grid with cover, and every on-chain alternative was outside the grid and therefore
  only Unverifiable.

- **The guarantee this invalidates.** `GatherCandidates` claimed a hop could never place a visitor across the
  player, because two hops reach a measured 1,784 units at worst and the floor is 2,000. The arithmetic was
  measured from the wrong origin: the budget is spent from a **chain point**, and chain points sit anywhere
  inside the floor. 2,003 units of westward reach from a chain point east of the player is what the log shows.

- **Why units fix it and a bigger road weight does not.** Bounding the reach at `b` means every accepted
  candidate — necessarily 2,000 units or more from the player — hangs off a chain point at least `2000 - b`
  out. At 400 that is 1,600: far enough along the route that the candidate inherits the route's direction.
  Crossing the player would cost more than `b` by construction. The floor's directional guarantee becomes
  true instead of asserted, with no half-plane and no corridor.

- **Not a weights problem.** The user's first reading was that the coarse route around the mountain was losing
  to something straighter, and it is worth saying plainly that the chain disproves it: the coarse run in that
  log is east, at difficulty 1, for 11,000 units. Re-weighting would have changed nothing, because the route
  was never the thing choosing the arrival.

**Verify [CLAUDE]:** removing the reach check puts the far-side point back in the candidate set and fails the
Nightgate case.

**Done.** `iVisitChainHopReachUnits`, 400. Widening it a thousandfold fails
"VisitArrivalPoint expands off the road for a candidate" and nothing else.

That case had to be rewritten, and the rewrite is the interesting part: it used to hang its spur off the
player's own node, which is precisely the shape this step forbids — a point reachable within the reach of a
chain point at the player's feet is inside the floor by construction. So the old fixture encoded the Nightgate
behaviour as correct. It now hangs the spur off a chain point out along the route, where expansion is meant to
look, and a second section keeps the old geometry to assert the decline.

**Verify [USER]:** at Nightgate Inn, a visitor from Winterhold arrives from the east.

---

### Step 15 — A dedicated trace of the whole journey

- [X] Complete

**[CLAUDE]**

**Goal:** Make an arrival diagnosable from a file, so the next fault costs a read rather than a hand
simulation or another play session.

**Files:** `include/VisitorTravelLog.h`, `src/VisitorTravelLog.cpp`,
`src/VisitorTravelLog.engine.test.cpp`, `src/ApproachChain.cpp`, `src/VisitArrivalPoint.cpp`,
`src/NPCVisitBeat.cpp`, `src/StuckRecovery.cpp`, `include/CameraVisibility.h`, `src/CameraVisibility.cpp`,
`include/Settings.h`, `src/Settings.cpp`, `statics/SKSE/Plugins/NarrativeEngine.ini`, `src/Plugin.cpp`,
`CMakeLists.txt`.

**Sub-tasks:**

1. `VisitorTravelLog`, on the GossipLog pattern: its own stream to
   `SKSE/NarrativeEngine_VisitorTravel.log`, rotated five deep, session-scoped, mutex-guarded, flushed per
   line. `bVisitorTravelLogEnabled`, **on by default** and gated on nothing else.
2. One tagged line per step, in the order the decision happens: `BEGIN ENDS GRID TIER GRAPH SEARCH CHAIN
   GATE COVER GRADE PICK WARP ESCORT END`. The tag column is padded so a reader can skim one tag down the
   file.
3. Instrument the whole path — both ends and the rung that answered them, the attached grid, every node set
   and attachment and bridge and laid line with its length and difficulty, the route and its cost, every
   chain point, every rejection with the number that failed it, every grade, the winner and what it beat,
   the snapshot and marker and warp, and every escort hop and retirement.
4. `CameraVisibility::CoverProbe` carries why a cover probe answered as it did — which ray, at what height
   and lateral offset, the hit fraction and the shortfall against the limit. Defaulted to `nullptr` so the
   ambush caller is untouched.
5. Only the visit escort mirrors into the trace; `StuckRecovery` is shared with the ambush beat.
6. Tests: the module's own file lifecycle, enable flag, rotation and post-close writes; plus
   "VisitArrivalPoint traces the whole search", which drives a real search and asserts the transcript's
   stages are present — including `GATE` and `GRADE`, because a transcript without the rejections is a
   summary with extra steps.

**Specifics:**

- **What decided the content.** Every defect in this subsystem so far read correctly in summary and wrongly
  in detail: a route round the right side of a mountain whose arrival came off an off-route neighbour; a
  cover claim resting on a hill three thousand units away; a fine network discarded for being on the wrong
  side of the nearest node; a visitor at the back door of an inn. In each case the summary line named the
  winner and not the loser, and the diagnosis cost a reproduction. **This file records the losers.**

- **Free-form lines rather than forty typed emitters.** `Write(tag, fmt, args...)` checks `IsActive()`
  before formatting, so a disabled log costs one atomic read. GossipLog already keeps `Note` for the same
  reason; a typed emitter per event would be forty functions whose only job is to format one line.

**Verify [CLAUDE]:** a real search writes a transcript whose stages a reader can follow end to end, with the
refused candidates in it.

**Verify [USER]:** after the next run, `NarrativeEngine_VisitorTravel.log` explains an arrival without
needing the game.

**What the first traced run showed, and three faults in the trace itself.** It did the job — both of the run's
visible problems were diagnosed from the file without touching the game, and one of them was diagnosed
*against* the summary that had looked fine. The faults, all mine and all fixed:

1. `ENDS` and `GRID` were written after the tier branches, so a doorstep or city visit recorded neither.
2. `End` was only called on the chain tier's return, so four of eleven visits had no closing line.
3. The `ESCORT armed` line read the ladder after `std::move`, and reported `0 chain fallback(s) and 0 road
   node(s)` against the plugin log's `6` and `859`. A use-after-move, caught only because the two logs
   disagreed — which is an argument for the trace rather than against it.

---

### Step 16 — Take the door somebody would walk out of, not the nearest one

- [X] Complete

**[CLAUDE]**

**Goal:** Stop a visitor waiting on a first-floor balcony because the player happened to be standing upstairs.

**Files:** `src/RoadRoute.cpp`, `src/RoadRoute.engine.test.cpp`, `testsupport/EngineMock.h`,
`testsupport/EngineMock.cpp`, `src/VisitorTravelLog.cpp` (the door choice into the trace).

**Sub-tasks:**

1. Among the doors that survive the `MinimalUse` filter, prefer the one whose **exterior landing is lowest**
   when another is more than `kDoorStoreyUnits` (128) above it. Nearest decides only within that band.
2. Log every door considered into the travel trace, not only the one taken — the plugin log records the
   choice and the travel log should record what it was chosen over.
3. Tests: a ground door and a balcony door with the player upstairs resolves to the ground one; two
   street-level doors still resolve to the nearer; the `MinimalUse` filter still runs first; the existing
   single-door and marker-fallback cases unchanged.

**Specifics:**

- **Measured.** `WhiterunBanneredMare` has three ways out in the user's load order. With the player upstairs
  the walk picked `0xFE1F8AAA` at 942 units — a mod-added balcony door, not flagged `MinimalUse`, whose
  landing at `(25784,-8003,-3043)` sits 194 units above the front door's `(25670,-7633,-3237)`. Nearest was
  the right answer to the wrong question: the player was upstairs, so the balcony *was* nearest.
- **Why elevation rather than another distance rule.** A building's main entrance is at street level and its
  upper doors are balconies, and a visitor on a balcony cannot be walked to at all. 194 units is the storey
  the measurement shows; 128 is below that and above the few-unit jitter between two doors on one floor.
- **The risk, stated.** A building whose main entrance is up outside steps would now lose to a lower side
  door. Nothing in vanilla's multi-door interiors looks like that — the ones that do, like Dragonsreach, are
  single-door — but it is the case to watch, and `MinimalUse` is what usually marks the loser anyway.

**Verify [CLAUDE]:** the balcony fixture resolves to the ground door from upstairs and from down, and the
two-street-door fixture still goes to the nearer.

**Done.** The balcony fixture stands its two doors 3,000 units apart with the balcony the nearer, and the
ground door still wins. Raising `kDoorStoreyUnits` a thousandfold fails that case and nothing else. Every
door considered now goes into the travel trace with its landing, its distance and its flag, so the file
records what the choice was made against.

**Verify [USER]:** a visit with the player upstairs in the Bannered Mare puts the visitor at the front door.

---

### Step 17 — Re-run Step 6's sites and settle the numbers

- [ ] Complete

**[USER + CLAUDE]**

**Goal:** Confirm Steps 7 to 9 changed the chain's shape rather than only its code, and settle the five
numbers that can only be settled against a real log.

**Files:** `statics/SKSE/Plugins/NarrativeEngine.ini`, `src/Settings.cpp` (defaults only), this doc.

**Sub-tasks [USER]:** re-run the sites from Step 6 — `cow Tamriel 17 11` (Nightgate Inn), `cow Tamriel -5 1`
(the Redoran's Retreat flats), `cow Tamriel -12 0` (Swindler's Den, no fine graph in the 5x5), and inside the
Bannered Mare — plus the two scenarios that were skipped:

1. **Visitor off-road.** `cow Tamriel -7 1`, `prid` a candidate and `moveto player`, return to
   `cow Tamriel -5 1`, dispatch. This is the Direct-to-Fine handoff, and nothing has measured it.
2. **A forced stall.** After arming, `prid` the sender from the log line and `setav speedmult 0`. This is the
   ladder, and nothing has measured that either.

**Sub-tasks [CLAUDE]:**

1. `fine > 0` on at least the two road sites, with a winning point inside the loaded grid. That pair is the
   whole test of Step 8 — a histogram that still reads `fine=0` means the ratio is still wrong and no tuning
   elsewhere matters.
2. Check the road preference is decisive and not overwhelming: a visitor taking a wildly indirect road to
   avoid a short off-road stretch means the direct difficulty is too high, and one cutting across country
   where a road existed means it is too low.
3. Check the unstuck hop against the stall. Hops that fail to clear the obstacle mean
   `iVisitChainUnstuckMinHopUnits` is too low; hops that visibly teleport the visitor backwards or sideways
   across a road mean it is too high or the retreat bound is too loose. Expect exactly one fine hop per visit,
   then chain fallbacks, then close-ins.
4. Confirm a cover or unseen grade is now reachable. Step 6 graded 40 of 42 candidates `outside-grid` and
   none on cover; a run that still grades everything out-of-grid means Step 9 did not land.
5. Change defaults, not knobs. All five stay independently tunable.
6. Record the final values and the log line that decided each in this doc, the way Phase 14 recorded
   `iVisitArrivalCoverRadiusUnits`.
7. Run `pwsh -File format.ps1`.

**Verify [USER]:** a visit at each of the three outdoor sites behaves the same way twice running, and a
visitor to an indoor player waits at the door the player walked in by.

---

## Done condition

All seventeen steps checked, and:

1. `pwsh -File build.ps1 build`, `pwsh -File build.ps1 test` and `pwsh -File format.ps1` are all clean.
2. `grep -rn "BearingHome\|CorridorTarget" src include` is empty, and `Tier` holds only `None`,
   `Doorstep`, `CityApproach` and `CityGate`.
3. Every visit log line names a `PointClass` for the winning point, and a declined visit names the reason.
4. The A-star correctness oracle in `src/ApproachChain.engine.test.cpp` passes against Dijkstra on every
   fixture graph in the suite.
5. `docs/engine-findings/land-height-outside-the-loaded-grid.md` exists, and the bridge section of this doc states
   one elevation source rather than a branch.
6. The eight new settings are in the deployed INI with their defaults and a line on what each decides.
7. No probe, harness or captured log from any step is in the repository.
8. A chain built on real terrain beside a road reads `fine > 0`, and the winning point is inside the loaded
   grid. Step 6 found neither, and the rest of the phase is scaffolding until both hold.
9. A visitor to an indoor player waits at the door the player walked in by, in a cell with more than one.
10. A visit with the player inside a walled city logs `tier=city-approach` with `survived > 0` at least once,
    and a gate arrival is never nearer the player than the distance floor.

**Explicitly not required:** progressive advancement along the chain, and any change to background travel.
Both are deferred by the Scope section and a Phase 16 that touches them has overrun.

The city and doorstep tiers are not in that list. They keep their shape, but the defects Step 6 found in them
— ground that cannot be validated inside the walls, a gate arrival with no floor, the wrong load door out of
an interior — are in machinery every tier shares, and Steps 10 to 12 repair them here.
