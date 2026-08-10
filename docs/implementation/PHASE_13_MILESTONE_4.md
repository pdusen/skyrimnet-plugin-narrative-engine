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
- [`PHASE_13_MILESTONE_3.md`](PHASE_13_MILESTONE_3.md) — the dedicated gossip simulation thread, and a hard
  prerequisite for this one. Bucket sweeps raise the per-sweep query count from 25 to ~88; Milestone 3 is
  what makes that free, by moving the whole tick onto a thread nothing waits on.

> **Doc status: planned, not built.** One thing described below as a problem — the harvest accumulator firing
> twice per interval — has already been fixed, and is recorded only because it distorted the measurements
> this design is based on.

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

### Assignment: hash the base form, do not comb it

Each `GossipGraph::Participant` gains a bucket index, computed once during the graph build from the TESNPC
base FormID and stored on the participant.

**The FormID goes through a finalising mix (splitmix64) before the modulo** — but not for the reason first
written down here, which measurement did not support. The original claim was that `formId % bucketCount`
correlates with the load order, giving buckets that are effectively "everyone from Dawnguard". It does not:
the mod index is in the FormID's high byte and a modulo reads the low bits, so it never sees the master.
Across all 6,525 vanilla + DLC NPC records the two assignments are indistinguishable — 10.4% vs 12.0% size
spread at ten buckets, and the same master mix in every bucket.

What a bare modulo *is* is a **comb over FormID order**. Records run near-sequentially inside a plugin and
the CK author adds a settlement's people together, so consecutive ids round-robin through the buckets in
lockstep. Ivarstead's eleven NPCs hit 8 of 8 buckets under a modulo and 5 of 8 under the mix — perfect
spread is what a comb does, and lumpiness is what randomness does.

That comb is a dependency on how somebody numbered records. It happens to hold for vanilla and cannot be
audited for mod-added NPCs, whose allocation strides are arbitrary: a follower pack adding ten NPCs on a
stride of ten puts every one of them in the same bucket. Three multiplies removes the question permanently,
which is the entire argument. It is insurance, not a fix for an observed bias.

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

Three decisions that make the history behave:

- **Every *selected* bucket is recorded, whether or not it produced a rumor.** Recording only productive
  buckets would leave a currently-empty bucket permanently eligible, so it could be drawn again and again
  while the rest of the population waited.
- **The history persists in the co-save**, as a bounded `std::deque<std::uint32_t>` on `GossipState`,
  written inside the existing gossip record at a bumped version. Without persistence a reload could
  immediately re-draw the bucket just used, and a validation run would stop being reproducible.

  It goes on `GossipState` rather than into a record of its own because `GossipState.h` states the rule
  directly: everything the simulation owns is saved from a single snapshot, so two facts can never be read
  from different instants. A separate record would allow exactly the drift that rule exists to prevent — a
  restored bucket history that disagrees with the rumors it produced.

- **The bucket count is saved beside the history, and a mismatch on load discards it.** Change
  `iGossipHarvestBuckets` mid-playthrough and every participant is reassigned, so the remembered indices no
  longer denote the same people — excluding them would exclude an arbitrary set. Discarding costs one
  unusually clustered cycle, needs no reasoning at the call site, and is logged so it does not read as a bug
  later.

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

### A sweep runs whole, and does not need pacing

A bucket holds roughly `participants / bucketCount` actors — about 88 at the default, against the 25 a
ranked sample examines today. That is three and a half times the per-actor queries in one sweep, and it
needs no accommodation whatsoever.

A tick is a single job on `GossipDispatch`'s dedicated thread, running harvest → generate → simulate → prune
→ publish start to finish with no time budget and nothing else queued behind it. The plugin thread's only
involvement is `GossipTick::Poll` deciding *when* to enqueue. An 88-actor sweep therefore blocks nobody, and
pacing it across polls would mean carrying resumable per-bucket state — which a load would have to cancel
and reconcile — to solve a problem that does not exist.

The cost is real but it is paid somewhere harmless: a bucket sweep takes longer in wall-clock time than a
top-25 sweep does. Bucket count is the lever for that, trading latency-to-first-rumor rather than frame
time.

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

Removed: `iGossipHarvestActorSampleSize`.

Unchanged: `iGossipRandomSeed` continues to seed the draw, so a run remains reproducible.

---

## Implementation plan

Ordered so that **the selection mechanism is fully built and observable before anything starts using it**.
Steps 1 and 2 are pure addition: the buckets exist, a bucket is drawn every tick, and the trace says which —
while the harvester carries on ranking by engagement exactly as it does today. That means the draw can be
watched across a long run, and its distribution argued with, before a single rumor depends on it.

Step 3 is the switch, and it is one call site. Step 4 is pure subtraction and is safe precisely because
Step 3 landed first.

---

### Step 1 — Buckets on the graph

- [x] Complete

**Goal:** Every participant has a bucket, assignment is stable across sessions, and the distribution can be
inspected. Nothing reads it yet.

1. `iGossipHarvestBuckets` (default 10) in `Settings`, the INI, and the INI's documented block.
2. A `bucket` field on `GossipGraph::Participant`, assigned during the graph build.
3. The assignment is `splitmix64(npc) % bucketCount` — **a finalising mix, never a bare modulo.** A bare
   modulo is a comb over FormID order, which couples bucket membership to how records were numbered; see the
   assignment section above for the measurements. Take the mixer as its own small `constexpr` function so
   the reason it exists is readable at the call site.
4. `GossipGraph::BucketMembers(std::uint32_t index)` returning a prebuilt `const std::vector<RE::FormID>&`,
   built once during `Initialize` alongside the household/settlement/hold indices it mirrors. A sweep must
   not scan 881 participants to find the ~88 it wants.
5. The startup census gains the per-bucket population, logged at `Initialize`.

Assignment is deliberately **not persisted**. The graph is rebuilt at `kDataLoaded` every session and a hash
of the base FormID is stable, so persisting it would only create an opportunity for the saved answer and the
computed one to disagree.

**Verification:** the census shows all N buckets non-empty with populations within roughly ±30% of
`participants / N`. Note that a size histogram cannot tell the mixer from a bare modulo — both are even, and
the modulo is marginally *more* even, because a comb is. What the census actually checks is that the
assignment ran and that no bucket starved.

Done. `Participant::bucket`, `GossipGraph::BucketMembers`, `GossipGraph::BucketCount` and
`iGossipHarvestBuckets` are in; `g_buckets` is built in the same pass as the household/settlement/hold
indices, and `LogCensus` prints every bucket's population.

The mixer's justification was tested against the Spriggit export before the comment was written, and **the
rationale in this doc was wrong** — corrected above. Both assignments were run over all 6,525 vanilla + DLC
NPC records:

| Buckets | Bare modulo spread | splitmix64 spread |
| ------- | ------------------ | ----------------- |
| 10      | 10.4%              | 12.0%             |
| 16      | 16.4%              | 12.3%             |
| 20      | 20.8%              | 18.7%             |

No load-order bias in either, at any bucket count. The mix stays because a bare modulo combs FormID order
and that coupling cannot be audited for mod-added NPCs — not because vanilla showed a problem.

---

### Step 2 — Bucket selection, with a rolling history that persists

- [x] Complete

**Goal:** Every tick draws a bucket and records it. The choice is traced and saved, and still nothing
consumes it.

1. `iGossipBucketHistoryLength` (default 6) in `Settings` and the INI.
2. `bucketHistory` as a `std::deque<std::uint32_t>` on `GossipState`, plus the `bucketCount` the history was
   recorded under. It rides the existing co-save record — bump `kRecordVersion` to 6 — because the history
   and the rumors it produced must be restorable from the same instant.
3. On load, **discard the history if the saved `bucketCount` differs from the configured one.** The indices
   no longer denote the same people, and a mid-playthrough setting change is exactly when a stale history is
   most misleading. Log it; a silent reset would look like a bug later.
4. The draw: uniform over the buckets **not** in the history, using the RNG seeded from `iGossipRandomSeed`.
   Clamp the effective history length to `bucketCount - 1` so that a history at least as long as the bucket
   count cannot leave the eligible set empty.
5. Called once per tick from `RunSweepImpl`, in the same place `++g_stats.sweeps` already sits — that is,
   **after** the `IsReady()` / `IsMemorySystemReady()` checks pass. Recording a selection the sweep then
   abandoned would spend a bucket's turn on nothing.
6. Every drawn bucket is recorded **whether or not it produced a rumor.** Recording only productive buckets
   would leave a currently-empty bucket permanently eligible, so it could be drawn repeatedly while the rest
   of the population waited.
7. A `NOTE` line per tick naming the bucket, its population, and the history it was drawn against.

**Verification:** run 30+ ticks with the harvester untouched and read the trace. No bucket repeats inside
the history window; every bucket appears at a comparable rate over the run; the order does not look like a
counting sequence. Save mid-run, reload, and confirm the next draw respects the restored history rather than
re-drawing what just came up. Change `iGossipHarvestBuckets` on an existing save and confirm the history is
discarded with a log line rather than reused.

Done. `bucketHistory` and `bucketCount` are on `GossipState` and ride the gossip record at
`kRecordVersion = 6`. `DrawBucket` sits in `GossipHarvest`, called beside `++g_stats.sweeps` — after the
ready checks, because a bucket's turn is spent by being drawn and a draw above the checks would burn one on
a sweep that then bailed.

The count-changed check exists in **two** places, not one. The load path covers a save made under a
different `iGossipHarvestBuckets`; `DrawBucket` covers settings reloaded mid-session, which reaches the same
inconsistent state with no serialisation callback involved. Both log.

The load reads the history bytes **whether or not it intends to keep them** — the rumor count follows them
in the stream, so skipping the read would desynchronise every field after it. Whether to keep is decided
after reading.

The draw was modelled over 300 iterations to check the properties above:

| Buckets | History | Clamped to | Min gap between repeats | Per-bucket count | Fixed cycle |
| ------- | ------- | ---------- | ----------------------- | ---------------- | ----------- |
| 10      | 6       | 6          | 7                       | 27-34            | no          |
| 10      | 9       | 9          | 10                      | 30-30            | yes         |
| 10      | 20      | 9          | 10                      | 30-30            | yes         |
| 10      | 0       | 0          | 1                       | 24-36            | no          |
| 1       | 6       | 0          | 1                       | 300              | yes         |

The default (10/6) never repeats inside its window, spreads within +/-12% over 300 draws, and does not
settle into a cycle. `bucketCount - 1` produces the fixed cycle the design predicts, an over-large setting
clamps onto it rather than emptying the eligible set, and a single bucket degenerates cleanly.

---

### Step 3 — Harvest the drawn bucket

- [x] Complete

**Goal:** The switch. Selection stops consulting player engagement.

1. `RunSweepImpl` collects from `GossipGraph::BucketMembers(drawn)` instead of from `RankActors`'s output.
   Every member is examined; there is no top-N cut, because there is no longer a ranking to take one of.
2. `CollectFrom` takes the participant directly. Its `RankedActor` parameter carried an `actorRef` and an
   `npc`, both of which are already on `Participant`.
3. The `HARVEST` trace line changes: `actors=25/144` described a sample of a ranking and now describes
   nothing. It becomes the bucket index and its population.
4. Memory-level ranking by `importance_score` is unchanged. Two levels of ranking collapse to one, and the
   surviving level is the axis that actually matters.

Everything downstream — qualification, claims, the eval/compose split, seeding — is untouched. This step
changes *whose* memories are examined and nothing else.

**Verification:** rumor origins over a long run come from across the province rather than from the player's
current questline. Concretely: compare the set of origin NPCs against the pre-change runs, where 13 of 14
seeds came from Winterhold College because that is where the player was. Per-sweep per-actor calls should
rise from 25 to roughly `participants / bucketCount`. Confirm a bucket holding no qualifying memories
produces no rumor **and is not skipped over** — the sweep ends quietly and the next tick draws a different
bucket.

Done. `RunSweepImpl` walks `GossipGraph::BucketMembers(bucket)` in full; `CollectFrom` takes a
`GossipGraph::Participant&` directly instead of the `RankedActor` pair. `RankActors` is now unreachable and
goes in Step 4.

Two consequences beyond the step as written:

- The `HARVEST` line's `actors=25/144` is gone, replaced by `bucket=N/M actors=P`. The old pair described a
  sample of a ranking, and both halves of that relationship have stopped existing. `actorsSeen` and
  `actorsSampled` survive on `HarvestStats` for one more step, unread, only so the dead ranking code still
  compiles.
- `Stats::actorsRanked` became `actorsExamined`, summing bucket populations. The name was describing the
  mechanism rather than the question, and the mechanism is what changed — the question ("how many
  participants has this session queried?") is the same one.

A participant whose graph entry carries no `actorRef` is skipped silently: there is no id SkyrimNet would
answer to, and it is a property of the graph rather than of the sweep. On a vanilla load order this is
never hit — all 881 resolve.

---

### Step 4 — Delete the engagement path

- [x] Complete

**Goal:** Pure subtraction. Nothing behaves differently.

Now dead, and removed:

| Removed                       | Why                                                   |
| ----------------------------- | ----------------------------------------------------- |
| The `GetActorEngagement` call | The biased ranking is the thing that was replaced     |
| `RankActors`, `RankedActor`   | No ranking left                                       |
| `ResolveEngagementRow`        | Participants come from the graph already resolved     |
| `rejectedNotParticipant`      | Everyone in a bucket is a participant by construction |

Also: the header comment on `GossipHarvest.h` opens by explaining harvesting as a two-call design — one
cheap global engagement call deciding where to spend the expensive per-actor ones. That rationale is gone
and the comment must be rewritten, not trimmed. It is the first thing anyone reads about this module.

`GossipGraph::FindByActorRef` loses its harvest caller. **Check for other callers before removing it** — the
reverse index may still earn its place elsewhere, and Milestone 3 leans on the base-form/placed-reference
distinction throughout.

`iGossipHarvestActorSampleSize` is deleted from `Settings`, `Settings.cpp` and the INI. A stale key left in
a shipped INI reads as a working knob.

**Verification:** behaviour identical to the end of Step 3 — same buckets drawn, same origins, same seeding
rate. `GetActorEngagement` appears nowhere in `GossipHarvest`. The build is clean and the INI documents no
setting the code does not read.

Done. `RankActors`, `RankedActor`, `ResolveEngagementRow`, `iGossipHarvestActorSampleSize` and the
`not-participant` counter are gone, along with the two `HarvestStats` fields Step 3 left alive to keep the
dead code compiling. `SkyrimNetAPI::GetActorEngagement` itself stays — `NPCLetterBeat` and
`SenderCandidatePool` are legitimate callers, and picking a letter's sender by player engagement is the
right question for a letter.

`GossipGraph::FindByActorRef` had exactly one caller and it was `ResolveEngagementRow`, so the reverse
index and `g_byActorRef` went with it. Every remaining crossing into SkyrimNet's id space runs
base-form-out through `ActorRefFor`; the reverse direction only existed because engagement rows arrived as
references. The `withActorRef` census count stays, since addressability is still worth knowing.

Three comments outside the harvester were describing the old design and were rewritten rather than
trimmed:

- **`GossipHarvest.h`'s opening rationale**, which explained the whole module as a two-call design. It now
  states the bucket scheme and, more usefully, why ranking was wrong in the first place.
- **`GossipTick.h`'s stamped-tick caveat.** It recorded one place a tick could not be exact: engagement
  reports as of *now*, so which actors got examined was current even when the memories were not. That
  exception no longer exists — the bucket depends on the saved history and nothing else — so a stamped tick
  is now stamped all the way down. This is a real correctness gain from Milestone 4 that its design did not
  anticipate.
- **The `Settings.h` and INI harvest preambles**, both of which opened by describing the two-stage sweep.

---

### Step 5 — In-game validation

- [ ] Complete

**[USER]**

Needs a save with an established memory corpus, and enough time passed to cover several full bucket cycles.
Note that **wait-driven time passage will not exercise this** — with no conversations happening there are no
new memories, so the sweep has only the existing corpus to draw on and the point of the milestone cannot be
observed. This wants real play, or at minimum a save whose corpus is already broad.

1. **Origins spread out.** The measure that matters. Rumor origins should reach people the player has never
   spoken to, in holds the player is not in.
2. **A farmer's odds match an Arch-Mage's.** Someone unremarkable with a notable memory seeds a rumor.
3. **Empty buckets pass quietly.** A sweep that finds nothing logs and ends. No skipping forward, no
   double-sweeping to compensate.
4. **The draw order still looks arbitrary** after the history has been running a while, and does not settle
   into an obvious cycle at the configured length. This is where open question 1 gets answered.
5. **Cost is acceptable.** ~88 per-actor queries per tick on the gossip thread. Confirm no stutter — there
   should be none, since nothing waits on that thread — and note how long a sweep takes end to end.
6. **Rumor quality is worse and that is expected.** The best memory in the drawn bucket seeds, not the best
   in the province. Confirm the drop reads as variety rather than as the system picking badly.

---

## Done condition

Milestone 4 is complete when:

- All 5 steps are checked off and Step 5 passes.
- Selection never consults `GetActorEngagement`; the call does not appear in `GossipHarvest`.
- Every graph participant is in exactly one bucket, and the assignment is identical across sessions.
- A bucket is never drawn twice within the history window, and the history survives a save/load round trip.
- Changing `iGossipHarvestBuckets` on an existing save discards the history with a log line.
- An empty bucket produces no rumor and is not skipped.
- `iGossipHarvestActorSampleSize`, `RankActors`, `RankedActor` and `ResolveEngagementRow` are gone.
- Rumor origins over a long run are distributed across the province rather than tracking the player.

---

## Open questions

1. **What history length reads best?** Nine of ten is a fixed cycle; one of ten barely constrains anything.
   Six is a guess at the midpoint and only play will tell whether the order feels arbitrary enough. Step 5
   is where the answer comes from.
2. **Is 12 game hours still the right interval** once each sweep covers a tenth of the province rather than
   the top 25 by engagement? The question this milestone answers is who gets examined, not how often — but
   the two interact, and the answer that felt right for a biased sample may not suit an even one. Left as a
   tuning question for after Step 5; nothing in the plan depends on it.
