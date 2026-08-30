#!/usr/bin/env python3
"""Verify every EditorID in the shipped PlotFactions.ini exists in vanilla.

A roster is worthless if a third of it is invented. This looks every `Faction`,
`MarkerFaction` and `Member` name up in the Spriggit export at
C:\\Projects\\spriggit-output rather than trusting recall, per
docs/VANILLA_RECORD_REFERENCE.md -- and checks the type as well as the
existence, since a name that resolves to the wrong record type fails at
runtime just as surely as one that resolves to nothing.

It also reports what each section will actually DO: how many NPCs are in the
primary faction, how many the markers or overrides distinguish, and whether the
section therefore produces a hierarchy at all. A section that parses cleanly
but leaves every member on the bottom rung is a section that does nothing, and
that is invisible from the file alone.

Usage:
    python check-plot-factions.py [--export DIR] [--ini PATH]

Exit 0 if every name resolves, 1 otherwise. Step 7 of
docs/implementation/PHASE_14_FACTION_PLOTS.md.
"""
from __future__ import annotations

import argparse
import collections
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[3]
DEFAULT_INI = REPO / "statics" / "SKSE" / "Plugins" / "NarrativeEngine" / "PlotFactions.ini"
DEFAULT_EXPORT = r"C:\Projects\spriggit-output"
MASTERS = ["Skyrim", "Update", "Dawnguard", "HearthFires", "Dragonborn"]


def index_records(export: str, folder: str) -> dict[str, str]:
    """EditorID -> file path, for one record folder across every master."""
    out = {}
    for master in MASTERS:
        d = os.path.join(export, master, folder)
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if not name.endswith((".yaml", ".yml")):
                continue
            path = os.path.join(d, name)
            with open(path, encoding="utf-8", errors="replace") as fh:
                head = fh.read(4096)
            m = re.search(r"^EditorID: (.+)$", head, re.M)
            if m:
                out[m.group(1).strip()] = path
    return out


def parse_ini(path: pathlib.Path) -> list[dict]:
    """Minimal reader: sections, and repeated keys kept as lists."""
    sections = []
    current = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = {"_id": line[1:-1], "_keys": collections.defaultdict(list)}
            sections.append(current)
            continue
        if current is None or "=" not in line:
            continue
        key, _, value = line.partition("=")
        current["_keys"][key.strip()].append(value.strip())
    return sections


def faction_members(export: str) -> dict[str, set[str]]:
    """faction EditorID -> set of NPC EditorIDs, so we can report coverage."""
    fac_name = {}
    for master in MASTERS:
        d = os.path.join(export, master, "Factions")
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            k = re.search(r"- ([0-9A-F]{6})_(\w+\.esm)", name)
            if not k:
                continue
            with open(os.path.join(d, name), encoding="utf-8", errors="replace") as fh:
                head = fh.read(2048)
            m = re.search(r"^EditorID: (.+)$", head, re.M)
            if m:
                fac_name[f"{k.group(1)}:{k.group(2)}"] = m.group(1).strip()

    members = collections.defaultdict(set)
    for master in MASTERS:
        d = os.path.join(export, master, "Npcs")
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            path = os.path.join(d, name)
            txt = open(path, encoding="utf-8", errors="replace").read()
            e = re.search(r"^EditorID: (.+)$", txt, re.M)
            if not e:
                continue
            npc = e.group(1).strip()
            block = re.search(r"^Factions:\n((?:- Faction:.*\n(?:  \w+:.*\n)*)+)", txt, re.M)
            if not block:
                continue
            for fm in re.finditer(r"- Faction: (\S+)\n", block.group(1)):
                fid = fac_name.get(fm.group(1))
                if fid:
                    members[fid].add(npc)
    return members


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", default=DEFAULT_EXPORT)
    ap.add_argument("--ini", default=str(DEFAULT_INI))
    args = ap.parse_args()

    ini_path = pathlib.Path(args.ini)
    if not ini_path.exists():
        print(f"no such file: {ini_path}")
        return 1

    print("indexing the export...", file=sys.stderr)
    factions = index_records(args.export, "Factions")
    npcs = index_records(args.export, "Npcs")
    members = faction_members(args.export)
    print(f"  {len(factions)} factions, {len(npcs)} NPCs", file=sys.stderr)

    problems = []
    for section in parse_ini(ini_path):
        sid = section["_id"]
        if not sid.startswith("Faction:"):
            problems.append(f"[{sid}]: sections must be named [Faction:<id>]")
            continue
        keys = section["_keys"]
        name = sid.split(":", 1)[1]

        primary = (keys.get("Faction") or [None])[0]
        method = (keys.get("RankMethod") or [None])[0]
        if not primary:
            problems.append(f"[{sid}]: no Faction")
            continue
        if primary not in factions:
            problems.append(f"[{sid}]: Faction '{primary}' does not exist in the export")
            continue
        if not (keys.get("DisplayName") or [None])[0]:
            problems.append(f"[{sid}]: no DisplayName")
        if method not in ("Rank", "Marker", "Explicit"):
            problems.append(f"[{sid}]: RankMethod '{method}' is not Rank, Marker or Explicit")

        pool = members.get(primary, set())
        distinguished = set()

        for marker in keys.get("MarkerFaction", []):
            if marker not in factions:
                problems.append(f"[{sid}]: MarkerFaction '{marker}' does not exist in the export")
                continue
            inside = members.get(marker, set()) & pool
            distinguished |= inside
            if not inside:
                problems.append(
                    f"[{sid}]: MarkerFaction '{marker}' shares no members with '{primary}' — it will "
                    f"never promote anyone"
                )

        for raw in keys.get("Member", []):
            who, _, rank = raw.partition(",")
            who = who.strip()
            if who not in npcs:
                problems.append(f"[{sid}]: Member '{who}' does not exist in the export")
                continue
            if not rank.strip().lstrip("-").isdigit():
                problems.append(f"[{sid}]: Member '{who}' has a non-numeric rank '{rank.strip()}'")
            if who not in pool:
                problems.append(
                    f"[{sid}]: Member '{who}' is not in '{primary}' — the override will never apply"
                )
            distinguished.add(who)

        if method == "Rank":
            top = (keys.get("MaxRank") or ["?"])[0]
            note = f"authored ranks, MaxRank={top}"
        else:
            note = f"{len(distinguished)} of {len(pool)} member(s) distinguished"
            if not distinguished:
                problems.append(f"[{sid}]: nothing distinguishes any member; the section does nothing")
        print(f"  OK  {name:<18} {primary:<30} {method:<9} {note}")

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print("  - " + p)
        return 1
    print("\nevery EditorID in PlotFactions.ini resolves, and every section distinguishes somebody.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
