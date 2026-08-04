#!/usr/bin/env python3
"""Build and analyse the Phase 13 gossip social graph from the Spriggit export.

Reads the vanilla record tree at C:\\Projects\\spriggit-output (see
docs/VANILLA_RECORD_REFERENCE.md) and reconstructs, offline, the same graph the
runtime would build at kDataLoaded:

  * the BGSLocation parent tree, tier-classified by LocType* keywords
  * NPC -> residence, from each Location's LCUN (UniqueActorReferencesStatic)
  * the BGSRelationship sparse edge list
  * faction co-membership, size-filtered

Then reports the statistics that Phase 13's open questions turn on. This is
analysis tooling, not shipped code -- nothing in the plugin reads its output.

Usage:
    python scripts/build-social-graph.py [--export DIR] [--json OUT.json]
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import statistics
import sys
from collections import Counter, defaultdict

import yaml

try:
    from yaml import CSafeLoader as Loader
except ImportError:  # pragma: no cover - fallback for builds without libyaml
    from yaml import SafeLoader as Loader

# Load order matters: later masters override earlier ones on the same FormKey.
MASTERS = ["Skyrim", "Update", "Dawnguard", "HearthFires", "Dragonborn"]

# Tier keyword sets, by EditorID. See PHASE_13 "The tier tree already exists in
# vanilla data" -- the criterion for household is "a group of people sleep here".
HOUSEHOLD_KW = {
    "LocTypeDwelling", "LocTypeHouse", "LocTypeStewardsDwelling", "LocTypePlayerHouse",
    "BYOH_LocTypeHomestead",
    "LocTypeInn", "LocTypeStore", "LocTypeGuild", "LocTypeTemple", "LocTypeCastle",
    "LocTypeFarm", "LocTypeLumberMill", "LocTypeMine",
    "LocTypeBarracks", "LocTypeMilitaryCamp", "LocTypeMilitaryFort", "LocTypeJail",
    "LocTypeBanditCamp", "LocTypeForswornCamp", "LocTypeVampireLair", "LocTypeWarlockLair",
    "LocTypeOrcStronghold", "LocTypeShip",
}
SETTLEMENT_KW = {
    "LocTypeHabitation", "LocTypeHabitationHasInn", "LocTypeCity", "LocTypeTown",
    "LocTypeSettlement", "LocTypeHoldCapital", "LocTypeOrcStronghold",
}
HOLD_KW = {"LocTypeHold", "LocTypeHoldMajor", "LocTypeHoldMinor"}

RANK_MULT = {
    "Lover": 4.0, "Confidant": 3.0, "Ally": 2.0, "Friend": 2.0, "Acquaintance": 1.2,
    "Rival": 0.4, "Foe": 0.15, "Enemy": 0.05, "Archnemesis": 0.0,
}

# Relative pull of each channel when a carrier picks who to talk to. These are
# WEIGHTS, not rates -- see CONVERSATIONS_PER_DAY. Treating them as per-pair
# rates makes a person's total social activity scale with the size of their
# settlement, which saturates the province in a day.
#
# The household:settlement ratio is what separates the three tiers of the
# design target. At 30:1 a city dweller still spends most conversations on
# neighbours, so settlements saturated as readily as households. At 800:1 a
# household is ~93% of a city dweller's conversations, which is what makes
# household saturation near-certain while leaving cities only partly covered.
TIER_WEIGHT = {"household": 600.0, "settlement": 1.0, "hold": 0.05, "province": 0.0001}
# Personal edges are the middle ring between "people I live with" and "anyone
# in town". At 4 they were too weak to be one; at 40 they measurably fill the
# 13-30 settlement band, which is where the dropoff was harshest.
FACTION_MULT = 40.0

# Distance attenuation on the personal-edge channel. A guild-mate in your own
# settlement is someone you see constantly; one three holds away you see
# rarely, if ever. The channel originally had NO distance term -- deliberate,
# because it is what carries a rumor between holds -- which was harmless while
# the weight was 4 but became the dominant cross-hold leak once it rose to 40.
PERSONAL_DISTANCE = {"settlement": 1.0, "hold": 0.5, "far": 0.06}

# SIR parameters. Transmissibility is CONSTANT for a rumor's whole life and an
# outbreak ends when susceptible contacts run out, not because the rumor "wore
# out".
#
# At scale 1.0 the per-conversation transmission probability IS the rumor's
# notability -- no intermediate constant to reason about. A notability-0.7
# rumor passes on in 70% of the conversations it comes up in.
# A longer infectious period at a LOWER per-conversation probability is what
# makes the household/settlement dropoff less steep. Household saturation only
# needs a few successes out of many attempts, so extra conversations buy it
# cheaply; outward spread is governed by beta, so lowering beta holds the
# settlement in check. That decoupling is what allows the household weight to
# come down from 800 to 600 without losing local saturation.
#
# The cost is that beta is no longer the notability outright -- it is
# notability x 0.7 -- and a carrier now simulates 6 conversations instead of 4.
INFECTIOUS_DAYS = 3.0
TRANSMISSION_SCALE = 0.7

# Share of a person's conversations that goes to each tier, divided among that
# tier's members. Shares -- not per-peer weights.
#
# Per-peer weighting made the outward share collapse as households grew (7.3%
# for a couple in a city, 1.0% for an eight-person household), because the
# household's total pull scaled with its membership. That is backwards: an inn
# or a barracks should be a HUB, not a sink. With shares, outward flow is
# constant regardless of household size, which is what flattens the
# household-to-settlement dropoff.
TIER_SHARE = {"household": 0.62, "personal": 0.14, "settlement": 0.20,
              "hold": 0.03, "province": 0.01}

# A person's finite daily budget of GOSSIP-CAPABLE conversations, split across
# their contacts in proportion to the weights above. This is the term that keeps
# a 90-resident city from turning everyone into a hub.
#
# Outcomes are invariant along `conversations x transmission_scale ~ 2.1`, so
# 6 x 0.35, 3 x 0.70 and 21 x 0.10 all produce the same epidemic. 2 x 1.0 is
# chosen because it simulates the fewest conversations AND removes a tuning
# constant: beta becomes the notability itself.
#
# 2/day is the floor. At 1/day the product falls to 1.0, which is below what
# beta <= 1 can compensate for: large-household saturation collapses to 62%
# and cross-hold jumps stop happening entirely.
CONVERSATIONS_PER_DAY = 2.0

# Faction EditorID fragments that mark a bucket rather than a social group.
# Size filtering alone does not separate these: JobInnkeeperFaction has 29
# members and every one of them is a stranger to the others, while
# DLC2RRSeverinManorFaction has 3 and they share a roof.
FACTION_DENY = (
    # Attribute buckets: everyone with a property, who are strangers to each other.
    "crime", "town", "job", "potential", "current", "favor", "gov", "guard",
    "trainer", "friend", "nopickpocket", "marriage", "follower", "playerfaction",
    "excluded", "vendor", "prisoner", "bard", "witness", "shared", "generic",
    # Quest and scripting scaffolding, which spans holds for reasons that have
    # nothing to do with who talks to whom. Left un-denied, these dominate the
    # cross-hold transmission count and make the faction channel look far more
    # province-spanning than the real organisations do.
    "alias", "partyguest", "housecarl", "carriage", "adoptable", "disallow",
    "castlehide", "additem", "exclusion", "neverfill", "council", "immune",
    "hide", "dialogue", "scene", "patrons",
)


# --------------------------------------------------------------------------
# Loading


# Location subrecord arrays that DLC plugins EXTEND rather than restate. A
# naive later-master-wins merge silently drops the base record's list -- e.g.
# Dawnguard/HearthFires override WindhelmLocation and MarkarthLocation without
# restating LCUN, so the whole city's unique-NPC list disappears. The engine
# does the equivalent union itself at load (BGSLocation::OverrideData holds the
# ACPR/RCPR arrays), so BGSLocation::uniqueNPCs is already merged at runtime;
# only offline analysis has to redo it. Vanilla ships no *Removed counterpart
# for LCUN, so a plain union is exact here.
ADDITIVE_ARRAYS = [
    ("UniqueActorReferencesStatic", "UniqueActorReferencesAdded", None),
    ("PersistentActorReferencesStatic", "PersistentActorReferencesAdded",
     "PersistentActorReferencesRemoved"),
]


def load_folder(export: str, folder: str, additive: bool = False) -> dict:
    """Merge one record folder across all masters, later masters winning.

    When `additive` is set, the Location override arrays above are unioned
    across the load order instead of being replaced.
    """
    out = {}
    for master in MASTERS:
        root = os.path.join(export, master, folder)
        if not os.path.isdir(root):
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                if not fn.endswith((".yaml", ".yml")):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    with open(path, "r", encoding="utf-8-sig") as fh:
                        rec = yaml.load(fh, Loader=Loader)
                except Exception as exc:  # a single malformed record must not abort the run
                    print(f"  ! skipped {path}: {exc}", file=sys.stderr)
                    continue
                if not (isinstance(rec, dict) and rec.get("FormKey")):
                    continue
                key = rec["FormKey"]
                prev = out.get(key)
                if additive and prev:
                    for static, added, removed in ADDITIVE_ARRAYS:
                        merged = list(prev.get(static) or [])
                        seen = {json.dumps(e, sort_keys=True) for e in merged}
                        for e in (rec.get(static) or []) + (rec.get(added) or []):
                            sig = json.dumps(e, sort_keys=True)
                            if sig not in seen:
                                seen.add(sig)
                                merged.append(e)
                        drop = set(rec.get(removed) or []) if removed else set()
                        if drop:
                            merged = [e for e in merged
                                      if (e.get("Ref") if isinstance(e, dict) else e) not in drop]
                        rec[static] = merged
                elif additive:
                    for static, added, _removed in ADDITIVE_ARRAYS:
                        rec[static] = list(rec.get(static) or []) + list(rec.get(added) or [])
                out[key] = rec
    return out


FK_RE = __import__("re").compile(r"([0-9A-Fa-f]{6}:[^\s]+)")


def scan_cells(export: str, cache: str | None = None) -> dict:
    """NPC FormKey -> (cell location, persistent location) from placed references.

    This is the design's residence fallback: the ACHR placements that LCUN does
    not cover. Line-scanned rather than YAML-parsed -- the Cells tree is ~220 MB
    and only three fields per PlacedNpc block are wanted.
    """
    if cache and os.path.exists(cache):
        with open(cache, "r", encoding="utf-8") as fh:
            return {k: tuple(v) for k, v in json.load(fh).items()}

    placed = {}
    files = 0
    for master in MASTERS:
        root = os.path.join(export, master, "Cells")
        if not os.path.isdir(root):
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                if not fn.endswith((".yaml", ".yml")):
                    continue
                files += 1
                cell_loc = None
                in_npc = False
                base = perloc = None

                def flush():
                    if base and base not in placed:
                        placed[base] = (cell_loc, perloc)

                with open(os.path.join(dirpath, fn), "r", encoding="utf-8-sig",
                          errors="replace") as fh:
                    for line in fh:
                        line = line.rstrip("\n").rstrip("\r")
                        if line.startswith("Location: "):
                            m = FK_RE.search(line)
                            cell_loc = m.group(1) if m else None
                        elif line.lstrip().startswith("- MutagenObjectType:"):
                            if in_npc:
                                flush()
                            in_npc = line.strip().endswith("PlacedNpc")
                            base = perloc = None
                        elif in_npc:
                            s = line.strip()
                            if s.startswith("Base:"):
                                m = FK_RE.search(s)
                                base = m.group(1) if m else None
                            elif s.startswith("PersistentLocation:"):
                                m = FK_RE.search(s)
                                perloc = m.group(1) if m else None
                    if in_npc:
                        flush()
    print(f"  scanned {files} cell files -> {len(placed)} placed NPCs", file=sys.stderr)
    if cache:
        with open(cache, "w", encoding="utf-8") as fh:
            json.dump({k: list(v) for k, v in placed.items()}, fh)
    return placed


def display_name(rec: dict) -> str:
    """Spriggit writes Name either as a plain string or a translated block."""
    name = rec.get("Name")
    if isinstance(name, str):
        return name
    if isinstance(name, dict):
        for entry in name.get("Values", []) or []:
            if entry.get("Language") == "English":
                return entry.get("String") or ""
    return ""


def is_unique(npc: dict) -> bool:
    cfg = npc.get("Configuration") or {}
    return "Unique" in (cfg.get("Flags") or [])


def is_person(npc: dict, npc_races: set) -> bool:
    """Unique-flagged is not the same as 'a person who can gossip'.

    Excludes character-creation presets, test actors, and anything whose race
    lacks the ActorTypeNPC keyword (dragons, Alduin, creature uniques).
    """
    eid = (npc.get("EditorID") or "").lower()
    if "preset" in eid or eid.startswith("test") or "dummy" in eid:
        return False
    if not display_name(npc):
        return False
    return npc.get("Race") in npc_races


# --------------------------------------------------------------------------
# Graph construction


def classify(loc: dict, kwname: dict) -> set:
    tiers = set()
    names = {kwname.get(k, "") for k in (loc.get("Keywords") or [])}
    if names & HOUSEHOLD_KW:
        tiers.add("household")
    if names & SETTLEMENT_KW:
        tiers.add("settlement")
    if names & HOLD_KW:
        tiers.add("hold")
    return tiers


def walk_tiers(start, parent, tiers_of):
    """First ancestor (inclusive) satisfying each tier. Cycle-safe."""
    found = {}
    seen = set()
    cur = start
    while cur and cur not in seen:
        seen.add(cur)
        for t in tiers_of.get(cur, ()):
            found.setdefault(t, cur)
        cur = parent.get(cur)
    return found


def build(export: str, cells_cache: str | None = None, use_cells: bool = True):
    print("Loading records...", file=sys.stderr)
    keywords = load_folder(export, "Keywords")
    locations = load_folder(export, "Locations", additive=True)
    npcs = load_folder(export, "Npcs")
    relations = load_folder(export, "Relationships")
    factions = load_folder(export, "Factions")
    races = load_folder(export, "Races")
    print(
        f"  keywords={len(keywords)} locations={len(locations)} npcs={len(npcs)} "
        f"relationships={len(relations)} factions={len(factions)}",
        file=sys.stderr,
    )

    kwname = {k: (v.get("EditorID") or "") for k, v in keywords.items()}
    parent = {k: v.get("ParentLocation") for k, v in locations.items()}
    tiers_of = {k: classify(v, kwname) for k, v in locations.items()}

    # NPC -> editor location, from every Location's LCUN array.
    editor_loc = {}
    lcun_rows = 0
    for lk, loc in locations.items():
        for entry in (loc.get("UniqueActorReferencesStatic") or []):
            actor = entry.get("Actor")
            if not actor:
                continue
            lcun_rows += 1
            # Per-entry Location is the NPC's own editor location and is finer
            # than the location carrying the entry; fall back to the carrier.
            editor_loc.setdefault(actor, entry.get("Location") or lk)

    placed = scan_cells(export, cells_cache) if use_cells else {}

    actortype_npc = next((k for k, v in keywords.items()
                          if (v.get("EditorID") or "") == "ActorTypeNPC"), None)
    npc_races = {k for k, v in races.items()
                 if actortype_npc in (v.get("Keywords") or [])}
    print(f"  races carrying ActorTypeNPC: {len(npc_races)} of {len(races)}", file=sys.stderr)

    all_unique = {k: v for k, v in npcs.items() if is_unique(v)}
    uniques = {k: v for k, v in all_unique.items() if is_person(v, npc_races)}
    print(f"  unique-flagged {len(all_unique)} -> people {len(uniques)} "
          f"(dropped {len(all_unique) - len(uniques)} presets/creatures/unnamed)", file=sys.stderr)

    people = {}
    source_counts = Counter()
    for k, npc in uniques.items():
        # Residence resolution order, per PHASE_13 Part 1.
        cell_loc, per_loc = placed.get(k, (None, None))
        loc, source = None, "none"
        if editor_loc.get(k):
            loc, source = editor_loc[k], "LCUN"
        elif cell_loc:
            loc, source = cell_loc, "placement cell"
        elif per_loc:
            loc, source = per_loc, "persistent location"
        tiers = walk_tiers(loc, parent, tiers_of) if loc else {}
        source_counts[source] += 1
        people[k] = {
            "editorID": npc.get("EditorID") or "",
            "name": display_name(npc),
            "editorLoc": loc,
            "source": source,
            "household": tiers.get("household"),
            "settlement": tiers.get("settlement"),
            "hold": tiers.get("hold"),
            "factions": [f.get("Faction") for f in (npc.get("Factions") or []) if f.get("Faction")],
        }

    # Faction sizes measured over participants only -- a faction's usefulness as
    # a social signal is about how many *graph members* share it.
    fsize = Counter()
    for p in people.values():
        for f in p["factions"]:
            fsize[f] += 1

    rela = []
    for r in relations.values():
        a, b, rank = r.get("Parent"), r.get("Child"), r.get("Rank")
        if a and b:
            rela.append((a, b, rank or "Ally", r.get("AssociationType")))

    return dict(
        locations=locations, kwname=kwname, parent=parent, tiers_of=tiers_of,
        npcs=npcs, uniques=uniques, people=people, editor_loc=editor_loc,
        lcun_rows=lcun_rows, factions=factions, fsize=fsize, rela=rela,
        placed=placed, source_counts=source_counts,
    )


# --------------------------------------------------------------------------
# Reporting helpers


def hist(values, buckets):
    counts = Counter()
    for v in values:
        for lo, hi, label in buckets:
            if lo <= v <= hi:
                counts[label] += 1
                break
    return [(label, counts[label]) for _, _, label in buckets]


def bar(n, total, width=34):
    if total <= 0:
        return ""
    return "#" * max(0, int(round(width * n / total)))


def pct(n, d):
    return f"{100.0 * n / d:.1f}%" if d else "n/a"


def name_of(g, key):
    rec = g["locations"].get(key)
    if rec:
        return rec.get("EditorID") or display_name(rec) or str(key)
    return str(key)


def is_bucket(g, faction_key) -> bool:
    """True if this faction is an attribute bucket rather than a social group."""
    rec = g["factions"].get(faction_key)
    eid = ((rec.get("EditorID") if rec else None) or "").lower()
    return any(frag in eid for frag in FACTION_DENY)


def social_factions(g, parts, lo, hi):
    """faction -> members, restricted to plausible social groups."""
    size = Counter()
    for p in parts.values():
        for f in p["factions"]:
            size[f] += 1
    out = defaultdict(list)
    for k, p in parts.items():
        for f in p["factions"]:
            if lo <= size[f] <= hi and not is_bucket(g, f):
                out[f].append(k)
    return out


# --------------------------------------------------------------------------
# Analyses


def report_residence(g):
    print("\n" + "=" * 78)
    print("Q3 / Q4 -- RESIDENCE COVERAGE AND HOUSEHOLD SIZE")
    print("=" * 78)

    people = g["people"]
    total = len(people)
    with_lcun = sum(1 for p in people.values() if p["editorLoc"])
    hh = sum(1 for p in people.values() if p["household"])
    st = sum(1 for p in people.values() if p["settlement"])
    ho = sum(1 for p in people.values() if p["hold"])
    orphan = [p for p in people.values() if not p["hold"]]

    print(f"\nUnique-flagged NPCs, all masters : {total}")
    print(f"  resolved to SOME location      : {with_lcun:5d}  ({pct(with_lcun, total)})")
    print(f"  resolving to a household node  : {hh:5d}  ({pct(hh, total)})")
    print(f"  resolving to a settlement node : {st:5d}  ({pct(st, total)})")
    print(f"  resolving to a hold node       : {ho:5d}  ({pct(ho, total)})")
    print(f"  no tier at all (excluded)      : {len(orphan):5d}  ({pct(len(orphan), total)})")
    print(f"\nLCUN rows after additive merge: {g['lcun_rows']} across "
          f"{sum(1 for l in g['locations'].values() if l.get('UniqueActorReferencesStatic'))} locations")

    print("\nWhich fallback tier resolved each NPC:")
    for src in ("LCUN", "placement cell", "persistent location", "none"):
        n = g["source_counts"][src]
        print(f"  {src:<22} {n:5d}  ({pct(n, total):>5})  {bar(n, total)}")

    # Yield per tier: of the NPCs a tier resolved, how many reached a household?
    print("\nTier quality (did the resolved location classify?):")
    for src in ("LCUN", "placement cell", "persistent location"):
        grp = [p for p in people.values() if p["source"] == src]
        if not grp:
            continue
        gh = sum(1 for p in grp if p["household"])
        gt = sum(1 for p in grp if p["hold"])
        print(f"  {src:<22} n={len(grp):5d}  -> household {pct(gh, len(grp)):>6}  "
              f"-> any tier {pct(gt, len(grp)):>6}")

    # Participants = anyone with at least a hold. This is the population the
    # simulation actually runs over.
    parts = {k: p for k, p in people.items() if p["hold"]}
    print(f"\nPARTICIPANT POPULATION: {len(parts)}")

    tiered = Counter()
    for p in parts.values():
        if p["household"]:
            tiered["household+"] += 1
        elif p["settlement"]:
            tiered["settlement only"] += 1
        else:
            tiered["hold only"] += 1
    for k in ("household+", "settlement only", "hold only"):
        print(f"  {k:<16} {tiered[k]:5d}  {bar(tiered[k], len(parts))}")

    # Household sizes.
    hh_members = defaultdict(list)
    for k, p in parts.items():
        if p["household"]:
            hh_members[p["household"]].append(k)
    sizes = sorted((len(v) for v in hh_members.values()), reverse=True)
    print(f"\nHOUSEHOLDS: {len(hh_members)} distinct, "
          f"median size {statistics.median(sizes):.0f}, mean {statistics.mean(sizes):.1f}, max {sizes[0]}")
    for label, n in hist(sizes, [(1, 1, "1 person"), (2, 3, "2-3"), (4, 6, "4-6"),
                                 (7, 12, "7-12"), (13, 25, "13-25"), (26, 10 ** 6, "26+")]):
        print(f"  {label:<10} {n:4d}  {bar(n, len(hh_members))}")
    print("\n  largest households:")
    for loc, members in sorted(hh_members.items(), key=lambda kv: -len(kv[1]))[:10]:
        print(f"    {len(members):4d}  {name_of(g, loc)}")

    # Settlement sizes.
    st_members = defaultdict(list)
    for k, p in parts.items():
        if p["settlement"]:
            st_members[p["settlement"]].append(k)
    ssizes = sorted((len(v) for v in st_members.values()), reverse=True)
    print(f"\nSETTLEMENTS: {len(st_members)} distinct, median size "
          f"{statistics.median(ssizes):.0f}, mean {statistics.mean(ssizes):.1f}, max {ssizes[0]}")
    for label, n in hist(ssizes, [(1, 2, "1-2"), (3, 5, "3-5"), (6, 10, "6-10"),
                                  (11, 25, "11-25"), (26, 50, "26-50"), (51, 10 ** 6, "51+")]):
        print(f"  {label:<10} {n:4d}  {bar(n, len(st_members))}")
    print("\n  largest settlements:")
    for loc, members in sorted(st_members.items(), key=lambda kv: -len(kv[1]))[:10]:
        print(f"    {len(members):4d}  {name_of(g, loc)}")

    print("\n  HOLDS:")
    ho_members = defaultdict(list)
    for k, p in parts.items():
        ho_members[p["hold"]].append(k)
    for loc, members in sorted(ho_members.items(), key=lambda kv: -len(kv[1])):
        print(f"    {len(members):4d}  {name_of(g, loc)}")

    # What did we lose?
    print(f"\n  SAMPLE OF EXCLUDED (no tier), {len(orphan)} total:")
    for p in orphan[:15]:
        print(f"    {p['editorID'][:38]:<38} loc={name_of(g, p['editorLoc']) if p['editorLoc'] else '(none)'}")

    return parts, hh_members, st_members


def report_factions(g, parts):
    print("\n" + "=" * 78)
    print("Q1 -- FACTION SIZE BAND")
    print("=" * 78)

    fsize = Counter()
    for p in parts.values():
        for f in p["factions"]:
            fsize[f] += 1

    sizes = sorted(fsize.values(), reverse=True)
    print(f"\nFactions with >=1 participant member: {len(fsize)}")
    print(f"  median {statistics.median(sizes):.0f}, mean {statistics.mean(sizes):.1f}, max {sizes[0]}")
    print("\nSize distribution:")
    for label, n in hist(sizes, [(1, 1, "1"), (2, 2, "2"), (3, 5, "3-5"), (6, 10, "6-10"),
                                 (11, 20, "11-20"), (21, 40, "21-40"), (41, 100, "41-100"),
                                 (101, 10 ** 6, "101+")]):
        print(f"  {label:<8} {n:5d}  {bar(n, len(fsize))}")

    print("\nLargest factions (the buckets a size filter must exclude):")
    for f, n in fsize.most_common(18):
        rec = g["factions"].get(f)
        eid = (rec.get("EditorID") if rec else None) or str(f)
        print(f"  {n:5d}  {eid}")

    print("\nA sample of the 3-40 band that SIZE ALONE would keep:")
    band = [(f, n) for f, n in fsize.items() if 3 <= n <= 40]
    band.sort(key=lambda kv: -kv[1])
    for f, n in band[:14]:
        rec = g["factions"].get(f)
        eid = (rec.get("EditorID") if rec else None) or str(f)
        flag = "  <- BUCKET" if is_bucket(g, f) else ""
        print(f"  {n:5d}  {eid}{flag}")

    print("\nSize band vs. size band + name denylist:")
    for lo, hi in [(2, 20), (3, 40), (3, 60), (2, 10 ** 6)]:
        for deny in (False, True):
            kept, pairs = [], set()
            for f, n in fsize.items():
                if not (lo <= n <= hi):
                    continue
                if deny and is_bucket(g, f):
                    continue
                kept.append(f)
                members = [k for k, p in parts.items() if f in p["factions"]]
                for i in range(len(members)):
                    for j in range(i + 1, len(members)):
                        pairs.add((min(members[i], members[j]), max(members[i], members[j])))
            tag = "size+deny" if deny else "size only"
            hs = "inf" if hi > 10 ** 5 else str(hi)
            print(f"  {lo}-{hs:<4} {tag:<10} factions {len(kept):5d}   pairs {len(pairs):7d}")

    print("\nWhat survives size 3-40 PLUS the denylist (a real social-group sample):")
    survivors = sorted(((f, n) for f, n in fsize.items()
                        if 3 <= n <= 40 and not is_bucket(g, f)), key=lambda kv: -kv[1])
    for f, n in survivors[:16]:
        rec = g["factions"].get(f)
        print(f"  {n:5d}  {(rec.get('EditorID') if rec else None) or str(f)}")
    print(f"  ... {len(survivors)} total")

    # How far does a faction reach? This is the statistic that decides whether
    # the faction channel is a local or a province-spanning mechanism.
    print("\nGeographic span of surviving social factions (size 3-40 + denylist):")
    fac = social_factions(g, parts, 3, 40)
    span = Counter()
    multi = []
    for f, members in fac.items():
        holds = {parts[m]["hold"] for m in members}
        span[min(len(holds), 5)] += 1
        if len(holds) > 1:
            multi.append((len(holds), len(members), f))
    for k in sorted(span):
        lbl = f"{k}{'+' if k == 5 else ''} hold(s)"
        print(f"  {lbl:<12} {span[k]:4d} factions  {bar(span[k], len(fac))}")
    cross = sum(v for k, v in span.items() if k > 1)
    print(f"  -> {cross} of {len(fac)} ({pct(cross, len(fac))}) span more than one hold")

    print("\n  widest-spanning social factions (the real long-range carriers):")
    for holds, n, f in sorted(multi, reverse=True)[:12]:
        rec = g["factions"].get(f)
        print(f"    {holds} holds, {n:3d} members  {(rec.get('EditorID') if rec else None) or str(f)}")

    return fsize


def report_relationships(g, parts):
    print("\n" + "=" * 78)
    print("RELATIONSHIP EDGES")
    print("=" * 78)

    ranks = Counter(r[2] for r in g["rela"])
    print(f"\nRecords: {len(g['rela'])}")
    for rank, n in ranks.most_common():
        print(f"  {rank:<14} {n:5d}  {bar(n, len(g['rela']))}")

    both = [r for r in g["rela"] if r[0] in parts and r[1] in parts]
    print(f"\nEdges where BOTH ends are participants: {len(both)}  ({pct(len(both), len(g['rela']))})")

    # Do relationship edges actually reach across holds?
    spans = Counter()
    for a, b, _rank, _assoc in both:
        pa, pb = parts[a], parts[b]
        if pa["household"] and pa["household"] == pb["household"]:
            spans["same household"] += 1
        elif pa["settlement"] and pa["settlement"] == pb["settlement"]:
            spans["same settlement"] += 1
        elif pa["hold"] == pb["hold"]:
            spans["same hold"] += 1
        else:
            spans["CROSS-HOLD"] += 1
    print("\nGeographic span of relationship edges:")
    for k in ("same household", "same settlement", "same hold", "CROSS-HOLD"):
        print(f"  {k:<16} {spans[k]:5d}  {bar(spans[k], max(1, len(both)))}")

    withrel = len({x for r in both for x in (r[0], r[1])})
    print(f"\nParticipants with >=1 relationship edge: {withrel}  ({pct(withrel, len(parts))})")
    return both, spans


def report_degree(g, parts, fsize, rela_both, fac_lo=3, fac_hi=40):
    print("\n" + "=" * 78)
    print("Q2 -- DEGREE DISTRIBUTION AND HUBS")
    print("=" * 78)

    fac_members = social_factions(g, parts, fac_lo, fac_hi)

    # The "personal edge list" the relationship channel samples from.
    personal = defaultdict(set)
    for a, b, _r, _a2 in rela_both:
        personal[a].add(b)
        personal[b].add(a)
    for members in fac_members.values():
        for i in range(len(members)):
            for j in range(i + 1, len(members)):
                personal[members[i]].add(members[j])
                personal[members[j]].add(members[i])

    deg = [len(personal.get(k, ())) for k in parts]
    nz = [d for d in deg if d]
    print(f"\nPersonal-edge degree (relationship + faction band {fac_lo}-{fac_hi}):")
    print(f"  participants {len(deg)}, with >=1 edge {len(nz)} ({pct(len(nz), len(deg))})")
    print(f"  mean {statistics.mean(deg):.1f}, median {statistics.median(deg):.0f}, max {max(deg)}")
    for label, n in hist(deg, [(0, 0, "0"), (1, 2, "1-2"), (3, 5, "3-5"), (6, 10, "6-10"),
                               (11, 20, "11-20"), (21, 40, "21-40"), (41, 10 ** 6, "41+")]):
        print(f"  {label:<8} {n:5d}  {bar(n, len(deg))}")

    srt = sorted(deg, reverse=True)
    tot = sum(srt) or 1
    for frac in (0.01, 0.05, 0.10, 0.20):
        cut = max(1, int(len(srt) * frac))
        print(f"  top {frac * 100:4.0f}% of nodes hold {pct(sum(srt[:cut]), tot)} of all edge endpoints")

    # Full contact-weight degree, including the proximity channel.
    hh_members = defaultdict(list)
    st_members = defaultdict(list)
    for k, p in parts.items():
        if p["household"]:
            hh_members[p["household"]].append(k)
        if p["settlement"]:
            st_members[p["settlement"]].append(k)

    # Reach: how many distinct people is a carrier able to talk to at all?
    reach = {}
    for k, p in parts.items():
        peers = set(personal.get(k, ()))
        if p["household"]:
            peers |= set(hh_members[p["household"]])
        if p["settlement"]:
            peers |= set(st_members[p["settlement"]])
        peers.discard(k)
        reach[k] = len(peers)

    vals = sorted(reach.values(), reverse=True)
    tot = sum(vals) or 1
    print("\nDistinct reachable peers per participant (household + settlement + personal):")
    print(f"  mean {statistics.mean(vals):.1f}, median {statistics.median(vals):.0f}, max {vals[0]}")
    for frac in (0.01, 0.05, 0.10, 0.20, 0.50):
        cut = max(1, int(len(vals) * frac))
        print(f"  top {frac * 100:4.0f}% of nodes hold {pct(sum(vals[:cut]), tot)} of all reachable-peer slots")
    srt = sorted(reach.values())
    gini = sum((2 * i - len(srt) + 1) * v for i, v in enumerate(srt)) / (len(srt) * tot)
    print(f"  Gini coefficient: {gini:.3f}   (0 = uniform, 1 = one node holds everything)")

    print("\n  widest-reach participants (the natural hubs):")
    for k, w in sorted(reach.items(), key=lambda kv: -kv[1])[:12]:
        p = parts[k]
        print(f"    {w:5d}  {(p['name'] or p['editorID'])[:26]:<26} "
              f"{name_of(g, p['household'])[:34] if p['household'] else '(no household)'}")

    return personal, reach, hh_members, st_members


def simulate(g, parts, personal, hh_members, st_members, tempo=1.0, trials=60, seed=7,
             infectious_days=INFECTIOUS_DAYS, transmission_scale=TRANSMISSION_SCALE, notability=1.0,
             province_w=None, dt=0.25, hh_weight=None, conversations=None,
             personal_w=None, use_shares=False, personal_far=None):
    # use_shares selects the REJECTED tier-share contact model (iteration 7).
    # It defaults off: leaving it on made every direct caller of simulate()
    # silently run a model that reaches 687 of 857 participants instead of 11.
    """SIR propagation over the social graph.

    Susceptible -> Infectious (for a fixed period) -> Recovered (immune
    forever). Transmissibility is CONSTANT: a rumor does not become less
    catching because it has changed hands. What ends an outbreak is the
    exhaustion of susceptible contacts, exactly as in a real epidemic.

    Per time step an infectious carrier holds Poisson(conversations * dt)
    conversations. Each partner is drawn in proportion to that pair's
    contact weight, so a housemate comes up far more often than any one
    settlement neighbour does. Every conversation with a susceptible
    partner transmits with probability `notability * transmission_scale`;
    conversations with the already-infected or the recovered are the
    wasted opportunities that bring the outbreak to a halt.
    """
    import bisect

    hh_w = TIER_WEIGHT["household"] if hh_weight is None else hh_weight
    conv = CONVERSATIONS_PER_DAY if conversations is None else conversations
    pers_w = TIER_WEIGHT["settlement"] * FACTION_MULT if personal_w is None else personal_w
    personal_far = PERSONAL_DISTANCE["far"] if personal_far is None else personal_far
    if province_w is None:
        province_w = TIER_WEIGHT["province"]

    rng = random.Random(seed)
    rela_mult = {}
    for a, b, rank, _assoc in g["rela"]:
        rela_mult[(a, b)] = rela_mult[(b, a)] = RANK_MULT.get(rank, 1.0)

    by_hold = defaultdict(list)
    for k, p in parts.items():
        by_hold[p["hold"]].append(k)
    all_keys = list(parts)

    cache = {}

    def contacts(k):
        # (peers, cumulative weights, total). Sampling is a bisect.
        if k in cache:
            return cache[k]
        p = parts[k]
        w = {}

        def add(peer, weight):
            if peer != k and weight > 0:
                w[peer] = w.get(peer, 0.0) + weight

        if use_shares:
            # Each tier gets a fixed slice of attention, split among its
            # members. A tier the person does not have is simply absent and
            # its share is renormalised away below.
            groups = []
            if p["household"]:
                groups.append(("household", [o for o in hh_members[p["household"]] if o != k]))
            if p["settlement"]:
                groups.append(("settlement", [o for o in st_members[p["settlement"]] if o != k]))
            if p["hold"]:
                groups.append(("hold", [o for o in by_hold[p["hold"]] if o != k]))
            groups.append(("personal", [o for o in personal.get(k, ()) if o != k]))
            live = [(t, m) for t, m in groups if m]
            denom = sum(TIER_SHARE[t] for t, _ in live) + TIER_SHARE["province"]
            for tier, members in live:
                per = (TIER_SHARE[tier] / denom) / len(members)
                for o in members:
                    add(o, per)
            prov_share = TIER_SHARE["province"] / denom
        else:
            if p["household"]:
                for o in hh_members[p["household"]]:
                    add(o, hh_w)
            if p["settlement"]:
                for o in st_members[p["settlement"]]:
                    add(o, TIER_WEIGHT["settlement"])
            if p["hold"]:
                for o in by_hold[p["hold"]]:
                    add(o, TIER_WEIGHT["hold"])
            for o in personal.get(k, ()):
                q = parts.get(o)
                if q and p["settlement"] and q["settlement"] == p["settlement"]:
                    d = PERSONAL_DISTANCE["settlement"]
                elif q and q["hold"] == p["hold"]:
                    d = PERSONAL_DISTANCE["hold"]
                else:
                    d = personal_far
                add(o, pers_w * d)
            prov_share = None
        peers = list(w)
        weights = [w[o] for o in peers]
        peers.append("__province__")
        weights.append(prov_share * sum(weights) / max(1e-9, 1.0 - prov_share)
                       if prov_share is not None else province_w * len(all_keys))
        cum, run = [], 0.0
        for x in weights:
            run += x
            cum.append(run)
        cache[k] = (peers, cum, run)
        return cache[k]

    def poisson(lam):
        # Knuth. lam is small here (a fraction of a conversation per step).
        acc = math.exp(-lam)
        u = rng.random()
        cum_p = acc
        n = 0
        while u > cum_p and n < 60:
            n += 1
            acc *= lam / n
            cum_p += acc
        return n

    results = []
    for _t in range(trials):
        origin = rng.choice(all_keys)
        beta = min(1.0, notability * transmission_scale)
        ever = {origin}
        infectious = {origin: infectious_days}
        gen = {origin: 0}
        when = {origin: 0.0}
        day = 0.0
        transmissions = 0
        wasted = 0
        peak_day = 0.0
        lam = conv * tempo * dt
        while infectious and day < 400.0:
            day += dt
            newly = {}
            for k in list(infectious):
                infectious[k] -= dt
                if infectious[k] <= 0:
                    del infectious[k]
                    continue
                peers, cum, total = contacts(k)
                if total <= 0:
                    continue
                for _ in range(poisson(lam)):
                    o = peers[bisect.bisect_left(cum, rng.uniform(0, total))]
                    if o == "__province__":
                        o = rng.choice(all_keys)
                    if o in ever or o == k:
                        wasted += 1
                        continue
                    if rng.random() < beta:
                        ever.add(o)
                        newly[o] = infectious_days
                        gen[o] = gen[k] + 1
                        when[o] = day
                        transmissions += 1
                        peak_day = day
            infectious.update(newly)
            if len(ever) > 1200:
                break

        p0 = parts[origin]
        holds = {parts[k]["hold"] for k in ever if parts[k]["hold"]}
        setts = {parts[k]["settlement"] for k in ever if parts[k]["settlement"]}
        results.append(dict(
            seed=origin, reached=len(ever), days=peak_day, transmissions=transmissions,
            wasted=wasted, depth=max(gen.values()), holds=len(holds), settlements=len(setts),
            ever=ever, when=when, total_days=day,
            hh_size=len(hh_members[p0["household"]]) if p0["household"] else 0,
            st_size=len(st_members[p0["settlement"]]) if p0["settlement"] else 0,
        ))
    return results


def coverage_report(results, parts, hh_members, st_members, label=""):
    """Per-tier saturation — the terms the design target is stated in."""
    by_hold = defaultdict(list)
    for k, p in parts.items():
        by_hold[p["hold"]].append(k)

    tiers = {"household": hh_members, "settlement": st_members, "hold": by_hold}
    cov = {t: [] for t in tiers}
    full = {t: 0 for t in tiers}
    seen = {t: 0 for t in tiers}
    # Coverage of the ORIGIN's own units, which is what "did it saturate
    # where it started" actually means.
    home = {t: [] for t in tiers}
    for r in results:
        for tier, members in tiers.items():
            units = Counter()
            for k in r["ever"]:
                u = parts[k][tier]
                if u:
                    units[u] += 1
            oh = parts[r["seed"]][tier]
            for u, n in units.items():
                total = len(members[u])
                if total < 2:
                    continue
                frac = n / total
                cov[tier].append(frac)
                seen[tier] += 1
                if n == total:
                    full[tier] += 1
                if u == oh:
                    home[tier].append(frac)

    # Coverage of units the rumor reached in a DIFFERENT hold from where it
    # started -- the design target that matters most. A rumor landing in a
    # fresh population should spread there at full strength, because
    # transmissibility never decayed on the way.
    fresh_hh, fresh_st, jumps = [], [], 0
    for r in results:
        oh = parts[r["seed"]]["hold"]
        away = [k for k in r["ever"] if parts[k]["hold"] != oh]
        if not away:
            continue
        jumps += 1
        for tier, members, sink in (("household", hh_members, fresh_hh),
                                    ("settlement", st_members, fresh_st)):
            units = Counter()
            for k in away:
                u = parts[k][tier]
                if u:
                    units[u] += 1
            for u, n in units.items():
                if len(members[u]) >= 2:
                    sink.append(n / len(members[u]))

    R = sorted(r["reached"] for r in results)
    D = sorted(r["days"] for r in results)
    print("\n--- " + label + " ---")
    print("  reach   median {:5.0f}  mean {:6.1f}  p90 {:5.0f}  max {:5.0f}".format(
        statistics.median(R), statistics.mean(R), R[int(len(R) * 0.9)], R[-1]))
    print("  days    median {:5.1f}  p90 {:5.1f}  max {:5.1f}".format(
        statistics.median(D), D[int(len(D) * 0.9)], D[-1]))
    print("  depth   median {:.0f}   wasted-per-transmission {:.1f}".format(
        statistics.median([r["depth"] for r in results]),
        sum(r["wasted"] for r in results) / max(1, sum(r["transmissions"] for r in results))))
    hc = Counter(r["holds"] for r in results)
    print("  holds   " + ", ".join("{}:{}".format(k, v) for k, v in sorted(hc.items()))
          + "   (left origin hold {:.0f}%)".format(
              100 * sum(v for k, v in hc.items() if k > 1) / len(results)))
    for tier in ("household", "settlement", "hold"):
        if not cov[tier]:
            continue
        hm = home[tier]
        print("  {:<11} home-unit {:5.1f}%   any-unit {:5.1f}%   fully saturated {:4d}/{:<5d} ({:4.1f}%)".format(
            tier,
            100 * statistics.mean(hm) if hm else 0.0,
            100 * statistics.mean(cov[tier]),
            full[tier], seen[tier], 100 * full[tier] / max(1, seen[tier])))

    # Size-banded coverage. A blended settlement average is misleading: most
    # settlements have two residents and saturate trivially, which drowns out
    # what happens in the cities the design target is really about.
    for tier, members in (("household", hh_members), ("settlement", st_members)):
        rows = []
        for lo, hi, lab in ((2, 2, "2"), (3, 5, "3-5"), (6, 12, "6-12"),
                            (13, 30, "13-30"), (31, 10 ** 6, "31+")):
            vals, fulls = [], 0
            for r in results:
                units = Counter()
                for k in r["ever"]:
                    u = parts[k][tier]
                    if u:
                        units[u] += 1
                for u, n in units.items():
                    total = len(members[u])
                    if total >= 2 and lo <= total <= hi:
                        vals.append(n / total)
                        if n == total:
                            fulls += 1
            if vals:
                rows.append("{}: {:.0f}% ({}/{} full)".format(
                    lab, 100 * statistics.mean(vals), fulls, len(vals)))
        if rows:
            print("  {:<11} by size -> ".format(tier) + "  ".join(rows))

    if jumps:
        print("  cross-hold  {} of {} rumors jumped; in the NEW hold they covered "
              "households {:.0f}%, settlements {:.0f}%".format(
                  jumps, len(results),
                  100 * statistics.mean(fresh_hh) if fresh_hh else 0.0,
                  100 * statistics.mean(fresh_st) if fresh_st else 0.0))

    return dict(
        reach=statistics.median(R), days=statistics.median(D),
        hh_home=100 * statistics.mean(home["household"]) if home["household"] else 0.0,
        st_home=100 * statistics.mean(home["settlement"]) if home["settlement"] else 0.0,
        ho_home=100 * statistics.mean(home["hold"]) if home["hold"] else 0.0,
        hhfull=100 * full["household"] / max(1, seen["household"]),
        stfull=100 * full["settlement"] / max(1, seen["settlement"]),
        hofull=100 * full["hold"] / max(1, seen["hold"]),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", default=r"C:\Projects\spriggit-output")
    ap.add_argument("--json", default=None, help="write the resolved graph to this path")
    ap.add_argument("--tempo", type=float, default=1.0)
    ap.add_argument("--trials", type=int, default=60)
    ap.add_argument("--faction-band", default="3,40")
    ap.add_argument("--cells-cache", default=None, help="cache the Cells scan here")
    ap.add_argument("--no-cells", action="store_true", help="skip the placement fallback scan")
    ap.add_argument("--infectious-days", type=float, default=INFECTIOUS_DAYS)
    ap.add_argument("--transmission-scale", type=float, default=TRANSMISSION_SCALE)
    ap.add_argument("--notability", type=float, default=1.0)
    ap.add_argument("--hh-weight", type=float, default=None)
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--sweep-days", default="1,2,3")
    ap.add_argument("--sweep-scale", default="0.1,0.2,0.3")
    ap.add_argument("--sweep-hh", default="30,120")
    ap.add_argument("--sweep-conv", default=None)
    ap.add_argument("--sweep-personal", default="4")
    ap.add_argument("--personal-weight", type=float, default=None)
    ap.add_argument("--shares", action="store_true")
    ap.add_argument("--personal-far", type=float, default=None)
    ap.add_argument("--conversations", type=float, default=CONVERSATIONS_PER_DAY)
    ap.add_argument("--province-weight", type=float, default=None)
    ap.add_argument("--quiet-graph", action="store_true", help="simulation section only")
    args = ap.parse_args()

    lo, hi = (int(x) for x in args.faction_band.split(","))
    g = build(args.export, cells_cache=args.cells_cache, use_cells=not args.no_cells)
    parts, hh_members, st_members = report_residence(g)
    fsize = report_factions(g, parts)
    rela_both, _spans = report_relationships(g, parts)
    personal, reach, hh_m, st_m = report_degree(g, parts, fsize, rela_both, lo, hi)

    if args.sweep:
        print("\n" + "=" * 78)
        print("SIR PARAMETER SWEEP")
        print("=" * 78)
        days = [float(x) for x in args.sweep_days.split(",")]
        scales = [float(x) for x in args.sweep_scale.split(",")]
        hws = [float(x) for x in args.sweep_hh.split(",")]
        convs = [float(x) for x in args.sweep_conv.split(",")] if args.sweep_conv else [args.conversations]
        for D in days:
            for ts in scales:
                for hw in hws:
                    for cv in convs:
                        for pw in [float(x) for x in args.sweep_personal.split(",")]:
                            res = simulate(g, parts, personal, hh_m, st_m, trials=args.trials,
                                           infectious_days=D, transmission_scale=ts, hh_weight=hw,
                                           conversations=cv, personal_w=pw,
                                           province_w=args.province_weight,
                                           notability=args.notability, use_shares=args.shares,
                                           personal_far=args.personal_far)
                            coverage_report(res, parts, hh_m, st_m,
                                            f"conv={cv} beta={ts} inf={D}d hh={hw} personal={pw}")
        return 0

    res = simulate(g, parts, personal, hh_m, st_m, trials=args.trials,
                   infectious_days=args.infectious_days,
                   transmission_scale=args.transmission_scale,
                   notability=args.notability,
                   province_w=args.province_weight,
                   hh_weight=args.hh_weight, conversations=args.conversations,
                   personal_w=args.personal_weight, use_shares=args.shares,
                   personal_far=args.personal_far)
    coverage_report(res, parts, hh_m, st_m,
                    f"infectious={args.infectious_days}d  scale={args.transmission_scale}  "
                    f"notability={args.notability}  hhWeight={args.hh_weight or TIER_WEIGHT['household']}")

    if args.json:
        payload = {
            "participants": {
                str(k): {
                    "editorID": p["editorID"], "name": p["name"],
                    "household": name_of(g, p["household"]) if p["household"] else None,
                    "settlement": name_of(g, p["settlement"]) if p["settlement"] else None,
                    "hold": name_of(g, p["hold"]) if p["hold"] else None,
                    "personalEdges": len(personal.get(k, ())),
                    "reachablePeers": reach.get(k, 0),
                }
                for k, p in parts.items()
            }
        }
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=1, sort_keys=True)
        print(f"\nWrote {args.json} ({len(payload['participants'])} participants)", file=sys.stderr)


if __name__ == "__main__":
    main()
