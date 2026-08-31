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


factions_index: dict[str, str] = {}
npcs_unique: set[str] = set()


def unique_npcs(export: str) -> set[str]:
    """EditorIDs of the unique-flagged NPCs -- the population plots draw from.

    The court rosters printed below are filtered to these, because the guards
    that fill out a hold's bottom rung are generic and never reach the
    simulation; listing them would bury the four people who matter.
    """
    out = set()
    for master in MASTERS:
        d = os.path.join(export, master, "Npcs")
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            txt = open(os.path.join(d, name), encoding="utf-8", errors="replace").read(4096)
            e = re.search(r"^EditorID: (.+)$", txt, re.M)
            if e and re.search(r"^  - Unique$", txt, re.M):
                out.add(e.group(1).strip())
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


def faction_members(export: str):
    """(faction -> set of NPC EditorIDs, (npc, faction) -> authored rank).

    The rank comes from `Rank`, which Spriggit omits when it is 0. `Fluff` on
    the same entry is three always-zero padding bytes and is NOT the rank -
    reading it yields 0 for every NPC in the game, including ones with a plain
    `Rank: 6` on the next line.
    """
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
    ranks = {}
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
            for fm in re.finditer(r"- Faction: (\S+)\n((?:  \w+:.*\n)*)", block.group(1)):
                fid = fac_name.get(fm.group(1))
                if not fid:
                    continue
                members[fid].add(npc)
                rm = re.search(r"^  Rank: (-?\d+)", fm.group(2), re.M)
                ranks[(npc, fid)] = int(rm.group(1)) if rm else 0
    return members, ranks


def standing_of(npc, section, members, ranks):
    """Mirror of PlotFactionStanding.cpp, so the shipped roster can be checked
    against the real export rather than a fixture.

    Kept deliberately close to the C++ - method dispatch, then overrides
    layered on top, then normalise - because the point is to catch the two
    implementations disagreeing.
    """
    keys = section["_keys"]
    primary = keys["Faction"][0]
    method = keys["RankMethod"][0]

    top = 0
    derived = 0

    if method == "Rank":
        top = int(keys.get("MaxRank", ["1"])[0])
        derived = ranks.get((npc, primary), 0)
    elif method == "Marker":
        markers = keys.get("MarkerFaction", [])
        top = len(markers)
        for i, marker in enumerate(markers):
            if npc in members.get(marker, set()):
                derived = top - i
                break

    overridden = None
    for raw in keys.get("Member", []):
        who, _, rank = raw.partition(",")
        try:
            value = int(rank.strip())
        except ValueError:
            continue
        top = max(top, value)
        if who.strip() == npc:
            overridden = max(overridden or 0, value)

    rung = overridden if overridden is not None else derived
    if top <= 0 or rung <= 0:
        return 0.0
    return min(1.0, rung / top)


def members_of(section, members, ranks):
    """Mirror of the membership rule in PlotPopulation.cpp.

    Membership = Faction is the primary faction outright. Membership = Ranked
    is only the people the ladder places -- plus anyone a Member line names
    with a rank above 0, who is admitted even when the primary faction misses
    them.
    """
    keys = section["_keys"]
    primary = keys["Faction"][0]
    ranked = (keys.get("Membership") or ["Faction"])[0] == "Ranked"

    pool = set(members.get(primary, set()))
    if not ranked:
        return pool

    for raw in keys.get("Member", []):
        who, _, rank = raw.partition(",")
        try:
            if int(rank.strip()) > 0:
                pool.add(who.strip())
        except ValueError:
            pass
    return {npc for npc in pool if standing_of(npc, section, members, ranks) > 0.0}


def check_tenure(sections, members, ranks, notes):
    """Simulate the civil war, and check each court against both outcomes.

    The courts rank by job factions that hold the understudy as well as the
    incumbent -- JobJarlFaction has Balgruuf AND Vignar, JobHousecarlFaction
    has Unmid AND Maul -- so a court with no tenure gate seats two jarls per
    hold and lets a man who holds no office give orders to the sitting
    steward. A court that hardcodes the incumbent instead is right until the
    war moves and wrong forever after.

    So this does not check a state; it checks a FUNCTION. For each hold it
    plays out the rule CWGovernmentScript itself applies --

        if the hold is Imperial: everyone in GovImperial is installed,
        everyone in GovSons is exiled, and everyone in neither is untouched

    -- once for each outcome, and asserts the court that comes out is the
    right one both times. That is the only way to catch a roster that is
    accidentally correct for the game's opening minute.
    """
    problems = []

    def gated(sec):
        keys = sec["_keys"]
        office = (keys.get("OfficeFaction") or [None])[0]
        candidates = keys.get("OfficeCandidate") or []
        return office, candidates

    def seated(npc, office, candidates, installed, members):
        """The gate, with `installed` standing in for live GovRuling."""
        if not office:
            return True
        if not any(npc in members.get(c, set()) for c in candidates):
            return True  # the war does not touch this person
        return npc in members.get(installed, set())

    print("\ncourt tenure, simulated over both outcomes:")

    for sec in sections:
        sid = sec["_id"]
        if not sid.startswith("Faction:court_"):
            continue
        office, candidates = gated(sec)
        primary = (sec["_keys"].get("Faction") or [None])[0]
        if not primary:
            continue

        if not office:
            # Only defensible when the war cannot reach the hold at all.
            pool = members.get(primary, set())
            exposed = sorted(
                n for n in pool if any(n in members.get(c, set()) for c in ("GovImperial", "GovSons"))
            )
            if exposed:
                problems.append(
                    f"[{sid}]: no tenure gate, yet {len(exposed)} member(s) change with the civil war "
                    f"({', '.join(exposed[:4])}...)"
                )
            else:
                notes.append(f"[{sid}]: no tenure gate, and nobody here is in either government")
            continue
        for c in candidates:
            if c not in factions_index:
                problems.append(f"[{sid}]: OfficeCandidate '{c}' does not exist in the export")

        courts = {}
        for branch in candidates:
            roster = []
            for npc in sorted(members_of(sec, members, ranks)):
                if not seated(npc, office, candidates, branch, members):
                    continue
                st = standing_of(npc, sec, members, ranks)
                if st > 0 and npc in npcs_unique:
                    roster.append((st, npc))
            roster.sort(reverse=True)
            courts[branch] = roster

            top = [n for st, n in roster if st == 1.0]
            if len(top) != 1:
                problems.append(
                    f"[{sid}]: under {branch} the court has {len(top)} head(s) of state "
                    f"({', '.join(top) or 'none'}); exactly one is right"
                )

        shown = " | ".join(
            f"{b.replace('Gov', '')}: " + ", ".join(f"{n} {st:.2f}" for st, n in r[:4])
            for b, r in courts.items()
        )
        print(f"  {sid.split(':')[1]:<18} {shown}")

        # A hold that changes hands must produce a DIFFERENT court, or the
        # gate is present and doing nothing.
        pool = members_of(sec, members, ranks)
        changes_hands = any(
            n in members.get("GovImperial", set()) and n not in members.get("GovSons", set()) for n in pool
        )
        if changes_hands and len(candidates) > 1:
            rosters = [set(n for _, n in r) for r in courts.values()]
            if all(r == rosters[0] for r in rosters):
                problems.append(f"[{sid}]: the hold changes hands, yet both outcomes give the same court")

    return problems


def check_orderings(sections, members, ranks):
    """The orderings the roster exists to produce, on the real data."""
    by_faction = {}
    for sec in sections:
        if sec["_id"].startswith("Faction:") and sec["_keys"].get("Faction"):
            by_faction[sec["_keys"]["Faction"][0]] = sec

    def standing(npc, faction):
        sec = by_faction.get(faction)
        return standing_of(npc, sec, members, ranks) if sec else 0.0

    problems = []
    print("\nstanding on the real export:")

    cases = [
        ("Ulfric over Galmar", "Ulfric", "Galmar", "CWSonsFaction"),
        ("Tullius over Rikke", "GeneralTullius", "Rikke", "CWImperialFaction"),
        ("Mercer over Brynjolf", "MercerFrey", "Brynjolf", "ThievesGuildFaction"),
    ]
    for label, high, low, faction in cases:
        a, b = standing(high, faction), standing(low, faction)
        print(f"  {label:<26} {high} {a:.2f} vs {low} {b:.2f}")
        if not a > b:
            problems.append(f"{label}: {a:.2f} is not above {b:.2f}")

    # Savos Aren must top the College, above every other member of it.
    college = "CollegeofWinterholdFaction"
    savos = standing("SavosAren", college)
    others = [(standing(n, college), n) for n in members.get(college, set()) if n != "SavosAren"]
    best_other = max(others) if others else (0.0, "-")
    print(f"  {'Savos tops the College':<26} SavosAren {savos:.2f} vs {best_other[1]} {best_other[0]:.2f}")
    if not savos > best_other[0]:
        problems.append(f"Savos Aren does not top the College ({savos:.2f} vs {best_other[0]:.2f})")

    # Kodlak must top the Companions via the marker method.
    companions = "CompanionsFaction"
    kodlak = standing("KodlakWhitemane", companions)
    circle = standing("AelaTheHuntress", companions)
    print(f"  {'Kodlak over the Circle':<26} KodlakWhitemane {kodlak:.2f} vs AelaTheHuntress {circle:.2f}")
    if not kodlak > circle > 0.0:
        problems.append(f"Companions marker order wrong: Kodlak {kodlak:.2f}, Aela {circle:.2f}")

    # And a leader must reach the top of the scale, so that leaders of
    # different factions weigh the same however deep their ladders are.
    for who, faction in [("Ulfric", "CWSonsFaction"), ("SavosAren", college),
                         ("KodlakWhitemane", companions), ("Astrid", "DarkBrotherhoodFaction")]:
        v = standing(who, faction)
        if v != 1.0:
            problems.append(f"{who} should be at the top of their faction (1.00), got {v:.2f}")
    print("  every faction leader normalises to 1.00")

    # The court ladder, on Whiterun, which is the hold that exercises every
    # rung of it. Strictly descending, and the hold's other residents are not
    # in the court at all.
    court = by_faction.get("CrimeFactionWhiterun")
    if court is None:
        problems.append("no section for CrimeFactionWhiterun to check the court ladder against")
    else:
        ladder = [
            ("jarl", "BalgruuftheGreater"),
            ("steward", "ProventusAvenicci"),
            ("housecarl", "Irileth"),
            ("wizard", "FarengarSecretFire"),
            ("captain", "CommanderCaius"),
            ("guard", "GuardWhiterunCityGeneric"),
        ]
        values = [(label, npc, standing_of(npc, court, members, ranks)) for label, npc in ladder]
        print("  Whiterun court ladder      " + " > ".join(f"{lb} {v:.2f}" for lb, _, v in values))
        for (la, na, va), (lb, nb, vb) in zip(values, values[1:]):
            if not va > vb > 0.0:
                problems.append(f"court ladder: {la} {na} {va:.2f} does not outrank {lb} {nb} {vb:.2f}")

        court_members = members_of(court, members, ranks)
        hold = members.get("CrimeFactionWhiterun", set())
        for outsider in ("Ysolda", "Alvor", "DanicaPureSpring"):
            if outsider in hold and outsider in court_members:
                problems.append(f"court of Whiterun: {outsider} is a hold resident, not a courtier")
        print(f"  {'court is not the hold':<26} {len(court_members)} courtier(s) "
              f"of {len(hold)} hold resident(s)")

    return problems


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
    members, ranks = faction_members(args.export)
    global factions_index, npcs_unique
    factions_index = factions
    npcs_unique = unique_npcs(args.export)
    print(f"  {len(factions)} factions, {len(npcs)} NPCs", file=sys.stderr)

    problems = []
    notes = []
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

        membership = (keys.get("Membership") or ["Faction"])[0]
        if membership not in ("Faction", "Ranked"):
            problems.append(f"[{sid}]: Membership '{membership}' is not Faction or Ranked")
        ranked = membership == "Ranked"

        pool = members.get(primary, set())
        distinguished = set()

        for marker in keys.get("MarkerFaction", []):
            if marker not in factions:
                problems.append(f"[{sid}]: MarkerFaction '{marker}' does not exist in the export")
                continue
            inside = members.get(marker, set()) & pool
            distinguished |= inside
            if not inside:
                # An empty rung is a problem only when the marker faction is
                # empty everywhere -- that is a dud name. A real faction that
                # does not reach into this hold is a SPACER, and a deliberate
                # one: the courts share a single six-rung ladder so the same
                # office weighs the same in every hold. Drop the rung in
                # Falkreath for want of a court wizard and Falkreath's
                # housecarl starts outranking Whiterun's.
                if members.get(marker):
                    notes.append(
                        f"[{sid}]: MarkerFaction '{marker}' has no members inside '{primary}'; "
                        f"the rung is a spacer here"
                    )
                else:
                    problems.append(
                        f"[{sid}]: MarkerFaction '{marker}' has no members at all — it will never "
                        f"promote anyone"
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
                if ranked:
                    # Under Ranked, being named IS a way in -- the only way to
                    # reach someone the primary faction misses.
                    notes.append(f"[{sid}]: Member '{who}' is not in '{primary}'; admitted by name")
                else:
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
        if ranked:
            # Under Ranked the "N of M" framing misleads -- N can exceed M,
            # because a Member line admits people the primary faction misses.
            note = (f"{len(members_of(section, members, ranks))} ranked member(s), "
                    f"scoped by {len(pool)} in '{primary}'")
        print(f"  OK  {name:<18} {primary:<30} {method:<9} {note}")

    problems += check_orderings(parse_ini(ini_path), members, ranks)
    problems += check_tenure(parse_ini(ini_path), members, ranks, notes)

    if notes:
        print("\nNOTES (deliberate, not failures):")
        for n in notes:
            print("  - " + n)

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print("  - " + p)
        return 1
    print("\nevery EditorID resolves, every section distinguishes somebody, and the orderings hold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
