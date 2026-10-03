# A landscape-based ground probe is blind inside the walled cities

## TL;DR

`StuckRecovery::IsStandable` — `TES::GetLandHeight` for a ground Z, then `IsOnNavmesh` at that
point — answers almost nothing inside Whiterun. Measured in a live session: **38 of 45** bearing
samples rejected as off-navmesh on one visit and **45 of 45** on the next, plus three consecutive
escort close-in steps that found nowhere standable along a straight line between two points an
actor was standing on.

The consequence is that any search gated on standability cannot place anything inside a city
worldspace, and the escort cannot find intermediate ground to warp an actor to in there.

## The evidence

From the Phase 16 Step 6 session, two visits with the player inside Whiterun:

```text
city[considered=45 span=[2000,8000] tooNear=0 pastReach=0 offNavmesh=38 notLevel=3 noCorridor=4 unseen=0 survived=0]
city[considered=45 span=[2000,8000] tooNear=0 pastReach=0 offNavmesh=45 notLevel=0 inView=0 noCorridor=0 unseen=0 survived=0]
```

`survived=0` both times, with off-navmesh carrying the whole rejection. The arc being sampled is
the ground between the player at the Gildergreen and the main gate 6,270 units away — ordinary
city street that NPCs walk all day.

The escort then failed the same way on the same ground:

```text
'Arniel Gane' close-in step 1 found nowhere standable near (21509,-5718,-3545)
'Arniel Gane' close-in step 2 found nowhere standable near (22043,-5462,-3447)
'Arniel Gane' close-in step 3 found nowhere standable near (22576,-5205,-3350)
```

Those three points interpolate between an actor who was standing still at 4,228 units and a player
standing still at the far end. Both ends were standable by observation; nothing between them
answered.

## Landscape coverage inside the city worldspaces

One plausible mechanism is that there is no landscape record to read. Counted over the Spriggit
export, per city worldspace:

| Worldspace | Exterior cells | With a `Landscape` record | With navmesh |
| --- | --- | --- | --- |
| WindhelmWorld | 57 | 2 | 10 |
| WhiterunWorld | 113 | 7 | 13 |
| RiftenWorld | 29 | 12 | 9 |
| SolitudeWorld | 62 | 20 | 12 |
| MarkarthWorld | 146 | 140 | 5 |

Whiterun and Windhelm are nearly bare; Markarth is nearly complete. So "cities have no landscape"
is not a general rule — it is true of the two cities where the symptom was measured and false of
Markarth.

## What is not established

**Which of the two links fails.** `IsStandable` can fail at `GetLandHeight` (no landscape record
to read) or at `IsOnNavmesh` (a height was returned, but it is the terrain under the city rather
than the static street the navmesh is built on). Whiterun sits on a plateau of static geometry,
so a landscape surface hundreds of units below the streets would produce exactly the observed
symptom while `GetLandHeight` succeeded.

The export tells us coverage, not which call returned false, and the session log records only the
rejection reason. Separating them needs a probe that logs both results at known points inside the
walls — one standing in Whiterun's market, one in Markarth's, since those two cities differ most
in coverage. Until that runs, the safe statement is the measured one: the probe as composed does
not answer inside Whiterun.

**The shipped fix does not depend on the answer.** `StuckRecovery::IsStandable` tries terrain and
then reads the navmesh's own surface whenever that did not produce a point on navmesh — which
covers both mechanisms, since a missing landscape record and a landscape 400 units under the
paving fail at different calls and look identical from outside. The probe is still worth running
to turn the inference above into a measurement, but nothing is waiting on it.

## What this means

A standability gate is the wrong instrument inside a city worldspace. The ground there is
authored static geometry, and what makes it walkable is the navmesh, not the terrain. Two
directions that do not depend on landscape:

- Query the navmesh directly for a point and take its own Z, rather than grounding against
  terrain first and then asking whether the result is on navmesh. See
  [`navmesh-queries-in-commonlibsse-ng.md`](navmesh-queries-in-commonlibsse-ng.md).
- Fall back to positions the engine already vouches for — a load door's `ExtraTeleport` landing,
  a marker, or the actor's own current position — which is what the doorstep and city-gate tiers
  already do and why they work where the arc sampling does not.

## Related

- [`land-height-outside-the-loaded-grid.md`](land-height-outside-the-loaded-grid.md) — the same
  function answering nothing outside the attached grid. Same shape: no landscape loaded, no
  answer. A city is the case where the landscape was never authored rather than merely unloaded.
- [`city-worldspaces-share-tamriels-origin.md`](city-worldspaces-share-tamriels-origin.md) — why
  a position inside a city can be compared against one in Tamriel at all.
