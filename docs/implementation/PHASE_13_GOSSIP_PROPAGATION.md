# Phase 13 — Gossip Propagation

A background social simulation in which notable events become rumors, and rumors spread NPC-to-NPC across
Skyrim's unique-actor population over in-world time — first within a household, then through a settlement,
then along the personal and organisational ties that reach across holds. Every transmission writes a paired set of
SkyrimNet memories at the moment it happens, so the Director's beats can later draw on gossip as fodder the
same way they draw on any other memory.

The simulation runs continuously regardless of where the player is or what they are doing. It is not gated on
loaded cells, and it never requires an actor to be resident in memory.

> **Propagation model superseded.** Parts 2 and 3 below describe an earlier model in which a rumor's
> notability decayed with every telling and a per-carrier quota bounded its spread. That model was wrong in a
> way tuning could not fix — it made a rumor's local intensity a function of how far it had already travelled
> — and it has been replaced by an **SIR epidemic**: constant transmissibility, a fixed infectious period,
> permanent immunity, and termination by exhaustion of susceptible contacts.
>
> The current model, its calibration, and the parameters the plugin actually ships are in
> [`tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md`](tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md).
> Everything else in this document — the social graph, residence resolution, faction filtering, seeding,
> threading, the stub-memory scheme and the implementation plan — is unaffected and still current.
>
> **Doc status: design settled; implementation staged as a validation harness first.** The design below is
> the target. The implementation plan at the end of this document deliberately builds **only** the parts
> needed to run the model in-game with stub content — no LLM calls, no real seeding, no memory-text
> generation. Those come in a follow-on phase once the propagation behaviour has been observed in the actual
> engine. See **Milestone 1 scope** at the head of the implementation plan for exactly what is and is not
> being built.

---

## Why this phase exists

NarrativeEngine already produces notable events — ambushes, visits, letters, combat, travel — and SkyrimNet
already stores per-actor memories of them. What is missing is any mechanism by which a thing that happened to
one NPC becomes something a *different* NPC knows. Every NPC's knowledge is currently an island.

The payoff is the oldest promise in this genre and still one of the least convincingly delivered: **the world
talks about you when you are not there.** A player who does something notable in Riften and, three weeks
later, hears a garbled version of it from a stranger in Solitude has experienced something no amount of
per-NPC dialogue quality can substitute for.

Secondarily, gossip is *raw material*. A background simulation that only the player ever sees is worth less
than one whose output the Director can consume. Rumors in the memory store are eligible inputs to visit
beats, letter beats, and any future beat that wants a pretext for one NPC to seek out another.

The feature exists in IntelEngine and is documented at
[`../prior-art/dm-features/gossip-propagation.md`](../prior-art/dm-features/gossip-propagation.md). The
prompt discipline there is worth keeping and the propagation mechanism is not; §4 of that document is a
catalogue of exactly the failure modes this design has to avoid, and the relevant ones are called out by
number below.

---

## Scope

### In scope

- A **social graph** over Skyrim's unique-actor population, derived entirely from data the load order already
  ships: the `BGSLocation` parent hierarchy, `BGSRelationship` records, and faction co-membership. A
  relationship-expansion mod is a **soft requirement** — it deepens the graph substantially and needs no
  integration work, but a vanilla-only load order remains supported.
- A **rumor simulation** — a discrete-event model that spreads rumors across that graph as in-world time
  passes, with epidemic-like growth and burnout.
- **Memory writes at spread time**: every transmission immediately produces a memory on the teller and a
  memory on the listener, via `SkyrimNetAPI::AddMemory`.
- **Rumor content generation**, LLM-driven, banded by hop depth so that the story mutates as it travels
  without costing an LLM call per transmission.
- **Seeding** from NarrativeEngine's own event logs and from high-notability existing memories.
- **Bounded persistent state** with an explicit reaping policy, sized at design time rather than hoped for.

### Deferred (explicitly out)

- **Any player-facing dialogue surface.** Surfacing rumors in an NPC's prompt context is a separate concern
  and should not be entangled with the simulation. The simulation writes memories; SkyrimNet's existing
  memory retrieval already puts memories in front of the LLM.
- **Any new beat.** Nothing dispatches an NPC anywhere because of gossip in this phase. The Director consuming
  gossip memories as beat fodder is the point, but it needs no new beat to do so.
- **Player participation.** The player is a *subject* of gossip, never a node in the graph. No mechanic for
  the player to inject, suppress, or trace a rumor.
- **Mobile carriers.** Rumors cross holds via personal edges — overwhelmingly shared membership of a
  province-spanning organisation, see Part 2 — not by simulating NPC travel. A mobility model is a plausible
  later refinement, not a prerequisite.
- **Dashboard visualisation.** Desirable for debugging and probably for the player, but not load-bearing.

---

## Design overview

### Part 1 — The social graph

#### The tier tree already exists in vanilla data

`BGSLocation::parentLoc` forms exactly the hierarchy this feature needs, and every level is keyword-tagged:

```text
WhiterunBanneredMareLocation  [LocTypeDwelling, LocTypeInn]
  → WhiterunLocation          [LocTypeHabitation, LocTypeCity]
    → WhiterunHoldLocation    [LocTypeHold]
      → TamrielLocation
```

Tier classification is therefore a keyword test on the parent walk, not a depth count — which matters,
because depth is not uniform across Skyrim.

The bottom tier is named **household** rather than "house", because the criterion is not architectural. A
household is *any location where a group of people sleep* — which takes in inns, shops with living quarters
above them, guild halls, barracks, temples, farms, ships, and bandit camps just as much as it takes in
Breezehome. That is the correct unit for gossip: the people who sleep under one roof talk constantly,
whatever the roof is over.

**Household indicators** — any one of these marks a location as household-tier:

- Domestic: `LocTypeDwelling`, `LocTypeHouse`, `LocTypeStewardsDwelling`, `LocTypePlayerHouse`,
  `BYOH_LocTypeHomestead`
- Commercial and communal: `LocTypeInn`, `LocTypeStore`, `LocTypeGuild`, `LocTypeTemple`, `LocTypeCastle`
- Working: `LocTypeFarm`, `LocTypeLumberMill`, `LocTypeMine`
- Martial: `LocTypeBarracks`, `LocTypeMilitaryCamp`, `LocTypeMilitaryFort`, `LocTypeJail`
- Outlaw and other: `LocTypeBanditCamp`, `LocTypeForswornCamp`, `LocTypeVampireLair`, `LocTypeWarlockLair`,
  `LocTypeOrcStronghold`, `LocTypeShip`

**Settlement indicators:** `LocTypeHabitation`, `LocTypeHabitationHasInn`, `LocTypeCity`, `LocTypeTown`,
`LocTypeSettlement`, `LocTypeHoldCapital`, `LocTypeOrcStronghold`.

**Hold indicators:** `LocTypeHold`, `LocTypeHoldMajor`, `LocTypeHoldMinor`.

Given any location, walk `parentLoc` upward and record the first ancestor (inclusive) satisfying each tier.
That yields the NPC's `(household, settlement, hold)` triple. Any of the three may be absent; absence is a
supported state, not an error.

**A single node may satisfy more than one tier, and the tiers then collapse onto it.** This is not a
degenerate case — it is how the small end of Skyrim actually works:

- An **Orc stronghold** is both a settlement and a household. Everyone in Largashbur genuinely lives together,
  and collapsing the tiers gives the stronghold the tight internal cohesion it should have rather than the
  looser settlement-tier rate.
- An isolated **farm** is typically its own settlement *and* its own household — 17 farm locations in
  `Skyrim.esm`, most of them a single family.

The collapse rule is simply that a node satisfying both contributes its household rate, not both rates
summed.

**Deliberately excluded**, because nobody sleeps there or nobody who sleeps there is a person: animal dens,
cemeteries, dragon and dragon-priest lairs, draugr crypts, Dwarven ruins and automaton sites, Falmer hives,
giant camps, hagraven nests, Riekling camps, shipwrecks, spriggan groves, werewolf and werebear lairs, and
the generic `LocTypeDungeon` / `LocTypeClearable` markers. `LocSet*` keywords are environment classification
(cave, Nordic ruin, outdoor) and play no part in tier assignment.

Two entries worth naming explicitly because they look like mistakes and are not:

- **`LocTypeJail`** is household-tier. Prisoners and their gaolers sleep in the same building and talk more
  than almost anyone else in Skyrim. Gossip in a jail is a feature, not an artefact.
- **`LocTypeHoldCapital`** appears on **zero** locations in `Skyrim.esm`. It is carried in the settlement set
  anyway so that a mod which does use it behaves correctly; in vanilla it is inert.

The whole tree is buildable at `kDataLoaded` from `GetFormArray<BGSLocation>()`, needs no cell to be loaded,
and picks up mod-added locations automatically. 638 Location records in `Skyrim.esm`, 617 of which have a
parent.

#### Residence resolution

The original sketch for this was a priority list ending in a static table generated offline from the Spriggit
export. That turns out to be unnecessary: **`BGSLocation::uniqueNPCs` (the `LCUN` subrecord) is exactly the
NPC→residence map, it is resident at `kDataLoaded`, and it does not require any cell to be loaded.**

```cpp
struct UniqueNPCData  // LCUN
{
    Actor*       actor;      // 00
    FormID       refID;      // 08 — the placed ACHR
    BGSLocation* editorLoc;  // 10 — the NPC's own editor location
};
// BSTArray<UniqueNPCData> BGSLocation::uniqueNPCs;  // 88
```

Each entry hands over both the ACHR FormID and, more usefully, the NPC's *own* editor location — which is
finer-grained than the location carrying the entry. `WhiterunLocation`'s `LCUN` array, for instance, lists
the Gray-Manes with `editorLoc = WhiterunHouseGrayManeLocation` and Ysolda's neighbours with their own
dwellings. The settlement-level record aggregates; the per-entry `editorLoc` disambiguates down to the
household.

The resolution order, then:

1. **`LCUN` `editorLoc`** — the primary path. Enumerate every `BGSLocation`, walk `uniqueNPCs`, index by the
   entry's base NPC. **Measured: 861 of 1034 gossip-eligible uniques (83.3%)** across all masters.
2. **`TESObjectREFR::GetEditorLocation()`** on the `refID` from the same entry, when `editorLoc` is null but
   the ref resolves.
3. **Persistent location**, for NPCs reachable through `PersistentActorReferencesStatic` / the persistent
   cell but absent from `LCUN`.
4. **Opportunistic runtime learning** — record `actor → GetEditorLocation()` whenever a unique actor is
   observed loaded. Covers mod-added NPCs whose plugin author did not populate `LCUN`, and NPCs the game has
   permanently relocated. Accumulates across a playthrough; never blocks anything.

A note on the sleep-package idea: it collapses into the same signal. Vanilla NPCs overwhelmingly use
`DefaultSleepEditorLoc*` package templates, which sleep *near the editor location* rather than naming a cell
— Ysolda's sleep package is `DefaultSleepEditorLoc24x8`. Packages with an explicit `PackageLocation` of type
`kInCell` exist but are the minority, and they would resolve to the same household node anyway. Not worth a
separate tier; if a cheap explicit-location package scan proves otherwise during validation, it slots in
above step 2.

**No leaf is not a failure.** An NPC whose residence resolves only to a settlement joins the graph at the
settlement tier; one that resolves only to a hold joins at the hold tier. Such an NPC simply shares no
household-tier contact rate with anyone.

**LCUN must be merged additively across the load order, not replaced.** DLC plugins override vanilla Location
records — `WindhelmLocation`, `MarkarthLocation`, `FalkreathLocation`, `DawnstarLocation` and others — *without
restating* their `LCUN` array, extending it through the `ACUN` override subrecord instead. A naive
later-plugin-wins merge therefore deletes the entire unique-NPC list for most of Skyrim's major cities. This
was not hypothetical: the first run of the extraction script did exactly that and reported 388 LCUN rows
instead of 911, with Whiterun showing 21 residents instead of 74.

At runtime this is free — `BGSLocation` is a single merged in-memory form and the engine performs the union
itself during load, which is what `BGSLocation::OverrideData` (`ACPR`/`RCPR`) exists for, so
`BGSLocation::uniqueNPCs` is already complete when we read it. Only offline analysis has to redo the merge.
Recorded here so nobody rediscovers it the hard way. Vanilla ships no `RCUN` counterpart, so a plain union is
exact.

**Measured coverage**, all masters, after filtering the unique-flagged set down to actual people (see below):

| Outcome                               | Count   | Share     |
| ------------------------------------- | ------- | --------- |
| Gossip-eligible unique NPCs           | 1034    | —         |
| Resolved to some location             | 918     | 88.8%     |
| → household tier                      | 695     | 67.2%     |
| → settlement tier (no household)      | 57      | 5.5%      |
| → hold tier only                      | 105     | 10.2%     |
| No tier at all (excluded)             | 177     | 17.1%     |
| **Participant population**            | **857** | **82.9%** |

The `LCUN` path resolved 861; the placement-cell fallback added 57 more. The persistent-location fallback
(step 3) contributed **zero** — every NPC it could have resolved was already resolved by the placement cell.
It is worth keeping as a cheap safety net for mod-added content but it earns nothing on vanilla.

**"Unique-flagged" is not the same as "a person."** 251 of the 1285 unique-flagged NPC records are
character-creation presets (`BretonFemalePreset01`…), test actors, unnamed records, or creatures whose race
lacks `ActorTypeNPC` (Alduin, dragon uniques, daedric princes). Filtering on *race carries `ActorTypeNPC` and
the record has a display name* removes all of them cleanly. The residual 177 with no tier are overwhelmingly
Daedric princes, wandering Khajiit caravan members, and quest actors staged in holding cells — genuinely
homeless, correctly excluded.

#### The relationship channel

`RE::BGSRelationship` is fully bound in CommonLibSSE-NG, including the native
`BGSRelationship::GetRelationship(npc1, npc2)`, which also reflects relationships changed at runtime
(marriage). The rank enum matches the design one-for-one.

The vanilla census is worth internalising before leaning on it:

| Rank            | Skyrim.esm |
| --------------- | ---------- |
| Ally            | 248        |
| Friend          | 143        |
| Confidant       | 88         |
| Acquaintance    | 65         |
| Rival           | 48         |
| Foe             | 8          |
| Enemy           | 1          |
| **Lover**       | **0**      |
| **Archnemesis** | **0**      |

~673 records across all masters (Skyrim 642, Dragonborn 27, HearthFires 3, Dawnguard 1), over ~1100 unique
NPCs. `Lover` appears only at runtime, via marriage. `Archnemesis` never appears at all.

This is a **sparse, high-signal annotation layer** — mostly family and marriage bookkeeping — and it will be
identity for the overwhelming majority of pairs. It is a multiplier on contact rate, never the backbone.
Indicative multipliers:

| Rank         | ×   | Rank        | ×    |
| ------------ | --- | ----------- | ---- |
| Lover        | 4.0 | Rival       | 0.4  |
| Confidant    | 3.0 | Foe         | 0.15 |
| Ally         | 2.0 | Enemy       | 0.05 |
| Friend       | 2.0 | Archnemesis | 0.0  |
| Acquaintance | 1.2 | *(none)*    | 1.0  |

Because the records are so few, the graph should be materialised once as an explicit sparse edge list at
`kDataLoaded` from `GetFormArray<BGSRelationship>()`, with a periodic refresh to pick up runtime changes.
~673 entries is nothing.

#### Relationship expansion as a soft requirement

A relationship-expansion mod — one that adds `BGSRelationship` records for the connections vanilla never
bothered to author — is a **soft requirement** for this feature. It needs no integration work whatsoever:
enumerating `GetFormArray<BGSRelationship>()` picks up every record in the load order automatically,
regardless of which plugin authored it, and `GetRelationship` resolves against the merged set.

Two consequences the design has to hold to:

- **It must degrade gracefully without one.** A vanilla-only load order is a supported configuration. It
  yields a quieter relationship channel and correspondingly less long-range spread, which is a different
  flavour of the same feature rather than a broken one. Nothing may hard-depend on an edge existing.
- **Global tempo probably has to be edge-density-aware.** The relationship channel's contact rate is
  per-edge, so a carrier with thirty relationships legitimately talks more than one with two — that part
  scales correctly on its own. But *total* transmission volume across the province scales with total edge
  count, so a tempo tuned against 673 edges will be substantially hotter at 10,000. Normalising the
  relationship channel's global rate against measured edge density at load, rather than shipping a fixed
  constant, is the obvious defence.

Relationships are also the mechanism for **asymmetric interest**: gossip *about* a rival travels faster than
gossip *to* one. Modelled as a separate multiplier on topic transmissibility when the rumor's subject is a
rival/foe of the teller, not as a change to contact rate.

#### The faction channel

`TESNPC::factions` has near-total coverage and captures the association the location tree misses: colleagues,
guild-mates, shift partners, court staff. Two NPCs who share membership in a small faction talk more than
their residences suggest.

Two rules, both deliberate:

- **Non-stacking.** Sharing ten factions applies the same multiplier as sharing one. Overlap count is a proxy
  for nothing in particular — vanilla faction membership is bookkeeping, and NPCs with dense faction lists
  are usually quest-heavy, not sociable. A single flat multiplier (indicatively ×1.5) keeps this honest.
- **Size-filtered *and* name-filtered.** This is the correction the data forced. A size band alone does not
  work: `JobInnkeeperFaction` has 29 members and every one of them is a stranger to the others, while
  `DLC2RRSeverinManorFaction` has 3 and they share a roof. Size is uncorrelated with sociality.

**Measured.** Of 676 factions with at least one participant, a 3–40 size band keeps 230 of them and admits
10,001 co-membership pairs. Adding an EditorID denylist keeps 113 and admits 2,687 pairs — **73% of what size
alone admits is junk.** Two kinds of junk, both requiring exclusion:

1. **Attribute buckets** — `CrimeFaction*`, `Town*Faction`, `Job*Faction`, `Potential*`, `Current*`,
   `Favor*`, `Gov*`, `*Guard*`, `*Trainer*`, `*Vendor*`. Everyone sharing a property, mutual strangers.
2. **Quest and scripting scaffolding** — `MQ201PartyGuestFaction`, `CWCastleHideFaction`,
   `WINeverFillAliasesFaction`, `PlayerHousecarlFaction`, `CarriageSystemFaction`. These are the more
   dangerous class, because they span holds for reasons unrelated to who talks to whom. Left in, they
   accounted for the majority of cross-hold transmissions and made the faction channel look far more
   province-spanning than the real organisations are: excluding them dropped the share of rumors leaving
   their origin hold from 32% to 17%.

What survives is recognisably a list of social groups: `ThievesGuildFaction` (22), `CollegeofWinterholdFaction`
(18), `DarkBrotherhoodFaction` (14), `CompanionsFaction` (13), `DragonsreachOccupants` (12),
`MarkarthKeepFaction` (10), `RiftenMistveilKeepFaction` (9), `WhiterunMarketShoppers` (8).

The practical conclusion: **size band plus denylist is the automatable 80% solution, but the right answer is a
curated allowlist.** Only ~113 factions survive the first cut on a vanilla load order — small enough to
hand-audit once — and the denylist will never generalise cleanly to mod-added factions. Ship the automatic
filter, keep an override list for the cases it gets wrong.

#### Participants

Unique-flagged, named, and possessing at least a hold-tier node. Alive and not disabled is checked at
transmission time rather than at graph-build time, so death removes an NPC from circulation without a rebuild.
Children participate — excluding them would be both wrong and less fun.

Expected participant count on a vanilla load order: 700–900.

---

### Part 2 — The transmission model

The single "transmission score" in the original sketch conflates two independent quantities. Separating them
is what makes the system tunable:

- **Contact rate `λ(a, b)`** — how often A and B talk *at all*, per in-world day. A pure property of the
  social graph. Independent of what is being discussed.
- **Topic transmissibility `p(rumor, t)`** — given that they are talking, does *this* come up. A pure property
  of the rumor: notability, age, local saturation.

One knob controls how chatty Skyrim is; the other controls how juicy a particular story is. Blended into a
single score, every tuning change fights itself.

> **Superseded below.** The finite-budget contact model is retained (it is still correct and still
> load-bearing), but the notability decay, telling quota and topic-probability roll described in this part
> were replaced by SIR. See `PHASE_13_SIR_VALIDATION_LOG.md`.

#### Contact rate — a finite budget, not a per-pair rate

The obvious formulation is a per-pair rate: `λ(a, b) = tierRate(a, b) × relMult × factionMult`. **It is
wrong, and catastrophically so.** A per-pair rate makes a person's total social activity scale linearly with
the size of their settlement, so a resident of 90-person Riften has 90× the daily conversations of a farmer.
Simulated with the rates this document originally proposed (2.0/day household, 0.3/day settlement), **every
rumor reached ~500 of the 857 participants within a single game day and touched all ten holds.** There is no
tuning of notability decay that rescues it; the contact term alone is supercritical.

The correct formulation gives each NPC a **finite daily social budget**, divided among their contacts in
proportion to relative weights:

```text
weight(a, b) = tierWeight(tier(a, b)) × relMult(a, b) × factionMult(a, b)
λ(a, b)      = conversationsPerDay(a) × weight(a, b) / Σ_o weight(a, o)
```

| Channel                         | Relative weight |
| ------------------------------- | --------------- |
| Same household                  | 30.0            |
| Personal edge (see below)       | 4.0             |
| Same settlement                 | 1.0             |
| Same hold, different settlement | 0.05            |
| Different hold, no connection   | 0.0001          |

with `conversationsPerDay ≈ 6`, scaled by a per-NPC `sociability` scalar. The weights are ratios, so a
two-person farmstead and a 90-person city both produce a plausible number of daily conversations — the city
resident simply spreads theirs more thinly. This is the single most important correction the simulation
produced.

`sociability` is derived from class and faction membership — innkeepers, merchants, court staff and guards
score high; hermits and dungeon-bound uniques score low. It shapes hub behaviour on top of the structural
degree distribution.

**Small settlements are the common case, not the edge case.** Measured: 46 of 58 settlements have 25 or fewer
participants, and 20 have two or fewer. Nothing in this model may gate on a minimum group size — a two-person
household must transmit normally. (IntelEngine discarded location groups with fewer than two NPCs and then
wondered why propagation felt urban.)

#### Topic transmissibility

```text
p(rumor, t) = notability × recencyDecay(t - seedTime) × (1 - localSaturation)
```

`localSaturation` is the fraction of the teller's tier-local population that already carries the rumor. It is
what ends epidemics: as it approaches 1, transmissions increasingly land on people who already know, the
effective reproduction number falls below 1, and the rumor burns out. This is emergent, not scripted — there
is no "the rumor now stops" rule anywhere in the model.

#### Two-channel listener sampling

Materialising a full adjacency matrix is not viable (800 participants ≈ 320k pairs) and not necessary. But
the obvious cheap alternative — sample a tier, then sample uniformly within it, then accept/reject on the
relationship multiplier — **is wrong for this design**, because it can only ever surface a listener the
proximity channel already proposed. With a cross-hold tier rate of 0.0005/day, a cousin in Markarth would
effectively never be selected, and the relationship multiplier could not rescue her.

So each carrier runs **two independent contact processes**:

1. **Proximity channel.** Sample a tier weighted by `tierRate × tierPopulation`, then sample uniformly within
   that tier, then apply relationship and faction multipliers as an accept/reject step. O(1), no matrix.
2. **Personal-edge channel.** Sample directly from the carrier's own explicit edge list — relationship
   partners plus social-faction co-members — at its own rate, with **no distance term at all**. Measured
   degree: mean 5.5, median 3, max 34; 77% of participants have at least one such edge.

The two processes are superposed. A carrier with no personal edges is driven entirely by proximity; a
well-connected one has a second, distance-blind route out.

#### Correction: organisations carry gossip between holds, not families

This document originally attributed cross-hold spread to relationship edges. **The simulation says otherwise,
and the structural data explains why.**

Of 636 relationship edges with both ends in the graph, **371 (58%) are same-household and 212 (33%) are
same-settlement** — 92% are fully redundant with the proximity channel, because Skyrim's `RELA` records are
overwhelmingly family bookkeeping among people who already live together. Only **25 edges cross a hold
boundary in the entire game**.

Faction co-membership is where the long-range structure actually lives. 13 of the 113 surviving social
factions span more than one hold, and they are exactly the organisations you would want carrying news:

| Faction                     | Holds spanned | Members |
| --------------------------- | ------------- | ------- |
| `DarkBrotherhoodFaction`    | 4             | 14      |
| `ThalmorFaction`            | 4             | 7       |
| `ThievesGuildFaction`       | 2             | 22      |
| `CWImperialFaction`         | 2             | 8       |
| `DLC1FerrySystemFaction`    | 3             | 3       |

Measured attribution of hold-crossing transmissions, with the province channel at 0.0001:
**faction 78%, relationship 16%, province 6%.** With the province channel set to exactly zero, faction still
accounts for 92% of crossings.

This is a better story than the one it replaces. A rumor reaching Solitude because the Thieves Guild moved it
is more legible and more satisfying than a cousin's letter, and it needs no new mechanism — it falls out of
faction co-membership the graph already has. The kinship framing from `BGSAssociationType` remains valuable
for the *memory text*; it simply is not the transport.

---

### Part 3 — The simulation

#### Rumor-centric, not NPC-centric

Do not iterate NPCs. Iterate *rumors*, as a discrete-event system — the standard formulation for propagation
on a contact network. Work becomes proportional to the number of transmissions rather than to the size of the
population, and an idle world costs nothing.

```text
Rumor {
    id
    originActor, originLocation, seedGameDay
    tags[], baseNotability
    generationText[0..3]                 // see Part 4
    carriers: actorId → { notability, generation, toldBy, heardOnGameDay, boredom }
    status: Live | BurningOut | Dead
}

Global: priority_queue<(nextTellGameDay, rumorId, carrierId)>
```

When an NPC acquires a rumor, exactly one future event is scheduled: their next telling, at
`now + Exponential(rate)` where `rate = sociability × notability × globalTempo`. When that event pops:

1. Sample a listener via the two-channel process above.
2. Roll acceptance against `p(rumor, t)`. On failure, increment the teller's boredom counter.
3. On success, add the listener as a carrier at `notability × generationDecay`, at `generation + 1`, and
   schedule *their* first telling.
4. Reschedule the teller, unless boredom, a notability floor, or an age limit retires them.

Everything the design wants falls out of this: exponential early growth, saturation, burnout, and a
transmission count that scales with how interesting the rumor is rather than with how many NPCs exist.

#### Time and work budget

The simulation advances on in-world time, so **sleeping eight hours or fast-travelling across the province
advances the rumor mill** — that is a feature, not a leak. Game time is read as a *sampled value*
(`GameDaysPassed` delta per tick), never used as a timer; see
[`the standing note on GameTime`](../../CLAUDE.md) and the existing Tick-driven accumulator convention.

A large time jump therefore makes many scheduled events eligible at once. The response is to cap **events
processed per real-time tick** (indicatively 20–25) and let the queue drain over the following seconds of
real time, rather than to cap simulated time. A 30-day `wait` producing ~300 events drains in about a dozen
ticks — a few seconds of wall clock, entirely off the main thread, invisible.

#### Termination and bounds

Emergent burnout is the primary mechanism, but it is backstopped by hard limits, because an unbounded rumor
store is the single most expensive mistake to fix retroactively (prior art §4.7 — IntelEngine accumulated
per-NPC gossip state with no expiry, no sweep, and no cleanup on death):

- Max live rumors (~12), max carriers per rumor (~80), max generation depth (~8).
- Carrier retirement on **telling quota**, notability floor, or elapsed game days since hearing.
- Dead rumor → drop the carrier map, retain a small digest for dedup, LRU-evict beyond a cap.
- Periodic sweep for dead and disabled carriers.

Co-save sizing, by construction rather than by hope: 12 rumors × 80 carriers × ~14 bytes ≈ 13 KB, plus
4 generation texts × ~200 chars × 12 rumors ≈ 10 KB. Under 25 KB total, and bounded above regardless of
playthrough length.

Note that the *memories* written into SkyrimNet are not bounded by this — they are handed off to SkyrimNet's
memory database, which is built to hold large volumes and to retrieve by relevance. That is the correct
division: NarrativeEngine owns the bounded simulation state, SkyrimNet owns the unbounded memory corpus.

---

### Part 4 — Rumor content

#### Generation bands, and the telephone game

Generating text per transmission is unaffordable and unnecessary. Instead, generate one canonical retelling
per **hop band** — generations 0–1, 2–3, 4–5, 6+ — for four LLM calls per rumor total, whether it reaches
eight people or three hundred.

The payoff is that each band is generated **from the previous band's text, not from the original**. That is
the telephone game, for free, and it produces exactly the distortion the design wants without any bespoke
mutation machinery: names drop out, details migrate, the fishmonger becomes the Jarl's steward.

**Notability should not decay monotonically.** Real gossip amplifies at least as often as it fades — "and
then he killed the dragon bare-handed" — so band regeneration is permitted to *raise* notability as well as
lower it. A rumor that catches a second wind three hops out is far more interesting than one that reliably
peters out, and the mechanism costs nothing beyond letting the generation prompt return a notability delta.

Every free-form string returned by these calls passes through `LLMTextSanitizer::Sanitize` at the point of
extraction, per the standing project rule. (Prior art §4.18: IntelEngine sanitized nothing on this path, and
raw LLM text reached StorageUtil, the bio prompt, and `DirectNarration` unnormalised.)

#### Memories are written at spread time

Every transmission immediately writes two memories via `SkyrimNetAPI::AddMemory`: one on the teller
(*"told X about…"*) and one on the listener (*"X told me…"*). This is non-negotiable for the feature's
secondary purpose — the Director cannot use gossip as beat fodder if the memories only materialise when the
player happens to walk into the room.

Crucially, **writing at spread time does not imply an LLM call at spread time.** The memory is composed
deterministically from three pieces that are all already available:

1. The rumor's **band text** — already generated and cached on the rumor.
2. A **framing template** selected by the relationship between teller and listener and by the distance
   between them.
3. **Names**, substituted in.

That is a string build. It is instant, free, and can run on the worker thread; only the `AddMemory` call
itself needs marshalling.

#### Relationship-aware framing

The framing templates are where cross-hold transmission earns its keep narratively, and vanilla data supplies
the vocabulary. `BGSRelationship::assocType` points at a `BGSAssociationType`, which carries the exact kinship
labels, already gendered:

```cpp
// BGSAssociationType
BSFixedString associationLabels[Members::kTotal][Sexes::kTotal];  // {Parent, Child} × {Male, Female}
```

So "cousin", "sister", "father" are read out of the record rather than invented, and the correct term is
selected by which side of the relationship each party is on and by their sex. Framing then keys on
`(association, distance)`:

| Case                          | Framing                                                      |
| ----------------------------- | ------------------------------------------------------------ |
| Same household                | *"Over supper, my sister mentioned…"*                        |
| Same settlement, friend       | *"Ysolda told me at the market…"*                            |
| Different hold, kin           | *"My cousin visited from Markarth, and she told me…"*        |
| Different hold, kin, no visit | *"I had a letter from my sister in Solitude. She mentions…"* |
| No relationship at all        | *"I heard a rumor that…"*                                    |

The last row is the honest fallback and will be the most common. It is also fine — that is genuinely how most
gossip arrives.

An optional LLM polish pass over composed memories is a plausible later refinement, but it must be
rate-limited and it must not be on the critical path: the memory has to exist the instant the transmission
happens, not when a queue drains.

#### Provenance

Each carrier record stores origin actor, origin memory reference, generation, and the immediate teller — as
**FormIDs, not display names**. Prior art §4.13 stored a display name one hop back and nothing else, which
made the mod's headline promise ("gossip traced to real events") unverifiable three hops out and collided
catastrophically on Skyrim's many identically-named actors.

Full provenance is what makes the best available player experience possible: tracing a rumor backwards by
asking each named source in turn. That requires the chain to be real data, not an LLM's assertion.

---

### Part 5 — Seeding

Two sources, deliberately unequal:

1. **Event-derived (primary, free).** NarrativeEngine already logs structured notable events —
   `CombatEventLog`, `TravelEventLog`, beat completions, ambush outcomes. These have a known origin location,
   a known witness set, and a known subject. They cost nothing to detect and they are the events the player
   actually caused, which is the whole point.
2. **Memory-derived (secondary, rate-limited).** Pull an NPC with high engagement (the existing
   `SkyrimNetAPI::GetActorEngagement` path that `SenderCandidatePool` already uses), fetch their memories, and
   ask the LLM whether anything is tavern-worthy. This is IntelEngine's approach and it is worth keeping, but
   it costs an LLM call per attempt and must be heavily throttled.

Seeding must also **dedup against live and recently-dead rumors**. Prior art §4.15: IntelEngine's rumor
generator had no visibility into which rumors already existed, so it re-seeded the same story indefinitely and
could never deliberately advance an existing thread.

The prompt discipline from prior art §2 is worth adopting nearly verbatim — the explicit "NOT gossip-worthy"
list (opened a door, bought an item, walked somewhere), the traceability requirement, and the instruction to
reject rather than invent. That author clearly fought the failure mode where an LLM narrates "Ysolda opened a
door" as breaking news, and the resulting prompt text is the most reusable thing in that codebase.

---

### Part 6 — Threading and residency

The simulation is pure arithmetic over immutable data and belongs off the main thread entirely. **The steady
state needs no main-thread hop at all** — which is worth stating explicitly, because the obvious candidates
each turn out not to require one.

- **Graph build** at `kDataLoaded`, which is a main-thread message, matching the existing `HoldGrid` /
  `TravelGraph` pattern. Const for the session thereafter, except for the relationship refresh.
- **Event loop** on the plugin thread or a dedicated worker, driven by the existing Tick accumulator.
- **Co-save record** of its own, for the rumor set, the carrier maps, and the event queue.

#### Why the steady state stays off main

**Sampling game time.** `RE::Calendar::GetSingleton()` and the game-time globals behind it are plain reads of
values the main thread writes. No hop. The worst case is a value one frame stale, which for a simulation
whose smallest unit is a fraction of a game day is not a distinction that exists.

**Resolving actors and checking alive / disabled.** No hop, and not merely by tolerance —
CommonLibSSE-NG's `TESForm::LookupByID` acquires the engine's own read-write lock over the all-forms map
before touching it:

```cpp
[[nodiscard]] static TESForm* LookupByID(FormID a_formID)
{
    const auto& [map, lock] = GetAllForms();
    [[maybe_unused]] const BSReadWriteLock l{ lock };
    ...
}
```

So the lookup is genuinely synchronised against concurrent form-table mutation, not just usually fine.
`IsDead()` and `IsDisabled()` are flag reads on top of that; a stale answer costs at most one wasted
transmission attempt.

The one discipline this does require is the threading model's convention-only rule (enforcement item 6):
**the `RE::Actor*` must not outlive the read.** Resolve, read the flags, return a plain value, drop the
pointer. Never store it on a carrier record or anywhere else the worker can reach it later.

**`AddMemory`.** Off main, but for a different reason than the other two, and with a caveat that should not
be papered over.

`PublicAddMemory` is *not* documented as thread-safe. The blanket guarantee in `PublicAPI.h` is scoped
narrowly: the file header says "All **data query** functions (v3+) are thread-safe", and the section note
repeating it sits over `Data Queries (v3+)`. `PublicAddMemory` lives in its own `Memory Creation (v5+)`
section, which carries no such note. The "runs on SkyrimNet's ThreadPool" property documented elsewhere in
that header applies to **callbacks** — the direction where SkyrimNet calls *us* — not to this synchronous
entry point, which executes on the caller's thread and returns a memory ID.

That still argues for keeping it off main rather than on: the call does vector-embedding and a database
insert inline, so it is synchronous work of unbounded duration, and the main thread is the worst place for
it. The residual risk is concurrency between our writes and SkyrimNet's own, which we cannot see into. The
defence available on our side is to **serialise every gossip `AddMemory` onto the single simulation worker**,
so NarrativeEngine contributes at most one concurrent writer. Confirming SkyrimNet's actual write-path
locking would let that constraint relax; until then it holds.

Every function still takes a `PluginThread::Token` or `MainThread::Token` per the standing threading
discipline — the tokens are about proving context, and a subsystem that never needs `MainThread::Run` simply
passes `PluginThread::Token` throughout.

---

## Validation

The graph was built offline and simulated before any C++ was written, by
[`tests/gossip-spread/build-social-graph.py`](tests/gossip-spread/build-social-graph.py). It parses the
Spriggit export, reconstructs the same graph the runtime would build at `kDataLoaded`, and Monte-Carlos the
propagation model
over it. Every measured figure in this document comes from that script and can be regenerated:

```text
python scripts/build-social-graph.py --trials 400 --province-weight 0.0001
```

### Measured behaviour of the tuned model

857 participants; `conversationsPerDay` 6, telling-quota mean 2.5, topic probability 0.35, notability floor
0.15, decay 0.5–0.8 per hop with a 1-in-7 chance of amplification. 400 seeded rumors:

| Metric                             | Result                                      |
| ---------------------------------- | ------------------------------------------- |
| Reach per rumor                    | median 13, p90 36, max 100 (never 151+)     |
| Duration of active spread          | median 4.8 game days, p90 9.0, max 16.8     |
| Generation depth                   | median 5, max 12                            |
| Rumors staying in one hold         | 333 / 400 (83.2%)                           |
| Rumors reaching 3+ holds           | 9 / 400 (2.3%)                              |
| Transmissions per rumor            | 16.0 (plus 10.8 wasted tellings)            |
| Memories per rumor                 | 32                                          |

Reach distribution: 11% of rumors die at 2–3 people, 24% reach 4–8, 35% reach 9–20, 28% reach 21–50, 3% reach
51–150. That is the shape the design wanted — most gossip stays in the neighbourhood, a minority travels, and
nothing runs away.

Contact-weight inequality lands at a **Gini of 0.46** — heavy-tailed enough that hubs matter without any
hand-authored hub list. The natural hubs the graph produces unprompted are Ragged Flagon and Thieves Guild
regulars, keep and market NPCs: exactly right.

**Q5 answered — small settlements are a feature, not a problem.** Reach by seed settlement size: ≤5 residents
→ median 7 over 3.6 days; 6–20 → median 8 over 3.5 days; ≥40 → median 17 over 4.9 days. Hamlets saturate fast
and stay local, cities sustain longer chains. Both are legible, and neither needed a special case.

**Q6 answered — cost is affordable.** At 0.5 seeded rumors per game day the steady state is ~8 transmissions
and ~16 memories per game day, about **480 memories per game month**. At 1.0/day it is ~960/month. Given
lazy-free storage in SkyrimNet's vector DB and 4 LLM calls per rumor for the generation bands, 0.5/day is the
recommended starting tempo and 2.0/day is the ceiling before the memory store becomes noise.

---

## Open questions

Answered by the validation run above: **Q1** (faction band — size alone is insufficient, see the faction
channel section), **Q2** (degree distribution is heavy-tailed, Gini 0.46), **Q3** (residence coverage 82.9%),
**Q4** (household sizes: 249 distinct, median 2, max 12 — flat rate holds, no taper needed), **Q5** and **Q6**
above. What remains genuinely open:

1. **The curated faction allowlist.** The automatic size+denylist filter is the 80% solution and will not
   generalise to mod-added factions. ~113 factions survive the first cut on vanilla — small enough to
   hand-audit once, and that audit has not been done.
2. **`sociability` derivation.** The structural degree distribution already produces good hubs without it, so
   it may be unnecessary. If it is kept, `Job*Faction` membership is the obvious source — those factions are
   useless as *edges* but are an excellent occupation signal.
3. **Tempo at a second edge density.** All the numbers above are vanilla-only. A relationship-expansion mod
   changes edge count, and the tempo answer has to be re-measured with one installed.
4. **Whether four generation bands is the right granularity**, or whether three (fresh / worn / garbled)
   reads the same and costs 25% less. Measured generation depth is median 5, max 12, so four bands with the
   last one absorbing 6+ is well matched — but this is a qualitative call, not a measurable one.
5. **`PublicAddMemory`'s actual write-path locking.** See Part 6. Unchanged by this validation.
6. **Tempo, at two edge densities.** How many transmissions per in-world day produces a world that feels
   alive without producing a memory store full of noise? This has to be answered twice — once on a vanilla
   load order and once with a relationship-expansion mod installed — and the gap between the two answers is
   what determines whether density normalisation is genuinely required or merely tidy.
7. **Whether four generation bands is the right granularity**, or whether three (fresh / worn / garbled)
   reads the same and costs 25% less.
8. **`PublicAddMemory`'s actual write-path locking.** The header does not document it as thread-safe (see
   Part 6). If SkyrimNet's memory writes are internally locked, the single-worker serialisation constraint
   can be dropped and gossip writes could fan out. Answerable by reading SkyrimNet's source or asking
   upstream; worth doing before tempo is tuned high enough for it to matter.

---

## File map

New files, all Milestone 1:

| File                                                      | Role                                                                                                                                     |
| --------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `include/GossipGraph.h` / `src/GossipGraph.cpp`           | Tier tree, residence resolution, participant set, relationship and faction edge channels. Built once at `kDataLoaded`, const thereafter. |
| `include/GossipSim.h` / `src/GossipSim.cpp`               | Rumor state, carrier maps, the game-time event queue, the transmission model, co-save record `'NEGS'`.                                   |
| `include/GossipLog.h` / `src/GossipLog.cpp`               | The dedicated rumor-tracing log. Own spdlog sink, own file, session-rotated. Independent of `bDebugMode`.                                |
| `include/GossipSeeder.h` / `src/GossipSeeder.cpp`         | **Temporary.** The stub-rumor seeder that makes the in-game simulation possible. Marked for deletion in the follow-on phase.             |
| `statics/SKSE/Plugins/NarrativeEngine_GossipFactions.ini` | Faction denylist and allowlist overrides, authorable without a rebuild.                                                                  |

Modified:

| File                                       | Change                                                                                                                   |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------ |
| `src/Plugin.cpp`                           | `Initialize` calls at `kDataLoaded`; `OnSessionStart` / `OnSessionEnd` at `kNewGame` / `kPostLoadGame` / `kPreLoadGame`. |
| `src/Tick.cpp`                             | `GossipSim::Poll(pt, elapsedSec)` and `GossipLog::Poll(pt, elapsedSec)` added to the unpaused poll body.                 |
| `include/Settings.h` / `src/Settings.cpp`  | The `[Gossip]` block below.                                                                                              |
| `statics/SKSE/Plugins/NarrativeEngine.ini` | Author defaults for the same.                                                                                            |

---

## Implementation plan

### Milestone 1 scope — a validation harness, not the feature

Everything in this plan exists to answer one question: **does the propagation model behave in the real engine
the way it behaved in the offline simulation?** The offline script proved the model can be tuned into the
intended range against a graph reconstructed from file data. It cannot prove that the runtime graph matches
that reconstruction, that game-time sampling behaves under waiting and fast travel, or that the cost estimates
survive contact with SkyrimNet's memory database.

**In scope for Milestone 1:**

- The full social graph, built at runtime from the same sources the offline script used.
- The full transmission model — budget-based contact sampling, both channels, quota-based retirement,
  notability decay and amplification.
- **Stub memories.** Every transmission writes two real `AddMemory` calls with placeholder content and a
  notability-derived importance. The mechanism is real; the text is not.
- A dedicated rumor-tracing log, separate from the main plugin log.
- A temporary seeder that plants a spread of stub rumors at session start.
- A documented in-game test procedure.

**Explicitly NOT in scope for Milestone 1** — do not build any of this yet:

- Any LLM call. No generation bands, no telephone-game text evolution, no notability deltas from a model.
- Real seeding from `CombatEventLog` / `TravelEventLog` / beat outcomes, or from existing memories.
- Relationship-aware memory framing, `BGSAssociationType` kinship labels, or provenance-rich memory text.
- Any player-facing surface: no dialogue submodule, no dashboard tab, no new beat.
- `sociability` scaling. The structural degree distribution is doing the work for now; whether the scalar is
  needed at all is an open question and Milestone 1's data is what answers it.

The stub content is deliberately conspicuous so it can be found and purged later. See Step 7.

> **Run this on a throwaway save.** Stub memories are written into SkyrimNet's memory database and are not
> automatically removed. A validation run at the recommended tempo writes several hundred of them. Do not do
> this on a playthrough you care about.

---

### Step 1 — Settings and INI surface

- [x] Complete

**Goal:** Every knob the later steps need, in place before they need it, with the validated tuning as the
baked-in defaults.

Add a `[Gossip]` block to `Settings::Config`, `ReadIniInto`, and the shipped
`statics/SKSE/Plugins/NarrativeEngine.ini`:

| Key                                   | Default       | Meaning                                                       |
| ------------------------------------- | ------------- | ------------------------------------------------------------- |
| `bGossipEnabled`                      | `false`       | Master switch. Ships **off**; the validation run turns it on. |
| `bGossipLogEnabled`                   | `true`        | The dedicated trace log. Independent of `bDebugMode`.         |
| `iGossipTickIntervalSeconds`          | `2`           | Accumulator threshold for the sim poll.                       |
| `iGossipMaxEventsPerTick`             | `25`          | Work cap. Bounds the catch-up burst after a long wait.        |
| `fGossipConversationsPerDay`          | `6.0`         | Per-NPC daily social budget.                                  |
| `fGossipWeightHousehold`              | `30.0`        | Channel weights. Relative, not rates.                         |
| `fGossipWeightPersonalEdge`           | `4.0`         |                                                               |
| `fGossipWeightSettlement`             | `1.0`         |                                                               |
| `fGossipWeightHold`                   | `0.05`        |                                                               |
| `fGossipProvinceShare`                | `0.01`        | Share of conversations, not a weight — see the note below     |
| `fGossipTellQuotaMean`                | `2.5`         | Mean of the geometric telling quota, scaled by notability.    |
| `fGossipTopicProbability`             | `0.35`        | Chance the rumor comes up in a given conversation.            |
| `fGossipNotabilityFloor`              | `0.15`        | Carrier retirement threshold.                                 |
| `fGossipDecayMin` / `fGossipDecayMax` | `0.5` / `0.8` | Per-hop notability multiplier range.                          |
| `fGossipAmplifyChance`                | `0.14`        | Chance a hop *raises* notability instead.                     |
| `fGossipAmplifyFactor`                | `1.2`         |                                                               |
| `iGossipMaxLiveRumors`                | `12`          |                                                               |
| `iGossipMaxCarriersPerRumor`          | `80`          |                                                               |
| `iGossipMaxGenerationDepth`           | `8`           |                                                               |
| `iGossipCarrierMaxAgeDays`            | `30`          | Hard retirement regardless of quota.                          |
| `iGossipFactionSizeMin` / `Max`       | `3` / `40`    | Faction band.                                                 |
| `bGossipSeedStubsOnLoad`              | `false`       | **Temporary.** The Step 8 seeder.                             |
| `iGossipStubSeedCount`                | `12`          |                                                               |

Every default matches the tuning the offline run validated, so the first in-game run is a like-for-like
comparison rather than a fresh tuning exercise.

**Verification:** launch with no `[Gossip]` block in either INI and confirm the baked-in defaults log
correctly; add an MCM override for one float and confirm it takes.

---

### Step 2 — `GossipGraph`: tier tree, residence, participants

- [x] Complete

**Goal:** Reproduce, at runtime, the graph the offline script built — and prove it by comparing the numbers.

Build once at `kDataLoaded`, after `HoldGrid::Initialize` (same placement rationale as `TravelGraph`), const
for the session thereafter. Mutex-guarded reads, as with `HoldGrid`.

1. **Tier tree.** Enumerate `TESDataHandler::GetFormArray<BGSLocation>()`. For each, read `parentLoc` and
   `keywordData`, classify against the three keyword sets in Part 1. Store `(parent, tiers)` per location.
2. **Residence.** Walk every `BGSLocation::uniqueNPCs` array; index base NPC → `editorLoc`, falling back to
   `TESObjectREFR::GetEditorLocation()` on the entry's `refID` when `editorLoc` is null. Note that the
   runtime needs **no** additive merge — the engine has already unioned `ACUN` into `uniqueNPCs` by the time
   we read it. This is the one place the offline script had to do extra work the plugin does not.
3. **Participants.** A `TESNPC` qualifies when: `IsUnique()`, has a non-empty `GetFullName()`, its `TESRace`
   carries `ActorTypeNPC`, and its residence resolves to at least a hold node. Alive/disabled is checked at
   transmission time, not here.
4. **Member lists.** Build `household → [npc]`, `settlement → [npc]`, `hold → [npc]` reverse indexes.

**Verification — this step's whole point.** Log a summary block and compare against the offline measurements
below. On a vanilla-plus-DLC load order expect approximately:

| Quantity              | Offline figure             |
| --------------------- | -------------------------- |
| Participants          | 857                        |
| With a household node | 695                        |
| Households            | 249, median size 2, max 12 |
| Settlements           | 58, largest Riften at 90   |
| Holds                 | 10, Rift largest at 136    |

Exact equality is not expected — a modded load order legitimately differs, and the placement-cell fallback the
script used is not available at runtime. **A large shortfall is a bug, not a difference.** If participants come
in near 326, the `LCUN` read is wrong in the same way the script's first run was. If Riften reports ~10
residents, likewise. Dump the per-settlement census to the log at `debugMode` so this is checkable at a glance.

---

### Step 3 — `GossipGraph`: relationship and faction channels

- [x] Complete

**Goal:** The personal-edge list each carrier samples from.

1. **Relationships.** Enumerate `GetFormArray<BGSRelationship>()`, keep records where both `npc1` and `npc2`
   are participants, store a symmetric sparse map with the rank multiplier from Part 1. Refresh on session
   start to pick up runtime marriage.
2. **Factions.** Count participant membership per faction. Admit a faction when its size is within
   `[iGossipFactionSizeMin, iGossipFactionSizeMax]` **and** its EditorID does not match the denylist. Expand
   admitted factions into pairwise edges.
3. **The denylist is data, not code.** Ship it in `NarrativeEngine_GossipFactions.ini` with the fragments from
   the validation log, a `[Deny]` section of substrings and an `[Allow]` section of exact EditorIDs that
   override a deny match. Comment the file well enough that adding an entry needs no source access — the
   `AttackerGroups.ini` precedent from Phase 11 is the model.

**Verification:** log the surviving faction count and the pairs admitted. Offline figures: 636 relationship
edges with both ends in the graph, 113 surviving factions, 2687 faction pairs, personal-edge degree mean 5.5 /
median 3 / max 34. Log the ten widest-spanning factions by hold count and confirm the list reads like
`DarkBrotherhoodFaction`, `ThalmorFaction`, `ThievesGuildFaction` — **not** like `PlayerHousecarlFaction` or
`MQ201PartyGuestFaction`. If the quest-scaffolding factions are present, the denylist did not deploy.

---

### Step 4 — `GossipLog`: the dedicated rumor-tracing log

- [x] Complete

**Goal:** A log whose entire contents are the life of the rumor mill, readable end to end without wading
through anything else. This is the primary instrument for the whole milestone, so it comes before the code it
measures.

Follow the `EventHistoryWriter` pattern: session-scoped file at
`Data/../SKSE/NarrativeEngine_Gossip.log`, rotated five deep on `OnSessionStart`, flushed and closed on
`OnSessionEnd`. Unlike `EventHistoryWriter` it needs its own spdlog logger and sink rather than the default
one, so nothing it writes reaches `NarrativeEngine.log` and nothing from elsewhere reaches it.

Gate on `bGossipLogEnabled` alone — **not** on `bDebugMode`. The point is to be able to run a long validation
session with a quiet main log and a complete gossip trace.

Line format, one event per line, absolute in-game timestamp first:

```text
[4E4R23 07:41] SEED    r03  notability=0.85  origin=Ysolda            @WhiterunLocation/WhiterunBanneredMare
[4E4R23 09:15] TELL    r03  gen=1  n=0.61  Ysolda -> Carlotta Valentia   via=household  @WhiterunBanneredMare
[4E4R23 11:02] TELL    r03  gen=1  n=0.55  Ysolda -> Belethor            via=settlement @WhiterunLocation
[4E4R23 11:02] WASTED  r03  Ysolda -> Carlotta Valentia  (already knows)  quota_left=1
[4E4R24 03:30] TELL    r03  gen=2  n=0.66  Belethor -> Delvin Mallory    via=faction:ThievesGuildFaction  XHOLD Whiterun->Rift
[4E4R24 18:44] RETIRE  r03  Ysolda  reason=quota
[4E4R29 22:10] BURNOUT r03  reach=14  depth=4  holds=2  settlements=3  days=6.6  transmissions=13  wasted=9
```

Requirements that make it analysable rather than merely verbose:

- Every `TELL` line names the **channel** (`household` / `settlement` / `hold` / `province` /
  `faction:<EditorID>` / `relationship:<Rank>`). This is what let the offline run overturn the cross-hold
  story, and it is the single most valuable field in the file.
- Hold crossings are flagged inline (`XHOLD <from>-><to>`) so they can be grepped out directly.
- `BURNOUT` carries the whole per-rumor summary, so a run can be assessed from those lines alone.
- A `CENSUS` block on `OnSessionEnd` listing every live rumor and its carrier count.

**Verification:** with the sim not yet written, emit a synthetic `SEED` / `TELL` / `BURNOUT` triple at
initialize and confirm the file rotates, the format renders, and nothing leaks into the main log.

---

### Step 5 — `GossipSim`: rumor state, the game-time queue, and co-save

- [x] Complete

**Goal:** The lifecycle skeleton, persisting correctly, ticking on game time — with no transmission logic yet.

1. **State.** `Rumor { id, originActor, originLocation, seedGameDay, baseNotability, status }` plus
   `carriers: actorId → { notability, generation, toldBy, heardOnGameDay, quotaRemaining }`, plus a global
   priority queue keyed on `nextTellGameDay`.
2. **Tick.** `GossipSim::Poll(pt, elapsedSec)` on the Tick-driven accumulator pattern. Each firing samples
   `GameDaysPassed` and computes a delta. **Game time is sampled as a value, never used as a timer** — see the
   standing note in Part 6.
3. **Catch-up.** Pop at most `iGossipMaxEventsPerTick` due events per firing. Simulated time advances by the
   full delta; the *work* drains over subsequent real-time ticks. A 30-day wait must not produce a stall — log
   the queue depth when it exceeds the cap so the drain is observable.
4. **Co-save.** Record `'NEGS'`. Rumors, carrier maps, and the queue. Skip-and-log on an unrecognized version
   rather than failing the load.
5. **Reaping.** Enforce `iGossipMaxLiveRumors` and `iGossipMaxCarriersPerRumor` at insert time; sweep dead and
   disabled carriers periodically.

**Verification:** seed one rumor by hand with a single carrier and no transmission. Confirm it persists across
save/load, that the queue survives, that a 30-day wait drains without a hitch, and that the co-save payload
stays under the ~25 KB the design budgets.

---

### Step 6 — The transmission model

- [x] Complete — **rewritten** to SIR after in-game runs 1-3; see `PHASE_13_SIR_VALIDATION_LOG.md`

**Goal:** The SIR algorithm, with the constants calibrated in `PHASE_13_SIR_VALIDATION_LOG.md`.

1. **Contact set.** For a carrier, assemble `(peer, weight)` across all channels, apply the relationship
   multiplier and the personal-edge distance attenuation, then divide `fGossipConversationsPerDay` among them
   in proportion to weight. **The budget division is load-bearing** — a per-pair rate is supercritical and
   saturates the province in one game day.
2. **Province channel.** A single aggregate pseudo-peer, resolved to a random participant only when selected.
3. **Simulation step.** Each infectious carrier is scheduled every `fGossipStepDays`. At each step it holds
   `Poisson(fGossipConversationsPerDay × fGossipStepDays)` conversations.
4. **Transmission.** Per conversation: sample a partner; if they have ever carried this rumor, it is a wasted
   opportunity — log it and move on. Otherwise transmit with probability
   `notability × fGossipTransmissionScale`, constant for the rumor's whole life.
5. **Recovery.** A carrier stops transmitting at `heardOnGameDay + fGossipInfectiousDays` and is immune
   permanently. A rumor with no infectious carriers burns out.

**Every conversation must be simulated, including the wasted ones.** The scheduling cannot be thinned by the
transmission probability — thinning hides the conversations that land on immune people, and those are the
entire termination mechanism. An earlier revision did thin, and it is why the model appeared to work while
being unable to terminate for the right reason.

**Verification:** on a vanilla+DLC graph, expect reach median ~11 / p90 ~68, duration median ~5 game days,
household coverage 96-98% at every size band, settlements of 31+ residents around 38% and never saturated,
holds never saturated, and ~11% of rumors leaving their origin hold.

### Step 7 — Stub memory writes

- [x] Complete

**Goal:** Exercise the real memory-write path with fake content, so the cost and the plumbing are measured
even though the text is not yet generated.

On every successful transmission, two `SkyrimNetAPI::AddMemory` calls — teller and listener — serialised onto
the sim worker per Part 6.

```text
teller:   [NE-GOSSIP-STUB r03 gen1] I told Carlotta Valentia a rumor. (placeholder content, notability 0.61)
listener: [NE-GOSSIP-STUB r03 gen1] Carlotta Valentia told me a rumor. (placeholder content, notability 0.61)
```

- `importance` = the carrier's current notability. This is the field the milestone is actually testing —
  it must vary across rumors and decay across hops, because that variation is what a later phase's retrieval
  will key on.
- `memoryType` = `"KNOWLEDGE"`, `emotion` = `""`, `location` = the transmission's settlement name.
- `tagsJson` = `["gossip","stub"]`. The `stub` tag is the purge handle.
- The `[NE-GOSSIP-STUB <rumor> <gen>]` prefix is mandatory and must be greppable. It is how these get found
  and removed later, and how they are distinguished from real memories if a save is accidentally carried
  forward.

No sanitization is required here because no string comes from an LLM. **When Milestone 2 replaces this content
with generated text, every free-form field must go through `LLMTextSanitizer::Sanitize` at the point of
extraction** — note it in the code now so the requirement is not lost.

**Verification:** confirm memory IDs come back `> 0`, that `GetMemoriesForActor` returns the stubs, and that
importance values vary as expected. Time the `AddMemory` call and log the distribution — if it is slow enough
to matter, the single-worker serialisation constraint from Part 6 becomes a throughput ceiling worth knowing
about before Milestone 2.

---

### Step 8 — Temporary stub seeder

- [x] Complete

**Goal:** Plant a controlled spread of rumors at session start so the simulation has something to propagate.
**This module is temporary and exists only for Milestone 1.** Mark it as such in the header, and delete it in
the phase that wires real seeding.

Gated on `bGossipSeedStubsOnLoad`. On `OnSessionStart`, after the graph is built, seed `iGossipStubSeedCount`
rumors chosen to span the interesting axes rather than at random:

| Slice                                                         | Count | Purpose                                                |
| ------------------------------------------------------------- | ----- | ------------------------------------------------------ |
| Large settlement (Riften/Solitude/Whiterun/Markarth/Windhelm) | 4     | High notability 0.9–1.0. Should travel furthest.       |
| Mid settlement (6–20 residents)                               | 3     | Notability 0.5–0.7. The common case.                   |
| Small settlement (≤5 residents)                               | 3     | Notability 0.5–0.7. Tests the saturation-and-die path. |
| Any settlement, low notability                                | 2     | Notability 0.2–0.3. Should die within a hop or two.    |

Pick the origin actor uniformly from the chosen settlement's participants. Log each seed with its slice,
settlement, participant count, and notability, so the resulting spread can be read against the seed
conditions.

Deterministic seeding matters here: take the RNG seed from a setting so a run can be repeated exactly. A test
that cannot be re-run identically is much less useful for comparing a tuning change.

**Verification:** confirm all twelve appear in the gossip log within a second of session start, that the
settlement slices are what was asked for, and that re-loading the same save with the same RNG seed produces
identical seeds.

---

### Step 9 — In-game simulation run

- [ ] Complete

**[USER]** — Steps 1-8 are built, compiling clean, and deployed. This step needs a running game and is the
only thing standing between here and Milestone 1's done condition.

**Implementation notes carried out of Steps 1-8**, worth reading before the run:

- Two deviations from the plan, both recorded in code comments. The relationship multiplier is applied once
  to a peer's *whole* accumulated contact weight rather than folded into the personal-edge term — folding it
  in double-applies it for any pair that is both a neighbour and a relation, which is ~92% of vanilla
  relationship edges. And `via` attribution prefers the specific channel (faction / relationship) over the
  proximity tier when a peer is reachable both ways, since that is the more informative answer in the log.
- **Known limitation: dead NPCs keep gossiping.** The viability check catches deleted forms only, so a
  murdered NPC carries on until their carrier recovers. Negligible over a 20-day run.

  **Correction:** this was originally written up as hard — death living on an `Actor` reference that is not
  loaded — and that reasoning was wrong. `BGSLocation` stores unique NPCs as `UniqueNPCData { Actor* actor;
  … }` while storing ordinary persistent refs as `UnloadedRefData { FormID refID; FormID parentSpaceID;
  CellKey cellKey; }`. The engine would not hold a raw `Actor*` to something destroyed on cell unload:
  unique NPCs are always resident and only their 3D unloads, which is why `GetDead` works as a condition on
  them anywhere. The fix is retaining `entry.refID` from the `LCUN` walk and reading
  `ActorState::GetLifeState()`, not a death-event sink. See
  [`PHASE_13_MILESTONE_2.md`](PHASE_13_MILESTONE_2.md).
- Game-time deltas are clamped to 3 days per poll. A larger jump is logged as a `catch-up` NOTE line rather
  than credited in full, so a `set timescale` experiment cannot schedule an unbounded burst.

**Goal:** Observe the model in the real engine and compare it against the offline predictions. This is the
gating step for the milestone.

#### Setup

1. **Use a throwaway save.** Stub memories are written to SkyrimNet's database and are not cleaned up.
2. Set in `Data/SKSE/Plugins/NarrativeEngine.ini`:

   ```ini
   [Gossip]
   bGossipEnabled=true
   bGossipLogEnabled=true
   bGossipSeedStubsOnLoad=true
   iGossipStubSeedCount=12
   ```

3. Rebuild and deploy — `statics/` changes only reach the mod folder through `build.ps1`. An edit to the INI
   in the source tree that has not been built is not live.
4. Load the save. **Stand somewhere quiet and unpopulated** — an owned house or an empty wilderness cell.
   Nothing about the simulation depends on where the player is, and being somewhere dull makes that easy to
   confirm.
5. Confirm `NarrativeEngine_Gossip.log` appears in your SKSE log folder with 12 `SEED` lines.

#### How much in-game time to wait

The offline run put median rumor duration at **4.8 game days**, p90 at **9.0**, and maximum at **16.8**. To
see essentially every rumor through to burnout, allow **20 in-game days**. Run both methods below — they
exercise different code paths and the difference between them is itself a result.

**Method A — repeated waits (fast; exercises the catch-up path).**

Wait 24 hours at a time using the wait menu (`T`). After each wait, **stand still for about 30 real seconds**
before waiting again. The simulation advances on sampled game time but drains its event queue at
`iGossipMaxEventsPerTick` per real-time tick, so the transmissions caused by a 24-hour jump land over the
following few real seconds. Waiting repeatedly without pausing will not lose events — the queue is
persistent — but it makes the log harder to read and delays the drain.

Twenty waits ≈ 20 game days ≈ 12 real minutes including the pauses.

**Method B — accelerated time (realistic; exercises the steady state).**

In the console:

```text
set timescale to 2000
```

At timescale 2000, one real minute is about 33 game hours, so **20 game days takes about 15 real minutes**.
Walk around normally during this. When finished:

```text
set timescale to 20
```

20 is the vanilla default. Setting it back matters — timescale is global game state and leaving it at 2000
will make the rest of that save unplayable.

Method B is the more faithful test because events become due gradually rather than in bursts. Method A is
the better stress test. A material difference in outcome between the two is a finding worth reporting.

#### What to check

Read `NarrativeEngine_Gossip.log`, primarily the `BURNOUT` lines, which carry a full per-rumor summary each.

1. **Reach.** Offline: median 13, p90 36, max 100, and *nothing* above 150. A rumor reaching several hundred
   people means the contact model is running per-pair somewhere — the failure mode Part 2 describes.
2. **Duration.** Offline: median 4.8 game days, p90 9.0. Everything burning out in under a day means
   notability is decaying too slowly or the quota is too generous.
3. **Nothing runs away, nothing dies instantly.** Some rumors dying at 2–3 people is correct (offline: 11%).
   All of them dying at 2–3 is a broken graph — check the Step 2 census first.
4. **Hold crossings.** Offline: 83% of rumors never left their origin hold; 2.3% reached three or more.
   `grep XHOLD` and confirm the channel attribution is roughly faction 78% / relationship 16% / province 6%.
   Heavy province attribution means `fGossipProvinceShare` is too high.
5. **Notability slices behave differently.** The 0.9–1.0 seeds should visibly outrun the 0.2–0.3 seeds. If
   they do not, notability is not reaching the transmission roll.
6. **Small settlements stay local.** Offline: ≤5 residents → median reach 7 over 3.6 days. They should
   saturate quickly and stop.
7. **The player is irrelevant.** Confirm rumors propagate in holds you never visited during the run. This is
   the claim that the whole "runs regardless of where the player is" design rests on.
8. **Cost.** Count `TELL` lines. Offline predicts ~16 transmissions per rumor, so ~190 transmissions and ~380
   stub memories across 12 seeds. Confirm the memory writes actually landed via `GetMemoriesForActor` on a
   couple of named carriers.
9. **No stutter.** Watch for frame hitches during the drain after a long wait. The simulation should be
   invisible; if it is not, `iGossipMaxEventsPerTick` is the knob.
10. **Save/load mid-run.** Save at roughly day 10, reload, and confirm the in-flight rumors resume rather than
    restarting or vanishing.

#### Success criteria

- Every seeded rumor reaches burnout or is still plausibly live at day 20; none wedge.
- Reach distribution is recognisably the offline shape — most rumors in single digits to low tens, a small
  tail into the dozens, nothing in the hundreds.
- Channel attribution of hold crossings is faction-dominated.
- No main-thread stutter, no crash, no co-save growth beyond the design budget.
- The graph census at Step 2 matches the offline figures within a margin the load order explains.

Record the actual numbers against the offline predictions in
[`tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md`](tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md)
under a new "In-game run" section. Where they diverge, the in-game numbers win — the offline model was
always a stand-in for this.

---

## Done condition

Milestone 1 is complete when:

- All 9 steps are checked off.
- Step 9's run passes its success criteria, with the observed numbers recorded in the validation log.
- The runtime graph census is within an explicable margin of the offline measurements.
- The gossip log is complete enough that a run can be assessed from it alone, without reading the main log.
- Stub memories are greppable by their `[NE-GOSSIP-STUB` prefix and their `stub` tag, so Milestone 2 can find
  and purge every one.
- `bGossipEnabled` ships `false`, so no player gets the harness by accident.

Milestone 2 — real seeding, LLM generation bands, relationship-aware framing, and the dialogue surface — is a
separate phase document, written after Step 9's data is in.

---

## Appendix — measured vanilla data

Read out of the Spriggit export at `C:\Projects\spriggit-output\` or the CommonLibSSE-NG headers, and
recorded here so the design's assumptions can be rechecked rather than re-derived.

All figures below are the output of `scripts/build-social-graph.py` over the full load order
(Skyrim + Update + Dawnguard + HearthFires + Dragonborn), unless a row says Skyrim.esm only.

| Quantity                              | Value                                                                         |
| ------------------------------------- | ----------------------------------------------------------------------------- |
| Location records, merged              | 763 (638 in Skyrim.esm, 617 with a `ParentLocation`)                          |
| Locations carrying `LCUN` after merge | 151, holding 911 rows                                                         |
| ...before the additive merge (wrong)  | 124 locations, 388 rows                                                       |
| NPC records, merged                   | 6362                                                                          |
| ...flagged `Unique`                   | 1285                                                                          |
| ...that are actually people           | 1034 (251 presets / creatures / unnamed dropped)                              |
| ...resolved to a location             | 918 (88.8%): 861 via `LCUN`, 57 via placement cell, 0 via persistent          |
| **Participants (any tier)**           | **857 (82.9%)**                                                               |
| ...with a household node              | 695                                                                           |
| Households                            | 249 distinct; median 2, mean 2.8, max 12                                      |
| Settlements                           | 58 distinct; median 4, mean 12.8, max 90 (Riften)                             |
| Holds                                 | 10, from 39 (Hjaalmarch) to 136 (Rift) participants                           |
| Relationship records                  | 673 (Skyrim 642, DB 27, HF 3, DG 1); 636 with both ends in the graph          |
| Relationship ranks present            | Ally 307, Friend 152, Confidant 89, Acquaintance 68, Rival 48, Foe 8, Enemy 1 |
| Relationship ranks absent in vanilla  | Lover, Archnemesis                                                            |
| Relationship edge span                | 371 same-household, 212 same-settlement, 28 same-hold, **25 cross-hold**      |
| Factions with >=1 participant         | 676                                                                           |
| ...surviving size 3-40 + denylist     | 113, admitting 2687 pairs (size alone: 230 factions, 10001 pairs)             |
| ...of those, spanning >1 hold         | 13                                                                            |
| Personal-edge degree                  | mean 5.5, median 3, max 34; 77% of participants have >=1                      |
| Reachable peers per participant       | mean 40.8, median 34, max 95; Gini 0.46                                       |

Largest households: Candlehearth Hall 12, Understone Keep 12, Dark Brotherhood Sanctuary 11, Dragonsreach 11,
Windpeak Inn 10, Jorrvaskr Basement 10, Bards College 10, Ragged Flagon 10. Largest settlements: Riften 90,
Solitude 82, Whiterun 74, Markarth 74, Windhelm 64, Dawnstar 35, Raven Rock 25.
Relevant CommonLibSSE-NG bindings, all confirmed present:

| Symbol                                                | Use                                                |
| ----------------------------------------------------- | -------------------------------------------------- |
| `BGSLocation::parentLoc`, `::keywordData`             | Tier tree and tier classification                  |
| `BGSLocation::uniqueNPCs` (`BSTArray<UniqueNPCData>`) | Residence, without loading cells                   |
| `TESObjectREFR::GetEditorLocation()`                  | Residence fallback                                 |
| `BGSRelationship::GetRelationship(npc1, npc2)`        | Pair lookup, sees runtime marriage                 |
| `BGSRelationship::RELATIONSHIP_LEVEL`                 | Rank enum, matches design one-for-one              |
| `BGSAssociationType::associationLabels[2][2]`         | Gendered kinship labels for memory framing         |
| `TESDataHandler::GetFormArray<T>()`                   | Bulk enumeration of Locations, NPCs, Relationships |

Keyword FormIDs used for tier classification. All are `Skyrim.esm` except `BYOH_LocTypeHomestead`, which is
`HearthFires.esm`:

| Tier       | Keyword                   | FormID     | Keyword                    | FormID     |
| ---------- | ------------------------- | ---------- | -------------------------- | ---------- |
| Hold       | `LocTypeHold`             | `0x016771` | `LocTypeHoldMajor`         | `0x0868E1` |
| Hold       | `LocTypeHoldMinor`        | `0x0868E3` |                            |            |
| Settlement | `LocTypeHabitation`       | `0x039793` | `LocTypeHabitationHasInn`  | `0x0A6E84` |
| Settlement | `LocTypeCity`             | `0x013168` | `LocTypeTown`              | `0x013166` |
| Settlement | `LocTypeSettlement`       | `0x013167` | `LocTypeHoldCapital`       | `0x0868E2` |
| Household  | `LocTypeDwelling`         | `0x0130DC` | `LocTypeHouse`             | `0x01CB85` |
| Household  | `LocTypeInn`              | `0x01CB87` | `LocTypeStore`             | `0x01CB86` |
| Household  | `LocTypeGuild`            | `0x01CD5A` | `LocTypeTemple`            | `0x01CD56` |
| Household  | `LocTypeCastle`           | `0x01CD57` | `LocTypeBarracks`          | `0x01CD55` |
| Household  | `LocTypeStewardsDwelling` | `0x0504F9` | `LocTypePlayerHouse`       | `0x0FC1A3` |
| Household  | `LocTypeFarm`             | `0x018EF0` | `LocTypeLumberMill`        | `0x018EF2` |
| Household  | `LocTypeMine`             | `0x018EF1` | `LocTypeJail`              | `0x01CD59` |
| Household  | `LocTypeMilitaryCamp`     | `0x0130E8` | `LocTypeMilitaryFort`      | `0x0130E7` |
| Household  | `LocTypeBanditCamp`       | `0x0130DF` | `LocTypeForswornCamp`      | `0x0130EE` |
| Household  | `LocTypeVampireLair`      | `0x0130EB` | `LocTypeWarlockLair`       | `0x0130EC` |
| Household  | `LocTypeShip`             | `0x01CD5B` | `BYOH_LocTypeHomestead`    | `0x004D57` |
| Both       | `LocTypeOrcStronghold`    | `0x0130E9` | *(settlement + household)* |            |

Keyword placement facts worth not re-deriving: `LocTypeHoldMajor` sits on three hold locations (Reach, Rift,
Whiterun) and `LocTypeHoldMinor` on four (Falkreath, Hjaalmarch, Pale, Winterhold) — both are hold-tier, not
settlement-tier, despite the naming. `LocTypeHoldCapital` appears on zero `Skyrim.esm` locations.
`LocTypeFarm` covers 17 locations; `LocTypeOrcStronghold` covers four (Dushnikh Yal, Largashbur, Mor Khazgur,
Narzulbur).
