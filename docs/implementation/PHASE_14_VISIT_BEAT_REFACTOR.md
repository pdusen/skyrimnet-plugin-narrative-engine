# Phase 14 — NPC Visit Beat Refactor

A structural rework of the shipped NPC Visit beat, on the same spine Phase 11 gave the ambush beat: C++ owns
placement and alias fill; the quest owns persistence and packages; nothing the beat needs is discovered by an
alias fill rule that can silently fail.

The difference from Phase 11 is where the placement comes from. An ambush asks *"where near the player can a
group hide?"* — a direction-agnostic question, which is why `AmbushSpawnPoints` searches a ring. A visit asks
*"where would this specific person be, having walked here from where they live?"* That question has one answer
per visitor, and answering it is what the Phase 12 road graphs were built for.
[`PHASE_12_ROAD_GRAPH.md`](PHASE_12_ROAD_GRAPH.md) names this beat as the intended first consumer of
`RoadRoute` and ships nothing that calls it. This phase is that caller.

Five things change relative to the shipped beat:

1. **The arrival point is routed, not discovered.** A road-graph query from the player outward toward the
   visitor's home replaces the "find the nearest authored XMarkerHeading that passes five conditions" alias
   rule.
2. **Arrival direction becomes meaningful.** Today a visitor materialises wherever Bethesda happened to put a
   marker, which bears no relation to where they live. After this phase they come up the road from home.
3. **Alias fill can no longer fail the beat.** `Sender` and `ReturnAnchor` become `Optional` with no fill
   rule, force-filled from C++ after quest start, verified on a later tick.
4. **The distance knobs become real.** `iVisitMarkerMinDistanceUnits` / `iVisitMarkerMaxDistanceUnits` are
   read into `Settings` today and consumed by nothing; the live band is hardcoded in the alias conditions.
5. **The approach gets a recovery path.** `StuckRecovery`, built in Phase 11 and never wired into this beat,
   takes over when a warped sender cannot reach the player.

---

## Why this phase exists

The visit beat has one structural flaw it shares with the pre-Phase-11 ambush beat, one narrative flaw that is
the real reason to do this work now, and four smaller gaps.

### The structural flaw

`SpawnMarker` (alias ID 4 on `_ne_VisitQuest`) is a Find-Matching-Reference alias gated on five stacked
conditions: `IsInList _ne_SpawnMarkerTypeList`, distance `< 8000`, distance `>= 3000`, no line of sight, and
not interior. It carries no `Optional` flag, so a failed fill fails `EnsureQuestStarted`, and `DispatchQuest`
rolls the whole beat back. Alias fill is one-shot at quest start; there is no retry.

This is the same shape Phase 11 removed from the ambush beat, and Phase 11 already recorded it killing this
one: deleting the shared form list at `000801` left the first condition pointing at nothing, and NPC Visit was
dead on every build between that removal and the restore.

It fails more gracefully than the ambush version did — the beat rolls back cleanly instead of wedging, because
`DispatchQuest` checks the return value — but it still fails, it fails in exactly the open terrain where a
visit reads best, and each failure has already spent a compose LLM call.

### The narrative flaw

Even when the fill succeeds, the marker it finds is an arbitrary piece of authored world furniture. The
conditions constrain distance and sightline; nothing constrains *bearing*. A visitor from Riften and a visitor
from Solitude arrive from the same spot, because the same marker was nearest both times.

That is the flaw worth spending a phase on. The other items here are robustness; this one is the feature.

### The four gaps

- **Interior visits are impossible, and nothing gates them out.** `IsInInterior == 0` on the marker means no
  fill can ever happen while the player is indoors, and `NPCVisitBeat::IsAvailable` never checks. The Director
  picks a visit, COMPOSE fires the compose LLM, and the beat dies at quest start.
- **`Sender` (alias ID 1) is a global faction-rank scan, also not `Optional`.** Its only purpose is to find
  whoever `PromoteSenderToDesignated` just placed at rank 4 in `_ne_VisitSenderFaction`. It carries no
  `MatchingRefClosest`, so a rank-4 leak from an aborted run — demote is best-effort on several paths — can
  match the wrong actor, and any fill failure ends the beat.
- **`ReturnAnchor` (alias ID 5) is chained off `Sender`.** Its fill is Create Reference To Object, XMarker, at
  `Alias: Sender`. `Sender` fails, the anchor fails with it. A null anchor is fatal at compose, and at cleanup
  it is worse than fatal: `senderActor->MoveTo(anchorRef)` is simply skipped, so the sender is abandoned
  wherever they happen to be standing, with no fallback to the position `VisitState::Snapshot` is already
  holding.
- **No stuck recovery on the approach.** A sender warped onto a ledge or the wrong side of a river burns the
  full `iVisitApproachTimeoutSeconds` and rolls back. `StuckRecovery::Escort` exists and this beat does not
  call it.

---

## Scope

### In scope

- A new **`VisitArrivalPoint`** module: given the sender and the player, return a world position on the road
  between them, out of view, inside the configured distance band — plus ranked fallbacks further along the
  same road for `StuckRecovery` to escalate through.
- **First production consumer of `RoadRoute`**, `FineRoads` and `TravelGraph`. All three are consumed as-is;
  this phase changes none of them.
- Rework of `_ne_VisitQuest`'s aliases: `SpawnMarker` deleted, `Sender` and `ReturnAnchor` become `Optional`
  with no fill rule.
- `_ne_VisitQuest.psc` gains force-fill trampolines and loses `MoveSenderToSpawnMarker()`; the Stage 10
  fragment loses its body.
- C++-side warp: place a temporary XMarker, `Actor::MoveTo` the sender onto it, delete it.
- COMPOSE becomes an explicit sub-state machine with a fill-verification tick, mirroring `AmbushBeat`.
- `StuckRecovery::Escort` wired into the Salutation approach and the post-combat ReEngage approach.
- Retirement of `_ne_VisitSenderFaction` (`000819`) and `_ne_SpawnMarkerTypeList` (`000801`), both of which
  exist only to serve fill rules this phase deletes.
- `iVisitMarkerMinDistanceUnits` / `iVisitMarkerMaxDistanceUnits` become live, plus a small number of new
  `[Beats]` keys for the arrival search.
- A calibration probe for the cover gate — see **The cover gate** below.

### Deferred (explicitly out)

- **Road-aware departure.** The return package targets `ReturnAnchor`, and the engine already moves travelling
  actors outside the loaded grid along the same NAVI preferred-path data `TravelGraph` is built from, so the
  walk home already follows roads unaided. `RoadRoute` gives this a better answer whenever we want one; it is
  not this phase's problem.
- **Routability as a sender-selection filter.** A candidate whose home cannot be routed to the player will
  always fall to a lower tier of the ladder below. `TravelGraph::DistanceField` exists precisely so many
  candidates can be scored against one destination with a single Dijkstra, so filtering on it is affordable —
  but it changes *who visits you*, which is a design change and not a robustness fix.
- **Any change to `TravelGraph`, `FineRoads`, `RoadRoute`, `CameraVisibility` or `StuckRecovery`.** All five
  are consumed as-is. In particular, centreline thinning of the fine graph stays deferred; see **What the
  ribbon costs us**.
- **Persisting fine road coverage across sessions.** Still session-scoped, still Phase 12's deferral.
- **Changes to the RUNNING state machine's conversation logic.** The Discuss substate cycle, the conclusion
  poll, the nudge counter and the valediction path are all untouched. This phase ends at "the sender is
  standing in front of the player"; everything after that already works.
- **Any change to the ambush beat.** `AmbushSpawnPoints` is not generalised, not renamed, and not shared. The
  two beats ask different questions, and the only thing they have in common is the primitives they both call.
- **Reusing the retired records.** `000819` and `000801` are retired in place, not repurposed.

---

## Design overview

### Beat shape

The four-state `IBeat` lifecycle is unchanged. RUNNING and CLEANUP keep their current stage dispatch. COMPOSE
becomes a sub-state machine in the shape `AmbushBeat` and `NPCLetterBeat` already use:

| Sub-phase        | Work                                                                                      |
| ---------------- | ----------------------------------------------------------------------------------------- |
| `ComposingLLM`   | Unchanged — fire the compose LLM, store the briefing in `VisitState`.                     |
| `SelectingPoint` | Resolve both origins, run `VisitArrivalPoint::Find`. Failure → CLEANUP with a reason.     |
| `StartingQuest`  | Snapshot the sender's pose, place the return anchor, `EnsureQuestStarted`; stage 0 → 10.  |
| `Warping`        | Place the arrival marker, `MoveTo` the sender, delete the marker; VM-dispatch both fills. |
| `VerifyingFill`  | Read `BGSRefAlias::GetReference()` back on `Sender` and `ReturnAnchor` on a later tick.   |
| `Arming`         | `EvaluatePackage()`, begin the `StuckRecovery` escort; → RUNNING.                         |

`SelectingPoint` runs before `StartingQuest` deliberately. The arrival search is the step most likely to fail,
and failing it before the quest starts means there is no quest to tear down — the beat returns to
`NOT_RUNNING` having spent an LLM call and nothing else.

The split between `Warping` and `VerifyingFill` is forced by `QuestUtils::VMDispatchOnQuest` being
fire-and-forget: it reports whether queuing succeeded, not whether the call did. This is the same
verify-after-dispatch shape `AmbushBeat` uses.

### Arrival point selection

#### Why not `AmbushSpawnPoints`

`AmbushSpawnPoints::Find` samples a ring of azimuths around the player and ranks the forward arc ahead of the
rear as a hard tier — because attackers lie in wait ahead of a traveller. Its whole structure encodes "any
direction will do, prefer this one." A visit needs the opposite: one direction, fixed by where the visitor
lives, and no freedom to substitute another.

Reworking it into a shared module with a pluggable ranking would leave two callers sharing a ring search that
only one of them wants. What the two genuinely share is primitives — `StuckRecovery::IsOnNavmesh`,
`IsUnderwater`, ground height, and `CameraVisibility::IsPositionBehindCover` — and those are already factored
out and already shared. `VisitArrivalPoint` is a new module that calls the same primitives.

#### The route query

The fine road graph covers the loaded cell grid and moves with the player. That is what makes the query work,
because it puts the high-resolution half of any route exactly where the arrival point has to be.

1. **Find where the visitor is.** `RoadRoute::ResolveOrigin` answers this for anything loaded, and follows a
   load door out when they are indoors — but it reads `GetParentCell()`, which is `return parentCell` and is
   null for any reference the engine has not attached, and its load-door walk enumerates references an
   unloaded cell does not have. **A visit sender is almost never loaded** — bringing somebody from elsewhere
   is the entire beat — so that call answers for exactly the visitor who did not need bringing. The first
   in-game run failed here on every dispatch. So the origin comes off a ladder, best first: the live position
   when they are loaded; else `GetSaveParentCell()` plus `data.location`, which travel with a reference
   whether or not it has 3D; else — when that save cell is an **interior**, which is where most people are
   most of the time — the map marker of that cell's `Location`, climbing `parentLoc` until one carries a
   marker; else the marker of whatever `Location` the record itself files them under. The rung that answered
   is logged, because a route staged off the lower ones points at where the visitor *lives* rather than where
   they are.

   The `parentLoc` climb is not optional detail. A room does not usually carry a map marker — "Hall of
   Attainment" has none, "College of Winterhold" does — so stopping at the cell's own `Location` resolves for
   almost nobody who lives indoors. The second in-game run failed on exactly that, having already been fixed
   once for the unloaded case.
2. **`RoadRoute::Route(worldSpace, playerPos, senderOrigin)`** — note the argument order. We route *from the
   player outward toward the sender*, not in the direction the visitor travels. `Plan::finePath` is documented
   as "ordered walkable points from the start outward", so routing this way traces the road away from the
   player in the direction of the visitor's home. Walking that polyline outward is walking backward along the
   route the visitor would have taken.
3. **Walk `finePath` outward and take the first point that passes every gate.** Because fine nodes are
   centroids of `kPreferred`-flagged navmesh triangles, every candidate is on navmesh and on standable ground
   *by construction*. The gates that make up most of `AmbushSpawnPoints` — ground height resolvable, roughly
   level, not underwater, on navmesh — are not needed here. What remains is cheap:
   - straight-line distance to the player within
     `[iVisitMarkerMinDistanceUnits, iVisitMarkerMaxDistanceUnits]`
   - the parent cell is loaded
   - `CameraVisibility::IsPositionBehindCover`
4. **Keep the remaining passing points, in road order, as `fallbacks`.** This is strictly better supply than
   the ambush version gets: every fallback is another point on the same road, further from the player, still
   in the right direction. Escalating through them moves the sender back along their own route rather than
   sideways onto unrelated terrain.

Distance for the band is straight-line to the player, not length along the road. The band exists to control
whether the arrival is perceptible and how long the approach takes to watch; both are functions of how far
away the sender actually is.

#### The degradation ladder

`RoadRoute::Route` degrades in steps rather than failing, and the arrival search inherits that:

- **Tier 1 — a point on the fine road.** The walk above. Reads as *"they came up the road."* This is the
  outcome the phase exists to produce.
- **Tier 2 — coarse bearing.** No fine coverage on the relevant stretch, or the player is indoors and
  `FineRoads::Poll` has left the graph empty. Take the bearing from the player toward the first node of
  `Plan::coarsePath` and search along it for a standing point in the band, using the `AmbushSpawnPoints` gates
  because none of the by-construction guarantees hold off-road. Reads as *"they came from the right
  direction"* — not on the road, but not contradicting the story either.
- **Tier 3 — clean COMPOSE failure.** No any-direction fallback. A visitor materialising opposite the
  direction of their home is worse than no visit at all: it is precisely the tell this phase removes, and
  shipping it as a fallback would mean the beat's worst case is the thing we set out to fix.

The ladder is worth stating as a policy and not just a code path, because the tempting fourth tier — "spawn
them anywhere plausible rather than skip the beat" — trades the whole point of the phase for a frequency
number.

#### The cover gate

**This is the highest-risk piece of the design**, in the same way the navmesh gate was Phase 11's.

`CameraVisibility::IsPositionBehindCover` is deliberately strict: it samples a silhouette across the body's
width and height rather than one vertical line, every ray must be blocked, and it fails toward *not* covered —
because a wrong "yes" lets the player watch someone materialise in front of them.

Roads are open by nature, and sightlines along them are long. On the flat approach to Whiterun there may be no
fully-covered point anywhere in an 800–2500 unit band. What saves the design in the common case is that
following the road polyline is exactly what carries a point behind a bend, a rise or a treeline — the terrain
feature that hides an arrival is usually a property of the road's own shape. But the failure case is real, and
its frequency is not knowable by reading code.

So the thresholds are not chosen up front. An early step probes the gate against real terrain — a spread of
origins and player positions across open plain, forest, mountain pass and city approach — and reports, per
band, how often Tier 1 finds a covered point. The answer picks between three responses, and the probe exists
to make that choice on data rather than on taste:

- widen the band, which costs approach time,
- relax the gate from "fully covered" to "least visible candidate in the band", which risks a visible pop-in,
- or accept the Tier 1 miss rate and let Tier 2 carry those cases.

`coverRadiusUnits` is a single actor here, not a cluster, so it is the narrow end of that parameter's range —
one more reason the visit gate may behave quite differently from the ambush one that shaped it.

**The height passed matters as much as the radius, and is not the actor's height.** `IsPositionBehindCover`
samples three heights — 10%, 50% and 90% of what it is given — so the topmost ray lands at `0.9h` above the
feet rather than at `h`. Passing the nominal humanoid 128 put the top ray at 115 units while a visitor's crown
sits near 128, and an Altmer's nearer 138: cover that stopped all three rays could still leave a head in plain
view. A tester watched exactly that happen. The gate is now given 160, putting the top ray at 144 and clear of
the tallest playable race. Deliberately conservative — being too tall costs a usable spot, being too short
costs the illusion — and it makes the gate stricter, so it moves the Tier 1 hit rate this step is measuring.

#### What the ribbon costs us

Fine nodes are raw triangle centroids, so the graph is a ribbon two or three nodes wide rather than a thinned
centreline; Phase 12 deferred thinning deliberately. Two consequences, both acceptable:

- `finePath` zigzags across the width of the road. Harmless for choosing a point — any point on the ribbon is
  on the road — and the engine paths on navmesh between successive targets anyway.
- Path length along `finePath` is not a trustworthy distance measure. This phase never uses it as one; the
  band is straight-line and the traversal only needs the ordering.

### Alias discipline

`Sender` and `ReturnAnchor` become `Optional` with no fill rule and are force-filled from C++ after
`EnsureQuestStarted` returns. Phase 11 established that this ordering is the supported one, and that Phase 04's
decision to abandon `ForceRefTo` does not generalise: Phase 04's failures were `ForceRefTo` on a *stopped*
quest, and the Creation Kit's inability to author a genuinely-empty Specific Reference fill. Neither applies to
an `Optional` alias with no fill rule, filled after start.

Aliases remain the right holder for both references, for the reasons Phase 11 set out: alias-held references
are persistent and survive cell unload; the `Sender` alias carries both AI packages in its `PackageData` and
the engine instances them per alias instance; and teardown is a walk over the aliases rather than over a list
we maintain ourselves across saves.

`ReferenceAlias.ForceRefTo` has no CommonLibSSE-NG binding — `RE::BGSRefAlias` exposes only `GetReference()`
and `GetActorReference()` — so both fills go through the quest script, passed as FormIDs rather than
references, per
[`passing-references-to-papyrus-from-cpp.md`](../engine-findings/passing-references-to-papyrus-from-cpp.md).

### Retiring the sender faction

`_ne_VisitSenderFaction` exists for one reason: to give the `Sender` alias's Find-Matching-Reference rule
something to match on. Once that rule is gone, so is the faction's only job.

Both packages were checked for dependencies and neither has any — `_ne_VisitFollow` and `_ne_VisitReturnTravel`
are conditioned on `GetStage` against the owning quest and target `PlayerRef` and `Alias: ReturnAnchor`
respectively. Nothing else in the plugin reads the faction.

Retiring it deletes `PromoteSenderToDesignated` / `DemoteSenderToCandidate` and their call sites, and takes the
rank-leak failure mode with them. `_ne_SpawnMarkerTypeList` goes the same way: after this phase the visit quest
is its only referent, and the visit quest no longer refers to it.

### The return anchor

C++ creates the anchor itself, at the sender's home position, *before* the warp — `sender->PlaceObjectAtMe`
places in the sender's own cell, which is what the alias-chained Create Reference To Object was doing
implicitly and less controllably. It is then force-filled like any other slot.

Separately, a null anchor stops being fatal. `VisitState::Snapshot` already carries `returnPosition`,
`returnAngleZ` and `returnCellFormID`; every cleanup path that today skips `MoveTo` when the anchor is missing
falls back to those instead. The snapshot's dynamic-FormID remap already handles the `0xFF`-range reference
this produces, so creating the anchor from C++ rather than from a fill rule costs nothing in persistence.

### Stuck recovery on the approach

`StuckRecovery::Escort` begins at `Arming`, supplied with the `fallbacks` from the arrival search, and runs for
the duration of the Salutation approach. The post-combat ReEngage approach reuses the same escort.

This is the piece that makes the tiered ladder safe to lean on. A Tier 2 point is chosen with less information
than a Tier 1 point and is correspondingly more likely to strand someone; the escort is what turns that from a
rolled-back visit into a slightly longer walk.

---

## Open engine questions

Each is resolved by an early step, with a fallback so no step can dead-end.

1. **Can the sender path to a player who is indoors?** If `RoadRoute::ResolveOrigin` is used on the *player* as
   well, the arrival point can be placed on the road outside the building and the Follow package left to bring
   them through the load door. Vanilla follow packages do traverse load doors, but that is a runtime-behaviour
   claim about package templates, not something the headers settle, and Phase 11's experience with the
   non-existent AI Package "Priority" field is the reason we do not design on it unverified. Fallback: gate
   indoor players out in `IsAvailable` — which at least fails before the compose LLM call rather than after
   it.
2. **How often does Tier 1 find a covered point?** See **The cover gate**. Resolved by probe. Fallback: the
   three responses listed there, chosen on the probe's numbers.
3. **Does the fine graph reach far enough for the band?** The band tops out at 2500 units and the loaded grid
   is roughly five cells across, so it should, but the graph covers only cells carrying flagged road triangles
   and a player off-road may sit near its edge. Fallback: Tier 2 already covers it; the question is how often
   Tier 2 fires, not whether it works.
4. **Does `Actor::MoveTo` land the sender correctly across a worldspace boundary?** `MoveTo` is used on this
   beat's cleanup paths today, so the binding works; what is unverified is a long-haul move onto a
   dynamically-placed marker. Fallback: `SetPosition` plus `Update3DPosition` after a `MoveTo` into the
   player's cell, which is the two-step `AmbushBeat` already performs for a different reason.

---

## ESP content

No new records. The highest FormID currently in use is `000832` (`_ne_AmbushApproach`) and this phase allocates
nothing.

| Record                    | Type | FormID   | Change                                                 |
| ------------------------- | ---- | -------- | ------------------------------------------------------ |
| `_ne_VisitQuest`          | QUST | `00082D` | Alias 4 deleted; aliases 1 and 5 re-flagged `Optional` |
| `_ne_VisitSenderFaction`  | FACT | `000819` | Retired — its only referent was the alias 1 fill rule  |
| `_ne_SpawnMarkerTypeList` | FLST | `000801` | Retired — its only referent was the alias 4 fill rule  |
| `_ne_VisitFollow`         | PACK | `00082E` | Unchanged                                              |
| `_ne_VisitReturnTravel`   | PACK | `00082F` | Unchanged                                              |

This mod is unreleased, so there are no saves whose ChangeForms need protecting, and no reason to keep either
retired record as an inert shell — the staged-removal dance Phase 11 performed for `000800` does not apply.
`000801` has now been deleted, restored and deleted again across two phases; this time nothing refers to it.

### Quest stages

Unchanged. Stage 10's fragment loses its body — the warp it used to trigger now happens in C++ before the stage
is ever read — and the stage itself stays, because the packages are conditioned on it.

### Aliases

| Alias          | ID | Fill before                                      | Fill after          | Flags after |
| -------------- | -- | ------------------------------------------------ | ------------------- | ----------- |
| `PlayerRef`    | 0  | Forced → `000014:Skyrim`                         | unchanged           | unchanged   |
| `Sender`       | 1  | Find Matching Ref, `GetFactionRank >= 4`         | none, forced by C++ | `Optional`  |
| `SpawnMarker`  | 4  | Find Matching Ref, five conditions               | deleted             | —           |
| `ReturnAnchor` | 5  | Create Ref To Object, XMarker at `Alias: Sender` | none, forced by C++ | `Optional`  |

`Sender` keeps its `PackageData` — both packages stay on the alias, and the engine keeps swapping between them
on `GetStage` exactly as it does today.

### Papyrus

`esp/Source/Scripts/_ne_VisitQuest.psc`:

- **Added:** `FillSenderSlot(int aiFormID)` and `FillReturnAnchorSlot(int aiFormID)` — thin `ForceRefTo`
  trampolines, taking FormIDs for the reason the engine-findings doc records.
- **Removed:** `MoveSenderToSpawnMarker()`, and the `SpawnMarker` property with it.
- **Unchanged:** `StartReturnTravel`, `RunSenderAction`, `RunSenderNarration`, `RunSenderSilentSceneEvent`,
  `Shutdown`.

---

## Settings

Two existing `[Beats]` keys stop being dead. They keep their current names and defaults — the names describe
what they always claimed to do, and this phase is the one that makes the claim true.

| Key                            | Default | Change                                                           |
| ------------------------------ | ------- | ---------------------------------------------------------------- |
| `iVisitMarkerMinDistanceUnits` | `800`   | Now live. Was parsed and never read; the alias hardcoded `3000`. |
| `iVisitMarkerMaxDistanceUnits` | `5000`  | Now live. Was parsed and never read; the alias hardcoded `8000`. |

New keys. The set is deliberately small; the arrival search has one genuinely uncertain parameter and the rest
is structure.

| Key                               | Default | Meaning                                                            |
| --------------------------------- | ------- | ------------------------------------------------------------------ |
| `iVisitArrivalCoverRadiusUnits`   | TBD     | Silhouette width passed to `IsPositionBehindCover`. Set by probe.  |
| `bVisitArrivalAllowCoarseBearing` | `true`  | Whether Tier 2 may run, or Tier 1 failure goes straight to Tier 3. |

`iVisitArrivalCoverRadiusUnits` has no default until the cover-gate probe reports. Writing a number here before
then would be exactly the kind of chosen-by-feel magic constant the IntelEngine guidance warns about.

---

## Persistence

No cosave schema change.

`'NEVS'` (`VisitState`) keeps its current `Snapshot` layout. `returnAnchorFormID` now holds a reference C++
created rather than one an alias fill created, but it was already a dynamic `0xFF`-range FormID either way, and
the existing remap-on-load path handles it unchanged.

`'NBVS'` (`NPCVisitBeat_Persistence`) is untouched — it carries the per-sender cooldown table and memory
watermark, neither of which this phase reaches.

---

## File map

| File                                             | Change                                                  |
| ------------------------------------------------ | ------------------------------------------------------- |
| `include/VisitArrivalPoint.h`                    | New — the arrival search's API                          |
| `src/VisitArrivalPoint.cpp`                      | New — route query, tier ladder, gates, fallbacks        |
| `src/VisitArrivalPoint.engine.test.cpp`          | New                                                     |
| `src/NPCVisitBeat.cpp`                           | COMPOSE sub-state machine, C++ warp, force-fill, escort |
| `include/NPCVisitBeat.h`                         | Lifecycle comment, `Initialize` contract                |
| `src/NPCVisitBeat.engine.test.cpp`               | Fill expectations rewritten around force-fill           |
| `esp/Source/Scripts/_ne_VisitQuest.psc`          | Fill trampolines in, warp function out                  |
| `esp/plugin/Quests/_ne_VisitQuest - …`           | Alias rework                                            |
| `esp/plugin/Factions/_ne_VisitSenderFaction - …` | Deleted                                                 |
| `esp/plugin/FormLists/_ne_SpawnMarkerTypeList …` | Deleted                                                 |
| `include/Settings.h`, `src/Settings.cpp`         | Two new keys; two existing keys become load-bearing     |
| `statics/…/NarrativeEngine.ini`                  | New keys documented                                     |

---

## Implementation plan

Sequential. Unlike Phase 11 there is **no Creation Kit step**: this phase adds no records, and the three edits
it makes to `_ne_VisitQuest` — delete one alias, re-flag two — are expressible directly in the Spriggit YAML,
whose exact shape for an `Optional` alias with no fill rule is already visible in `_ne_AmbushQuest`. Step 2
carries a CK fallback in case the round-trip disagrees.

Steps 2 and 3 are the one place the tree is knowingly broken in between: Step 2 removes the fill rules and
Step 3 supplies the force-fill that replaces them. Do not stop between them.

The four steps that need a running game are last on purpose. Cover-gate calibration in particular reads a log
that only exists once visits are being dispatched, so putting it early would have meant writing a probe driver
into `NPCVisitBeat.cpp` for the sole purpose of making the search run — and Step 3 would then have deleted it.
Every step before Step 6 is Claude's alone and verifiable from a build and a test run.

Every step is entirely Claude's work or entirely the user's, never mixed. Verification is attributed
separately, since a step Claude implements may still need a running game to confirm.

### Test coverage is part of every step, not a step of its own

**A step that changes C++ lands its tests in the same step.** There is no catch-up testing step at the end,
and no step is complete with its tests deferred to the next one. Every branch a step adds — including every
failure path and every early return — is covered before its box is checked.

This is not the usual "write tests eventually" boilerplate. The whole branch this phase starts from is test
coverage: every module in `src/` acquired a suite, and the last stand-ins were retired in the process. Landing
this refactor with holes would undo that, and the holes would be in exactly the failure paths that are hardest
to reach by playing — a search that finds nothing, a fill that never lands, a cleanup with no anchor.

Three consequences that are easy to get wrong:

- **Deleting code means deleting its tests and checking what that uncovers.** Step 5 removes the faction
  machinery; the risk is not that its tests fail but that they quietly disappear and take a branch's coverage
  with them.
- **A step whose verification is in-game still says so explicitly.** Step 2 touches no C++ at all; its entry
  records that, so the absence reads as a decision rather than an oversight.
- **`pwsh -File build.ps1 test` passing is a floor, not the bar.** It also has to be true that the new
  branches are reached — a suite that builds and passes while never entering the new code is worse than no
  suite, because it looks like coverage.

The `/unit-test` skill is the tool for the new module in Step 1: it writes a Catch2 suite for one C++ module in
four gated stages, which is exactly the shape of `src/VisitArrivalPoint.engine.test.cpp`.

---

### Step 1 — Settings surface and the `VisitArrivalPoint` module

- [X] Complete

**[CLAUDE]**

**Goal:** The arrival search, standalone and observable, before anything depends on it. It produces the data
Step 6 reads, so its diagnostic logging is the deliverable as much as the search is.

**Files:** `include/VisitArrivalPoint.h`, `src/VisitArrivalPoint.cpp`, `src/VisitArrivalPoint.engine.test.cpp`,
`include/Settings.h`, `src/Settings.cpp`, `CMakeLists.txt`,
`statics/SKSE/Plugins/NarrativeEngine/NarrativeEngine.ini`.

**Sub-tasks:**

1. Settings: wire `iVisitMarkerMinDistanceUnits` / `iVisitMarkerMaxDistanceUnits` into the new module — names
   and defaults unchanged. Add `iVisitArrivalCoverRadiusUnits` (provisional `64`, final value set by Step 6)
   and `bVisitArrivalAllowCoarseBearing` (`true`). Document both in the deployed INI.
2. Implement the module's API:

   ```cpp
   enum class Tier : std::uint8_t { None, FineRoad, CoarseBearing };

   struct Result
   {
       RE::NiPoint3 point{};
       std::vector<RE::NiPoint3> fallbacks;  // further along the same road, best-first
       Tier tier = Tier::None;
       bool Ok() const { return tier != Tier::None; }
   };

   Result Find(const PluginThread::Token& pt, RE::Actor* sender, RE::Actor* player);
   ```

3. Tier 1: `RoadRoute::ResolveOrigin` on both actors, `RoadRoute::Route(worldSpace, playerPos, senderOrigin)`,
   then walk `finePath` outward applying the three gates. First pass wins; every later pass becomes a
   fallback.
4. Tier 2: bearing from the player toward `coarsePath`'s first node, sampling along it for a standing point in
   the band with the full `AmbushSpawnPoints` gate set, since none of the on-road guarantees hold off-road.
   Skipped entirely when `bVisitArrivalAllowCoarseBearing` is false.
5. Tier 3: return `Tier::None`. Not an error, and not logged as one.
6. **The per-search debug log**, one line per call, debug-mode gated: sender, both resolved origins and whether
   either came via a load door, worldspace, `finePath` node count, how many candidates died at each gate,
   the tier reached, the winning point with its straight-line distance, and the fallback count. A failed
   search must name the gate that killed it. This is the phase's most valuable diagnostic and Step 6 reads
   nothing else.
7. Add the module to `NARRATIVEENGINE_MOCKED_SOURCES` and write `src/VisitArrivalPoint.engine.test.cpp` with
   the `/unit-test` skill, covering: Tier 1 happy path, each of the three gates rejecting in isolation, Tier 2
   on an empty fine graph, Tier 2 suppressed by `bVisitArrivalAllowCoarseBearing`, Tier 3 when both tiers
   fail, fallback ordering, the band's two boundaries, an origin resolved via load door, and a sender and
   player in different worldspaces.
8. Run `pwsh -File format.ps1`.

**Specifics:**

- **Keep `Route` off the main thread.** `ResolveOrigin` and `IsPositionBehindCover` touch engine state and
  marshal through `MainThread::Run`; the routing between them is documented as a pure query needing no token,
  and Dijkstra has no business on the main thread. See
  [`MAIN_THREAD_STUTTER_AUDIT.md`](../MAIN_THREAD_STUTTER_AUDIT.md).
- Distance for the band is straight-line to the player. Road order is used only to decide traversal order.
- Never return a point in an unloaded cell. The sender is warped there and has to have 3D.
- Empty is a normal outcome, exactly as it is for `AmbushSpawnPoints::Find`.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` both succeed.
- A fixture with a synthetic fine graph reaches Tier 1; deleting the fine graph from the same fixture drops it
  to Tier 2; disabling coarse bearing drops it to Tier 3.

---

### Step 2 — ESP alias rework and Papyrus trampolines

- [X] Complete (Claude's half; user CK verification outstanding)

**[CLAUDE]**, with a **[USER]** Creation Kit fallback.

**Goal:** Remove every fill rule the beat currently depends on, and put the force-fill trampolines in place for
Step 3 to call. **The beat is broken at the end of this step** — that is expected and Step 3 fixes it.

**Files:** `esp/plugin/Quests/_ne_VisitQuest - 00082D_NarrativeEngine.esp.yaml`,
`esp/Source/Scripts/_ne_VisitQuest.psc`, `esp/Source/Scripts/_ne__QF__ne_VisitQuest_0500FB2E.psc`.

**Sub-tasks:**

1. Quest YAML: delete alias ID 4 (`SpawnMarker`); give aliases 1 and 5 `Flags: [Optional]` and remove their
   `Conditions` / `CreateReferenceToObject` blocks. Leave `Sender`'s `PackageData` untouched. Leave
   `NextAliasID` at 6 — IDs are never reused.
2. Quest YAML: drop the `Alias_SpawnMarker` and `SpawnMarker` script properties from both `VirtualMachineAdapter`
   entries.
3. `_ne_VisitQuest.psc`: delete the `SpawnMarker` property and `MoveSenderToSpawnMarker()`; add
   `FillSenderSlot(int aiFormID)` and `FillReturnAnchorSlot(int aiFormID)`, both resolving with
   `Game.GetFormEx` and calling `ForceRefTo`, both tracing what they filled.
4. Fragment script: empty `Fragment_1` (Stage 10) and remove its `Alias_SpawnMarker` property.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- `Game.GetFormEx`, not `Game.GetForm` — the return anchor is a dynamically-created reference at `0xFF……` and
  needs the full 32-bit range.
- Take FormIDs, not `ObjectReference`s. A reference passed from C++ arrives non-`None` but unpacks to null
  inside `ForceRefTo`; see
  [`passing-references-to-papyrus-from-cpp.md`](../engine-findings/passing-references-to-papyrus-from-cpp.md).
- Copy the `Optional`-with-no-fill-rule shape from `_ne_AmbushQuest`'s attacker aliases rather than inventing
  it. `AllowDead` is *not* wanted here — a sender who dies mid-visit is a hard-abort, not a state to keep
  holding.
- The mod-folder ESP must be clean before editing. If it has drifted from a prior CK session, `sync-esp.ps1`
  refuses to auto-resolve and the edit will be clobbered or rejected.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` succeeds, including the Papyrus compile and the ESP deserialize.
- Re-serializing the mod-folder ESP reproduces the hand-edited YAML — the round-trip is stable.
- **No unit tests are added, and that is deliberate:** this step changes only ESP records and Papyrus, neither
  of which either test executable can reach.
- **Corrected after the fact:** this step predicted the `NPCVisitBeat` suite would go red until Step 3. It does
  not. `NPCVisitBeat.engine.test.cpp` fabricates its own quest through `AddQuestWithAliases`, so it never
  reads the shipped record and an ESP edit is invisible to it. The suite stays green across this step, which
  means **Step 2 has no automated signal at all** — the build, the round-trip and the CK are the whole of its
  verification.
- What does break is the runtime: `NPCVisitBeat_Init::Initialize` requires all three aliases, so with
  `SpawnMarker` gone it logs `SpawnMarker=MISSING`, sets `g_pointersCriticallyMissing`, and the beat reports
  itself unavailable for the session. That is the intended broken window between Steps 2 and 3, and it fails
  closed rather than wedging.

**Verify [USER]:**

- The Creation Kit opens `_ne_VisitQuest` without complaint, shows three aliases, and shows `Sender` and
  `ReturnAnchor` as Optional with no fill type.
- **Fallback if either verification fails:** author the same three changes in the CK by hand and let the sync
  serialize them back. Nothing later in the phase depends on which route produced the record.

---

### Step 3 — COMPOSE rework: arrival, warp, force-fill, verification

- [X] Complete (Claude's half; user in-game verification outstanding)

**[CLAUDE]**

**Goal:** The beat works again, on the new spine. This is the load-bearing step.

**Files:** `src/NPCVisitBeat.cpp`, `include/NPCVisitBeat.h`, `src/NPCVisitBeat.engine.test.cpp`.

**Sub-tasks:**

1. Replace COMPOSE's current flow with the six sub-phases from **Beat shape**. `SelectingPoint` runs before
   `StartingQuest` so a failed search costs no quest teardown.
2. `StartingQuest`: snapshot the sender's pose into `VisitState` as today, then `sender->PlaceObjectAtMe`
   the return anchor **before** anything moves, then `EnsureQuestStarted`.
3. `Warping`: `player->PlaceObjectAtMe` a temporary XMarker, `SetPosition` it onto the arrival point,
   `senderActor->MoveTo` that marker, then delete the marker. VM-dispatch `FillSenderSlot` and
   `FillReturnAnchorSlot`.
4. `VerifyingFill`: on a **later** tick, read `BGSRefAlias::GetReference()` on both aliases. Either missing
   after a bounded number of ticks → CLEANUP with a specific `failure_reason`.
5. `Arming`: `EvaluatePackage()` on the sender, then → RUNNING. Stage stays at 10; the Salutation handler is
   unchanged.
6. Give every failure path a distinct `failure_reason` — `arrival_no_point`, `arrival_tier3`,
   `quest_start_failed`, `warp_failed`, `sender_fill_unverified`, `anchor_fill_unverified`.
7. Rewrite the suite's fill expectations around force-fill, and add a case per new sub-phase transition: the
   happy path through all six; each of the six `failure_reason` paths; a fill that lands only on the tick
   after the readback, proving the verification waits rather than failing early; and the temporary arrival
   marker being deleted on both the success and the failure path. The harness already mocks
   `PlaceObjectAtMe`, `SetPosition` and `MoveTo`, so no harness work is expected — if any is needed, that is
   itself worth recording.
8. Run `pwsh -File format.ps1`.

**Specifics:**

- Read the aliases back on a **later tick**, never the same one. `VMDispatchOnQuest` reports only that the
  call was queued.
- Delete the temporary arrival marker in the same sub-phase that created it. It exists only as a `MoveTo`
  target and nothing later refers to it; the return anchor is the one that persists.
- Order matters at `StartingQuest`: the anchor is placed at the sender's home position, so it has to be
  created before the warp moves them.
- `MoveTo` over `SetPosition` for the sender — the sender may be in a different worldspace entirely, which raw
  coordinates would not survive.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` both succeed, and the `NPCVisitBeat` suite is
  green again for the first time since Step 2.
- A test forces each of the six `failure_reason` paths and asserts CLEANUP is reached with no quest left
  running and no reference left behind.
- Every one of the six COMPOSE sub-phases is entered by at least one test. A suite that passes while skipping
  `VerifyingFill` is the failure this step is most likely to hide.

**What came out differently:**

- **`arrival_tier3` was a duplicate and is not implemented.** `Find` reports success or failure, not which
  tier it stopped at, so the beat cannot tell "declined at Tier 3" from "declined for any other reason" —
  both are `arrival_no_point`. The tier that *did* win is logged by `VisitArrivalPoint` itself. Five distinct
  reasons ship: `arrival_no_point`, `quest_start_failed`, `warp_failed`, `sender_fill_unverified`,
  `anchor_fill_unverified`, alongside the compose-LLM and parse failures that already existed.
- **The arrival marker is deleted at the top of `VerifyingFill`, not at the end of `Warping`.** The step said
  to delete it in the sub-phase that created it. `MoveTo` is believed synchronous, but that is a claim about
  engine internals nobody here has verified, and deleting a reference an actor is mid-move onto would strand
  them. Waiting one tick costs nothing and does not rest on the claim.
- **Harness work was needed after all, and it was one function.** `EngineMock::AddStatic` — a bound object
  that is not an NPC, which is what every marker in the game is. `PlaceObjectAtMe` branches on exactly that
  distinction, so standing a Book in for an XMarkerHeading would have tested the wrong path.
- **A sender already standing beside the player can no longer be visited.** Routing from the player to
  someone a hundred units away yields a one-node path entirely inside the minimum distance, so the search
  declines and the beat gives up. That is defensible — there is no journey to stage — but it is a real
  narrowing of who the Director can pick, it is not what the old marker-based beat did, and nothing in this
  phase's design called for it. Worth a decision during Step 8 rather than leaving it as a side effect.

**Verify [USER]:**

With `bDebugMode=true`, standing outdoors on or near a road:

1. Force-dispatch `npc_visit` from the dashboard's Dispatch tab.
2. The log names the tier reached and the chosen point.
3. The sender appears out of view, **on the road, on the side of the player that their home is on**. Check the
   direction against where the sender actually lives — this is the phase's whole point and the one thing no
   test can assert.
4. They walk up the road and deliver their opening line; the visit proceeds exactly as before.
5. The visit completes, the sender walks home, and no marker is left behind at either end.

---

### Step 4 — Approach hardening: escort, and a non-fatal return anchor

- [X] Complete

**[CLAUDE]**

**Goal:** A sender who cannot reach the player gets recovered instead of timing out, and a missing return
anchor stops stranding people.

**Files:** `src/NPCVisitBeat.cpp`, `src/NPCVisitBeat.engine.test.cpp`.

**Sub-tasks:**

1. Begin a `StuckRecovery::Escort` at `Arming`, supplied with the arrival search's `fallbacks`, and run it for
   the Salutation approach. Reuse the same escort for the post-combat ReEngage approach.
2. Replace every `if (senderActor && anchorRef)` cleanup guard with a fallback to `VisitState::Snapshot`'s
   `returnPosition` / `returnAngleZ` / `returnCellFormID` when the anchor is missing. Four call sites.
3. Make a missing anchor non-fatal at compose: log it, carry on, let cleanup use the snapshot.
4. Tests: an escort that exhausts its fallbacks still resolves the beat; a null anchor at each cleanup path
   still returns the sender home.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- The escort's supply is road points from the same route, so escalation walks the sender *back along their own
  path* rather than sideways. Preserve that ordering; do not sort the fallbacks by distance to the player.
- The approach timeout stays as the outer backstop. The escort reduces how often it fires; it does not replace
  it.

**What came out differently:**

- **The self-MoveTo was worse than documented.** `RunReturnHomeShutdown` did not skip the move when the
  anchor was missing — it moved the sender onto their own reference, which moves nobody anywhere while
  reading like a fallback. All three paths now go through one `SendSenderHome`, which uses the anchor when
  there is one and the snapshotted position when there is not.
- **The fallback cannot cross a cell boundary.** It is a position, not a reference, so a visit that started
  indoors and lost its anchor leaves the sender outside their own front door. Recorded in the code; better
  than the nothing it replaced, and not worth a second marker to fix.
- **Removing a one-shot latch in `Initialize` was necessary, and it fixed the suite rather than the beat.**
  `g_pointersResolved` made the function resolve forms on its first call only. Production calls it once at
  `kDataLoaded`, so nothing changes there — but in the harness every test leaf after the first was running
  against pointers into `EngineMock` storage a later case had already recycled. The faction assertions had
  been passing on the coincidence that the recycled address still held a faction. `RegisterSinks` has its
  own guard, so the latch was protecting nothing. Found by accident when an unrelated addition to the test
  world shifted the allocation pattern and the coincidence stopped holding.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 test` succeeds.
- No cleanup path remains that can skip returning the sender home.
- Disabling the escort drive makes the stalled-visitor case fail, so the case is testing the escort rather
  than describing it.

---

### Step 5 — Retire the sender faction and the spawn-marker form list

- [X] Complete

**[CLAUDE]**

**Goal:** Delete the two records and the C++ that served them, now that nothing reads either.

**Files:** `src/NPCVisitBeat.cpp`, `include/NPCVisitBeat.h`,
`esp/plugin/Factions/_ne_VisitSenderFaction - 000819_NarrativeEngine.esp.yaml`,
`esp/plugin/FormLists/_ne_SpawnMarkerTypeList - 000801_NarrativeEngine.esp.yaml`.

**Sub-tasks:**

1. Delete `PromoteSenderToDesignated`, `DemoteSenderToCandidate`, `kSenderRankCandidate`,
   `kSenderRankDesignated`, `kVisitFactionEditorID`, the resolved faction handle, and every call site.
2. Drop the faction from `NPCVisitBeat_Init::Initialize` and from the header's documented contract.
3. Delete both YAML records.
4. Delete the suite's faction expectations along with the code, then **read the diff for what stopped being
   asserted**. Any branch that was only reached because a test set up a faction rank needs a replacement case
   that reaches it another way; a test removed without checking that is a coverage hole disguised as a
   cleanup.
5. Add one case asserting `Initialize` succeeds with the faction absent — the failure mode this step could
   introduce is a stale lookup that logs a missing form and leaves `g_pointersCriticallyMissing` set, which
   would disable the beat silently.
6. Run `pwsh -File format.ps1`.

**Specifics:**

- Confirm the referent count is zero before deleting, not after: grep `esp/plugin/` for `000819` and `000801`
  and expect only the records' own files. Phase 11 deleted `000801` on that assumption without checking and
  took NPC Visit down with it.
- `NPCLetterBeat` has its own separate sender faction and its own promote/demote pair. Leave both alone — the
  names are nearly identical and the letter beat still needs its fill rule.

**What came out differently:**

- **Four suite sections went with the code, and none of them lost coverage.** Three asserted the sender was
  put back down to candidate rank on an abort or rollback, and one that they were promoted in the first
  place. Their subject was the fill rule leaking a designated sender onto somebody else's visit — with no
  fill rule there is nothing to leak, so these are not tests that lost their subject but tests whose subject
  stopped existing. What releases the sender now is the quest's `Reset()` clearing the forced fill, which
  "tears the visit down" already covers.
- **Deleting the faction lookup would have failed silently.** `Initialize` set `ok = false` when the faction
  did not resolve, which disables the beat for the session behind one log line. Leaving that lookup in place
  against a deleted record is exactly the failure this step could have shipped, so a case now starts the beat
  up in a world with no faction in it and asserts it still offers itself.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` both succeed, and
  `grep -rn "VisitSenderFaction\|SpawnMarkerTypeList"` over `src/`, `include/` and `esp/` returns nothing.
- A full visit still runs end to end in the test suite.
- The suite's case count did not drop by more than the number of cases that tested the deleted code itself.
  A larger drop means a branch lost its only cover.

---

### Step 6 — Cover-gate calibration

- [ ] Complete

**[USER]** to gather, **[CLAUDE]** to analyse and set the defaults.

**Goal:** Resolve open questions 2 and 3 on data. Decide `iVisitArrivalCoverRadiusUnits`, and decide whether
the gate stays "fully covered" or relaxes to "least visible candidate in the band."

**Files:** `include/Settings.h`, `src/Settings.cpp`, this doc's **Settings** table, and — if the gate relaxes —
`src/VisitArrivalPoint.cpp`.

**Sub-tasks [USER]:**

1. Build with `bDebugMode=true` and force-dispatch `npc_visit` repeatedly from the dashboard's Dispatch tab,
   moving between terrain types between dispatches: the plain outside Whiterun, forest around Falkreath, a
   mountain pass, the approach to a walled city, and at least one stretch of tundra with no cover for a long
   way.
2. Hand over the resulting log.

**Sub-tasks [CLAUDE]:**

1. Aggregate the per-search lines into tier rates and per-gate kill counts, bucketed by terrain. **The
   aggregation script is throwaway and goes in the scratchpad**, not in the repo.
2. Pick `iVisitArrivalCoverRadiusUnits` from the cover-gate kill rate.
3. Choose between the three documented responses — widen the band, relax the gate, or accept the Tier 1 miss
   rate — and record which, and why, in **Post-implementation**.
4. Update this doc's **Settings** table to replace `TBD` with the chosen value.
5. **If the gate relaxes**, that is a behaviour change and needs its own cases: a band in which no candidate
   is fully covered now returns the least-visible one rather than nothing, and a band in which one candidate
   *is* fully covered still prefers it over a less-covered nearer one. Both go into
   `src/VisitArrivalPoint.engine.test.cpp` in this step.
6. Whether or not the gate relaxes, pin the chosen `iVisitArrivalCoverRadiusUnits` in a test that fails if the
   default moves — the value is the output of a measurement, and a silent edit to it should not pass.
7. Run `pwsh -File format.ps1`.

**Specifics:**

- **This step is deliberately late.** It was originally second, which would have needed a throwaway probe
  driver written into `NPCVisitBeat.cpp` purely to make the search run — wiring that Step 3 would then have
  rewritten. Sitting here instead, every dispatched visit already calls `Find` and logs a line, so the
  measurement comes off the real beat and needs no scaffolding at all.
- One search per dispatched visit is a slower rate than a polling probe would have given, so this wants
  several sessions' worth of dispatches rather than one. Sample size is the cost of not building the probe.
- The per-search line carries both resolved origins as positions, the band and cover radius in force, the
  bearing home against the bearing actually chosen, and the per-gate kill counts. A visit that reads wrong in
  game can therefore be diagnosed from the log rather than by running it again — which matters because these
  are expensive runs and the interesting failures are intermittent.
- A Tier 1 rate that is *too* high is also a finding — it would mean the gate is not actually rejecting
  anything and open-road pop-in will show up in Step 8.

**Verify [CLAUDE]:**

- The chosen value is justified by a number in the log, not by taste.
- `pwsh -File build.ps1 test` succeeds, and the suite fails if `iVisitArrivalCoverRadiusUnits` is edited.

---

### Step 7 — Resolve the player-indoors question

- [ ] Complete

**[USER]** to answer, **[CLAUDE]** to implement whichever branch the answer picks.

**Goal:** Resolve open question 1 and act on it. This is the only step whose outcome changes scope.

**Files:** `src/NPCVisitBeat.cpp`, and `src/VisitArrivalPoint.cpp` if indoor support lands.

**Sub-tasks [USER]:**

1. Stand inside an inn. Force-dispatch `npc_visit`. The arrival search will reach Tier 2 at best, since
   `FineRoads` is empty indoors.
2. Report whether the sender reaches the player: do they path to the building, open the load door, and come
   in — or do they stand outside until the approach timeout?

**Sub-tasks [CLAUDE], branch A (they come in):**

1. Resolve the player's origin through `RoadRoute::ResolveOrigin` as well, so the arrival point lands on the
   road outside the building the player is in rather than being measured from a position on the wrong side of
   a load door.
2. Extend the distance band's meaning for this case: straight-line from the *player's load-door origin*, not
   from the player.
3. Tests in `src/VisitArrivalPoint.engine.test.cpp`: a player whose origin resolves via load door measures the
   band from the door and not from their interior position; an interior with no load door falls back to the
   location marker; an interior with neither returns Tier 3 rather than a point at the origin.
4. Record the verified load-door behaviour in `docs/engine-findings/`.

**Sub-tasks [CLAUDE], branch B (they do not):**

1. Gate indoor players out in `NPCVisitBeat::IsAvailable`, before the candidate-pool count so the cheap check
   runs first.
2. Tests in `src/NPCVisitBeat.engine.test.cpp`: an interior player makes `IsAvailable` false; an exterior
   player with the same candidate pool makes it true; and the interior check short-circuits before
   `SenderCandidatePool::CountViable` is called, which is the ordering the step exists to get right.
3. Record the finding in `docs/engine-findings/` anyway — a negative result about package templates and load
   doors is worth the same as a positive one.

**Specifics:**

- Do not design branch A before the answer arrives. Vanilla follow packages are widely assumed to traverse
  load doors; Phase 11's non-existent AI Package "Priority" field is why this project does not build on widely
  assumed.

**Verify [CLAUDE]:**

- `pwsh -File build.ps1 test` succeeds, with whichever branch's cases landed.

**Verify [USER]:**

- Branch A: a visit dispatched while indoors completes, with the sender entering through the door.
- Branch B: a visit is never offered while indoors, and the Director picks something else without burning a
  compose call.

---

### Step 8 — End-to-end in-game validation

- [ ] Complete

**[USER]**

**Goal:** Confirm the phase's actual claim — that visitors arrive from where they live, by road — across
enough varied situations to believe it.

**Sub-tasks:**

1. Dispatch at least six visits from senders whose homes lie in clearly different directions, from at least
   three different player positions. Record, for each: the sender, where they live, the tier reached, and the
   compass direction they arrived from.
2. Dispatch one visit each from: a spot with no road within the band, a mountain pass, and a city gate.
3. Force a Tier 3 (dispatch somewhere with no route at all — a remote interior-only area or across a
   worldspace boundary) and confirm a clean CLEANUP with `arrival_tier3`, no quest left running, and no
   orphaned references.
4. Kill the sender mid-approach. Confirm hard-abort still fires and cleans up.
5. Save mid-visit, quit to main menu, reload. The beat resumes or falls to CLEANUP; it must not wedge.
6. Watch one full arrival from a third-person camera pointed at the arrival direction, to confirm the cover
   gate is doing its job and nobody pops in on screen.

**Verify [USER]:**

- No visit in the run arrives from a direction that contradicts where the sender lives.
- No run leaves an orphaned XMarker at either end.

---

## Done condition

This phase is complete when:

- All 8 implementation steps are checked off.
- Step 8's validation run passes without intervention.
- `pwsh -File build.ps1 test` is green, and every branch this phase added is entered by at least one test —
  each COMPOSE sub-phase, each `failure_reason`, each arrival tier, and each cleanup path with and without a
  return anchor. No step deferred its tests to a later one, and `src/VisitArrivalPoint.cpp` did not ship
  without `src/VisitArrivalPoint.engine.test.cpp` beside it.
- The four open engine questions are resolved, with their answers recorded in **Post-implementation** and any
  engine-behaviour findings written up under `docs/engine-findings/`.
- A visitor's arrival direction is observably a function of where they live — the claim in **The narrative
  flaw**, confirmed against at least six senders with distinct home directions.
- No alias fill failure can end the beat: forcing either fill to fail produces a clean CLEANUP with a specific
  `failure_reason`, never a wedge and never a rollback that strands the sender.
- `iVisitMarkerMinDistanceUnits` and `iVisitMarkerMaxDistanceUnits` demonstrably change arrival distance.
- `_ne_VisitSenderFaction` and `_ne_SpawnMarkerTypeList` are gone, and nothing in the tree refers to either.
- No orphaned dynamic references remain after any completion, rollback, or abort path.

---

## Post-implementation additions

*Populated after implementation completes, mirroring Phase 09's and Phase 11's practice. The four open engine
questions' answers land here, along with the cover-gate calibration result and anything that arrived beyond
the numbered plan.*
