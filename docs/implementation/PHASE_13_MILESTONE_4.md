# Phase 13 — Milestone 4: Even Sampling

Milestone 2 made rumors real: harvested from SkyrimNet memories, claimed so nothing seeds twice, and given
generated text. What it did not do is choose *whose* memories get considered in a way that reflects a living
province. Every rumor on the first working run came from the handful of people the player had just been
adventuring with. This milestone replaces the selection mechanism so that the whole population gets turns.

Prerequisites, all complete:

- [`PHASE_13_GOSSIP_PROPAGATION.md`](PHASE_13_GOSSIP_PROPAGATION.md) — the social graph and the Milestone 1
  plan.
- [`PHASE_13_MILESTONE_2.md`](PHASE_13_MILESTONE_2.md) — harvesting, the claim ledger, content generation,
  and the discretion/duplication verdicts.
- [`PHASE_13_MILESTONE_3.md`](PHASE_13_MILESTONE_3.md) — the dedicated gossip simulation thread. Not a
  prerequisite in the strict sense, but it lands first: bucket sweeps raise the per-sweep query count from
  25 to ~88, which is exactly the cost this milestone's chunking was invented to spread and which Milestone 3
  handles structurally instead.

> **Doc status: design only.** No implementation plan yet, and nothing here is built. One thing described
> below as a problem — the harvest accumulator firing twice per interval — has already been fixed, and is
> recorded here only because it distorted the measurements this design is based on.

---

## The problem

`GossipHarvest` selects who to look at by calling `GetActorEngagement(0, …)` and ranking every active actor
by `recentMemoryImportanceMedium`, keeping the top 25. That ranking is the whole difficulty:
**`GetActorEngagement` is a measure of interaction with the player.** Ranking by it can only ever surface
people the player has recently been near, so the gossip mill reports on the player's own itinerary back to
them.

Measured against a real save, the pool it draws from is far narrower than the graph suggests:

| Quantity                                     | Value     |
| -------------------------------------------- | --------- |
| Graph participants                           | 881       |
| Participants holding **any** seedable memory | **41**    |
| Seedable memories in total                   | 323       |
| Share held by the single most active actor   | **24.1%** |
| Share held by the top 3                      | **54.8%** |
| Share held by the top 10                     | 78.6%     |

Because selection is a global ranking over that distribution, the same few people win every sweep, and the
other ~840 participants are structurally incapable of ever seeding a rumor no matter how long the game runs.
The first working run bears it out exactly: ten rumors, from six people, all inside the player's current
questline.

Note also that raising the sample size does not fix this. The problem is not that the top-25 cut is too
tight — it is that the ranking function answers the wrong question.

---

## Scope

### In scope

1. **Bucketed participants.** Every graph participant is assigned to one of N buckets at build time.
2. **Bucket-at-a-time harvesting.** A sweep considers exactly one bucket, and looks at every participant in
   it rather than a ranked subset.
3. **`GetActorEngagement` removed from harvesting.** Selection no longer consults player interaction at all.
4. **Deterministic-but-unpredictable bucket order**, with a persisted rolling history.
5. **Chunked sweeps** across Tick polls, so a bucket's per-actor queries are not all done in one poll.

### Deferred (explicitly out)

- **Weighting buckets by anything.** Buckets are drawn without regard to how much material they hold. A
  bucket that produces nothing is a valid outcome — see below.
- **Rebalancing buckets as memories accumulate.** Assignment is by identity, fixed for the life of a save.
- **Changing qualification, claims, or the seed call.** Milestone 2's rules are untouched; only the question
  of *whose memories are examined* changes.

---

## Design

### Buckets are the affordability mechanism, not a randomiser

The obvious fix to a biased ranking is to stop ranking and simply look at everyone. That was considered in
Milestone 2 and rejected on cost: `PublicGetMemoriesForActor` is per-actor, so a province-wide sweep would
mean ~880 calls into SkyrimNet's vector database every harvest.

Bucketing is what makes the unbiased approach affordable. Splitting the population ten ways turns one
impossible sweep into ten tractable ones, and every participant still gets examined — just not all in the
same sweep. The apparent randomness of the draw order is a property of *how the buckets are walked*, and is
secondary; the reason buckets exist at all is that they make engagement-free selection possible.

This is worth stating plainly because it determines what "correct" means here. The measure of success is not
that rumor origins look shuffled. It is that a farmer in Rorikstead with a notable memory has the same
prospect of seeding a rumor as an Arch-Mage the player talks to daily.

### Assignment: hash the base form, do not take a modulo of it

Each `GossipGraph::Participant` gains a bucket index, computed once during the graph build from the TESNPC
base FormID and stored on the participant.

**The FormID must go through a proper mixer — `formId % bucketCount` is wrong.** FormIDs carry the load
order's mod index in their high byte and run sequentially within a plugin, so a raw modulo correlates
strongly with which plugin a record came from. The result would be buckets that are effectively "everyone
from Dawnguard" and "everyone from Update.esm", which is a worse bias than the one being removed. A
finalising mix (splitmix64, FNV-1a, or equivalent) over the FormID gives low bits that are actually
distributed.

Assignment must be **stable across sessions**, which a hash of the base FormID is: the graph is rebuilt at
`kDataLoaded` every session, and the same participant lands in the same bucket every time without anything
being persisted.

### Selection: random, minus a rolling history

Each sweep draws a bucket at random from those **not among the last N selected**, using the RNG seeded from
`iGossipRandomSeed` so a run is reproducible.

This gives an order that looks arbitrary while guaranteeing spread, and it degrades into a cycle naturally
as the history lengthens. With ten buckets and a history of nine, exactly one bucket is eligible at each
step, so the order becomes a fixed repeating cycle after the first ten draws — the same result as
pre-shuffling a permutation, arrived at without special-casing. A shorter history leaves genuine choice at
every step while still preventing a bucket from coming up twice in quick succession.

Two decisions that make the history behave:

- **Every *selected* bucket is recorded, whether or not it produced a rumor.** Recording only productive
  buckets would leave a currently-empty bucket permanently eligible, so it could be drawn again and again
  while the rest of the population waited.
- **The history persists in the co-save.** Its own record type, one entry per remembered selection, with
  anything older than the most recent N pruned on write. Without persistence a reload could immediately
  re-draw the bucket just used, and a validation run would stop being reproducible.

### An empty bucket is a correct outcome, not a gap to paper over

A bucket whose participants hold no qualifying memories produces no rumor that sweep. **This is the intended
behaviour and should not be worked around** by scanning forward to the next non-empty bucket.

Early in a playthrough only a handful of characters have any memories at all. Skipping to them would mean
that handful's experiences get amplified across the whole province — the exact concentration this milestone
exists to remove, reintroduced at the point where it does the most damage to the fiction. Sparse rumors
during a quiet opening are the better outcome: there genuinely is not much for the province to talk about
yet.

The condition is also self-limiting. As later phases add background simulation that generates memories from
sources having nothing to do with the player, empty buckets become rarer and shorter-lived on their own.

On the current save the question is moot in any case — hashing the 41 memory-holding actors ten ways leaves
**zero empty buckets**, though the distribution is uneven (one bucket holds a single memory, another holds
98).

### Sweeps are chunked across polls

A bucket holds roughly `participants / bucketCount` actors — about 88 at the default. Querying all of them
in a single poll would hold the harvest mutex for the duration of ~88 vector-database round trips.

The sweep is therefore spread over consecutive Tick polls in the house accumulator style: a bounded number
of actors per poll, accumulating candidates, and the seeding decision made once the bucket is exhausted.
Each poll stays short and the work is unchanged in total.

**This does not exist today.** `RunSweepLocked` performs the engagement call, all 25 per-actor calls, and
seeding in one poll while holding the mutex — the Tick accumulator only decides *when* a sweep begins, not
how the work inside one is paced.

### What this removes

Dropping the engagement call takes several things with it:

| Removed                             | Why                                                            |
| ----------------------------------- | -------------------------------------------------------------- |
| `GetActorEngagement` call           | The biased ranking is the thing being replaced                 |
| `ResolveEngagementRow`              | Nothing needs resolving; participants come from the graph      |
| `iGossipHarvestActorSampleSize`     | There is no actor ranking left to take a top-N of              |
| `not-participant` rejection counter | Everyone in a bucket is a participant by construction          |

Ranking collapses from two levels to one. Today actors are ranked by engagement and then their memories are
ranked by importance; afterwards there is only the memory-level sort by `importance_score`, which is simpler
and is the axis that actually matters.

`GossipGraph::FindByActorRef` becomes unused by the harvester. It should be checked for other callers before
removal — the reverse index may still earn its place elsewhere.

### What this costs, and the lever

Per-sweep per-actor calls rise from 25 to roughly 88, because a bucket is examined in full rather than
sampled. **Bucket count is the direct control over that cost**, trading against how long any given NPC waits
for a turn:

| Buckets | Calls per sweep | A given NPC's turn comes up | At a 12-game-hour interval |
| ------- | --------------- | --------------------------- | -------------------------- |
| 10      | ~88             | 1 sweep in 10               | ~5 game days               |
| 20      | ~44             | 1 sweep in 20               | ~10 game days              |
| 40      | ~22             | 1 sweep in 40               | ~20 game days              |

All 881 participants currently resolve to a placed reference (`participantsWithActorRef=881`), so there is no
population that is unreachable for querying.

One measurement caveat worth recording: until recently the harvest accumulator fired **twice** per interval,
because it clamped its carry-over to exactly one interval and so re-crossed the threshold on the very next
poll. Every observed sweep count, query count and seeding rate from before that fix is therefore double what
the configuration implies. It is fixed; the numbers in this document already account for it.

### The trade being accepted

Today the globally most important memory in the province is the one that seeds. Afterwards, the most
important memory *in the chosen bucket* seeds, which will sometimes be mediocre while something far better
sits unexamined in another bucket for several more sweeps.

That is the intended exchange — variety and reach in place of peak quality per rumor — and it is worth
naming so that a future "why did it gossip about something dull when X had just happened" has an answer
already written down.

---

## Settings

| Key                            | Proposed default | Meaning                                                        |
| ------------------------------ | ---------------- | -------------------------------------------------------------- |
| `iGossipHarvestBuckets`        | 10               | How many buckets the population splits into; the cost lever    |
| `iGossipBucketHistoryLength`   | 6                | How many recent selections are excluded from the next draw     |
| `iGossipHarvestActorsPerPoll`  | 12               | Actors queried per Tick poll while working through a bucket    |

Removed: `iGossipHarvestActorSampleSize`.

Unchanged: `iGossipRandomSeed` continues to seed the draw, so a run remains reproducible.

---

## Open questions

1. **What history length reads best?** Nine of ten is a fixed cycle; one of ten barely constrains anything.
   Six is a guess at the midpoint and only play will tell whether the order feels arbitrary enough.
2. **What happens when `iGossipHarvestBuckets` changes mid-playthrough?** Every participant is reassigned,
   and the persisted history refers to indices that no longer denote the same people. Probably acceptable —
   the effect is one disordered cycle — but it should be a deliberate answer rather than an accident.
3. **Should a bucket's sweep be abandoned if the game is saved or loaded mid-chunk?** A partially-walked
   bucket has no persisted state under this design, so a reload restarts it. That is likely fine, but it
   means frequent saving could starve later members of a bucket.
4. **Is 12 game hours still the right interval** once each sweep covers a tenth of the province rather than
   the top 25 by engagement? The question this milestone answers is who gets examined, not how often — but
   the two interact, and the answer that felt right for a biased sample may not suit an even one.
