#!/usr/bin/env python3
"""Monte Carlo the plot simulation over the real Skyrim population.

Step 6's probes prove the individual rules hold. This proves the system built
from them produces a world worth playing: that plots start, advance, adapt and
end at sane rates, that casting spreads across the province rather than
converging on the same handful of NPCs, and that the LLM call rate the design
assumed is the one the model actually produces.

It is a MIRROR of the C++, not a second design. Where the two could drift the
mirror is written to match PlotResolution.cpp and PlotCasting.cpp line for
line, because a harness that models something the game does not is worse than
no harness -- it produces numbers that are confidently wrong.

Input is population.json from build-plot-population.py. Run that first.

Usage:
    python simulate-plots.py [--days 365] [--trials 5] [--seed 1337]
    python simulate-plots.py --sweep-step-scale 0.7,1.0,1.4

Step 9 of docs/implementation/PHASE_14_FACTION_PLOTS.md.
"""
from __future__ import annotations

import argparse
import collections
import json
import math
import pathlib
import random
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent

# --- the shipped settings, mirrored from [Plots] ---------------------------
TICK_HOURS = 12.0
MAX_CONCURRENT = 10
MASTERMIND_COOLDOWN_DAYS = 5.0
ACTOR_COOLDOWN_DAYS = 1.5
# PlotCasting::kOutrankMargin and kSelfWeightShare.
OUTRANK_MARGIN = 0.25
SELF_WEIGHT_SHARE = 1.0
# PlotCasting::kRung3StandingCeiling.
RUNG3_STANDING_CEILING = 0.75
MAX_ADAPTATIONS = 3
# Absolute work per tick, in the same units as a step's threshold. NOT a
# fraction of the threshold -- see the note in Settings.h. When it was a
# fraction, ticks-to-finish was 1/mean(fraction) and the threshold cancelled
# out entirely, which is the bug this harness found.
RATE_MIN = 1.5
RATE_MAX = 5.0
MAX_FRACTION = 0.45
MISHAP_BASE = 0.015

# --- the manifest, mirrored from PlotModel.h -------------------------------
# name -> (base budget ticks, base threshold, conspicuous)
STEPS = {
    "locate": (2, 8.0, False),
    "acquire": (3, 14.0, True),
    "deliver": (2, 6.0, False),
    "surveil": (5, 10.0, True),
    "recruit": (4, 16.0, True),
    "discredit": (5, 18.0, True),
    "sabotage": (3, 15.0, True),
    "conceal": (2, 9.0, True),
}

# Mirrored from PlotTick.cpp's kStubLadders.
LADDERS = [
    ["locate", "surveil", "acquire", "conceal"],
    ["locate", "acquire", "deliver"],
    ["surveil", "discredit"],
    ["locate", "recruit", "sabotage", "conceal"],
    ["surveil", "recruit", "discredit"],
]

# Mirrored from PlotResolution::Suitability.
SKILL_FOR = {
    "recruit": ("speech",),
    "discredit": ("speech",),
    "deliver": ("speech",),
    "locate": ("speech", "sneak"),
    "surveil": ("sneak",),
    "conceal": ("sneak",),
    "sabotage": ("sneak",),
    "acquire": ("sneak", "pickpocket"),
}


def minimum_viable_budget(max_fraction: float) -> int:
    return math.ceil(1.0 / max(0.0001, min(1.0, max_fraction)))


def size_budget(step: str, travel: float, max_fraction: float, scale: float) -> int:
    base = STEPS[step][0] * scale
    ticks = round(base * (1.0 + min(max(travel, 0.0), 1.0) * 2.0))
    return max(minimum_viable_budget(max_fraction), int(ticks))


def size_threshold(step: str, importance: float) -> float:
    return STEPS[step][1] * (1.0 + min(max(importance, 0.0), 1.0) * 1.5)


def suitability(step: str, skills: dict) -> float:
    keys = SKILL_FOR[step]
    return sum(skills.get(k, 0.5) for k in keys) / len(keys)


class Member:
    __slots__ = ("id", "name", "hold", "competence", "skills", "factions", "ties", "standing")

    def __init__(self, raw):
        self.id = raw["id"]
        self.name = raw["name"]
        self.hold = raw["hold"]
        self.competence = raw["competence"]
        self.skills = raw["skills"]
        self.factions = raw["factions"]
        self.ties = {t["other"]: t["sharedFaction"] for t in raw["ties"]}
        self.standing = max((f["standing"] for f in self.factions), default=0.0)

    def mastermind_weight(self):
        # Mirrors PlotCasting::MastermindWeight: base, +membership,
        # +standing, with standing taken as the MAXIMUM across factions.
        #
        # The standing term is EXPONENTIAL, doubling four times across
        # the ladder, because a mastermind needs somebody to send and
        # that is not evenly distributed. Keep this in step with the C++
        # or the distribution this harness reports is fiction.
        w = 1.0
        if self.factions:
            w += 0.5
        return w + 2.0 ** (4.0 * min(max(self.standing, 0.0), 1.0)) - 1.0


class Sim:
    def __init__(self, members, rng, step_scale=1.0, mishap=True):
        self.members = members
        self.by_id = {m.id: m for m in members}
        self.rng = rng
        self.step_scale = step_scale
        self.mishap = mishap

        # occupancy: id -> [plot_id or None, role, mm_available, actor_available]
        self.occ = {}
        self.plots = []
        self.next_id = 1
        self.day = 0.0

        self.stats = collections.Counter()
        self.mastermind_tally = collections.Counter()
        self.actor_tally = collections.Counter()
        self.plot_durations = []
        self.plot_steps = []
        self.adaptations = []
        self.occupancy_samples = []
        self.llm_calls = 0
        self.rung_tally = collections.Counter()

    # --- occupancy -------------------------------------------------------
    def row(self, npc):
        return self.occ.setdefault(npc, [None, None, 0.0, 0.0])

    def available(self, npc, role):
        r = self.occ.get(npc)
        if r is None:
            return True
        if r[0] is not None:
            return False
        return self.day >= (r[2] if role == "mastermind" else r[3])

    def engage(self, npc, role, plot_id):
        r = self.row(npc)
        r[0] = plot_id
        r[1] = role

    def release(self, npc):
        r = self.occ.get(npc)
        if r is None:
            return
        if r[1] == "mastermind":
            r[2] = self.day + MASTERMIND_COOLDOWN_DAYS
        else:
            r[3] = self.day + ACTOR_COOLDOWN_DAYS
        r[0] = None

    # --- casting ---------------------------------------------------------
    def pick_mastermind(self):
        pool = [m for m in self.members if self.available(m.id, "mastermind")]
        if not pool:
            return None
        weights = [m.mastermind_weight() for m in pool]
        return self.rng.choices(pool, weights=weights, k=1)[0]

    def pick_actor(self, boss):
        # Mirrors PlotCasting::SelectActor, including the rung-3 outrank
        # guard and the mastermind competing inside the rung-3 draw
        # rather than only beneath it. Keep this in step with the C++ or
        # the ladder split this harness reports is fiction.
        boss_standing = {f["faction"]: f["standing"] for f in boss.factions}
        for rung in (1, 2, 3):
            pool = []
            for m in self.members:
                if m.id == boss.id or not self.available(m.id, "actor"):
                    continue
                tied = m.id in boss.ties or boss.id in m.ties
                shared_faction = boss.ties.get(m.id, m.ties.get(boss.id, False))
                subordinate = any(
                    f["faction"] in boss_standing and f["standing"] < boss_standing[f["faction"]]
                    for f in m.factions
                )
                # Nobody a long way above the mastermind is someone they
                # could ask. Rungs 1 and 2 already require a subordinate,
                # which says more than this does.
                # Relative AND absolute: the relative test alone is
                # inert for a mastermind already at 1.0, which is
                # exactly who the exponential weighting draws most.
                #
                # The C++ ALSO excludes hostile ties here. This harness
                # cannot: population.json records a tie as
                # {other, sharedFaction} and carries no relationship
                # polarity, so its rung-3 pool is wider than the game's
                # by however many Foe edges it contains.
                outranks = (m.standing > boss.standing + OUTRANK_MARGIN
                            or m.standing > RUNG3_STANDING_CEILING)
                if (rung == 1 and subordinate and tied) or (rung == 2 and subordinate) or (
                    rung == 3 and tied and not shared_faction and not outranks
                ):
                    pool.append(m)

            if rung == 3:
                # The mastermind takes half the draw: by the time a
                # scheme is down to distant acquaintances, doing it
                # yourself is the competitive option. Weighted rather
                # than uniform so twelve neighbours do not make an NPC
                # twelve times less likely to act for themselves.
                weights = [1.0] * len(pool) + [max(1.0, SELF_WEIGHT_SHARE * len(pool))]
                pick = self.rng.choices(pool + [boss], weights=weights, k=1)[0]
                return (boss, 4) if pick is boss else (pick, 3)

            if pool:
                return self.rng.choice(pool), rung
        return boss, 4  # the mastermind does it themselves

    # --- the race --------------------------------------------------------
    def travel(self, actor, target):
        if actor.hold is None or target.hold is None:
            return 0.5
        return 0.2 if actor.hold == target.hold else 0.8

    def importance(self, target):
        if not target.factions:
            return 0.25
        return min(1.0, 0.3 + 0.7 * max(f["standing"] for f in target.factions))

    def roll_progress(self, step):
        actor = 0.5 * step["competence"] + 0.5 * step["suitability"]
        blended = 0.5 * actor + 0.5 * self.rng.random()
        # Absolute work per tick, then the one relative ceiling.
        rate = RATE_MIN + blended * (RATE_MAX - RATE_MIN)
        return max(0.0001, min(rate, step["threshold"] * MAX_FRACTION))

    def roll_mishap(self, step):
        if not self.mishap:
            return False
        chance = MISHAP_BASE
        if STEPS[step["type"]][2]:
            chance *= 2.5
        chance *= 1.0 - 0.5 * step["competence"]
        return self.rng.random() < chance

    # --- lifecycle -------------------------------------------------------
    def birth(self):
        boss = self.pick_mastermind()
        if boss is None:
            return
        ladder = self.rng.choice(LADDERS)
        plot = {
            "id": self.next_id,
            "boss": boss.id,
            "plan": [{"type": t, "state": "planned"} for t in ladder],
            "cursor": 0,
            "adaptations": 0,
            "born": self.day,
            "steps_run": 0,
        }
        self.next_id += 1
        for s in plot["plan"]:
            s["target"] = self.rng.choice(self.members).id
        self.engage(boss.id, "mastermind", plot["id"])
        self.plots.append(plot)
        self.mastermind_tally[boss.id] += 1
        self.stats["born"] += 1
        self.llm_calls += 1  # plot birth

    def dispatch(self, plot):
        boss = self.by_id[plot["boss"]]
        step = plot["plan"][plot["cursor"]]
        actor, rung = self.pick_actor(boss)
        target = self.by_id[step["target"]]

        step["actor"] = actor.id
        step["state"] = "running"
        step["elapsed"] = 0
        step["progress"] = 0.0
        step["competence"] = actor.competence
        step["suitability"] = suitability(step["type"], actor.skills)
        step["budget"] = size_budget(step["type"], self.travel(actor, target), MAX_FRACTION, self.step_scale)
        step["threshold"] = size_threshold(step["type"], self.importance(target))

        if actor.id != boss.id:
            self.engage(actor.id, "actor", plot["id"])
        self.actor_tally[actor.id] += 1
        self.rung_tally[rung] += 1
        self.stats["dispatched"] += 1
        self.llm_calls += 1  # dispatch memories

    def advance(self, plot):
        step = plot["plan"][plot["cursor"]]
        step["elapsed"] += 1
        step["progress"] += self.roll_progress(step)

        if self.roll_mishap(step):
            return self.resolve(plot, "caught")
        if step["progress"] >= step["threshold"]:
            return self.resolve(plot, "succeeded")
        if step["elapsed"] >= step["budget"]:
            return self.resolve(plot, "timeout")
        return None

    def resolve(self, plot, outcome):
        step = plot["plan"][plot["cursor"]]
        step["state"] = "done"
        plot["steps_run"] += 1
        self.stats[outcome] += 1
        self.llm_calls += 1  # resolution memories

        actor = step.get("actor")
        if actor and actor != plot["boss"]:
            self.release(actor)

        if outcome == "succeeded":
            plot["cursor"] += 1
            if plot["cursor"] >= len(plot["plan"]):
                self.end(plot, "succeeded")
            return None

        # adaptation
        if plot["adaptations"] >= MAX_ADAPTATIONS:
            self.end(plot, "cap")
            return None
        plot["adaptations"] += 1
        self.llm_calls += 1  # adaptation
        ladder = self.rng.choice(LADDERS)
        plot["plan"] = plot["plan"][: plot["cursor"]] + [
            {"type": t, "state": "planned", "target": self.rng.choice(self.members).id} for t in ladder
        ]
        return None

    def end(self, plot, why):
        plot["done"] = True
        self.stats["ended_" + why] += 1
        self.plot_durations.append(self.day - plot["born"])
        self.plot_steps.append(plot["steps_run"])
        self.adaptations.append(plot["adaptations"])
        self.release(plot["boss"])

    def tick(self):
        self.day += TICK_HOURS / 24.0
        live = [p for p in self.plots if not p.get("done")]
        if len(live) < MAX_CONCURRENT:
            self.birth()
        for plot in [p for p in self.plots if not p.get("done")]:
            if plot["cursor"] >= len(plot["plan"]):
                self.end(plot, "succeeded")
                continue
            step = plot["plan"][plot["cursor"]]
            if step["state"] == "planned":
                self.dispatch(plot)
            elif step["state"] == "running":
                self.advance(plot)
        self.occupancy_samples.append(len([p for p in self.plots if not p.get("done")]))


def run(members, days, seed, step_scale, mishap):
    sim = Sim(members, random.Random(seed), step_scale, mishap)
    for _ in range(int(days * 24 / TICK_HOURS)):
        sim.tick()
    return sim


def report(sims, days, label=""):
    st = collections.Counter()
    for s in sims:
        st.update(s.stats)
    resolved = st["succeeded"] + st["timeout"] + st["caught"]
    n = len(sims)

    print(f"\n===== {label or 'run'}: {n} trial(s) x {days:.0f} in-world days =====")
    print("  STEP OUTCOMES")
    for k in ("succeeded", "timeout", "caught"):
        pct = 100.0 * st[k] / resolved if resolved else 0.0
        print(f"    {k:<12}{st[k]:>7}  {pct:5.1f}%")

    print("  PLOTS")
    ended = st["ended_succeeded"] + st["ended_cap"]
    print(f"    born{st['born']:>14}")
    print(f"    ended succeeded{st['ended_succeeded']:>7}  "
          f"{100.0*st['ended_succeeded']/ended if ended else 0:5.1f}%")
    print(f"    ended at the cap{st['ended_cap']:>6}  "
          f"{100.0*st['ended_cap']/ended if ended else 0:5.1f}%")

    durations = [d for s in sims for d in s.plot_durations]
    steps = [x for s in sims for x in s.plot_steps]
    adapt = [a for s in sims for a in s.adaptations]
    if durations:
        print(f"    duration days   median {statistics.median(durations):5.1f}  "
              f"mean {statistics.mean(durations):5.1f}  max {max(durations):5.1f}")
        print(f"    steps per plot  median {statistics.median(steps):5.1f}  mean {statistics.mean(steps):5.1f}")
        print(f"    adaptations     mean {statistics.mean(adapt):5.2f}")

    occ = [x for s in sims for x in s.occupancy_samples]
    if occ:
        pinned = 100.0 * sum(1 for x in occ if x >= MAX_CONCURRENT) / len(occ)
        starved = 100.0 * sum(1 for x in occ if x <= 2) / len(occ)
        print(f"  BUDGET  mean {statistics.mean(occ):4.1f} of {MAX_CONCURRENT}   "
              f"pinned {pinned:4.1f}%   starved {starved:4.1f}%")

    mm = collections.Counter()
    ac = collections.Counter()
    rungs = collections.Counter()
    for s in sims:
        mm.update(s.mastermind_tally)
        ac.update(s.actor_tally)
        rungs.update(s.rung_tally)
    print(f"  CASTING  {len(mm)} distinct mastermind(s), {len(ac)} distinct actor(s)")
    if mm:
        top = mm.most_common(3)
        share = 100.0 * sum(c for _, c in mm.most_common(10)) / sum(mm.values())
        print(f"    top 10 masterminds hold {share:4.1f}% of all plots")
        print(f"    busiest: {', '.join(f'{c}x' for _, c in top)}")
    total_rungs = sum(rungs.values()) or 1
    print("    ladder rung: " + "  ".join(
        f"{r}={100.0*rungs[r]/total_rungs:4.1f}%" for r in (1, 2, 3, 4)))

    calls = sum(s.llm_calls for s in sims) / n
    print(f"  LLM CALLS  {calls:.0f} over {days:.0f} days = {calls/days:.1f} per in-world day")
    return {"calls_per_day": calls / days, "resolved": resolved}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--population", default=str(HERE / "population.json"))
    ap.add_argument("--days", type=float, default=365.0)
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--step-scale", type=float, default=1.0)
    ap.add_argument("--no-mishap", action="store_true")
    ap.add_argument("--mishap-base", type=float, default=None)
    ap.add_argument("--rate-max", type=float, default=None)
    ap.add_argument("--sweep-mishap", default=None)
    ap.add_argument("--sweep-step-scale", default=None)
    args = ap.parse_args()

    path = pathlib.Path(args.population)
    if not path.exists():
        sys.exit(f"no population at {path}; run build-plot-population.py first")
    raw = json.loads(path.read_text(encoding="utf-8"))
    members = [Member(m) for m in raw["members"]]
    print(f"population: {len(members)} members, "
          f"{sum(1 for m in members if m.standing > 0)} with standing", file=sys.stderr)

    global MISHAP_BASE, RATE_MAX
    if args.mishap_base is not None:
        MISHAP_BASE = args.mishap_base
    if args.rate_max is not None:
        RATE_MAX = args.rate_max

    if args.sweep_mishap:
        for m in [float(x) for x in args.sweep_mishap.split(",")]:
            MISHAP_BASE = m
            sims = [run(members, args.days, args.seed + i, args.step_scale, not args.no_mishap)
                    for i in range(args.trials)]
            report(sims, args.days, f"mishap-base {m}")
        return 0

    scales = ([float(x) for x in args.sweep_step_scale.split(",")]
              if args.sweep_step_scale else [args.step_scale])
    for scale in scales:
        sims = [run(members, args.days, args.seed + i, scale, not args.no_mishap)
                for i in range(args.trials)]
        report(sims, args.days, f"step-scale {scale}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
