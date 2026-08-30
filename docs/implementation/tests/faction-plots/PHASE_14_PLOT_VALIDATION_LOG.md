# Phase 14 — Plot Simulation Validation Log

The offline run that tuned the plot simulation, and what it found. Step 9 of
[`../../PHASE_14_FACTION_PLOTS.md`](../../PHASE_14_FACTION_PLOTS.md).

Everything here comes from `simulate-plots.py` over the population
`build-plot-population.py` extracts from the Spriggit export — 857 unique NPCs, their real holds, authored
skills, personal ties, and faction standing computed through the shipped `PlotFactions.ini`.

---

## What the harness caught before the game ever ran

### The threshold did nothing at all

The headline finding, and the reason this step exists.

Step 6 sized every step with two numbers: a **budget** in ticks, from travel distance, and a **threshold** of
work, from step type and the target's importance. Progress accrued per tick on a roll, and the design's stated
constraint was that the two be *independently derived* — otherwise every step carries identical odds and the
race is theatre.

The implementation expressed the per-tick roll as a **fraction of the threshold**, on the reasoning that
"retuning threshold sizing should not silently change step duration". That reasoning is exactly backwards.
Ticks-to-finish works out to `1 / mean(fraction)`, and the threshold cancels out of the arithmetic completely:

```text
threshold    5.0 -> mean 4.50 ticks to finish
threshold   10.0 -> mean 4.50
threshold   20.0 -> mean 4.50
threshold  500.0 -> mean 4.50
```

A step worth 5 and a step worth 500 took the same time. `SizeThreshold`, `TargetImportance`, and the whole
"how hard is the job" half of the model were decorative.

**Step 6's probe passed on this**, because it asserted that budget and threshold were *independently derived* —
and a value that affects nothing is trivially independent of everything. The probe was checking the
signatures, not the behaviour. Only running the model end to end and looking at the distribution exposed it.

The fix makes the per-tick roll **absolute work**, in the same units as the threshold, so ticks ≈ threshold /
rate. One relative clamp survives — no tick may clear more than `fPlotProgressMaxFraction` of the threshold —
which keeps a trivially small step from being one-shot and is what `MinimumViableBudget` derives from. After
the fix:

```text
threshold  6 -> 3.00 ticks     hopeless actor at threshold 18 -> 8.04 ticks
threshold 18 -> 6.00           ideal actor    at threshold 18 -> 4.91
threshold 45 -> 14.28
```

Settings changed with it: `fPlotProgressRollMin` / `RollMax` became `fPlotProgressRateMin` / `RateMax` (absolute
work per tick) plus `fPlotProgressMaxFraction` (the surviving relative clamp).

### Failure was nearly all mishap, and the clock almost never ran out

At the initial `fPlotMishapChanceBase = 0.04`, steps resolved 58.6% succeeded / 5.7% timed out / **35.7%
caught**. A base chance of 0.04, multiplied by 2.5 for a conspicuous step, is ~10% per tick — over a six-tick
step that is close to a coin flip. Being caught was the *normal* way to fail, which drains the distinction:
"caught" is supposed to be the loud, gossip-worthy outcome, not the default one.

| `fPlotMishapChanceBase` | succeeded | timed out | caught | plots reaching the adaptation cap |
| ----------------------- | --------: | --------: | -----: | --------------------------------: |
| 0.04                    |     58.6% |      5.7% |  35.7% |                             49.0% |
| 0.02                    |     73.0% |      6.8% |  20.2% |                             17.7% |
| 0.012                   |     79.5% |      7.3% |  13.2% |                              7.8% |
| 0.008                   |     83.4% |      8.0% |   8.6% |                              5.2% |

Settled on **0.015**.

### Budgets were too generous, so competence barely mattered

With mishap tamed, timeouts sat at 6.8% — the clock essentially never ran out, so an actor's competence had
little to decide. Tightening the base budgets by a fifth brings the timeout rate up to where a poor actor
sent a long way genuinely fails:

| budget scale | succeeded | timed out | caught | plots succeeding |
| -----------: | --------: | --------: | -----: | ---------------: |
|          1.3 |     80.0% |      2.5% |  17.6% |            93.5% |
|          1.0 |     74.7% |      6.8% |  18.5% |            85.4% |
|          0.8 |     69.2% |     12.9% |  17.8% |            75.6% |

Baked into `BaseBudgetTicks` rather than kept as a multiplier. Several types land below
`MinimumViableBudget` and are floored there, which is correct rather than a rounding accident: a short errand
is short, and the floor is what stops "short" becoming "impossible".

---

## The tuned run

5 trials × 365 in-world days, shipped settings.

```text
STEP OUTCOMES
  succeeded      3598   68.7%
  timeout         751   14.3%
  caught          891   17.0%

PLOTS
  born                     931
  ended succeeded          73.4%
  ended at the adaptation cap  26.6%
  duration days   median 18.5   mean 19.4   max 50.5
  steps per plot  median  6.0   mean  5.8
  adaptations     mean 1.54

BUDGET  mean 9.7 of 10   pinned 74.6%   starved 0.3%

CASTING  553 distinct masterminds, 723 distinct actors
  top 10 masterminds hold 6.2% of all plots
  ladder rung: 1=7.3%  2=0.0%  3=72.7%  4=20.0%

LLM CALLS  7.0 per in-world day
```

Against the step's own criteria:

| Criterion                                        | Result                                  |
| ------------------------------------------------ | --------------------------------------- |
| No outcome class below ~10% or above ~70%         | **met** — 68.7 / 14.3 / 17.0            |
| Several hundred distinct masterminds in a year    | **met** — 553, top 10 holding 6.2%      |
| Budget neither pinned nor starving                | **partly** — see below                  |
| A call rate the Part 3 estimate can be checked against | **met** — 7.0/day against an estimated 15 |

**The LLM call rate is less than half the design's estimate.** Part 3 reasoned "a five-step plot costs about
eleven calls, plots run about a week, ten concurrent slots" and arrived at ~15 calls per in-world day. The
model produces 7.0, because plots run nearer *nineteen* days than seven — the estimate was right about the
per-plot cost and wrong about the pace. That is comfortably affordable and leaves room to raise the plot
budget later if the world feels thin.

---

## A false finding, and the bug behind it

The first version of this log reported that **hierarchical delegation never happens** — ladder rungs 1 and 2
both at 0.0% — and explained it as arithmetic: only 25 of 857 NPCs have standing, so almost no mastermind has
a subordinate.

**That was wrong, and the explanation was wrong twice over.** It was a bug in this harness, and it should have
been obvious that it was: if General Tullius is ever drawn as a mastermind he has 287 subordinates, so an
outcome of *exactly* 0.0% over 5,400 dispatches cannot be a distribution.

### The bug

In `build-plot-population.py`, the loop over participants binds `key` to each NPC:

```python
for key, part in parts.items():
    ...
    for fac_editor_id, section in roster.items():
        key = fac_key.get(fac_editor_id)      # <-- shadows the NPC key
    ...
    members.append({"id": str(key), ...})     # <-- writes the FACTION id
```

Every member who belonged to a rostered faction had their own `id` silently rewritten to that faction's key.
So all 18 College members shared one id, all 20 Dark Brotherhood members shared another, and the simulator's
first filter —

```python
if m.id == boss.id or not self.available(m.id, "actor"):
    continue
```

— which is meant to skip only the mastermind themselves, skipped **the entire faction**. The subordinate pool
was empty by construction. It also collapsed every id-keyed lookup: personal ties dropped from 659 members to
582, which was visible in the extractor's own summary and which I did not question.

**The plugin is unaffected.** `PlotPopulation::Build` is a separate implementation with no such shadowing, and
`dump-faction-ranks.py` confirms the hierarchies resolve correctly from the game data.

### What the ranks actually are

`dump-faction-ranks.py` prints every rostered faction with all members ordered by resolved standing. The
hierarchies are correct, and every superior has real subordinates:

| Faction              | Method   | Superior          | Standing | Castable subordinates |
| -------------------- | -------- | ----------------- | -------: | --------------------: |
| College of Winterhold | Rank     | Savos Aren        |     1.00 |                    17 |
|                      |          | Mirabelle Ervine  |     0.83 |                    16 |
|                      |          | the masters       |     0.67 |                     9 |
| Companions           | Marker   | Kodlak Whitemane  |     1.00 |                    12 |
|                      |          | the Circle        |     0.50 |                     8 |
| Imperial Legion      | Explicit | General Tullius   |     1.00 |                     7 |
|                      |          | Legate Rikke      |     0.67 |                     6 |
| Stormcloaks          | Explicit | Ulfric Stormcloak |     1.00 |                     1 |
| Dark Brotherhood     | Explicit | Astrid            |     1.00 |                    13 |
| Thieves Guild        | Explicit | Mercer Frey       |     1.00 |                    21 |

### Delegation after the fix

```text
ladder rung: 1=7.3%   2=0.0%   3=72.7%   4=20.0%
```

Delegation works: four fifths of steps are handed to someone else. Rung 4 — the mastermind acting alone —
fell from 33% to 20%.

**Rung 2 is genuinely unreachable with this roster, and that is structural rather than broken.** Rung 1 is
"a subordinate *with a personal tie*" and rung 2 is "any subordinate"; rung 2's pool is a superset, so it only
fires when a mastermind has subordinates and none of them is a tie. Checking every standing-holder
individually, that case never arises:

```text
boss                  standing  subs  tied-subs  -> rung
  Savos Aren              1.00    17         17  -> 1
  Mercer Frey             1.00    21         21  -> 1
  Astrid                  1.00    13         13  -> 1
  Kodlak Whitemane        1.00    12         12  -> 1
  General Tullius         1.00     7          7  -> 1
  Ulfric Stormcloak       1.00     1          1  -> 1
  Galmar Stone-Fist       0.67     0          0  -> 3
```

Every subordinate is also a personal tie, because `GossipGraph` derives personal edges partly from shared
faction membership — so the same membership that makes someone a subordinate also makes them a tie. Rung 2
would matter for a large faction outside gossip's size band whose members share nothing else; none of the six
rostered factions is in that position.

The lesson worth keeping: **an aggregate of exactly zero deserves the same suspicion as an aggregate that is
obviously wrong.** The number was reported, explained, and written into three documents before anyone checked
whether the code path could fire at all.

## One finding that is not a tuning problem

### The budget runs near-saturated

Occupancy sits at 10/10 for 74.6% of ticks and never drops below 3. With plots lasting ~19 days and births
opportunistic on any free slot, that is expected rather than wrong — but it means `iPlotMaxConcurrent` is a
hard ceiling doing real work rather than a safety limit, and raising it would raise the call rate close to
proportionally.

## Reproducing

```text
python build-plot-population.py          # writes population.json from the Spriggit export
python dump-faction-ranks.py             # every rostered faction, every member, by standing
python simulate-plots.py --days 365 --trials 5
python simulate-plots.py --sweep-mishap 0.04,0.02,0.012,0.008
python simulate-plots.py --sweep-step-scale 0.8,1.0,1.3
```

`population.json` is generated and not committed. The simulator is a **mirror** of `PlotResolution.cpp` and
`PlotCasting.cpp`, not a second design — where the two could drift it is written to match line for line,
because a harness that models something the game does not produces numbers that are confidently wrong.
