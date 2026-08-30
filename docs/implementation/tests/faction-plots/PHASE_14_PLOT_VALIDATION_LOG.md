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
  succeeded      3548   67.8%
  timeout         764   14.6%
  caught          921   17.6%

PLOTS
  born                     930
  ended succeeded          72.6%
  ended at the adaptation cap  27.4%
  duration days   median 18.0   mean 19.3   max 56.0
  steps per plot  median  6.0   mean  5.8
  adaptations     mean 1.58

BUDGET  mean 9.7 of 10   pinned 74.7%   starved 0.3%

CASTING  524 distinct masterminds, 655 distinct actors
  top 10 masterminds hold 13.5% of all plots
  ladder rung: 1=0.0%  2=0.0%  3=67.4%  4=32.6%

LLM CALLS  7.1 per in-world day
```

Against the step's own criteria:

| Criterion                                        | Result                                  |
| ------------------------------------------------ | --------------------------------------- |
| No outcome class below ~10% or above ~70%         | **met** — 67.8 / 14.6 / 17.6            |
| Several hundred distinct masterminds in a year    | **met** — 524, top 10 holding 13.5%     |
| Budget neither pinned nor starving                | **partly** — see below                  |
| A call rate the Part 3 estimate can be checked against | **met** — 7.1/day against an estimated 15 |

**The LLM call rate is less than half the design's estimate.** Part 3 reasoned "a five-step plot costs about
eleven calls, plots run about a week, ten concurrent slots" and arrived at ~15 calls per in-world day. The
model produces 7.1, because plots run nearer *eighteen* days than seven — the estimate was right about the
per-plot cost and wrong about the pace. That is comfortably affordable and leaves room to raise the plot
budget later if the world feels thin.

---

## Two findings that are not tuning problems

### Hierarchical delegation never happens

Ladder rungs 1 and 2 — "a subordinate with a personal tie" and "any subordinate" — fire **0.0% of the time**.
Every delegation is rung 3 (a personal tie without a shared faction, 67.4%) or rung 4 (the mastermind does it
themselves, 32.6%).

This is arithmetic, not a bug. A subordinate is someone with *lower standing in a shared faction*, and only
**25 of 857** NPCs have any standing at all — the six rostered factions cover 77 people, and only their
leaders and the College's ladder sit above the bottom rung. A mastermind with standing 0, which is 832 of
857, has no subordinates anywhere by definition.

The design's intent still holds — *"the Thieves Guild has options, a Riverwood farmer has themselves"* — but
the "has options" case is currently vanishingly rare. This is a **roster coverage** question rather than a
model one: rostering more factions, or adding intermediate ranks to the ones already there, is what would move
it. Worth deciding before Step 11's in-game validation, since "who does the work" is one of the things that
step is meant to judge.

### The budget runs near-saturated

Occupancy sits at 10/10 for 74.7% of ticks and never drops below 3. With plots lasting ~18 days and births
opportunistic on any free slot, that is expected rather than wrong — but it means `iPlotMaxConcurrent` is a
hard ceiling doing real work rather than a safety limit, and raising it would raise the call rate close to
proportionally.

---

## Reproducing

```text
python build-plot-population.py          # writes population.json from the Spriggit export
python simulate-plots.py --days 365 --trials 5
python simulate-plots.py --sweep-mishap 0.04,0.02,0.012,0.008
python simulate-plots.py --sweep-step-scale 0.8,1.0,1.3
```

`population.json` is generated and not committed. The simulator is a **mirror** of `PlotResolution.cpp` and
`PlotCasting.cpp`, not a second design — where the two could drift it is written to match line for line,
because a harness that models something the game does not produces numbers that are confidently wrong.
