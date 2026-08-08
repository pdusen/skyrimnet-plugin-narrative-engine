#!/usr/bin/env python3
"""Saturation breakdown for a NarrativeEngine gossip trace.

Answers: for rumors of a given seed notability, how much of a household,
settlement or hold did they actually reach?

The BURNOUT lines in the trace carry reach and unit COUNTS, but not
coverage -- "settlements=2" says a rumor touched two settlements, not
whether it saturated either. This script reconstructs coverage by
replaying every TELL line to recover each rumor's carrier set, then
joining those carriers against the social graph (from
build-social-graph.py --json) to recover unit membership and unit sizes.

    python scripts/build-social-graph.py --json graph.json
    python scripts/analyze-gossip-log.py NarrativeEngine_Gossip.log graph.json

The join is BY DISPLAY NAME, because the trace logs names rather than
FormIDs. That is lossy in two ways worth keeping in mind when reading the
output: mod-added NPCs absent from the offline graph cannot be matched at
all, and Skyrim reuses display names freely. The unmatched rate is
reported so you can judge how much to trust the rest.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from collections import Counter, defaultdict

# Milestone 1 traces ended a SEED line with `slice=<label>`; Milestone 2
# replaced that with `memory=<id>`, naming the source memory the rumor came
# from. Both are accepted so older traces still parse. The location group is
# anchored on whichever trailer is present rather than on end-of-line: a
# non-greedy `@(.+?)` followed by an OPTIONAL trailer will happily swallow
# `memory=42` into the location and report no source at all.
SEED_RE = re.compile(
    r"SEED\s+r(\d+)\s+notability=([\d.]+)\s+origin=(.+?)\s+@(.+?)"
    r"(?:\s+slice=(\S+)|\s+memory=(-?\d+))?\s*$")
# `tier=` is optional so this parses traces from before it was added. The
# earlier hard requirement of `via=... @` silently stopped matching every
# TELL line the moment tier= was inserted, which made every rumor look like
# it had exactly one carrier.
TELL_RE = re.compile(
    r"TELL\s+r(\d+)\s+gen=(\d+)\s+n=([\d.]+)\s+(.+?)\s+->\s+(.+?)\s+via=(\S+)"
    r"(?:\s+tier=(\S+))?\s+@")
BURN_RE = re.compile(
    r"BURNOUT r(\d+)\s+reach=(\d+)\s+depth=(\d+)\s+holds=(\d+)\s+settlements=(\d+)\s+days=([\d.]+)"
)

# Seed-notability bands. The harness seeds four stratified slices, but a
# band is the more useful axis here because it is what the model actually
# keys on.
NOTE_BANDS = [(0.85, 1.01, "0.85-1.00"), (0.60, 0.85, "0.60-0.85"),
              (0.40, 0.60, "0.40-0.60"), (0.00, 0.40, "0.00-0.40")]

SIZE_BANDS = [(1, 2, "1-2"), (3, 5, "3-5"), (6, 12, "6-12"),
              (13, 30, "13-30"), (31, 1 << 30, "31+")]


def band(value, bands):
    for lo, hi, label in bands:
        if lo <= value < hi or (bands is SIZE_BANDS and lo <= value <= hi):
            return label
    return bands[-1][2]


def load_graph(path):
    raw = json.load(open(path, encoding="utf-8"))["participants"]
    by_name = {}
    collisions = Counter()
    for p in raw.values():
        n = p.get("name") or ""
        if not n:
            continue
        collisions[n] += 1
        by_name[n] = p
    sizes = {"household": Counter(), "settlement": Counter(), "hold": Counter()}
    for p in raw.values():
        for tier in sizes:
            if p.get(tier):
                sizes[tier][p[tier]] += 1
    return by_name, sizes, {n for n, c in collisions.items() if c > 1}


def parse_log(path):
    rumors = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if (m := SEED_RE.search(line)):
            rumors[int(m.group(1))] = {
                "notability": float(m.group(2)),
                "origin": m.group(3).strip(),
                # Milestone 1 stratification label, or the Milestone 2
                # source memory id. Only one is ever populated.
                "slice": m.group(5) or "?",
                "memory": int(m.group(6)) if m.group(6) is not None else None,
                "carriers": {m.group(3).strip()},
                "burned": False,
            }
        elif (m := TELL_RE.search(line)):
            r = rumors.get(int(m.group(1)))
            if r:
                r["carriers"].add(m.group(4).strip())
                r["carriers"].add(m.group(5).strip())
        elif (m := BURN_RE.search(line)):
            r = rumors.get(int(m.group(1)))
            if r:
                r["burned"] = True
                r["reach"] = int(m.group(2))
                r["days"] = float(m.group(6))
    return rumors


def report_sources(rumors):
    """No memory is ever used twice -- the central Milestone 2 property.

    Every rumor records the memory id it was seeded from, and the claim
    ledger is what makes a repeat impossible. That property is only
    actually verified if something reads the ids back, so it is checked
    here rather than left to be eyeballed across a session's worth of
    trace. A duplicate is a hard failure: the same story is circulating
    twice, and everyone who heard it the first time is susceptible again.
    """
    sourced = {rid: r["memory"] for rid, r in rumors.items() if r["memory"] is not None}
    print(f"\n{'=' * 76}\nSOURCE MEMORIES\n{'=' * 76}")
    if not sourced:
        print("No memory-sourced SEED lines -- a Milestone 1 trace, or seeding never ran.")
        return
    print(f"{len(sourced)} of {len(rumors)} rumors carry a source memory id.")

    seen = Counter(sourced.values())
    dupes = sorted(mid for mid, c in seen.items() if c > 1)
    if dupes:
        print(f"\n  *** {len(dupes)} memory/memories seeded more than one rumor "
              f"-- the claim ledger did not hold ***")
        for mid in dupes:
            rids = sorted(rid for rid, m in sourced.items() if m == mid)
            print(f"    m{mid}: {', '.join(f'r{r:02}' for r in rids)}")
    else:
        print("  No memory seeded twice.")

    unsourced = len(rumors) - len(sourced)
    if unsourced:
        print(f"  {unsourced} rumor(s) carry no source id -- pre-Milestone-2 lines in the same trace.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("graph")
    ap.add_argument("--min-unit-size", type=int, default=2,
                    help="ignore units smaller than this (default 2: a 1-person "
                         "'household' is saturated by definition and tells you nothing)")
    args = ap.parse_args()

    by_name, sizes, dupes = load_graph(args.graph)
    rumors = parse_log(args.log)
    if not rumors:
        print("No SEED lines found — is that a gossip trace?", file=sys.stderr)
        return 1

    report_sources(rumors)

    matched = unmatched = 0
    # (tier, notability band, size band) -> list of coverage fractions
    cov = defaultdict(list)
    # per rumor: how many units it fully saturated
    fully = defaultdict(Counter)

    for rid, r in sorted(rumors.items()):
        nb = band(r["notability"], NOTE_BANDS)
        # carrier -> unit, per tier
        touched = {"household": Counter(), "settlement": Counter(), "hold": Counter()}
        for name in r["carriers"]:
            p = by_name.get(name)
            if p is None:
                unmatched += 1
                continue
            matched += 1
            for tier in touched:
                if p.get(tier):
                    touched[tier][p[tier]] += 1
        for tier, hits in touched.items():
            for unit, knowers in hits.items():
                total = sizes[tier][unit]
                if total < args.min_unit_size:
                    continue
                frac = knowers / total
                cov[(tier, nb, band(total, SIZE_BANDS))].append(frac)
                if frac >= 0.999:
                    fully[nb][tier] += 1

    total_names = matched + unmatched
    print(f"rumors parsed: {len(rumors)}  ({sum(1 for r in rumors.values() if r['burned'])} burned out)")
    print(f"carrier name lookups: {matched} matched, {unmatched} unmatched "
          f"({100.0 * unmatched / max(1, total_names):.1f}% — mod-added NPCs absent from the offline graph)")
    if dupes:
        print(f"ambiguous display names in graph (collapsed): {len(dupes)}")

    for tier in ("household", "settlement", "hold"):
        print(f"\n{'=' * 76}\n{tier.upper()} COVERAGE — mean share of the unit's members who ended up knowing"
              f"\n{'=' * 76}")
        header = f"{'seed notability':<16}" + "".join(f"{lbl:>13}" for _, _, lbl in SIZE_BANDS)
        print(header)
        print("-" * len(header))
        for _, _, nb in NOTE_BANDS:
            row = f"{nb:<16}"
            for _, _, sb in SIZE_BANDS:
                vals = cov.get((tier, nb, sb))
                row += f"{(f'{100 * statistics.mean(vals):.0f}% (n={len(vals)})' if vals else '-'):>13}"
            print(row)

    print(f"\n{'=' * 76}\nUNITS FULLY SATURATED (every member knows), counted across all rumors\n{'=' * 76}")
    print(f"{'seed notability':<16}{'households':>14}{'settlements':>14}{'holds':>10}")
    print("-" * 54)
    for _, _, nb in NOTE_BANDS:
        f = fully[nb]
        print(f"{nb:<16}{f['household']:>14}{f['settlement']:>14}{f['hold']:>10}")

    print(f"\n{'=' * 76}\nPER-RUMOR SUMMARY\n{'=' * 76}")
    src_col = "memory" if any(r["memory"] is not None for r in rumors.values()) else "slice"
    print(f"{'rumor':<7}{'note':>6}{src_col:>19}{'reach':>7}{'days':>7}  best-saturated unit")
    print("-" * 76)
    for rid, r in sorted(rumors.items()):
        best = ("", 0.0, 0)
        for name in r["carriers"]:
            p = by_name.get(name)
            if not p:
                continue
            for tier in ("household", "settlement", "hold"):
                unit = p.get(tier)
                if not unit or sizes[tier][unit] < args.min_unit_size:
                    continue
                k = sum(1 for c in r["carriers"]
                        if (q := by_name.get(c)) and q.get(tier) == unit)
                frac = k / sizes[tier][unit]
                if frac > best[1]:
                    best = (unit, frac, sizes[tier][unit])
        label = f"{best[0]} {100 * best[1]:.0f}% of {best[2]}" if best[0] else "-"
        source = f"m{r['memory']}" if r["memory"] is not None else r["slice"]
        print(f"r{rid:<6}{r['notability']:>6.2f}{source:>19}"
              f"{r.get('reach', 0):>7}{r.get('days', 0):>7.1f}  {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
