#!/usr/bin/env python3
"""Verify every EditorID in the shipped PlotItems.ini exists in vanilla.

Same argument as check-plot-factions.py, and the same failure it guards
against: a pool is worthless if a third of it is invented. Every `EditorID` is
looked up in the Spriggit export at C:\\Projects\\spriggit-output rather than
trusted, per docs/VANILLA_RECORD_REFERENCE.md.

It also checks the TYPE, not only the existence. An `Acquire` step has to put
its object in somebody's inventory, so the record has to be a takeable item —
a name that resolves to a STAT or a CELL fails at runtime just as surely as
one that resolves to nothing, and does so much later.

Usage:
    python check-plot-items.py [--export DIR] [--ini PATH]

Exit 0 if every name resolves to an item, 1 otherwise. Step 15 of
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
DEFAULT_INI = REPO / "statics" / "SKSE" / "Plugins" / "NarrativeEngine" / "PlotItems.ini"
DEFAULT_EXPORT = r"C:\Projects\spriggit-output"
MASTERS = ["Skyrim", "Update", "Dawnguard", "HearthFires", "Dragonborn"]

# Record folders whose contents an actor can actually carry. Anything outside
# this set is the wrong kind of form even if the name resolves.
ITEM_FOLDERS = [
    "MiscItems",
    "Books",
    "Ingestibles",
    "Weapons",
    "Armors",
    "Ammunitions",
    "Keys",
    "SoulGems",
    "Scrolls",
    "AlchemicalApparatuses",
]

CATEGORIES = ["Valuable", "Document", "Contraband", "Drink"]


def index_items(export: str) -> dict[str, str]:
    """EditorID -> the record folder it was found in."""
    out: dict[str, str] = {}
    for master in MASTERS:
        for folder in ITEM_FOLDERS:
            d = os.path.join(export, master, folder)
            if not os.path.isdir(d):
                continue
            for name in os.listdir(d):
                m = re.match(r"^(.*) - [0-9A-Fa-f]{6}_", name)
                if m:
                    out.setdefault(m.group(1), folder)
    return out


def index_everything(export: str) -> dict[str, str]:
    """EditorID -> folder, across ALL record types.

    Only used to tell "does not exist" apart from "exists but is not an item",
    which are different mistakes and deserve different messages.
    """
    out: dict[str, str] = {}
    for master in MASTERS:
        root = os.path.join(export, master)
        if not os.path.isdir(root):
            continue
        for folder in os.listdir(root):
            d = os.path.join(root, folder)
            if not os.path.isdir(d):
                continue
            for name in os.listdir(d):
                m = re.match(r"^(.*) - [0-9A-Fa-f]{6}_", name)
                if m:
                    out.setdefault(m.group(1), folder)
    return out


def parse_ini(path: pathlib.Path) -> list[tuple[int, str, str, str]]:
    """(line number, category, EditorID, display name) for each item line."""
    rows = []
    section = None
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section != "Items" or "=" not in line:
            continue
        key, _, value = line.partition("=")
        editor_id, _, display = value.partition(",")
        rows.append((n, key.strip(), editor_id.strip(), display.strip()))
    return rows


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
    items = index_items(args.export)
    everything = index_everything(args.export)
    print(f"  {len(items)} carryable item(s), {len(everything)} record(s) total", file=sys.stderr)

    rows = parse_ini(ini_path)
    problems = []
    seen: dict[str, int] = {}
    by_category: collections.Counter[str] = collections.Counter()

    for n, category, editor_id, display in rows:
        if category not in CATEGORIES:
            problems.append(f"line {n}: category '{category}' is not one of {', '.join(CATEGORIES)}")
            continue
        if not editor_id or not display:
            problems.append(f"line {n}: expected '<EditorID>, <display name>'")
            continue
        if editor_id in seen:
            problems.append(f"line {n}: '{editor_id}' already listed on line {seen[editor_id]}")
            continue
        seen[editor_id] = n

        if editor_id not in items:
            if editor_id in everything:
                problems.append(
                    f"line {n}: '{editor_id}' exists but is a {everything[editor_id]} record, "
                    f"which nobody can carry"
                )
            else:
                problems.append(f"line {n}: '{editor_id}' does not exist in the export")
            continue

        # The display name is dropped straight into a sentence, so it has to
        # read like one -- lower case, and with its article.
        if display[:1].isupper() and not display.split()[0].isupper():
            problems.append(f"line {n}: display name '{display}' should start lower case; it is used mid-sentence")
        by_category[category] += 1

    print(f"\n{len(rows)} item(s) in {ini_path.name}:")
    for category in CATEGORIES:
        print(f"  {category:<12} {by_category[category]}")

    for _, category, editor_id, display in rows:
        if editor_id in items:
            print(f"  OK  {category:<11} {editor_id:<38} {items[editor_id]:<14} {display}")

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print("  - " + p)
        return 1

    empty = [c for c in CATEGORIES if by_category[c] == 0]
    if empty:
        print(f"\nNOTE: no items in {', '.join(empty)}; the model has nothing to reach for there.")
    print("\nevery EditorID resolves to a carryable item.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
