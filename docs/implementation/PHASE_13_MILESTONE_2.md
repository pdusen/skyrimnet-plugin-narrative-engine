# Phase 13 — Milestone 2: Real Seeding

Milestone 1 built a working propagation model and drove it with placeholder rumors planted at random. This
milestone replaces that harness with the real thing: NarrativeEngine watches SkyrimNet's memory database for
things worth talking about, turns qualifying memories into rumors, and never uses the same memory twice.

Prerequisites, all complete:

- [`PHASE_13_GOSSIP_PROPAGATION.md`](PHASE_13_GOSSIP_PROPAGATION.md) — the social graph, threading, stub
  memory scheme, the structural graph measurements, and the Milestone 1 plan.
- [`tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md`](tests/gossip-spread/PHASE_13_SIR_VALIDATION_LOG.md)
  — the SIR model and its calibration. **The propagation model is settled and this milestone does not touch
  it.**

> **Doc status: steps 1-8 and 10 implemented, Step 9 (in-game validation) outstanding.** Everything builds
> clean and
> is deployed. `bGossipEnabled` still ships `false`.
>
> One deviation from the design below, in Step 7: rather than seeding a rumor into a `Pending` state and
> transitioning it on the callback, the claim alone holds the memory while generation is in flight and
> `SeedRumor` is not called until the text arrives. Functionally equivalent, and it removes a partial rumor
> state that would otherwise have to be serialised. The trade-off is that a session ending mid-generation
> leaves the claim held with no rumor behind it — self-healing, since the claim expires on schedule.
>
> Two decisions settled after the first implementation pass:
>
> - **No gossip-specific LLM variant.** Generation runs under the existing `narrative_engine_composer`
>   variant already declared in the SkyrimNet manifest. Gossip generation is creative writing in a
>   specific voice, which is the same task shape the composer variant exists for, so a variant of its own
>   would mean another manifest entry and another override category for the user to tune with nothing
>   behind it.
> - **The prompt name and the variant are compiled in, not settings.** The prompt ships in `statics/` and
>   its contract with `GossipContent` — the context keys it reads, the JSON shape it must return — is
>   compiled in, so repointing the call at a different asset could only break it.

---

## What Milestone 1 leaves behind

Three in-game runs plus an offline calibration established that the SIR model behaves as designed: households
saturate (96–98% at every size band), settlements of 31+ residents reach ~30% and never saturate, holds never
saturate, notability discriminates cleanly at every tier, and a rumor that crosses a hold boundary
re-saturates the household it lands in.

What is still fake is everything *upstream and downstream* of that:

| Concern                   | Milestone 1 state                                                    |
| ------------------------- | -------------------------------------------------------------------- |
| Where rumors come from    | `GossipSeeder` plants them at random, stratified for test coverage   |
| What a rumor is *about*   | Nothing. There is no subject, only a notability number               |
| Memory text               | `[NE-GOSSIP-STUB rNN genN] I told X a rumor. (placeholder content…)` |
| Re-seeding the same story | No concept of a source, so nothing to duplicate                      |

---

## Scope

### In scope

1. **Remove `GossipSeeder`** and its settings entirely.
2. **Memory harvesting** — a periodic, province-wide sweep for notable recent memories, not scoped to any
   particular actor.
3. **Qualification** — which memories may become rumors, and at what notability.
4. **The claim ledger** — a memory that has become a rumor is marked, persisted, and never reused.
5. **Claim reaping** — expired claims are swept on the same poll that reaps burned-out rumors.
6. **Provenance** — a rumor records the memory it came from, and carriers record who told them.
7. **Rumor content** — real generated text that says what the rumor is, evolving as it travels.
8. **Dead NPCs still gossiping**, carried over from Milestone 1 — a smaller fix than its Milestone 1
   write-up claimed.

### Deferred (explicitly out)

- **Player-facing dialogue surface.** Still a separate concern; SkyrimNet's own retrieval already puts
  memories in front of the LLM.
- **`sociability` scaling.** Milestone 1's degree distribution produced good hubs without it; there is no
  evidence it is needed.
- **Curated faction allowlist.** The size band plus denylist is holding up; revisit only if a specific
  faction misbehaves.
- **Event-log seeding.** The phase doc originally proposed `CombatEventLog` / `TravelEventLog` / beat
  outcomes as the *primary* seed source. Memory harvesting supersedes it — SkyrimNet already records those
  same events as memories, so seeding from both would double-count: a dramatic fight would seed once from
  our event log and again from the memory it produced. Memory harvesting becomes the single source of truth.
  Revisit only if some notable event class turns out not to produce a memory at all.

---

## Design

### The harvest problem: there is no global memory query

`PublicGetMemoriesForActor(formId, maxCount, contextQuery)` is strictly per-actor. The only endpoint that
accepts `formId = 0` for "all actors" is `PublicGetDiaryEntries`, which returns diary entries — a different
thing, and one the existing letter and visit composers deliberately filter *out*.

So a province-wide sweep has to be assembled from two calls:

**Stage 1 — find where the notable memories are.** `PublicGetActorEngagement(0, …)` returns *every* actor
with any activity, and each row carries `memoryCount`, `totalMemoryImportance`, `recentMemoryImportanceShort`
and `recentMemoryImportanceMedium`. Setting `mediumWindowSeconds` to the harvest window makes
`recentMemoryImportanceMedium` exactly the ranking signal wanted: *this actor has important things that
happened to them lately.*

**Stage 2 — fetch from the top of that ranking.** `GetMemoriesForActor` on the top N actors, then qualify
per-memory. One cheap global call decides where to spend the expensive per-actor ones.

This is a genuine constraint rather than a preference: the alternative is ~880 per-actor calls per harvest,
most returning nothing of interest.

### Qualification

A memory becomes a rumor candidate when **all** of:

| Rule                                  | Reason                                                                |
| ------------------------------------- | --------------------------------------------------------------------- |
| `game_time` within the harvest window  | Gossip is news. Also the invariant that makes claims work — see below |
| `importance_score` ≥ threshold        | The notability floor; also what maps to the rumor's β                 |
| Not tagged `gossip`                   | Excludes gossip's own output — see the feedback loop below            |
| Content is not a `Diary Entry:`       | Diaries share this table; they are journals, not witnessed events    |
| `id` not in the claim ledger          | Never twice                                                           |
| Owning actor is a graph participant   | Otherwise there is nowhere to seed from                               |

Notability maps directly: `rumor.notability = memory.importance`, both being 0–1. That correspondence is why
the SIR model takes notability as a per-conversation transmission probability — the two scales already agree,
with no conversion to justify.

### The feedback loop, and why the `gossip` tag carries the weight

**Gossip writes memories. If those memories can seed new rumors, the system feeds on its own output and never
stops.** A rumor reaching 20 people writes 40 memories; if each is eligible, the next harvest turns them into
40 more rumors, and so on.

**`tags` IS on the returned row**, so the `["gossip"]` tag written at `AddMemory` time is filtered on
directly. That is the entire feedback-loop guard: exact, one comparison, and it holds no matter what else
changes about qualification.

> **This paragraph originally listed `{id, text, importance, timestamp, type}`, copied from `PublicAPI.h`'s
> doc comment, and the implementation was written from it. Three of those five names are wrong.** The header
> is not authoritative for this endpoint; the SQL schema and this repo's existing consumers are. It cost an
> in-game run — see [`../engine-findings/skyrimnet-memory-json-field-names.md`](../engine-findings/skyrimnet-memory-json-field-names.md).
>
> **Superseded.** The paragraphs above were written on the belief that the response carried no `tags` field,
> which is why a memory-**type** allowlist (`EXPERIENCE,TRAUMA,JOY`) was drafted to carry the exclusion
> instead. A raw row dump from a running game shows `tags` right there on the row. The tag check replaced the
> allowlist, and `sGossipSourceMemoryTypes` is gone.
>
> Dropping it was not merely tidying. The allowlist was a proxy that happened to correlate with gossip's own
> output, and as a *quality* rule it was actively harmful — measured against a real save:
>
> | type         | count | avg importance |
> | ------------ | ----- | -------------- |
> | KNOWLEDGE    |   881 | 0.65           |
> | EXPERIENCE   |   437 | 0.55           |
> | RELATIONSHIP |    96 | 0.58           |
> | TRAUMA       |     5 | 0.78           |
> | JOY          |     0 | —              |
>
> `KNOWLEDGE` is the largest bucket *and* the highest-scoring one — SkyrimNet files most narrative material
> there — while `JOY`, one of the three admitted types, does not occur at all. Replaying the gates over that
> save, the allowlist cut candidates from 120 to 94 and discarded the two best items on it: a 0.95
> `RELATIONSHIP` memory about a public affair and a 0.92 `KNOWLEDGE` account of a confrontation in a Dwemer
> ruin. Both are exactly what this system exists to spread.
>
> The premise that "a rumor should come from an experience, not from someone's second-hand knowledge" reads
> well and is wrong about the data: SkyrimNet's `type` does not partition memories that way.

### The claim ledger

One record per memory that has been turned into a rumor:

```text
{ memoryId : int64, claimedOnGameDay : double, expiresOnGameDay : double }
```

The invariant that makes it work is **claim lifetime > harvest window**. A memory claimed the instant it was
created has its claim expire at age `claimExpiryDays`, but stops being harvestable at age `harvestWindowDays`.
At 60 and 50 those are 10 days apart, so a claim always outlives its memory's eligibility.

Reversed — a 30-day claim against a 50-day window — a memory claimed early becomes re-harvestable at day 30,
and the same story goes round twice. That ordering is the entire correctness argument, and it should be
asserted at load rather than trusted: a well-meaning tuning pass could quietly invert it.

Because a memory can be claimed at most once and can never re-enter the window afterwards, the immunity
question resolves itself. Under SIR, immunity is per-rumor and lives in that rumor's carrier map; if the same
story could be re-seeded as a *new* rumor, everyone who heard it the first time would be susceptible again
and the identical news would circulate twice with the same people reacting freshly. Preventing re-seeding
entirely means that never arises, and it is why the ledger does not need to retain carrier sets from
burned-out rumors.

Sizing is negligible: one entry per rumor seeded, retained for 60 game days. Even at the harness's
deliberately aggressive 4 rumors per game day that is ~240 entries, under 3 KB against the ~118 KB of live
rumor state. At realistic harvest rates it is a few dozen.

**A memory older than the harvest window can never seed, ever.** A dramatic event from a year ago cannot
resurface as gossip. That is almost certainly right — gossip is news — but it is a design choice rather than
a side effect, and it is the one thing that would need revisiting if "remember that time…" material ever
matters.

### Known gap: one event, several memories

If a combat event writes memories to four witnesses, those are four distinct `id`s describing the same thing,
and per-`id` dedup will happily seed four rumors from them. Mitigations, cheapest first:

1. **Cap seeds per harvest.** Seeding at most one rumor per sweep bounds the damage without solving it, and
   costs nothing.
2. **Collapse by `(relatedActors, timestamp ± ε)`.** The memories share a subject and a time.
3. **Collapse by text similarity.** Most robust, most expensive, and needs text we do not control.

Option 1 is the proposal; option 2 is the upgrade if it proves insufficient in play. Worth noting that if
seeding ever moves to NarrativeEngine's own event logs, dedup should key on the *event* rather than the
memory row, and this gap disappears.

### The player is excluded as a source

`GetActorEngagement` is called with `excludePlayer=true`, and the player's own memories never seed rumors.

The player already has a direct channel for anything they want spread — they can simply tell an NPC, and
SkyrimNet will record it. Harvesting their memories would duplicate that while removing the choice.

Note this does not make the player's deeds ungossipable: an event the player was part of is recorded on the
*witnesses* too, and those memories are eligible. What is excluded is the player's private record of it, not
the event.

### Provenance

`SeedRumor` gains the source memory id and its owning actor; the `Rumor` record stores both. Carriers already
store `toldBy`.

This is what makes the ledger auditable — without it, a claimed memory and the rumor it produced are only
correlated by timing, and the "no memory used twice" property cannot actually be verified from the trace.

### Rumor content

A rumor needs to be *about* something. This is the half of the system Milestone 1 left entirely fake, and it
is the only part of Milestone 2 that involves an LLM.

#### Content bands key on generation, not notability

The original design had notability decay double as the telephone-game mechanism: a rumor got quieter and
vaguer together. Under SIR notability no longer decays — it is a constant transmission probability — so
content drift needs its own axis.

**Generation is that axis**, and the separation is an improvement rather than a workaround: a garbled story
is not less catching. "And then he killed it bare-handed" spreads *better* than the accurate version. Keeping
distortion and contagiousness independent is closer to how rumors actually behave than tying them together
ever was.

Each rumor carries a small array of band texts, selected by the receiving carrier's generation:

| Band | Generations | Share of transmissions (30-day run) |
| ---- | ----------- | ----------------------------------- |
| 0    | 0–2         | 33%                                 |
| 1    | 3–5         | 43%                                 |
| 2    | 6+          | 23%                                 |

Three bands, not four. Measured generation depth reaches 12, but generations 9 and above carry only 4% of
traffic — a fourth band would be output spent on almost nothing.

#### One call, all bands

**Every band is generated in a single up-front LLM call at seed time.** The prompt receives the source memory
and returns all three versions at once, with the instruction that each successive tier is further removed
from the original and should show a mild case of the telephone game — a detail softened, a name confused, a
number inflated.

This is exactly one LLM call per rumor, for the rumor's entire life, regardless of how far it spreads or how
deep it goes. Against the 30-day run that is 120 calls for 120 rumors.

The alternative — generating each band from the previous one, lazily, as transmissions reach it — was
considered and rejected. Chaining would produce *genuine* drift rather than an author's impression of it, but
it costs three calls where one will do, requires a fallback for transmissions that arrive before their band
is ready, and re-parses text that has already been through the model once. It also tends to wander: each
paraphrase is locally reasonable while the arc across three of them is not. Generating the whole progression
in one pass lets the model shape a deliberate arc, which is the more controllable failure mode.

Response shape:

```json
{
  "should_seed": true,
  "bands": [
    "Ysolda was seen arguing with the steward about missing tribute payments.",
    "Ysolda had a shouting match with the steward over money that went missing.",
    "Some woman in Whiterun robbed the Jarl's steward blind, they say."
  ]
}
```

#### A rumor is not infectious until its text exists

`SendCustomPromptToLLM` is asynchronous and takes seconds. A rumor is therefore seeded into a **`Pending`**
state: claimed, recorded, but with no carrier scheduled. When the call returns with all three bands it
transitions to `Live` and the origin carrier's first step is scheduled.

Because every band arrives together, there is exactly one handshake and no mid-flight state where some bands
exist and others do not.

If generation fails, the rumor is abandoned and **its claim is released** — otherwise a transient LLM
failure would permanently burn a memory that never produced anything. A refusal is different: it is an
answer rather than an error, and Step 11 covers what each verdict does to the claims.

#### Memory text is composed, not generated

Every transmission writes two memories, and neither involves an LLM. They are assembled from three pieces
that already exist:

1. The rumor's **band text**, generated once at seed time.
2. A **framing template** chosen by the relationship between teller and listener and the distance between
   them.
3. **Names**, substituted in.

This is the property that makes the whole thing affordable: one LLM call per rumor, one string build per
transmission.

Framing draws its vocabulary from `BGSAssociationType::associationLabels[Members][Sexes]`, which supplies
gendered kinship terms ("sister", "cousin", "father") straight from the record rather than inventing them:

| Case                    | Framing                                            |
| ----------------------- | -------------------------------------------------- |
| Same household          | *"Over supper, my sister mentioned…"*              |
| Same settlement, friend | *"Ysolda told me at the market…"*                  |
| Different hold, kin     | *"My cousin came from Markarth, and she told me…"* |
| No relationship         | *"I heard a rumor that…"*                          |

The last row will be the most common, and it is fine — that is genuinely how most gossip arrives.

#### Prompt discipline

One prompt, under `statics/SKSE/Plugins/SkyrimNet/prompts/`. It receives the source memory and the number of
bands, and must be explicit about three things:

- **Each tier is further removed from the source.** Tier 0 is close to first-hand; tier 2 has passed through
  half a dozen mouths. Mild degradation per step — a softened detail, a confused name, an inflated number —
  not a different story. Drift, not replacement.
- **Traceability.** Every tier must still be recognisably about the source memory. A tier that invents a new
  event has failed, however entertaining.
- **Reject rather than invent.** The explicit "NOT gossip-worthy" list — opened a door, bought an item,
  walked somewhere — and `should_seed: false` when nothing in the memory would make someone stop and listen
  at a tavern.

That discipline is the one thing in IntelEngine's prior art worth copying nearly verbatim; its author
clearly fought the failure mode where an LLM narrates "Ysolda opened a door" as breaking news.

#### Sanitization

Every band string passes through `LLMTextSanitizer::Sanitize` **at the point of extraction from the response
JSON**, before being cached, persisted to the co-save, or written into a memory. See
[`../LLM_RESPONSE_HANDLING.md`](../LLM_RESPONSE_HANDLING.md).

Band text is persisted and rendered into NPC-facing memories, so smart quotes, em-dashes and zero-width
characters would reach both the co-save payload and SkyrimNet's prompt context. Note the single-call design
removes one hazard the chained design had — no band is fed back into another prompt — but sanitization is
still mandatory, and this is a place IntelEngine is explicitly not a model to copy.

### Carried over from Milestone 1

**Dead NPCs keep gossiping.** Milestone 1's viability check catches deleted forms only, so a murdered NPC
carries on until their carrier recovers.

The Milestone 1 notes claimed this was hard — that death lives on the `Actor` reference and those are not
loaded, so the honest fix was a death-event sink maintaining a persistent dead-set. **That was wrong.** The
engine headers settle it: `BGSLocation` stores its two reference lists differently, and the contrast is the
proof.

```text
LCUN (unique NPCs)  -> UniqueNPCData  { Actor* actor; FormID refID; BGSLocation* editorLoc; }
LCPR (persistent)   -> UnloadedRefData { FormID refID; FormID parentSpaceID; CellKey cellKey; }
```

The engine keeps a raw `Actor*` for unique NPCs and only a FormID-plus-coordinates for everything else. It
would not hold a raw pointer to an object destroyed on cell unload — unique NPCs' `Actor` objects are
persistent and always resident, and only their 3D unloads. That is precisely why `GetDead` works as a
condition on them regardless of where the player is.

So the fix is a field and a comparison:

- `GossipGraph` already reads `entry.refID` during its `LCUN` walk, but only as a fallback for resolving
  `editorLoc`. Retaining it on `Participant` is one field.
- `ActorState::GetLifeState()` is an inline read of `actorState1.lifeState` — no engine call, no lock, no 3D
  — so the check stays on the plugin thread. `TESForm::LookupByID` takes the engine's own read-write lock, so
  the resolve is safe there too.

Prefer `GetLifeState()` over `Actor::IsDead(bool a_notEssential = true)`. Only `kAlive` should count as
available — someone bleeding out or unconscious cannot hold a conversation — but `kDead` must be told apart
from the *temporary* `kEssentialDown`, `kBleedout` and `kUnconcious`, because retiring a carrier under SIR
makes them permanently immune and an NPC who is merely down will get back up and should resume. A boolean
collapses that distinction, and the parameter's polarity is undocumented in the bindings besides.

---

## Settings

Removed: `bGossipSeedStubsOnLoad`, `iGossipStubSeedCount`, `iGossipStubSeedIntervalGameHours`,
`iGossipStubSeedMaxTotal`.

Added:

| Key                               | Default                 | Meaning                                                     |
| --------------------------------- | ----------------------- | ----------------------------------------------------------- |
| `bGossipHarvestEnabled`           | true                    | Master switch for memory-derived seeding                    |
| `fGossipHarvestIntervalGameHours` | 12.0                    | How often the sweep runs                                    |
| `fGossipHarvestWindowDays`        | 50.0                    | Memories older than this are never candidates               |
| `fGossipClaimExpiryDays`          | 60.0                    | **Must exceed the harvest window.** See the invariant above |
| `fGossipMinMemoryImportance`      | 0.45                    | Notability floor for a candidate                            |
| `iGossipHarvestActorSampleSize`   | 25                      | Top-N actors by recent memory importance to fetch from      |
| `iGossipHarvestMemoriesPerActor`  | 10                      | `maxCount` per `GetMemoriesForActor` call                   |
| `iGossipMaxSeedsPerHarvest`       | 1                       | Bounds the one-event-many-memories case                     |
| `iGossipContentBands`             | 3                       | Generation bands produced by the one seed-time call         |

Deliberately **not** settings: the seed prompt's name and the LLM variant it runs under. Both are compiled
into `GossipContent`. The prompt's contract with the code is fixed, and the variant is
`narrative_engine_composer`, shared with letter composition.

Unchanged: `iGossipMaxLiveRumors` stays at 40. The payload is bounded by construction
(`maxLiveRumors × maxCarriersPerRumor`), which was the actual design goal; ~118 KB is unremarkable next to a
5–20 MB Skyrim save.

---

## Module structure

| Module          | Role                                                                                                                                                                         |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GossipSeeder`  | **Deleted.**                                                                                                                                                                 |
| `GossipHarvest` | The two-stage sweep, qualification, and seed selection. Game-time paced, plugin thread.                                                                                      |
| `GossipClaims`  | The ledger, its co-save record, and expiry sweeping. Driven by sampled game time rather than by rumor activity, so a quiet stretch with no live rumors still expires claims. |
| `GossipSim`     | `SeedRumor` gains a source-memory parameter; `Rumor` gains provenance. Otherwise untouched.                                                                                  |
| `GossipLog`     | `SEED` lines name the source memory; new `HARVEST`, `MEMORY` and `CLAIM` line types.                                                                                        |

---

## Implementation plan

Ordered so that the LLM enters as late as possible. Steps 1–5 are pure logic and fully testable with no model
in the loop; by the time content generation lands, seeding and claiming are already known-good. The last four
debugging rounds on this system were all logic failures, and separating them from prompt failures is worth
the sequencing.

---

### Step 1 — Settings and INI surface

- [x] Complete

**Goal:** Every knob the later steps need, in place before they need it.

Add the `[Gossip]` keys from the Settings section above; remove `bGossipSeedStubsOnLoad`,
`iGossipStubSeedCount`, `iGossipStubSeedIntervalGameHours` and `iGossipStubSeedMaxTotal`.

**Assert `fGossipClaimExpiryDays > fGossipHarvestWindowDays` at load**, and log an error naming both values
if it fails. That inequality is the entire correctness argument for the claim ledger — invert it and the same
memory becomes re-harvestable partway through its life. A tuning pass adjusting one number without the other
is exactly how it would break, so it needs to fail loudly rather than silently.

**Verification:** launch with no `[Gossip]` block and confirm the baked-in defaults; set expiry below the
window and confirm the complaint fires.

---

### Step 2 — Remove `GossipSeeder`

- [x] Complete

**Goal:** Delete the harness so nothing is written against a system that is about to disappear.

Both files, the `Tick` poll, the two `Plugin` calls, and the settings above. Subtraction only.

**Verification:** builds clean with no references remaining; a session with `bGossipEnabled=true` runs with
zero rumors, zero errors, and an empty gossip trace.

---

### Step 3 — Dead actors stop gossiping

- [x] Complete

**Goal:** Close the Milestone 1 gap, now that it is known to be a field and a comparison rather than a
subsystem.

1. `GossipGraph::Participant` gains `RE::FormID actorRef`, populated from `entry.refID` during the existing
   `LCUN` walk — the value is already read there as a fallback for `editorLoc` and simply discarded.
2. `GossipSim` gains `ActorAvailability(actorRef)` returning one of three outcomes.

**Only `kAlive` counts as available.** Anyone bleeding out, unconscious, restrained, dying, reanimated or
recycled cannot hold a conversation, so they cannot gossip either.

But "cannot gossip right now" and "out of the epidemic" are different outcomes, and conflating them would be
a real bug. Retiring a carrier under SIR makes them **permanently immune** — so an NPC knocked unconscious
for a day would be silently removed from the outbreak for good, rather than resuming when they get up.

| Life state                                                                                                                     | Outcome                                                                       |
| ------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------- |
| `kAlive`                                                                                                                       | Available                                                                     |
| `kDead`, or the form no longer resolves                                                                                        | **Permanent.** Retire the carrier; a prospective listener is skipped for good |
| Everything else — `kDying`, `kUnconcious`, `kBleedout`, `kEssentialDown`, `kRestrained`, `kReanimate`, `kRecycle`, or disabled | **Transient.** Skip and retry                                                 |

The two transient paths differ by role:

- **Carrier temporarily unavailable** — skip the whole simulation step and reschedule normally. Their
  infectious clock keeps running: time spent unconscious is time not spent talking, which is the right
  outcome and needs no clock-pausing machinery.
- **Listener temporarily unavailable** — no conversation happened. Do *not* count it as a wasted telling;
  wasted tellings are the saturation brake and mean "they already knew", which is a different thing. The
  listener stays susceptible and can catch it later.

`kDying` resolves itself: it is transient on this step and `kDead` on the next.

Both calls are safe on the plugin thread — `LookupByID` takes the engine's own read-write lock, and
`GetLifeState()` is an inline field read.

**Verification:** with a rumor in flight, kill a named carrier from the console and confirm they retire
permanently and the trace records it. Separately, knock a carrier unconscious, confirm they stop transmitting
while down and **resume once they recover** rather than being retired. Confirm an unavailable listener is not
logged as a wasted telling and can still catch the rumor afterwards.

---

### Step 4 — `GossipClaims`: the ledger

- [x] Complete

**Goal:** A memory that has become a rumor can never become one again.

Built before the harvester so the harvester can be written against a working claim check.

- `bool IsClaimed(std::int64_t memoryId)` — the qualification gate.
- `void Claim(std::int64_t memoryId, double nowGameDay)` — records `now + fGossipClaimExpiryDays`.
- `void Release(std::int64_t memoryId)` — **needed by Step 7.** A rumor whose content generation fails or is
  rejected must give its memory back, or a transient LLM error permanently burns it.
- `void Sweep(double nowGameDay)` — drops expired claims. Called from the same poll that reaps rumors, but
  **driven by sampled game time, not by rumor activity**, so a quiet stretch with no live rumors still
  expires claims.
- Co-save record `'NEGC'`, versioned, skip-and-log on mismatch.

**Verification:** claim an id, confirm `IsClaimed`, save/reload, confirm it survives; advance past expiry and
confirm the sweep drops it; confirm `Release` frees it immediately.

---

### Step 5 — `GossipHarvest`: the sweep, qualification, and provenance

- [x] Complete

**Goal:** Real memories become rumors. **Still no LLM** — a seeded rumor uses the source memory's own text
verbatim for all three bands, so the whole seeding path is testable on its own.

1. Game-time paced on `fGossipHarvestIntervalGameHours`, plugin thread, Tick accumulator.
2. `GetActorEngagement(0, excludePlayer=true, playerEventsOnly=false, short=1 day, medium=window)`.
3. Rank by `recentMemoryImportanceMedium`; keep the top `iGossipHarvestActorSampleSize` that are graph
   participants.
4. `GetMemoriesForActor(formId, iGossipHarvestMemoriesPerActor, "")` for each.
5. Qualify per the table in the design section.
6. Seed up to `iGossipMaxSeedsPerHarvest`, highest `importance` first. `SeedRumor` gains `sourceMemoryId` and
   the owning actor; `Rumor` stores both.

`maxCount=0` on `GetActorEngagement` returns *every* actor with any activity — potentially hundreds of rows
of JSON. Parse it once per harvest, not per candidate, and log the row count so an unexpectedly large
response is visible.

**Verification:** log each harvest with actors sampled, memories examined, and a per-memory rejection reason
(`too-old`, `low-importance`, `wrong-type`, `claimed`, `not-participant`). On a save with real play history,
confirm the chosen memories are ones a person would agree are worth gossiping about, and that every seeded
source id appears in the ledger.

Every number on a `HARVEST` line is **for that sweep alone**; session totals live in `GossipHarvest::Stats`
and stay out of the trace. Mixing the two makes the line unreadable — a per-sweep actor count printed beside
a since-session-start rejection count cannot be read as a rate, and the rate is the entire question a sweep
log answers. `not-participant` is counted in stage 1, where the ranking drops actors the graph does not
know: a sweep that finds nothing because the graph is small reads very differently from one where the
memories themselves did not qualify, and an uncounted filter cannot tell them apart.

---

### Step 6 — The seed prompt

- [x] Complete

**Goal:** The statics asset, authored and deployed before the code that calls it.

`statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_gossip_seed.prompt`. Receives the source memory text
and the band count; returns the JSON shape from the design section.

Three things the prompt must be explicit about: each tier is further removed from the source with **mild**
degradation (drift, not replacement); every tier stays recognisably about the source memory; and
`should_seed: false` rather than inventing, with the explicit "NOT gossip-worthy" list.

**`statics/` changes only reach the mod folder through `build.ps1`.** A prompt edited in the source tree and
not built is not live.

**Verification:** exercise it through SkyrimNet's own prompt tooling against a handful of real memories before
any C++ calls it. Read the three tiers as a person: does tier 2 read like the same story after six retellings,
or like a different event?

---

### Step 7 — `GossipContent`: generation and the `Pending` handshake

- [x] Complete

**Goal:** Real retold text, one LLM call per rumor.

1. `SeedRumor` puts the rumor in `Pending`: claimed and recorded, no carrier scheduled.
2. One `SendCustomPromptToLLM` per rumor. The wrapper already bridges SkyrimNet's foreign callback thread
   onto the plugin thread, so the continuation holds a `PluginThread::Token` and needs no marshalling of its
   own.
3. **Every band string through `LLMTextSanitizer::Sanitize` at the point of extraction from the response
   JSON** — before it is cached, persisted, or written into a memory.
4. On success: store the bands, transition to `Live`, schedule the origin carrier's first step.
5. On failure: abandon the rumor and `GossipClaims::Release` its memory. Refusal verdicts have their own
   claim handling — see Step 11.
6. Band selection at transmission time is `min(generation / 3, bands - 1)`.
7. Co-save gains the band strings. Three bands of ~150 characters across 40 live rumors is ~18 KB on top of
   the existing payload.

**Verification:** confirm exactly one LLM call per rumor regardless of reach; confirm a forced failure
releases the claim and the memory is harvestable again on the next sweep; confirm band text survives
save/load; confirm sanitization by feeding a response containing smart quotes and an em-dash.

---

### Step 8 — Composed memory text

- [x] Complete

**Goal:** Replace the stub memory text with the real composition. No LLM — this is a string build.

Band text, plus a framing template selected by relationship and distance, plus names. Kinship terms come from
`BGSAssociationType::associationLabels[Members][Sexes]`, indexed by which side of the relationship each party
is on and by sex, so "sister" and "cousin" are read from the record rather than invented.

The `[NE-GOSSIP-STUB` prefix and the `stub` tag go away; the `gossip` tag stays. Memory `type` stays
`KNOWLEDGE`, which is what keeps gossip's own output out of the harvester.

**Verification:** read a session's worth of memories back with `GetMemoriesForActor` and check they read as
something an NPC would plausibly say. Confirm kinship framing fires for a known vanilla pair — the Gray-Manes
are convenient, being both a household and a relationship cluster.

---

### Step 10 — Gossip dashboard tab

- [x] Complete

**Goal:** See what the rumor mill is doing without reading a trace file, and be able to make it do something
now rather than waiting on an accumulator.

Added after Steps 1–8 at the user's request. Not a prerequisite for Step 9, but it is the instrument Step 9
is easiest to run with, so it lands first.

A `Gossip` tab on the PrismaUI dashboard listing every rumor still in the simulation, newest first, in a
scrollable panel. Each row carries the rumor's band-0 text and its spread: carriers (and how many are still
telling), settlements, holds, deepest generation, tellings and wasted tellings, age and time since the last
telling. An expander adds provenance — origin NPC and location, source memory id — and every generation
band with the generation window it covers.

**State.** A rumor is `SPREADING` or `STALLED`.

**Stalled means every still-infectious carrier has run out of people to tell:** each of their named contacts
already carries the rumor. It is live, its carriers are still burning down their infectious windows, and it
is going nowhere.

The word *named* is load-bearing. Every carrier also holds the `kProvincePeer` sentinel — "somebody, anywhere
in Skyrim", weighted `fGossipWeightProvince` — which resolves to a random participant at transmission time.
Counting that as a vector would make the predicate answer "not stalled" until literally every participant in
the province carried the rumor, which never happens, so nothing would ever read as stalled. Excluding it
measures what a reader actually wants to know: whether the rumor's local social neighbourhood is saturated.
The consequence to accept is that a stalled rumor can still jump on the province lottery and un-stall itself.
That is the model working, not the readout lying.

No retention window and no reap change. The list holds exactly what the rumor map holds, so a rumor is listed
until its last carrier retires — which is the moment `FinishRumorLocked` runs, and the reap at the end of that
same poll removes it. `RumorView::live` is carried anyway so the pairing stays unambiguous if the reap is
ever decoupled from the poll.

**Read-only.** A "force gossip tick" button was built alongside this and then removed: it would have run a
harvest sweep and drained every due carrier step on demand, zeroing both accumulators. It served no purpose
worth the surface area — the tab is an observation instrument, and the simulation reaches the same state on
its own within a sweep interval. The tab issues no commands; `GossipSim` and `GossipHarvest` expose nothing
to it but reads.

**Surface.** `GossipSim` gains `RumorView` and `GetRumorViews()`. `DashboardUIManager` composes a `gossip`
section from that plus the two existing `GetStats()` calls and `GossipClaims::Count()`. Nothing in the path
touches the engine, so the whole compose stays on the plugin thread with no `MainThread` hop.

**Verification:** the JSON keys the C++ emits were diffed against the `RumorEntry` / `GossipTabState`
interfaces in `types.ts` — that file calls itself the schema contract but nothing enforces it, and a renamed
key compiles clean on both sides while reading as `undefined` at runtime. The tab was then server-rendered
against nine payloads including the awkward ones: a rumor with no bands and no text, sub-hour ages,
single-carrier singular/plural agreement, graph-not-ready, and forty rumors at once.

In game: confirm the list scrolls at high rumor counts, that ordering is newest-first and stable across
pushes, and that a saturated household's rumor flips to `STALLED`.

---

### The two FormID spaces

**Found by the first in-game run, which harvested six times and rejected all 131 active actors.** Recorded
here because it is not obvious from either side of the boundary and it will be reintroduced by anyone who
forgets it.

Gossip works in two different FormID spaces for the same person:

| Space                     | What it is                     | Who uses it                                                       |
| ------------------------- | ------------------------------ | ----------------------------------------------------------------- |
| **TESNPC base form**      | The character in the ESM       | `GossipGraph` keys, carrier maps, `SeedRumor`, every log line     |
| **Placed reference** (ACHR) | The instance standing in the world | Every SkyrimNet endpoint, without exception                   |

`GossipGraph` is built by walking `TESDataHandler::GetFormArray<TESNPC>()` and keying on
`npc->GetFormID()`, so it is base-form space throughout. SkyrimNet is reference space throughout —
`GetActorEngagement` rows, `GetMemoriesForActor`, `AddMemory`, and the related-actor arrays all mean the
placed reference. `SenderCandidatePool` already showed this: it resolves engagement rows with
`LookupByID(formId)->As<RE::Actor>()`.

`GossipHarvest` was passing engagement `formId` values straight to `GossipGraph::Find`. The two spaces never
collide, so **every** row missed, every sweep reported `0/131` actors ranked with `131 not-participant`, and
no rumor could ever be seeded. It fails totally and silently rather than partially and visibly, which is why
six clean-looking sweeps produced nothing.

The same confusion sat undetected in `GossipSim::WriteMemories`, which handed carrier ids — base forms — to
`AddMemory` as the memory owner and into the related-actor arrays. It was masked: no rumor ever seeded, so
that code never ran.

The crossings are now explicit and are the only sanctioned ones:

- `GossipGraph::FindByActorRef(ref)` — reference to participant, off a prebuilt index, no engine access.
  `GossipHarvest` falls back to `LookupByID` + `GetActorBase` for the residue.
- `GossipGraph::ActorRefFor(npc)` — participant to reference. Every id handed to SkyrimNet goes through it.

`Find()`'s header comment now says which space it is in, since the failure mode is a silent empty result.

**The census reports `participantsWithActorRef`,** because a participant with no placed reference cannot be
matched to an engagement row, cannot be written a memory — and, in `ActorAvailability`, is already treated as
permanently `Gone` and retired on its first step. A large shortfall against `participants` is therefore a cap
on everything gossip can do, and it should be read off the log before anything else.

**A correction to Step 3's reasoning.** That step argued the `Actor*` in `BGSLocation`'s `UniqueNPCData`
proved unique NPCs' `Actor` objects are always resident. `GossipGraph`'s own `LCUN` walk contradicts it —
it does `entry.actor->As<RE::TESNPC>()` and gets 999 distinct NPCs, so in practice that field holds the
**base form**, whatever CommonLibSSE-NG names its type. The Step 3 fix is unaffected, because it resolves
`entry.refID` rather than that pointer, but the argument offered for it was based on a misreading and should
not be reused.

---

### Step 11 — The seed call decides three things, not one

- [x] Complete

**Goal:** Stop seeding rumors an NPC would never repeat, and stop seeding a story that is already going
round — both judged in the call that was already being made.

The seed prompt receives two things it did not have before:

- **The owner's character profile**, via `render_character_profile("full", npc.UUID)` — the same mechanism
  the letter composer uses. `npc.UUID` is keyed on the placed reference, so `GossipGraph::ActorRefFor`
  converts the graph's base form first. Without it the prompt knew only a name, which is not enough to
  judge whether someone would repeat a thing.
- **Every unreaped rumor's band-0 text**, from `GossipSim::GetRumorViews`. Band 0 only; the later bands are
  the same story degraded and would be noise.

It answers three questions in order and stops at the first that settles it, returning exactly one verdict:

| Verdict      | Meaning                                      | Memory claim | Event claims |
| ------------ | -------------------------------------------- | ------------ | ------------ |
| `private`    | This person would not tell the world          | kept         | **released** |
| `not_worthy` | Nobody would stop to listen                   | kept         | **released** |
| `duplicate`  | A circulating rumor is already about this     | kept         | kept         |
| `seed`       | Passes all three; bands follow                | kept         | kept         |

The split in the last column is the whole point. `private` and `not_worthy` are judgements about **this
owner**, so their memory is spent — asking again next sweep would burn another call on the same answer —
but the happening is untouched, and another witness with their own account of it can still seed. `duplicate`
is a judgement about **the happening**, so the events stay claimed and no other account can start a second
rumor about it.

A failed call, an unparseable response, or an unrecognised verdict releases everything: nothing was learned,
so nothing should be spent. That is deliberately distinct from a refusal, which is an answer.

**Why this subsumes the per-subject cooldown.** A metadata gate was designed for the repeated-storyline case
(Faralda's two trysts) and then measured against the corpus. It does not work. Tag overlap is inverted — the
pair that must be blocked scores 0.25 while the same-cast-different-story pair that must survive scores 0.32.
Actor overlap separates those two controls but `related_actors` is far too broad to be a subject key: a
two-person tryst lists ten shared actors, and at any threshold that catches Faralda it also blocks hundreds
of plainly distinct stories. Scoping it to `RELATIONSHIP` memories narrowed the damage without fixing it.
The discriminator has to be meaning, which is what the `duplicate` verdict provides, and the `private`
verdict removes the observed case outright since neither tryst is something Faralda would repeat.

**Verification:** the prompt renders under Inja with rumors present and with an empty list, and with a
missing profile. In game: confirm a private memory returns `private` and that its events stay harvestable by
another witness; confirm a second account of a covered happening returns `duplicate`; confirm the claim
trace shows events freed for the first and held for the second.

---

### Step 9 — In-game validation

- [ ] Complete

**[USER]**

Run on a save with **real play history**. An empty memory database producing nothing at all is the correct
behaviour and worth confirming first.

0. **Actors resolve at all.** `HARVEST actors=N/M` with `N > 0`. `0/M` with every row counted
   `not-participant` means the FormID spaces have been crossed again — see the section above. Check
   `participantsWithActorRef` in the graph census at the same time.
1. **Harvests find something,** and the sources are memories a person would agree are gossip-worthy.
2. **No feedback loop.** Zero rumors sourced from a `KNOWLEDGE` memory. If gossip volume grows
   super-linearly over a long session, this is the first suspect.
3. **No duplicate sources.** Every `SEED` source id distinct across the whole session. `analyze-gossip-log.py`
   checks this directly and prints a `SOURCE MEMORIES` section naming any memory that seeded more than one
   rumor — this is the one property that must not be eyeballed.
4. **Claims survive save/load** and expire on schedule; a released claim becomes harvestable again.
5. **Propagation is unchanged** from Milestone 1 — reach, coverage and cross-hold rate should match, because
   nothing about the model moved. A divergence means seeding changed the *distribution* of origins, which is
   worth understanding rather than accepting.
6. **Content reads well.** Tier 0 close to first-hand, tier 2 recognisably the same story after several
   retellings. Not blander — wronger.
7. **Dead NPCs stop.** Kill a carrier mid-spread and confirm it.
8. **Seeding rate is sane.** Measure it: it sets both the memory-volume budget and the LLM call budget.

---

## Done condition

Milestone 2 is complete when:

- All 9 steps are checked off and Step 9 passes.
- No memory is ever sourced twice, verified across a long session.
- Gossip's own memories never seed rumors.
- The claim ledger survives save/load, expires correctly, and releases on generation failure.
- Exactly one LLM call per rumor, regardless of reach or depth.
- Propagation metrics match Milestone 1 within noise.
- `GossipSeeder` is gone and no stub text remains in written memories.
- The dashboard's Gossip tab lists every unreaped rumor newest-first, with its spread and its state.

---

## Open questions

1. **Does the one-event-many-memories collapse need building now,** or is one seed per harvest enough in
   practice? Answerable only from a save with real play history.
2. **What harvest interval feels right?** 12 game hours is a guess. Too frequent and the world is a rumour
   mill; too sparse and nothing propagates.
3. **What is the real seeding rate?** The harness ran at 4 per game day by fiat. Real harvesting will be far
   sparser, and that number sets both the memory-volume budget — the one quantity in this system with no
   upper bound — and the LLM call budget.
4. **Does one-shot generation produce convincing drift?** Asking a model to write three tiers of the same
   story at increasing remove is a different task from actually paraphrasing a paraphrase. The failure mode
   to watch for is tiers that differ in wording but not in *content* — blander rather than wronger — or the
   opposite, a tier 2 that has become a different event entirely. Only readable from real output, and the
   prompt is the lever either way.
