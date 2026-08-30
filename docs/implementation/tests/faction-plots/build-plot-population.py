#!/usr/bin/env python3
"""Extract the plot simulation's population from the Spriggit export.

Emits the offline equivalent of what PlotPopulation::Build produces at
kDataLoaded: one row per unique NPC with their hold, their authored competence
and skills, their memberships of admitted factions WITH RANK, and their
personal ties.

This deliberately does NOT re-implement the Spriggit reader. Phase 13's
build-social-graph.py already parses the record tree, classifies the location
tiers, resolves residence from LCUN and builds the relationship and
faction-co-membership edges, and it is 1,100 lines of hard-won correctness
about a format with several traps in it (additive ACUN merges, CRLF, load-order
override). It is imported as a module and its graph reused. What is added here
is the two things it has no reason to carry: faction RANK, and the authored
skills that PlotResolution::Suitability consults.

Usage:
    python build-plot-population.py [--export DIR] [--out population.json]

Step 7 of docs/implementation/PHASE_14_FACTION_PLOTS.md.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
GOSSIP = HERE.parent / "gossip-spread" / "build-social-graph.py"
CHECK = HERE / "check-plot-factions.py"
ROSTER = HERE.parents[3] / "statics" / "SKSE" / "Plugins" / "NarrativeEngine" / "PlotFactions.ini"
DEFAULT_EXPORT = r"C:\Projects\spriggit-output"

# Matches PlotPopulation.cpp: level is normalised against 50 rather than 100,
# because a level-50 NPC is already exceptional in vanilla and normalising
# against the theoretical maximum squashes almost everyone into the bottom
# quarter of the range.
LEVEL_CEILING = 50.0

# The three skills PlotResolution::Suitability actually consults. Reading only
# these keeps the walk short; adding more would mean changing both sides.
SKILLS = ("Speech", "Sneak", "Pickpocket")


def load_by_path(path, name):
    """Import a sibling script by path — the directory names are hyphenated."""
    if not path.exists():
        sys.exit(f"cannot find {path}")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_gossip_module():
    """The Phase 13 Spriggit reader, reused rather than reimplemented."""
    return load_by_path(GOSSIP, "build_social_graph")


def faction_rank(entry: dict) -> int:
    """Rank out of an NPC faction membership entry.

    Spriggit writes Mutagen's RankPlacement as

        - Faction: 029DA9:Skyrim.esm
          Rank: 3            <-- OMITTED ENTIRELY when the rank is 0
          Fluff: 0x000000    <-- three unused bytes, ALWAYS zero

    so `Rank` is the field and `Fluff` is padding. Reading Fluff -- or reading
    Rank only as a fallback behind it -- yields 0 for every NPC in the game,
    including the ones that plainly do have ranks.
    """
    raw = entry.get("Rank", 0)
    return int(raw) if isinstance(raw, (int, float)) else 0


def npc_skills(rec: dict) -> dict:
    """Authored skill values, normalised to 0..1.

    Spriggit writes PlayerSkills.SkillValues as a LIST of {Key, Value} pairs,
    not as a mapping. Reading it as a mapping silently yields the default for
    every skill, which is worse than an error: the simulation runs, every actor
    has identical suitability, and the suitability half of the progress roll
    quietly becomes a constant.

    A record that genuinely omits the block gets the midpoint rather than zero,
    because "not authored" is not the same claim as "hopeless".
    """
    out = {name.lower(): 0.5 for name in SKILLS}
    wanted = {name.lower() for name in SKILLS}
    values = (rec.get("PlayerSkills") or {}).get("SkillValues") or []
    if isinstance(values, list):
        for entry in values:
            if not isinstance(entry, dict):
                continue
            key = str(entry.get("Key", "")).lower()
            if key in wanted and isinstance(entry.get("Value"), (int, float)):
                out[key] = max(0.0, min(1.0, float(entry["Value"]) / 100.0))
    return out


def npc_level(rec: dict) -> float:
    """Authored competence, normalised.

    Two shapes, and most unique NPCs use the second:

      Level: 12                         a fixed level
      Level: {PcLevelMult, LevelMult}   levels WITH the player, bounded by
                                        CalcMinLevel / CalcMaxLevel

    For the PC-mult form there is no authored level to read, so the midpoint of
    the authored band is used instead. That is the honest reading: those NPCs
    are specified as a RANGE, and the range's centre is what the designer said
    about them relative to everyone else.
    """
    cfg = rec.get("Configuration") or {}
    level = cfg.get("Level")

    if isinstance(level, (int, float)):
        return max(0.0, min(1.0, float(level) / LEVEL_CEILING))

    if isinstance(level, dict):
        if isinstance(level.get("Level"), (int, float)):
            return max(0.0, min(1.0, float(level["Level"]) / LEVEL_CEILING))
        lo = cfg.get("CalcMinLevel")
        hi = cfg.get("CalcMaxLevel")
        if isinstance(lo, (int, float)) and isinstance(hi, (int, float)) and hi >= lo:
            return max(0.0, min(1.0, ((float(lo) + float(hi)) / 2.0) / LEVEL_CEILING))
        if isinstance(lo, (int, float)):
            return max(0.0, min(1.0, float(lo) / LEVEL_CEILING))

    return 0.5


def extract_ties(raw) -> list:
    """Normalise whatever report_degree hands back into {other, sharedFaction}.

    Its edge representation is not part of any contract, so this handles the
    plausible shapes rather than assuming one and silently producing garbage
    ids - which is exactly what an earlier version did, emitting every tie as
    "other": "0".
    """
    if not raw:
        return []
    out = []
    if isinstance(raw, dict):
        for other, payload in raw.items():
            shared = False
            if isinstance(payload, dict):
                shared = bool(payload.get("sharedFaction") or payload.get("faction"))
            elif isinstance(payload, (tuple, list)) and len(payload) > 1:
                shared = bool(payload[1])
            elif isinstance(payload, bool):
                shared = payload
            out.append({"other": str(other), "sharedFaction": shared})
        return out
    for entry in raw:
        if isinstance(entry, (tuple, list)) and entry:
            out.append({"other": str(entry[0]), "sharedFaction": bool(entry[1]) if len(entry) > 1 else False})
        elif isinstance(entry, dict):
            out.append({
                "other": str(entry.get("other") or entry.get("peer") or entry.get("npc")),
                "sharedFaction": bool(entry.get("sharedFaction") or entry.get("faction")),
            })
        else:
            out.append({"other": str(entry), "sharedFaction": False})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", default=DEFAULT_EXPORT)
    ap.add_argument("--out", default=str(HERE / "population.json"))
    ap.add_argument("--faction-min", type=int, default=3)
    ap.add_argument("--faction-max", type=int, default=40)
    args = ap.parse_args()

    bsg = load_gossip_module()

    print("building the social graph (this reads the whole export; give it a minute)...", file=sys.stderr)
    # Same call sequence build-social-graph.py's own main() uses. Its report_*
    # functions are the things that actually derive participants and edges;
    # build() only loads the records.
    graph = bsg.build(args.export)
    parts, _hh_members, _st_members = bsg.report_residence(graph)
    fsize = bsg.report_factions(graph, parts)
    rela_both, _spans = bsg.report_relationships(graph, parts)
    personal, _reach, _hh, _st = bsg.report_degree(graph, parts, fsize, rela_both, args.faction_min, args.faction_max)

    # The admitted-faction set, by the same size filter the runtime reuses.
    # One answer to "which organisations matter", not two that can drift.
    admitted_keys = set(bsg.social_factions(graph, parts, args.faction_min, args.faction_max))
    print(f"{len(admitted_keys)} admitted faction(s)", file=sys.stderr)

    npcs = graph.get("npcs") or {}

    # The shipped roster, applied through the same standing mirror
    # check-plot-factions.py uses -- so the simulation and the shipped file
    # cannot drift apart without the check catching it.
    check = load_by_path(CHECK, "check_plot_factions")
    fac_members, fac_ranks = check.faction_members(args.export)
    roster = {}
    for section in check.parse_ini(ROSTER):
        if section["_id"].startswith("Faction:") and section["_keys"].get("Faction"):
            roster[section["_keys"]["Faction"][0]] = section
    print(f"roster: {len(roster)} faction(s)", file=sys.stderr)

    # faction EditorID -> the graph's key, so rostered entries can be matched
    # against the gossip-derived ones.
    fac_key = {}
    for key in (graph.get("factions") or {}):
        rec_f = graph["factions"][key]
        eid = rec_f.get("EditorID")
        if eid:
            fac_key[eid] = key

    members = []
    for key, part in parts.items():
        rec = npcs.get(key) or {}

        # TWO sources, exactly as PlotPopulation::Build composes them.
        #
        # Gossip's size-filtered set is the FALLBACK: membership says
        # "belongs to an organisation" and nothing about rank, so standing is
        # 0 and its members are peers.
        #
        # The roster is the source of truth for hierarchy, and it may name
        # factions the size filter never admitted -- the Imperial Legion has
        # 288 members and the filter stops at 40 -- so rostered entries are
        # added outright rather than only annotating existing ones.
        factions = []
        seen = set()
        for entry in rec.get("Factions") or []:
            raw_faction = entry.get("Faction")
            if raw_faction not in admitted_keys:
                continue
            factions.append({"faction": str(raw_faction), "standing": 0.0, "rostered": False})
            seen.add(str(raw_faction))

        npc_editor_id = rec.get("EditorID") or ""
        for fac_editor_id, section in roster.items():
            if npc_editor_id not in fac_members.get(fac_editor_id, set()):
                continue
            standing = check.standing_of(npc_editor_id, section, fac_members, fac_ranks)
            key = fac_key.get(fac_editor_id)
            existing = next((f for f in factions if f["faction"] == str(key)), None)
            if existing is not None:
                existing["standing"] = standing
                existing["rostered"] = True
            else:
                factions.append({"faction": str(key), "standing": standing, "rostered": True})

        ties = extract_ties(personal.get(key))

        members.append({
            "id": str(key),
            "name": part.get("name") or part.get("editorID") or str(key),
            "hold": str(part.get("hold")) if part.get("hold") else None,
            "competence": npc_level(rec),
            "skills": npc_skills(rec),
            "factions": factions,
            "ties": ties,
        })

    payload = {
        "source": args.export,
        "factionSizeFilter": [args.faction_min, args.faction_max],
        "admittedFactions": len(admitted_keys),
        "members": members,
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=1, sort_keys=True)

    tied = sum(1 for m in members if m["ties"])
    ranked = sum(1 for m in members if any(f["standing"] > 0 for f in m["factions"]))
    rostered = sum(1 for m in members if any(f["rostered"] for f in m["factions"]))
    leaders = sum(1 for m in members if any(f["standing"] >= 0.999 for f in m["factions"]))
    distinct_skill = len({round(m["skills"]["speech"], 3) for m in members})
    distinct_comp = len({round(m["competence"], 3) for m in members})

    print(
        f"wrote {args.out}: {len(members)} members, {tied} with personal ties",
        file=sys.stderr,
    )
    # These three lines exist because each of them caught a silent extraction
    # bug that would have produced a plausible-looking but meaningless
    # simulation.
    print(f"  in a rostered faction: {rostered}; with standing > 0: {ranked}; at the top of one: {leaders}",
          file=sys.stderr)
    print(f"  distinct Speech values: {distinct_skill}", file=sys.stderr)
    print(f"  distinct competence values: {distinct_comp}", file=sys.stderr)
    if distinct_skill <= 1 or distinct_comp <= 1:
        print("  WARNING: a flat distribution means the extractor is not reading what it thinks",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
