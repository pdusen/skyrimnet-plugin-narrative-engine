# Phase 17 — Arrivals inside the walled cities

A visit to a player standing inside Whiterun, Riften, Solitude, Markarth or Windhelm takes a different path
through `VisitArrivalPoint` than every other visit: the two ends are in different worldspaces, so the chain is
never built, and `Tier::CityApproach` or `Tier::CityGate` answers instead. Phase 16 deferred both tiers
explicitly and left them untouched. Phase 16's Step 6 then measured them, and they do not work.

This phase is about the five cities and nothing else. It inherits no part of the chain.

---

## Why this phase exists

### The approach tier cannot validate any ground inside the walls

`Tier::CityApproach` samples a 45-point arc between the player and the gate and keeps whatever is hidden and
far enough out. Measured on two consecutive visits with the player inside Whiterun:

```text
city[considered=45 span=[2000,8000] tooNear=0 pastReach=0 offNavmesh=38 notLevel=3 noCorridor=4 unseen=0 survived=0]
city[considered=45 span=[2000,8000] tooNear=0 pastReach=0 offNavmesh=45 notLevel=0 inView=0 noCorridor=0 unseen=0 survived=0]
```

`survived=0` both times, off-navmesh carrying almost the whole rejection, on ordinary city street that NPCs
walk all day. The tier is not choosing badly — it is never choosing. Every city visit falls through to
`Tier::CityGate`, which means the tier's own reason for existing ("somewhere between the player and the gate
reads as the visitor having already come through it") has never once applied.

The probe underneath it is `StuckRecovery::IsStandable`: `TES::GetLandHeight` for a ground Z, then
`IsOnNavmesh` at that point. Why it fails inside a city, and which of the two calls fails, is written up in
[`standable-ground-inside-walled-cities.md`](../engine-findings/standable-ground-inside-walled-cities.md) —
including the part that is not yet established, which Step 1 settles.

### The gate tier has no distance floor

`Tier::CityGate` puts the visitor on the far side of the gate and lets them walk in. That is right when the
player is deep in the city and wrong when they are standing at the gate. From the same session, a player
1,168 units inside the gate:

```text
sender=0x0001C1B4 across worldspaces; player_indoors=false measured from (20532,-7510); gate at (19367,-7433), 1168u out
sender=0x0001C1B4 tier=city-gate at (19244,-7427,-3587) outside the gate
NPCVisitBeat[SALUTATION]: elapsed=1.3s, sender-to-player distance=789u
NPCVisitBeat[SALUTATION]: approach reached (789u) — firing opening line and advancing to Discuss
```

The visitor materialised 789 units away and greeted the player 1.3 seconds after arming. Nothing in the tier
consults `iVisitMarkerMinDistanceUnits`, or any other distance: the gate is wherever the gate is. Every other
tier in the beat has a floor; this one was never given one.

### The escort is blind in there too, with nothing to fall back on

The city tiers produce no chain, so `Escort` begins with an empty ladder — `escort begun with 0 chain
fallback(s)` on both city visits. When a visitor then fails to path, the escalation goes straight to the
close-in line probe, which is built on the same `IsStandable` that cannot answer inside the walls:

```text
'Arniel Gane' close-in step 1 found nowhere standable near (21509,-5718,-3545)
'Arniel Gane' close-in step 2 found nowhere standable near (22043,-5462,-3447)
'Arniel Gane' close-in step 3 found nowhere standable near (22576,-5205,-3350)
'Arniel Gane' moved only 0u and has no fallbacks left -> closed in to 1818u from goal
'Arniel Gane' moved only 95u and has no fallbacks left -> closed in to 1027u from goal
```

He sat 4,228 units out for fifteen seconds and was then dragged to 1,027 units — the "warped in right in
front of me" the user reported, arrived at by the only route left open to the escort.

---

## Scope

### In scope

- A standability answer that works inside a city worldspace, and the probe that establishes why the current
  one does not.
- A distance floor on `Tier::CityGate`, and what to do when the gate is nearer than the floor.
- A fallback supply for the escort on the city tiers, so a stalled visitor has somewhere to go that is not a
  bare line toward the player.

### Deferred (explicitly out)

- **The approach chain.** No city visit builds one and this phase does not change that. The two ends are in
  different worldspaces and a door joins them; that is the design and it is not in question.
- **Interior arrivals.** `Tier::Doorstep` works, and Phase 16's Step 7 fixed which door it reads.
- **Any new tier.** Whatever comes out of this is a repair of the two that exist.
- **The other worldspaces with their own coordinate frames.** Only the five walled cities are in scope;
  `WhiterunDragonsreachWorld`, `WindhelmPitWorldspace` and the dungeon worldspaces are not, and a visit with
  the player in one of them is a decline today and stays one.

---

## Open questions

These are the design decisions this phase has not made yet, and they are the reason it is a phase rather than
a patch.

1. **What replaces the landscape probe?** Asking the navmesh directly for a point and taking its own Z is the
   obvious candidate — see
   [`navmesh-queries-in-commonlibsse-ng.md`](../engine-findings/navmesh-queries-in-commonlibsse-ng.md) — but
   it is a different question from "is this ground", and a point on navmesh under a staircase is on navmesh.
   Step 1's probe should report enough to choose.
2. **Does the fix belong in `StuckRecovery::IsStandable` or beside it?** Changing it changes every caller,
   including the ambush beat and the chain's own gates, which today behave correctly because they only ever
   run where landscape exists. A city-only path is narrower and more honest; one probe that works everywhere
   is less to explain.
3. **When the gate is inside the floor, what wins?** Candidates: keep the gate and accept a close arrival,
   push the arrival outward along the road beyond the gate, or decline. The first is today's behaviour and
   the user has now seen it twice.
4. **Is the arc the right shape inside a city at all?** It was written for open ground. A city has streets,
   and a position 3,000 units from the player with a building between them may be better described by the
   street network than by a bearing and a radius.

---

## Implementation plan

Written to the same rule as Phase 16: test coverage is part of every step, and a step that cannot be verified
without the game says so and stops.

### Step 1 — Find out which call fails inside the walls

- [ ] Complete

**[USER + CLAUDE]**

**Goal:** Separate "no landscape record to read" from "a landscape height that is nowhere near the street",
because the two have different fixes and the session log cannot tell them apart.

**Files:** a probe under the scratchpad, never the repository.

**Sub-tasks:**

1. A probe that, at a given position, logs `GetLandHeight`'s bool and float, the grounded Z it would produce,
   and `IsOnNavmesh` at that point — all four, separately.
2. Run it at four places [USER]: Whiterun's market, Whiterun just inside the gate, Markarth's market, and one
   outdoor control in Tamriel. Whiterun and Markarth differ most in landscape coverage (7 cells of 113
   against 140 of 146), so they bracket the question.
3. Write the result up in
   [`standable-ground-inside-walled-cities.md`](../engine-findings/standable-ground-inside-walled-cities.md),
   replacing its "what is not established" section with what was measured.

**Verify [CLAUDE]:** the finding states which call returns what, per city, with the numbers behind it.

**No longer blocking.** Step 2 shipped before this ran, and it handles both mechanisms rather than choosing
between them: the terrain query is tried first and the navmesh's own surface is read whenever that does not
produce a point on navmesh, which covers "no landscape record" and "landscape far below the paving" alike.
The probe is still worth running to replace the inference in the finding with a measurement, but nothing is
waiting on it.

---

### Step 2 — A standability answer that works inside a city

- [X] Complete

**[CLAUDE]**

**Goal:** Give the city tiers a ground test that answers.

**Files:** `include/StuckRecovery.h`, `src/StuckRecovery.cpp`, `src/StuckRecovery.engine.test.cpp`.

**Sub-tasks:**

1. `NavmeshSurfaceZ` reads the navmesh's own surface height at a position's XY, taking the triangle nearest
   that position's own height, with no reference to landscape.
2. `IsStandable` tries terrain first and falls through to it. Terrain-first is what keeps outdoor behaviour
   where it was; the fall-through is reached both when the terrain query fails and when it succeeds but
   disagrees with the navmesh, which is why open question 1 did not have to be answered first.
3. `kNavmeshSurfaceWindowUnits`, 512. The fall-through is bounded because navmesh under a position is not
   always the ground at that position: a path at the foot of a cliff is navmesh under the XY of somebody
   standing on top of it. 512 is wider than a city's own tiers and far narrower than a ravine, and the
   arrival search's elevation gate is 400 anyway, so nothing it asks about can be accepted at the far end
   of the window.
4. Tests, in "StuckRecovery::IsStandable on a street with no terrain under it": a street with no landscape
   at all; the same street with landscape 400 units below the paving; a position off the edge of the navmesh,
   still refused; and a position 2,000 units above it, refused for the window.

**Verify [CLAUDE]:** the existing `IsStandable` tests still pass unchanged — whatever this adds, outdoor
behaviour does not move.

**Done.** All 420 tests pass, including every pre-existing `IsStandable`, `GroundPoint` and `Escort` case
untouched.

---

### Step 3 — Give `Tier::CityGate` a floor

- [X] Complete

**[CLAUDE + USER]**

**Goal:** Stop a visitor materialising in front of a player who is standing at the gate.

**Files:** `src/VisitArrivalPoint.cpp`, `src/VisitArrivalPoint.engine.test.cpp`.

**Sub-tasks:**

1. Open question 3 answered the second way: keep walking out along the player-to-gate line until
   `iVisitMarkerMinDistanceUnits` is satisfied, at `1`, `0.75`, `0.5` and `0.25` of the shortfall, taking the
   first that `IsStandable` accepts. Declining was not a candidate — a player standing at a gate is an
   ordinary place to stand — and keeping the gate is what the user has now watched fail twice.
2. The gate's own landing stays on the end of the fallback ladder. It is the one position in there the engine
   vouches for, being where it puts anybody who walks through the door.
3. Tests, in "when the player is standing at the gate": the arrival satisfies the floor; it stays on the
   bearing through the gate; it is on ground `IsStandable` accepts; the gate survives as a fallback; and the
   anchor is still the far door, since pushing the point out does not change which worldspace it is in.
4. The log line now names the arrival's distance, the gate's own, and the floor, because that is the number
   the next session is read against.

**Verify [USER]:** standing just inside Whiterun's gate, a visitor no longer appears within conversation
range.

---

### Step 4 — A fallback supply for the city tiers

- [X] Complete

**[CLAUDE]**

**Goal:** Stop the escort escalating straight to a blind line probe on every city visit.

**Files:** `src/VisitArrivalPoint.cpp`, `src/NPCVisitBeat.cpp`, `src/StuckRecovery.engine.test.cpp`.

**Sub-tasks:**

1. `Tier::CityApproach` already samples 45 points and keeps the survivors — with Step 2 those exist, and the
   ones it does not choose are the fallbacks. `Tier::CityGate` has one position by construction and needs a
   source: the gate's own far side plus the road outward from it is the candidate.
2. Tests: a city-tier result carrying fallbacks; the escort consuming them in order; and the close-in probe
   reached only once they are exhausted.

**Verify [CLAUDE]:** `escort begun with 0 chain fallback(s)` no longer appears on a city visit.

**Done** for the gate tier, which had none: the pushed-out probes it did not choose, plus the gate itself, are
now its ladder. The approach tier already kept the arc samples it did not choose, and with Step 2 those exist
for the first time.

---

### Step 5 — In-game validation

- [ ] Complete

**[USER]**

**Goal:** Confirm the five cities behave, which no fixture can establish.

**Sub-tasks:** a visit in each of Whiterun, Markarth and one of Riften/Solitude/Windhelm, each from a spot
deep in the city and again from just inside the gate, with debug mode on. Keep the log.

**Verify [USER]:** the visitor walks in rather than appearing, and arrives from the gate's direction.

**Verify [CLAUDE]:** `tier=city-approach` appears at all — it never has — and `survived` is non-zero on the
arc.

---

## Done condition

All five steps checked, and:

1. `pwsh -File build.ps1 build`, `pwsh -File build.ps1 test` and `pwsh -File format.ps1` are all clean.
2. A visit with the player inside a walled city logs `tier=city-approach` with `survived > 0` at least once.
3. No city visit logs `escort begun with 0 chain fallback(s)`.
4. The engine finding states which call fails inside which city, from measurement rather than inference.
5. No probe, harness or captured log from any step is in the repository.
