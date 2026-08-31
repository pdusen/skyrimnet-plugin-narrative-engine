#!/usr/bin/env python3
"""Read a NarrativeEngine_Plots.log and report on the run it records.

The plot log is written to be analysable rather than merely readable -- every
line that decides an outcome carries the quantities that decided it. This is
the reader that cashes that in. It is deliberately a separate program from the
plugin: it knows the resolution model only as arithmetic copied from
PlotResolution.cpp, so when its predictions and the log's outcomes disagree,
one of the two is wrong and the disagreement is the finding.

Usage:
    python analyze-plot-log.py [path-to-NarrativeEngine_Plots.log]

Defaults to the SKSE log directory under Documents.
"""

import os
import re
import statistics
import sys
from collections import Counter, defaultdict

# --- The resolution model, mirrored from src/PlotResolution.cpp -------------
#
# RollProgress: actor = 0.5*competence + 0.5*suitability
#               blended = 0.5*actor + 0.5*U(0,1)
#               progress = rateMin + blended*(rateMax-rateMin), capped at
#                          threshold*maxFraction
# Mirrored, not imported, on purpose: a copy that has to be updated by hand
# when the model changes is a copy that notices when the model changes.
RATE_MIN = 1.5
RATE_MAX = 5.0
MAX_FRACTION = 0.45


def rate_stats(competence, suitability):
    """Expected and best-case absolute work per tick for this actor."""
    actor = 0.5 * competence + 0.5 * suitability
    span = RATE_MAX - RATE_MIN
    expected = RATE_MIN + span * (0.5 * actor + 0.25)  # E[U] = 0.5
    best = RATE_MIN + span * (0.5 * actor + 0.5)  # U -> 1
    return expected, best


LINE = re.compile(r"^\[(?P<stamp>[^\]]*)\]\s+(?P<tag>[A-Z]+)\s+(?P<rest>.*)$")


def fields(rest):
    """key=value pairs, plus quoted key="value" pairs."""
    out = {}
    for key, val in re.findall(r'(\w+)="([^"]*)"', rest):
        out[key] = val
    for key, val in re.findall(r"(\w+)=(-?[\d.]+(?:/[\d.]+)?)", rest):
        out.setdefault(key, val)
    for key, val in re.findall(r"(\w+)=([a-z_]+)", rest):
        out.setdefault(key, val)
    return out


def main(path):
    if not os.path.exists(path):
        sys.exit("no such log: " + path)

    ticks = []  # (game day, active plots, concurrency cap)
    dispatches = []
    resolves = []
    borns = []
    ends = []
    adapts = []
    reaps = 0
    reaped_plots = 0
    reaped_rows = 0
    # Lines are emitted during a tick and the TICK line closes it, so a line's
    # game day is the day of the next TICK line.
    pending = []
    day_of = {}

    with open(path, encoding="utf-8", errors="replace") as handle:
        for lineno, raw in enumerate(handle, 1):
            match = LINE.match(raw.rstrip("\n"))
            if not match:
                continue
            tag, rest = match.group("tag"), match.group("rest")
            fld = fields(rest)

            if tag == "TICK":
                day = float(fld["day"])
                active, budget = fld["active"].split("/")
                ticks.append((day, int(active), int(budget)))
                for idx in pending:
                    day_of[idx] = day
                pending = []
                continue

            record = (lineno, tag, fld)
            pending.append(lineno)
            if tag == "DISPATCH":
                dispatches.append(record)
            elif tag == "RESOLVE":
                resolves.append(record)
            elif tag == "BORN":
                borns.append(record)
            elif tag == "END":
                ends.append(record)
            elif tag == "ADAPT":
                adapts.append(record)
            elif tag == "REAP":
                reaps += 1
                # "REAP  N plot(s) M occupancy row(s) day=..." -- the row
                # count is what says the occupancy table is not growing
                # for the life of the save.
                numbers = re.findall(r"(\d+) (?:plot|occupancy row)\(s\)", rest)
                if len(numbers) == 2:
                    reaped_plots += int(numbers[0])
                    reaped_rows += int(numbers[1])

    print(f"== {os.path.basename(path)} ==")
    print(
        f"{len(ticks)} ticks, {len(borns)} plots born, {len(ends)} ended, "
        f"{len(dispatches)} steps dispatched, {len(resolves)} resolved, "
        f"{len(adapts)} adaptations, {reaps} reaps"
    )
    # Only when the line actually carried the counts. Traces from before
    # the occupancy prune landed have a REAP line with no row count, and
    # reporting a confident zero for those would be worse than silence.
    if reaped_plots or reaped_rows:
        print(f"{'':13}reaped {reaped_plots} plot(s) and {reaped_rows} spent occupancy row(s)")

    # --- 1. Does the simulation clock advance? ------------------------------
    days = [day for day, _, _ in ticks]
    stalls = sum(1 for a, b in zip(days, days[1:]) if b <= a)
    print(f"\n[clock]      span {days[0]:.2f}..{days[-1]:.2f} days; non-advancing stamps: {stalls}")
    if stalls:
        print("             ^^ the sim clock is not monotonic")

    # --- 2. Budget occupancy -----------------------------------------------
    occ = [(a, b) for _, a, b in ticks]
    full = sum(1 for a, b in occ if a >= b)
    print(
        f"[budget]     {full}/{len(occ)} ticks at the concurrency cap "
        f"({100.0 * full / max(1, len(occ)):.1f}%); mean active "
        f"{statistics.mean(a for a, _ in occ):.1f}/{occ[0][1]}"
    )

    # --- 3. Steps that cannot be won ---------------------------------------
    # A step is a race: `budget` ticks of work against a `threshold`. Neither
    # is random, and the per-tick rate depends only on the actor -- so whether
    # the race is winnable at all is decidable up front, from the DISPATCH
    # line alone, before a single die is rolled.
    infeasible = []
    marginal = []
    by_kind = defaultdict(lambda: [0, 0, 0])
    for lineno, _, fld in dispatches:
        try:
            budget = int(fld["budget"])
            threshold = float(fld["threshold"])
            competence = float(fld["competence"])
            suitability = float(fld["suitability"])
            travel = float(fld["travel"])
        except (KeyError, ValueError):
            continue
        expected, best = rate_stats(competence, suitability)
        ceiling = threshold * MAX_FRACTION
        exp_total = budget * min(expected, ceiling)
        best_total = budget * min(best, ceiling)
        kind = fld.get("step", "?").split(" ")[0]
        key = (kind, travel)
        by_kind[key][0] += 1
        if best_total < threshold:
            infeasible.append((lineno, fld, exp_total, best_total, threshold))
            by_kind[key][1] += 1
        elif exp_total < threshold:
            marginal.append((lineno, fld, exp_total, best_total, threshold))
            by_kind[key][2] += 1

    pct = lambda n: 100.0 * n / max(1, len(dispatches))
    print(
        f"\n[feasibility] of {len(dispatches)} dispatched steps: "
        f"{len(infeasible)} ({pct(len(infeasible)):.0f}%) cannot be completed even on "
        f"a perfect roll every tick; {len(marginal)} ({pct(len(marginal)):.0f}%) more are "
        f"expected to fall short."
    )
    if infeasible or marginal:
        print("\n              step / travel     n   impossible  expected-short")
        for (kind, travel), (n, imp, marg) in sorted(by_kind.items()):
            if imp or marg:
                print(f"              {kind:<12} {travel:.2f} {n:>4}   {imp:>9}   {marg:>13}")

    # --- 4. Does infeasibility actually show up in the outcomes? -----------
    # Pair each DISPATCH with the RESOLVE that follows it for the same plot
    # and step, so the prediction can be scored against what happened.
    verdict = {}
    for lineno, _, fld in dispatches:
        verdict[lineno] = "ok"
    for lineno, _, _, _, _ in infeasible:
        verdict[lineno] = "impossible"
    for lineno, _, _, _, _ in marginal:
        verdict[lineno] = "short"

    open_steps = {}
    for lineno, _, fld in dispatches:
        open_steps.setdefault((fld.get("plot"), fld.get("step")), []).append(lineno)
    scored = Counter()
    for _, _, fld in resolves:
        key = (fld.get("plot"), fld.get("step"))
        queue = open_steps.get(key)
        if not queue:
            continue
        scored[(verdict[queue.pop(0)], fld.get("outcome", "?"))] += 1

    if scored:
        print("\n[outcomes]   predicted        outcome              n")
        for (pred, outcome), n in sorted(scored.items()):
            print(f"              {pred:<15}  {outcome:<20} {n:>3}")

    # --- 5. Plot lifetimes and how they end --------------------------------
    if ends:
        lifetimes = [float(f["days"]) for _, _, f in ends if "days" in f]
        print(
            f"\n[plots]      {len(ends)} ended; lifetime median "
            f"{statistics.median(lifetimes):.1f}d, max {max(lifetimes):.1f}d; "
            f"steps median {statistics.median(int(f['steps']) for _, _, f in ends):.0f}"
        )
        print("             " + ", ".join(f"{k}={v}" for k, v in Counter(f.get("outcome") for _, _, f in ends).items()))

    # --- 6. Who is doing the work -----------------------------------------
    rungs = Counter(f.get("rung") for _, _, f in dispatches)
    print("\n[casting]    ladder rung: " + ", ".join(f"{k}={v}" for k, v in sorted(rungs.items())))
    considered = Counter(f.get("considered") for _, _, f in dispatches)
    alone = considered.get("1", 0)
    print(
        f"             {alone}/{len(dispatches)} dispatches had exactly one candidate "
        f"({pct(alone):.0f}%) -- nobody to choose between"
    )
    masterminds = Counter(f.get("mastermind") for _, _, f in borns)
    print(f"             {len(masterminds)} distinct masterminds over {len(borns)} plots")
    weights = [float(f["weight"]) for _, _, f in borns if "weight" in f]
    if weights:
        print(
            f"             mastermind weight: min {min(weights):.2f}, "
            f"median {statistics.median(weights):.2f}, max {max(weights):.2f}"
        )

    # --- 7. Actors reused faster than their cooldown -----------------------
    # fPlotActorCooldownDays defaults to 1.5. The mastermind is exempt by
    # design -- they are already engaged on their own plot.
    last_seen = {}
    violations = []
    for lineno, _, fld in dispatches:
        actor = fld.get("actor")
        day = day_of.get(lineno)
        if actor is None or day is None:
            continue
        prev = last_seen.get(actor)
        if prev is not None:
            prev_day, prev_plot = prev
            if fld.get("plot") != prev_plot and day - prev_day < 1.5:
                violations.append((actor, prev_day, day, prev_plot, fld.get("plot")))
        last_seen[actor] = (day, fld.get("plot"))
    print(f"\n[cooldown]   {len(violations)} actor(s) re-engaged on a different plot inside 1.5 days")
    for actor, d0, d1, p0, p1 in violations[:5]:
        print(f"             {actor}: plot {p0} day {d0:.2f} -> plot {p1} day {d1:.2f}")


if __name__ == "__main__":
    default = os.path.expanduser(
        r"~\Documents\My Games\Skyrim Special Edition\SKSE\NarrativeEngine_Plots.log"
    )
    main(sys.argv[1] if len(sys.argv) > 1 else default)
