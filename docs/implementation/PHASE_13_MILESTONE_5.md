# Phase 13 — Milestone 5: Tiered Contact Selection

Milestone 1 gave the simulation a contact model: every participant carries a flat list of weighted peers, and
a conversation is drawn by weight from that list. This milestone replaces it with a two-stage draw — pick a
proximity **tier** first, then pick a peer uniformly from within it.

Prerequisites, all complete:

- [`PHASE_13_GOSSIP_PROPAGATION.md`](PHASE_13_GOSSIP_PROPAGATION.md) — the social graph, the channels, and
  the SIR model this replaces the contact half of.
- [`PHASE_13_MILESTONE_4.md`](PHASE_13_MILESTONE_4.md) — bucket sampling, which decides whose memories seed.
  This milestone decides who a carrier then tells, and the two are independent.

> **Doc status: planned, not built.** Weights are given as relative descriptions rather than numbers; every
> figure in the existing model was tuned against flat-list draws and none of it carries over.

---

## Why the flat list is being replaced

In the current model, how often a carrier talks to each tier is not chosen — it emerges from how many peers
happen to sit in each one, multiplied by that tier's per-peer weight. A College mage with four housemates
carries 4 × 600 = 2400 units of household weight against fifteen settlement neighbours at 1.0 apiece, so
household takes 99.4% of their conversations. Nobody picked that ratio.

The same arithmetic makes absence catastrophic. Ancano shares the Hall of the Elements with two NPCs who do
not exist yet at his point in the questline; at 600 apiece they hold 98% of his contact weight, so 44 live
College mages standing around him divide 1.4% of his conversations between them.

Both are consequences of one property: a peer's share of a carrier's attention is set by their weight
relative to everyone else's, so population size and absence both distort the channel mix. Drawing the tier
first removes that coupling. Household is drawn at its stated rate whether the carrier has one housemate or
eight, and an absent housemate cannot consume attention that belongs to another tier.

---

## The model

Each carrier holds **five conversations per simulation step** (`fGossipStepDays`, currently a quarter day).
Each conversation is resolved in three parts.

### 1. Draw a tier

One of seven, by relative weight. The ladder alternates: the odd rungs are the geographic tiers, and the
even rungs between them exist **only** to receive peers moved by a faction or relationship.

| Tier  | Name       | Relative chance | Natural membership                                     |
| ----- | ---------- | --------------- | ------------------------------------------------------ |
| **1** | Household  | High            | Participants sharing the carrier's household location  |
| 2     | —          | Medium-high     | *(nobody — reached only by adjustment)*                |
| **3** | Settlement | Medium          | Participants sharing the carrier's settlement location |
| 4     | —          | Low-medium      | *(nobody — reached only by adjustment)*                |
| **5** | Hold       | Low             | Participants sharing the carrier's hold                |
| 6     | —          | Very low        | *(nobody — reached only by adjustment)*                |
| **7** | Province   | Minimal         | Any participant in Skyrim                              |

The in-between rungs are the point of the ladder. A guild-mate in another hold moved one rung closer lands
on tier 6 — drawn more often than a stranger, but *not* thrown in among the carrier's hundred-odd
hold-mates where a single distant contact would be lost in the crowd. Every adjusted peer gets a pool of
their own size rather than being absorbed into a geographic one.

Membership above is stated by location alone. Factions and relationships then move individuals between
these pools, as described under [Tier adjustment](#tier-adjustment) below; a peer belongs to exactly one
tier at a time.

### 2. Draw a peer

Uniformly at random from that tier's membership. Every member of a drawn tier is equally likely; a carrier
with eight housemates reaches each of them a quarter as often as a carrier with two, while the household
tier as a whole is drawn just as often for both.

### 3. Roll transmission

The rumor passes with probability equal to its **notability**, derived from the source memory's importance
score. Notability is a property of the rumor alone — it does not vary by tier, by peer, or by generation. A
story told to a housemate is no more likely to stick than the same story told to a stranger; what differs is
how often each conversation happens.

---

## Tier adjustment

A peer's **natural tier** is the closest one they qualify for by location. Two things then adjust it, and the
result is where they are actually drawn from.

**A shared faction** moves a peer one tier closer.

**A relationship link** moves them by their rank:

| Relationship rank                          | Effect            |
| ------------------------------------------ | ----------------- |
| Lover, Ally, Confidant                      | One tier closer   |
| Friend, Acquaintance                        | No change         |
| Rival, Foe, Enemy, Archnemesis              | One tier further  |

The two adjustments sum, so a faction-mate you are on bad terms with cancels out and stays where geography
put them — forced into each other's company by the organisation, avoiding each other by choice. Two positive
sources do not stack: a faction-mate who is also your sister is one tier closer, not two.

Movement is **one rung of the seven**, not one geographic tier. A peer never jumps from Settlement straight
to Household; they land on the rung between the two.

The result is clamped at both ends. Nothing rises above tier 1 and nothing falls below tier 7.

| Natural tier     | One closer | One further |
| ---------------- | ---------- | ----------- |
| 1 — Household    | 1 (stays)  | 2           |
| 3 — Settlement   | 2          | 4           |
| 5 — Hold         | 4          | 6           |
| 7 — Province     | 6          | 7 (stays)   |

Because only the odd rungs have natural membership, the even rungs hold exactly the people a carrier has a
reason to speak to more or less often than geography alone would suggest. Tier 2 is "closer than a
neighbour, not quite family"; tier 6 is "further than a hold-mate, closer than a stranger".

> **Rank order is not enum order.** `RELATIONSHIP_LEVEL` counts *upward as warmth decreases* —
> `kLover = 0` through `kArchnemesis = 8` — so "Confidant or higher" is the numerically lowest three values.
> Read the table above by name, never by comparing the enum.

Adjustment **moves** a peer between pools rather than adding weight anywhere. A carrier's household tier may
hold people who do not live with them, and their settlement tier may hold people who do. Tier membership is a
statement about how readily two people fall into conversation, not about where they sleep.

### Examples

**Siblings in different cities.** Two participants share a hold but not a settlement, so their natural rung is
5. A Confidant link moves them to 4: spoken to more often than an ordinary hold-mate, less often than a
neighbour on the same street.

**A mother and son in different houses in the same city.** Natural rung 3, moved by their link to 2 — closer
than the rest of the town, short of the household they do not actually share.

**A guild-mate three holds away.** Sharing neither household, settlement nor hold, their natural rung is 7;
the shared faction moves them to 6. This is how rumors are meant to leave the hold they were seeded in, and
the reason rung 6 exists at all: a handful of distant colleagues in a pool of their own are reachable, where
the same people mixed into a hold of a hundred would never come up.

**Two enemies under one roof.** Natural rung 1, moved one further by an Enemy link to 2. They still live
together and still speak — but as the least of a carrier's close ties rather than the first.

**A rival in the same guild.** The shared faction moves one closer and the Rival link moves one further. They
land on their natural rung, unchanged.

**An Archnemesis in another hold.** Natural rung 7, moved one further, clamped back to 7. The floor means
hostility can strip away the advantages of proximity but never make someone less reachable than a stranger.

---

## Empty tiers

A drawn tier with no members produces **no conversation**. The draw is spent, nothing is said, and the
distribution is not adjusted to compensate.

This is deliberate. A carrier with no household loses the household share of their conversations and keeps
everything else, so they talk less than someone with a family — which is the intended reading. Renormalizing
the remaining tiers would erase that difference and make a hermit exactly as talkative as the head of a large
household.

The rule covers absence as well as vacancy. A member who is dead, disabled, or otherwise unavailable is not a
candidate, and a tier whose entire membership is unavailable behaves as an empty one.

It applies with particular force to the even rungs, which have no natural membership at all: a carrier with
no factions and no relationships has nothing on tiers 2, 4 or 6, and every draw landing there is spent in
silence. Their weight is therefore not free. Whatever is given to the adjustment rungs is lost outright for
the unconnected, which is a deliberate cost — a person with no ties to anyone talks less than one who has
them — but it means those weights cannot be set without measuring how much of the population is connected at
all.

### Example

**Ancano.** His household holds two NPCs who do not exist yet, so every household draw is spent in silence.
His settlement, hold and province draws are unaffected, and he reaches the College through them. He is a
quieter gossip than a mage with four live housemates, and that is the correct outcome: he genuinely has
nobody at home. Under the flat-list model the same two absences cost him 98% of his conversations rather than
the household share alone.

---

## What this changes elsewhere

**The isolation check becomes a sum over four tiers.** The share of a carrier's conversations that reach
anybody is the total weight of the tiers holding at least one available member. It is no longer necessary to
walk every peer and weight their availability individually.

**Channel attribution becomes exact.** The tier a conversation was drawn from is the channel it travelled by,
recorded directly rather than inferred afterwards from which explanation best fits a peer reachable several
ways. Promotion is recorded alongside it, so a Household-tier conversation between people who live apart is
legible in the trace as what it is.

**Notability governs distance through longevity, not through reach per conversation.** The tier weights are
identical for every rumor, so a juicier story is no more likely to cross a hold in any single conversation.
It crosses more often because it keeps more carriers infectious for longer and therefore accumulates more
conversations overall. Whether that alone produces the intended "easily across two or three holds, rarely the
whole province" is a question for tuning, and making the tier weights themselves depend on notability is the
lever held in reserve if it does not.
