#!/usr/bin/env python3
"""Print every rostered faction and the resolved rank of every member.

Reads the same records the game reads and applies the same rules
PlotFactions.ini declares, then prints each participating faction with all of
its members ordered from highest standing to lowest. The point is to make a
wrong hierarchy obvious at a glance rather than inferable from an aggregate.

Two populations are shown per faction, because the difference between them is
itself load-bearing:

  * IN THE PLOT POPULATION -- the unique, socially-graphed NPCs the simulation
    can actually cast. This is the set that decides whether delegation works.
  * EVERY MEMBER -- including templated generics like "CWSoldierSons", which
    the simulation never casts but which make up the bulk of some factions.

Usage:
    python dump-faction-ranks.py [--export DIR] [--all]

Step 9 diagnostics for docs/implementation/PHASE_14_FACTION_PLOTS.md.
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[3]
ROSTER = REPO / "statics" / "SKSE" / "Plugins" / "NarrativeEngine" / "PlotFactions.ini"
POP = HERE / "population.json"
DEFAULT_EXPORT = r"C:\Projects\spriggit-output"


def load_check():
    spec = importlib.util.spec_from_file_location("check_plot_factions", HERE / "check-plot-factions.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", default=DEFAULT_EXPORT)
    ap.add_argument("--all", action="store_true", help="list every member, not just the plot population")
    args = ap.parse_args()

    check = load_check()

    print("reading the export...", file=sys.stderr)
    members, ranks = check.faction_members(args.export)
    sections = [s for s in check.parse_ini(ROSTER) if s["_id"].startswith("Faction:")]

    # Which NPCs the simulation can actually cast. Without this the counts
    # below are meaningless -- a faction of 288 that contains 8 castable
    # people behaves like a faction of 8.
    castable = set()
    display = {}
    if POP.exists():
        pop = json.loads(POP.read_text(encoding="utf-8"))
        for m in pop["members"]:
            castable.add(m["name"])
            display[m["name"]] = m["name"]
        # population.json keys people by display name; the export keys them by
        # EditorID, so match on both.
        editor_ids = set()
        for master in check.MASTERS:
            d = os.path.join(args.export, master, "Npcs")
            if not os.path.isdir(d):
                continue
            for n in os.listdir(d):
                txt = open(os.path.join(d, n), encoding="utf-8", errors="replace").read(4096)
                e = re.search(r"^EditorID: (.+)$", txt, re.M)
                nm = re.search(r"^Name: (.+)$", txt, re.M)
                if e and nm and nm.group(1).strip() in castable:
                    editor_ids.add(e.group(1).strip())
        castable = editor_ids
    else:
        print("WARNING: no population.json; run build-plot-population.py for the castable column",
              file=sys.stderr)

    grand = collections.Counter()

    for section in sections:
        keys = section["_keys"]
        primary = keys["Faction"][0]
        name = keys.get("DisplayName", [primary])[0]
        method = keys.get("RankMethod", ["?"])[0]

        pool = sorted(members.get(primary, set()))
        rows = [(check.standing_of(npc, section, members, ranks), npc) for npc in pool]
        rows.sort(key=lambda r: (-r[0], r[1]))

        in_pop = [r for r in rows if r[1] in castable] if castable else rows
        shown = rows if args.all else in_pop

        print(f"\n{'=' * 74}")
        print(f"{name}   [{primary}]   method={method}")
        print(f"  {len(pool)} member(s) in the export, {len(in_pop)} of them castable by the simulation")
        above = sum(1 for s, _ in in_pop if s > 0)
        top = sum(1 for s, _ in in_pop if s >= 0.999)
        print(f"  castable with standing > 0: {above}   at the top: {top}   "
              f"on the bottom rung: {len(in_pop) - above}")
        print(f"{'-' * 74}")

        if not shown:
            print("  (nobody)")
        for standing, npc in shown:
            mark = "*" if npc in castable else " "
            bar = "#" * int(round(standing * 20))
            print(f"  {mark} {standing:5.2f} {bar:<20} {npc}")

        grand["factions"] += 1
        grand["castable"] += len(in_pop)
        grand["ranked"] += above

    print(f"\n{'=' * 74}")
    print(f"{grand['factions']} faction(s); {grand['castable']} castable membership(s); "
          f"{grand['ranked']} with standing above the bottom rung")
    if not args.all:
        print("(* = castable. Re-run with --all to include templated generics.)")

    # The question the aggregate was hiding: for each faction, how many
    # castable members could a given superior actually delegate to?
    print("\nDELEGATION REACH -- castable subordinates available to each ranked member")
    print("-" * 74)
    for section in sections:
        keys = section["_keys"]
        primary = keys["Faction"][0]
        name = keys.get("DisplayName", [primary])[0]
        pool = [n for n in sorted(members.get(primary, set())) if not castable or n in castable]
        rows = sorted(((check.standing_of(n, section, members, ranks), n) for n in pool), reverse=True)
        for standing, npc in rows:
            if standing <= 0:
                continue
            below = sum(1 for s, _ in rows if s < standing)
            print(f"  {name:<28} {npc:<24} {standing:4.2f} -> {below} subordinate(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
