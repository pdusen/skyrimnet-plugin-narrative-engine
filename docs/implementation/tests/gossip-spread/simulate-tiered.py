#!/usr/bin/env python3
"""Offline simulation of the Phase 13 Milestone 5 tiered contact model.

Builds the same social graph the runtime builds (reusing build-social-graph.py)
and then runs the MILESTONE 5 model over it, rather than the shipped flat-weight
one:

  * a conversation draws a TIER first, then a peer uniformly within it
  * factions and relationships move a peer one tier closer or further,
    composed and clamped per PHASE_13_MILESTONE_5.md
  * a drawn tier with no available members produces no conversation

Reports spread statistics over a month of in-world time and sweeps the tier
weights and transmission scale so the shipped defaults can be chosen from
measurements rather than guessed.

This is analysis tooling. Nothing in the plugin reads it.

Usage:
    python simulate-tiered.py [--export DIR] [--trials N] [--quick]
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import random
import statistics
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))


def load_builder():
    """Import build-social-graph.py despite the hyphen in its name."""
    path = os.path.join(HERE, "build-social-graph.py")
    spec = importlib.util.spec_from_file_location("gossip_graph_builder", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# Milestone 5 model constants

# Seven rungs, 0-indexed. The odd rungs (1,3,5,7 in the doc's numbering) are
# the geographic tiers; the even ones between them have NO natural membership
# and exist only to receive peers moved by a faction or relationship.
HOUSEHOLD, SETTLEMENT, HOLD, PROVINCE = 0, 2, 4, 6
NTIERS = 7
TIER_NAMES = ["t1-household", "t2", "t3-settlement", "t4", "t5-hold", "t6", "t7-province"]

# Rank -> tier movement. BY NAME. RELATIONSHIP_LEVEL counts upward as warmth
# decreases in CommonLibSSE (kLover = 0 ... kArchnemesis = 8), so any
# comparison against the enum selects the wrong set. Spriggit exports the
# name, which is why this table is keyed on it.
RANK_DELTA = {
    "Lover": -1, "Ally": -1, "Confidant": -1,
    "Friend": 0, "Acquaintance": 0,
    "Rival": +1, "Foe": +1, "Enemy": +1, "Archnemesis": +1,
}

# Faction size bounds, matching the shipped iGossipFactionMin/MaxSize.
FACTION_LO, FACTION_HI = 3, 40

STEP_DAYS = 0.25
INFECTIOUS_DAYS = 3.0
# Raised from the shipped 80: at 80 roughly a fifth of rumors hit it at the
# scales of interest, which truncates exactly the tail that decides how far a
# juicy rumor travels. 150 leaves the ceiling as a runaway backstop rather
# than a tuning parameter.
CARRIER_CAP = 150
SIM_DAYS = 30.0

# Candidate weight vectors, household:settlement:hold:province.
# Seven weights: household, t2, settlement, t4, hold, t6, province.
CANDIDATES = {
    # MONOTONIC by construction: each rung is drawn no more often than the one
    # closer than it. That is what the ladder means -- a peer moved to rung 6
    # must be spoken to less than a hold-mate on rung 5, not more.
    #                  t1    t2    t3    t4    t5    t6    t7
    "M1 steep":      (0.40, 0.22, 0.16, 0.10, 0.07, 0.04, 0.01),
    "M2 moderate":   (0.34, 0.20, 0.18, 0.12, 0.09, 0.06, 0.01),
    "M3 gentle":     (0.30, 0.20, 0.18, 0.14, 0.10, 0.07, 0.01),
    "M4 flat":       (0.26, 0.18, 0.18, 0.15, 0.12, 0.10, 0.01),
}


def compose_delta(shared_faction: bool, rank_delta: int) -> int:
    """One closer if either positive source, one further if negative; they sum.

    Two positive sources do not stack -- a faction-mate who is also your sister
    is one tier closer, not two.
    """
    closer = shared_faction or rank_delta < 0
    further = rank_delta > 0
    return (-1 if closer else 0) + (1 if further else 0)


def build_model(builder, export, cells_cache):
    g = builder.build(export, cells_cache)
    people = g["people"]
    parts = {k: p for k, p in people.items() if p["hold"]}

    hh_members, st_members, hold_members = defaultdict(list), defaultdict(list), defaultdict(list)
    for k, p in parts.items():
        if p["household"]:
            hh_members[p["household"]].append(k)
        if p["settlement"]:
            st_members[p["settlement"]].append(k)
        hold_members[p["hold"]].append(k)

    # Tier adjustment sources, composed per pair.
    shared_fac = defaultdict(set)
    for _f, members in builder.social_factions(g, parts, FACTION_LO, FACTION_HI).items():
        for i, a in enumerate(members):
            for b in members[i + 1:]:
                shared_fac[a].add(b)
                shared_fac[b].add(a)

    rank_of = {}
    for a, b, rank, _assoc in g["rela"]:
        if a in parts and b in parts:
            rank_of[(a, b)] = rank_of[(b, a)] = RANK_DELTA.get(rank, 0)

    delta = defaultdict(dict)
    for a, peers in shared_fac.items():
        for b in peers:
            delta[a][b] = compose_delta(True, rank_of.get((a, b), 0))
    for (a, b), rd in rank_of.items():
        if b not in delta[a]:
            d = compose_delta(False, rd)
            if d:
                delta[a][b] = d

    return g, parts, hh_members, st_members, hold_members, delta


def tier_pools(k, parts, hh_members, st_members, hold_members, delta, cache):
    """The three materialised tiers for one carrier. Province stays virtual."""
    if k in cache:
        return cache[k]
    p = parts[k]
    natural = {}
    for o in hh_members.get(p["household"], ()):
        if o != k:
            natural[o] = HOUSEHOLD
    for o in st_members.get(p["settlement"], ()):
        if o != k and o not in natural:
            natural[o] = SETTLEMENT
    for o in hold_members.get(p["hold"], ()):
        if o != k and o not in natural:
            natural[o] = HOLD

    pools = tuple([] for _ in range(NTIERS))
    adjusted = {}
    for o, t in natural.items():
        adjusted[o] = min(PROVINCE, max(HOUSEHOLD, t + delta[k].get(o, 0)))
    # A linked peer with no shared geography sits naturally on the province
    # rung; moved one closer they land on rung 6, in a pool of their own
    # rather than mixed into the carrier's hold.
    for o, d in delta[k].items():
        if o not in natural and o in parts and d < 0:
            adjusted[o] = PROVINCE - 1
    for o, t in adjusted.items():
        if t != PROVINCE:
            pools[t].append(o)
    cache[k] = pools
    return pools


def simulate_one(seed_npc, notability, scale, weights, parts, all_keys, pools_fn, rng):
    """One rumor, SIR over a month. Returns its outcome record."""
    beta = min(1.0, notability * scale)
    carriers = {seed_npc: 0.0}          # npc -> heard-on day
    active = {seed_npc: INFECTIOUS_DAYS}  # npc -> days of infectiousness left
    gens = {seed_npc: 0}
    stats = Counter()
    holds = {parts[seed_npc]["hold"]}
    setts = {parts[seed_npc]["settlement"]}
    max_depth = 0
    last_day = 0.0

    cum = []
    acc = 0.0
    for w in weights:
        acc += w
        cum.append(acc)

    day = 0.0
    while day < SIM_DAYS and active:
        day += STEP_DAYS
        for k in list(active):
            pools = pools_fn(k)
            for _ in range(CONVERSATIONS_PER_STEP):
                stats["conversations"] += 1
                r = rng.random() * acc
                tier = 0
                while tier < NTIERS - 1 and r > cum[tier]:
                    tier += 1
                if tier == PROVINCE:
                    listener = all_keys[rng.randrange(len(all_keys))]
                    if listener == k:
                        stats["silent"] += 1
                        continue
                else:
                    pool = pools[tier]
                    if not pool:
                        stats["silent"] += 1
                        continue
                    listener = pool[rng.randrange(len(pool))]
                stats["drew_" + TIER_NAMES[tier]] += 1
                if listener in carriers:
                    stats["wasted"] += 1
                    continue
                if rng.random() >= beta:
                    stats["notCaught"] += 1
                    continue
                if len(carriers) >= CARRIER_CAP:
                    stats["capped"] += 1
                    continue
                carriers[listener] = day
                gens[listener] = gens[k] + 1
                max_depth = max(max_depth, gens[listener])
                active[listener] = INFECTIOUS_DAYS
                holds.add(parts[listener]["hold"])
                setts.add(parts[listener]["settlement"])
                stats["told"] += 1
                stats["told_" + TIER_NAMES[tier]] += 1
                last_day = day
            active[k] -= STEP_DAYS
            if active[k] <= 0:
                del active[k]

    rec = dict(reach=len(carriers), depth=max_depth, holds=len(holds),
               settlements=len(setts), days=last_day)
    # Every outcome key, present or not: a rumor that told nobody is exactly
    # the case worth reporting, and it is the one where the counter is absent.
    for key in ("conversations", "told", "wasted", "notCaught", "silent", "capped",
                *("told_" + t for t in TIER_NAMES), *("drew_" + t for t in TIER_NAMES)):
        rec[key] = stats[key]
    return rec


def run(label, weights, scale, notabilities, trials, parts, all_keys, pools_fn, seed=11):
    rng = random.Random(seed)
    out = []
    for i in range(trials):
        origin = all_keys[rng.randrange(len(all_keys))]
        nota = notabilities[i % len(notabilities)]
        rec = simulate_one(origin, nota, scale, weights, parts, all_keys, pools_fn, rng)
        rec["notability"] = nota
        out.append(rec)
    return out


def summarise(label, results):
    reach = [r["reach"] for r in results]
    holds = [r["holds"] for r in results]
    conv = sum(r["conversations"] for r in results) or 1
    print(f"\n{label}")
    print(f"  reach      median {statistics.median(reach):6.1f}   mean {statistics.mean(reach):6.1f}   "
          f"max {max(reach):4d}   dead-on-arrival {sum(1 for r in reach if r == 1)/len(reach):5.1%}")
    print(f"  holds      1: {sum(1 for h in holds if h == 1)/len(holds):5.1%}   "
          f"2-3: {sum(1 for h in holds if 2 <= h <= 3)/len(holds):5.1%}   "
          f"4+: {sum(1 for h in holds if h >= 4)/len(holds):5.1%}   "
          f"all 9: {sum(1 for h in holds if h >= 9)/len(holds):5.1%}")
    print(f"  depth      median {statistics.median([r['depth'] for r in results]):4.1f}   "
          f"max {max(r['depth'] for r in results):3d}   "
          f"hit carrier cap {sum(1 for r in results if r['reach'] >= CARRIER_CAP)/len(results):5.1%}")
    print(f"  draws      told {sum(r['told'] for r in results)/conv:5.1%}  "
          f"knew {sum(r['wasted'] for r in results)/conv:5.1%}  "
          f"missed {sum(r['notCaught'] for r in results)/conv:5.1%}  "
          f"silent {sum(r['silent'] for r in results)/conv:5.1%}  "
          f"capped {sum(r['capped'] for r in results)/conv:5.1%}")


def by_tier(results):
    told = sum(r["told"] for r in results) or 1
    parts_ = " ".join(f"{t.split('-')[0]}:{sum(r['told_' + t] for r in results)/told:4.0%}"
                      for t in TIER_NAMES)
    print(f"  tellings by rung: {parts_}")


def by_notability(results):
    print("  by notability:")
    band = defaultdict(list)
    for r in results:
        band[r["notability"]].append(r)
    for n in sorted(band):
        rs = band[n]
        h = [r["holds"] for r in rs]
        print(f"    n={n:.2f}  reach median {statistics.median([r['reach'] for r in rs]):5.1f}  "
              f"holds median {statistics.median(h):3.1f}  "
              f"2+ holds {sum(1 for x in h if x >= 2)/len(h):5.1%}")


def main():
    global CONVERSATIONS_PER_STEP
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", default=r"C:\Projects\spriggit-output")
    ap.add_argument("--cells-cache", default=os.path.join(HERE, ".cells-cache.json"))
    ap.add_argument("--trials", type=int, default=240)
    ap.add_argument("--conversations", type=int, default=5)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--scale", type=float, default=0.04)
    ap.add_argument("--ablate", action="store_true")
    args = ap.parse_args()
    CONVERSATIONS_PER_STEP = args.conversations

    builder = load_builder()
    g, parts, hh, st, hold, delta = build_model(builder, args.export, args.cells_cache)
    all_keys = list(parts)
    cache = {}
    pools_fn = lambda k: tier_pools(k, parts, hh, st, hold, delta, cache)

    print(f"\nGRAPH: {len(parts)} participants, {len(hh)} households, "
          f"{len(st)} settlements, {len(hold)} holds")
    linked = sum(1 for k in parts if delta[k])
    closer = sum(1 for k in parts for d in delta[k].values() if d < 0)
    further = sum(1 for k in parts for d in delta[k].values() if d > 0)
    cancel = sum(1 for k in parts for d in delta[k].values() if d == 0)
    print(f"  tier adjustments: {linked} participants have at least one; "
          f"{closer} closer, {further} further, {cancel} cancelled")
    for t in range(NTIERS - 1):
        sizes = [len(pools_fn(k)[t]) for k in all_keys]
        print(f"  {TIER_NAMES[t]:<14} pool: median {statistics.median(sizes):5.0f}   "
              f"empty for {sum(1 for x in sizes if x == 0)/len(sizes):5.1%} of participants")

    notabilities = [0.45, 0.55, 0.65, 0.75, 0.88]
    trials = 60 if args.quick else args.trials

    print("\n" + "=" * 78)
    print(f"TIER WEIGHT SWEEP  ({CONVERSATIONS_PER_STEP} conversations/step, "
          f"{SIM_DAYS:.0f} days, {trials} rumors each)")
    print("=" * 78)
    for label, w in CANDIDATES.items():
        res = run(label, w, args.scale, notabilities, trials, parts, all_keys, pools_fn)
        summarise(f"{label}  weights={w}  scale={args.scale}", res)
        by_tier(res)

    if args.ablate:
        # What actually moves a rumor between holds? Remove one mechanism at a
        # time, everything else held fixed, and watch the hold-crossing rate.
        print()
        print("=" * 78)
        print(f"ABLATION  (weights = B, scale = {args.scale})")
        print("=" * 78)
        base = CANDIDATES["M2 moderate"]
        none_delta = defaultdict(dict)
        for name, w, d in [
            ("full model", base, delta),
            ("no province rung", base[:6] + (0.0,), delta),
            ("no faction/relationship promotion", base, none_delta),
            ("neither", base[:6] + (0.0,), none_delta),
        ]:
            c2 = {}
            fn = lambda k, d=d, c2=c2: tier_pools(k, parts, hh, st, hold, d, c2)
            res = run(name, w, args.scale, notabilities, trials, parts, all_keys, fn)
            summarise(name, res)
            by_tier(res)
        return

    print("\n" + "=" * 78)
    print("TRANSMISSION SCALE SWEEP  (weights = M2 moderate)")
    print("=" * 78)
    for scale in (0.060, 0.050, 0.045, 0.040, 0.035, 0.030):
        res = run("E", CANDIDATES["M2 moderate"], scale, notabilities, trials,
                  parts, all_keys, pools_fn)
        summarise(f"scale={scale}", res)
        by_tier(res)
        by_notability(res)


if __name__ == "__main__":
    CONVERSATIONS_PER_STEP = 5
    main()
