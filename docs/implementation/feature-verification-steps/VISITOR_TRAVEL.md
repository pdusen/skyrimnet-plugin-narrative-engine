# Verifying visitor travel

Run this when something about visitor arrivals needs re-checking: after a change to the approach chain, the
arrival search, the cover gate or the escort, or when a tester reports a visitor appearing somewhere odd.

Six scenarios across four sites. Each one is a `cow` to a measured spot, a dispatch, and a line in the log
that says whether it behaved. The coordinates are not guesses — they come from a scan of all 11,185 Tamriel
cells for road navmesh, object density and terrain flatness, and each site is the clearest example of the
case it tests.

Budget about forty minutes for the whole set, less if you only need one scenario.

---

## Before you start

1. **Nothing to enable.** Both road graphs, debug mode and the travel log are on in the shipped INI. Leave
   `bTraceMode=0`; everything below is written at debug level or to its own file.

2. **Know where the evidence lands**, under `Documents/My Games/Skyrim Special Edition/SKSE/`:

   | File                                | What it holds                                                       |
   | ----------------------------------- | ------------------------------------------------------------------- |
   | `NarrativeEngine_VisitorTravel.log` | the whole of each visit's reasoning — read this first               |
   | `NarrativeEngine.log`               | the one-line verdict per visit, plus everything else the plugin did |

   Both rotate five deep, so starting a fresh run never destroys the one that showed the problem.

3. **Optional, for a map of what the search could see:** set `bFineRoadsDebugBitmap=true` and
   `bTravelGraphDebugBitmap=true` in `Data/MCM/Settings/NarrativeEngine.ini`. The fine bitmap is overwritten
   on every grid change, so copy it aside before moving on.

4. **Pick two or three NPCs you have actually talked to** this playthrough — only NPCs with engagement and
   memories can be chosen as senders. Get each one's ref ID (click them in the console, or `help "<name>" 0`).
   Scenario 2 needs them.

5. **How to dispatch:** `F7` → Dispatch tab → `npc_visit`. Do not wait for the Director's own cadence.

6. **Expect a different visitor each time.** A completed visit puts that sender on a 72-game-hour cooldown. If
   dispatch reports no candidates, sleep three days. Who visits does not matter — only where they live, and
   every scenario below works with any sender.

---

## Site A — Nightgate Inn

```text
cow Tamriel 17 11
```

You land 2,400 units from the inn: 93 road triangles in this cell, 706 across the loaded 5×5, and the nearest
settlement of any kind 67,000 units away. Whoever visits is coming a long way over the coarse skeleton.

### Scenario 1 — open country, visitor far away

- [ ] Stand on the road and dispatch. Watch them walk in.
- [ ] **Expect** `tier=chain`, a long `coarse=` run in the middle of the histogram, and a winning point of
      `class=fine` or a synthetic class near you.
- [ ] **Watch for** an arrival west of you. The road from Winterhold sweeps around the east side of the
      mountain, so the visitor should come in from the east. West means something is choosing off-route
      again.

---

## Site B — the Redoran's Retreat flats

```text
cow Tamriel -5 1
```

The flattest, barest cell in Skyrim that still has a road through it: 23 placed objects, terrain spread of 5,
27 road triangles, 3,357 units from Redoran's Retreat. The road runs north–south through x=-5/-4, and
everything from x=-6 westward is road-free for six cells.

### Scenario 4 — nothing near you is hidden

- [ ] Dispatch twice: once facing north up the road, once facing south.
- [ ] **Expect** one of two legal outcomes, and record which. Either an in-grid point past 5,000 units behind
      your facing (`in_grid=true`), or one just past the grid boundary (`in_grid=false`).
- [ ] **A declined visit is the failure here**, not either of those.

### Scenario 2 — visitor a few thousand units off-road

- [ ] `cow Tamriel -7 1` — 8,192 units west of the road, no road triangles at all.
- [ ] `prid <refid>` then `moveto player`, for each of your two or three NPCs, so whoever gets picked is
      standing off-road.
- [ ] `cow Tamriel -5 1` back to the road and dispatch promptly — a staged NPC drifts back toward their
      schedule.
- [ ] **Expect** `direct=` nonzero in the histogram, handing off to `fine=`, rather than a detour out to the
      coarse skeleton and back.

---

## Site C — Swindler's Den

```text
cow Tamriel -12 0
```

1,026 units from Swindler's Den, and **no road triangles anywhere in the loaded 5×5** — the only spot in
Skyrim like that which is also flat, walkable and easy to reach. The honest version of "no fine graph": you
do not need to switch the subsystem off.

### Scenario 3 — no fine graph loaded

- [ ] Confirm `FineRoads: active graph rebuilt — 25 cell(s), 0 node(s)` before dispatching.
- [ ] Dispatch.
- [ ] **Expect** the visitor-to-player line load-bearing. The arrival may be `class=direct` or a coarse point
      just past the grid boundary; both are legal, and the in-grid stretch of that line is graded on its own
      merits rather than written off.

---

## Site D — Whiterun

All three need a sender who does not live in Whiterun's own worldspace. If the pick is a Whiterun resident
you will get `tier=chain` instead — note it and re-dispatch.

### Scenario 6 — the three short-circuit tiers

- [ ] **Doorstep, ground floor:** stand in the Bannered Mare and dispatch. **Expect** `tier=doorstep` and the
      visitor at the front door.
- [ ] **Doorstep, upstairs:** go up and dispatch again. **Expect the front door again** — not the balcony.
      This is the case that regressed once, so it is worth both halves.
- [ ] **City-approach:** stand by the Gildergreen and dispatch. The gate is ~3,500 units off, which leaves
      room for a point between you and it. **Expect** `tier=city-approach` with `survived` above zero.
- [ ] **City-gate:** stand just inside the main gate and dispatch. Nothing fits between you and a gate you
      are standing on. **Expect** `tier=city-gate`, an arrival at least 2,000 units out, and four fallbacks
      behind it.

---

## Scenario 5 — the stuck ladder

Do this on top of any dispatch above, once the visitor has been placed.

- [ ] Take the sender's ID from the log's `sender=0x...`, then `prid <that id>` and `setav speedmult 0`.
- [ ] Wait past one 4-second check. The escort sees under 100 units of movement and the ladder runs.
- [ ] `setav speedmult 100` afterwards so the beat can finish.
- [ ] **Expect** exactly one road hop — it is consumed once per visit — then chain fallbacks, then close-ins,
      with no node used twice.

If `speedmult` does not take, force every check to count as stalled instead: set
`iStuckRecoveryMovementThresholdUnits=100000` in `Data/MCM/Settings/NarrativeEngine.ini`, then open the MCM
page and toggle any NarrativeEngine setting. That fires the ModEvent which re-reads the whole override file
mid-session, and the ladder runs to exhaustion in one visit.

---

## Reading the results

Open `NarrativeEngine_VisitorTravel.log` and read one visit top to bottom. It is written in the order the
decision happens, and the tags are a column so you can skim one of them down the file:

| Tag      | Answers                                                                         |
| -------- | ------------------------------------------------------------------------------- |
| `ENDS`   | where both ends resolved, which rung answered, and every way out of an interior |
| `GRID`   | which cells the search was allowed to validate within                           |
| `TIER`   | which tier answered, and the settings it ran under                              |
| `GRAPH`  | every node set, attachment, bridge and laid line, with lengths and difficulties |
| `SEARCH` | the route, its cost, and the class histogram                                    |
| `CHAIN`  | every point on the route, its class, and whether it was in the grid             |
| `GATE`   | every candidate refused, naming the gate and the number that failed it          |
| `COVER`  | the cover probe — on a refusal, which ray and by how much                       |
| `GRADE`  | what each surviving candidate was graded                                        |
| `PICK`   | the winner, and the grade it beat                                               |
| `WARP`   | the snapshot, the marker, and where the visitor was put                         |
| `ESCORT` | the ladder it was armed with, and every hop, retirement and close-in            |

**Start from the refusals.** Every arrival fault found so far looked correct in the one-line summary and wrong
in the detail — the summary names the winner and never what it beat, which is exactly why the `GATE`, `COVER`
and `GRADE` lines are there.

The current numbers these are read against: a 2,000-unit distance floor, a 64-unit cover silhouette whose
blocker must sit within 512 units of the spot, hop expansion of 2 hops bounded to 400 units off the route,
and coarse edges laid within 16,384 units of the player.
