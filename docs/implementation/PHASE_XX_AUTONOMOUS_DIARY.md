# Phase XX — Autonomous Diary Generation

A scheduled, population-wide sweep that decides which NPCs have accumulated enough notable experience since
their last diary entry to be worth another one, and asks SkyrimNet's `DiaryManager` to write it.

The division of labour is the whole design: **NarrativeEngine owns selection, SkyrimNet owns generation.** We
never compose a diary entry, never call an LLM, and never write a memory. The only thing we persist is a
single co-save record holding the in-game time of the last sweep — the sweep clock, and nothing else. The sweep
reads two SkyrimNet endpoints, ranks the province, and fires a Papyrus native at the top few.

This is the cheapest simulation-adjacent thing on the ladder. No ESP work, no new prompt asset, no co-save
record, no `IBeat`, no quest. One worker thread, one scheduler, one job.

---

## Why this phase exists

**Diary generation already exists and is entirely manual.** SkyrimNet ships `DiaryManager`, a
`diary_entry.prompt` template, a diary table, and a hotkey — `TriggerGenerateDiaryBio()` — whose four target
scopes are *player*, *nearby actors*, *pinned actors*, and *crosshair target*. Every one of them is
player-centric and every one of them requires a keypress. Left alone, the province never journals.

**A diary entry is consolidation, and consolidation is what the substrate is short of.** An entry takes a
scatter of individually-unremarkable memory rows and produces one authored first-person account with explicit
continuity to the previous entry (the prompt takes `lastDiaryEntry` for exactly that). That is Milestone 15's
"consolidation of routine into digests" arriving early, for the cost of a scheduler — and it gets
substantially more valuable once Milestone 14 is writing memories for people the player has never met.

**Player-centric bias is correct here, and that is the interesting difference from gossip.** Phase 13
Milestone 4 tore out `GetActorEngagement` selection because a rumor mill that reports the player's own
itinerary back at them is worthless — gossip needed even sampling, and buckets exist to enforce it. A diary is
the opposite kind of object: a private record of one life, where the people living the most eventful lives
should write the most often. Selection here is by accumulated importance and lands wherever the drama is,
which in practice means followers and the recently-travelled-with journal more than a Rorikstead farmer. That
is the intended output, not a sampling defect.

---

## Scope

### In scope

- **`DiaryTick`** — the plugin-thread cadence check. Compares the game clock against the co-saved last-sweep
  timestamp and enqueues a sweep when the configured in-game interval has elapsed.
- **One co-save record** — a single `double`, the in-game time of the last sweep, rewritten each time one is
  enqueued.
- **`DiaryDispatch` + `DiaryThread::Token`** — a dedicated serial worker with cancellation-on-load, modelled
  on `GossipDispatch`. The sweep is too long to sit on `AsyncDispatch`.
- **`DiarySweep`** — the job: scan the population reading each actor's watermark and memories, score, rank,
  gate, dispatch.
- **Three new `SkyrimNetAPI` wrappers** — `GetDiaryEntries`, `IsActorBusy`, and the Papyrus bridge that
  invokes `SkyrimNetApi.GenerateDiaryEntryByUUID`. `GetRecentEvents` and `QueryMemoriesForActor` already exist
  and are consumed as-is.
- **A new INI section `[Diary]`**, MCM-overridable in the usual way.
- **A dedicated trace log** (`NarrativeEngine_Diary.log`) on the `GossipLog` pattern, flushed per line, with
  the parsed score printed next to every verdict.

### Deferred (explicitly out)

- **Composing entries ourselves.** `SendCustomPromptToLLM("diary_entry", …)` plus `AddMemory` would work and
  would give us tag control and a synchronous result — and it would fork a prompt that ships with the
  dependency, skip the diary table, and break `lastDiaryEntry` continuity. See the ownership split below.
- **Any persisted state beyond the sweep clock.** No watermark table, no attempt counters, no cooldown table,
  no per-NPC record of any kind. The one co-save record is a timestamp.
- **Dynamic bio updates.** `UpdateActorDynamicBio` is `DiaryManager`'s sibling and rides the same hotkey. It
  is a separate decision with a separate cadence; nothing here forecloses it.
- **Reading entries back.** Beats briefing an NPC from their own diary is a genuine downstream use and is not
  this phase. This phase only causes entries to exist.
- **The player's diary.** `GossipGraph::Participants()` is built from `BGSLocation::uniqueNPCs`, so the player
  is not in the population by construction.
- **A dashboard tab.** The trace log is the observability surface for this phase.
- **Any reaction to a written entry.** The entry lands in SkyrimNet's store and is picked up by whatever reads
  that store. Nothing here observes completion.
- **Extracting a generic `SerialDispatch<Token>`.** `AsyncDispatch`, `EvalDispatch`, `GossipDispatch` and now
  `DiaryDispatch` are near-identical. Collapsing them is a real question and a refactor of three shipped
  subsystems; it does not ride in on this phase.

---

## Design

### Ownership split: we select, SkyrimNet writes

The API surface forces most of this, and it is worth stating plainly because it is asymmetric:

| Operation                | Available where                                                                 |
| ------------------------ | ------------------------------------------------------------------------------- |
| **Generate** an entry    | **Papyrus only** — `SkyrimNetApi.GenerateDiaryEntry(Actor)` / `…ByUUID(String)`  |
| **Read** entries         | **C only** — `PublicGetDiaryEntries(formId, maxCount, startTime, endTime)`       |
| **Read** source memories | C — `PublicQueryMemoriesForActor` (v10+)                                         |

There is no C export that generates. `FindFunctions()` in `PublicAPI.h` resolves ~30 pointers and none of them
reach `DiaryManager`; the only generation surface is the two `Global Native` declarations in
`SkyrimNetApi.psc`. Both are documented as asynchronous — they return a status string meaning *submitted*, not
*written*.

Delegating rather than reimplementing is a deliberate choice, not just the path of least resistance:

- `diary_entry.prompt` renders `lastDiaryEntry` into every request. An entry we wrote ourselves through
  `AddMemory` would not be in the diary table, so the *next* entry — ours or SkyrimNet's — would have no
  continuity to it.
- The prompt also renders `render_subcomponent("system_head", "full")` and
  `render_template("components\\event_history_verbose")`, both assembled by SkyrimNet from state we would have
  to reconstruct.
- The feature being asked for is "make SkyrimNet's diary system autonomous", not "ship a second diary system".

The cost of delegating is that we get no completion signal. That costs nothing, because the watermark below
*is* the completion signal.

### The watermark is SkyrimNet's, and there is no local state

An NPC's watermark is **the in-game time of their most recent diary entry as SkyrimNet reports it**. No
per-NPC state is tracked on our side.

It is read **per actor**, inside the scan, with `GetDiaryEntries(formId, 1, 0, 0)` — one row, that actor's
most recent entry.

`PublicGetDiaryEntries` does accept `formId = 0` for *all actors*, and that is tempting: one call instead of
eight hundred. It is the wrong call for two reasons.

- **The payload is entire diary entries, not timestamps.** Rows carry all of `DiaryEntry::ToJson()`, so a
  global fetch drags every ~500-word entry in the save across the DLL boundary and through a JSON parse, every
  sweep, to extract one `double` per actor. That cost grows with the diary corpus forever.
- **`maxCount` truncates against an unspecified ordering.** Whatever bound is passed, prolific journallers
  occupy disproportionately many rows. An NPC who last wrote forty days ago falls off the end, reads as having
  no entry at all, and therefore scores their *entire* history — which puts them at the top of the ranking
  precisely because they were missed. A silent, self-reinforcing selection bug.

Per-actor also sequences naturally: the watermark is an input to that actor's memory query
(`gameTimeAfter`), so the two calls are consecutive within one loop iteration rather than a separate pass.
Because the cheap gates run first, both calls are only paid for participants that survive them.

A co-saved watermark would not merely be redundant, it would be **wrong**. SkyrimNet's database is not rolled
back by loading an earlier save, but our co-save is. Every restored watermark would sit behind reality and the
sweep would re-pick a province that had already journalled. This is the same asymmetry that makes
`GossipDispatch` cancel in-flight jobs on load rather than just discard their results.

An NPC who has never journalled gets an empty array back. Their watermark is `0`, so their whole history
counts as new — which is correct, and self-draining: they are picked, they get an entry, their watermark
becomes real. No bootstrap path, no special case.

Note the contrast with the sweep clock below, which *is* co-saved. The two are opposite cases and the same
rollback asymmetry decides both: the watermark describes SkyrimNet's store, which a load does not rewind, so
persisting it would desynchronize. The sweep clock describes *our* schedule against the world in the save, so
it must rewind with it.

### The sweep

One job, start to finish, on the diary worker:

| Stage        | What it does                                                                                       |
| ------------ | -------------------------------------------------------------------------------------------------- |
| **scan**     | Walk `GossipGraph::Participants()`; stable gates; then per survivor, watermark then memory query.    |
| **score**    | Sum importance over qualifying rows.                                                                 |
| **rank**     | Sort descending, take the top `N x overdraft`.                                                       |
| **dispatch** | Walk the ranked list applying the volatile gates; fire the Papyrus native until `N` have landed.      |

The scan is two SkyrimNet calls per surviving participant — `GetDiaryEntries(formId, 1, 0, 0)` for the
watermark, then `QueryMemoriesForActor` bounded by it. That is the sweep's whole cost, and it is why the cheap
gates run ahead of both.

The population comes from `GossipGraph` rather than a fresh enumeration. It is already built at `kDataLoaded`,
already immutable between session start and shutdown (so every accessor is lock-free off the main thread), and
already carries the two fields the sweep needs: `npc` and `actorRef`.

### Scoring

The per-actor query, keyed on the **reference** FormID — `Participant::actorRef`, the same key
`GossipHarvest::CollectFrom` uses:

```cpp
MemoryQuery q;
q.gameTimeAfter = watermark(npc);        // game-seconds, matching the row's `game_time`
q.excludeTags   = { "ne_gossip", ... };  // configurable skip list, applied server-side
q.orderBy       = MemoryOrder::ImportanceDesc;
q.maxCount      = cfg.diaryMemoriesPerActor;
```

Two properties of this are deliberate rather than incidental.

**The exclusion runs server-side.** v10 filters in SQL *before* truncating. Filtering our own writebacks on
this side would only ever filter what survived the cut, and plugin-written rows are always the newest — the
exact failure that starved gossip's harvest before the v10 addendum.

**The score is a top-K sum, not a total sum.** Because ordering is `ImportanceDesc` and `maxCount` is capped,
the sum covers the K most important new memories rather than all of them. This is the better metric anyway: an
NPC should not out-rank a genuine crisis by accumulating forty trivial rows, and it bounds both the payload
and the scan cost uniformly regardless of how long an NPC has gone unjournalled.

Rows whose `content` begins with `"Diary Entry:"` are dropped. SkyrimNet folds diary entries into the same
memories table typed `EXPERIENCE` with no server-side flag, and that prefix is the only discriminator — the
letter composer, the visit composer, and `GossipHarvest` all already filter on it. Without this, an entry
becomes part of the justification for the next one.

Field names come from what the endpoint actually returns, never from its doc comment: `content`,
`importance_score`, `game_time`. **`age_hours` is real-world elapsed time** and `decayed_importance` decays on
that same real-world clock, so neither is usable here. See
[`skyrimnet-memory-json-field-names.md`](../engine-findings/skyrimnet-memory-json-field-names.md) — this has
cost a test run twice.

### Eligibility, split by volatility

Gates exist to avoid **interrupting** an NPC. They are not a fairness mechanism; nothing here spreads entries
around, and it should not.

The split is by how fast the answer changes, not by which thread can read it:

| Stage        | Gate                                          | Why here                                                                          |
| ------------ | --------------------------------------------- | --------------------------------------------------------------------------------- |
| **scan**     | `GetLifeState() == kAlive`                    | Stable and free. Filtering a corpse before its memory query saves a round trip.    |
| **scan**     | `!IsDisabled()`                               | Same.                                                                              |
| **dispatch** | `!IsInCombat()`                               | Volatile — must be sampled at the moment of dispatch, not during the scan.         |
| **dispatch** | `!IsBleedingOut()`                            | Volatile.                                                                          |
| **dispatch** | `!IsActorBusy(formId)`                        | A SkyrimNet call; pointless to pay for someone who will not be picked.             |
| **dispatch** | no dialogue event within `fDiaryQuietMinutes` | A SkyrimNet call, and the gate that actually catches conversations.                |

**All of these reads are off-main.** `GossipSim::ActorAvailability` establishes the precedent and the
rationale: `LookupByID` takes the engine's own read-write lock over the all-forms map, `GetLifeState()` is an
inline field read, and unique NPCs' `Actor` objects are persistent and always resident — only their 3D
unloads. `IsInCombat()`, `IsDead()` and `IsBleedingOut()` are the same shape.

Two implementation notes follow from that:

- **Do not reach for `MainThreadEngine::LookupActor`.** It demands a `MainThread::Token` for a reason that is
  not the state flags — it calls `GetDisplayFullName()`, which is not a plain field read. The sweep needs a
  lean off-main helper, and it needs no display name at all: `GossipGraph::Participant::name` is cached at
  graph-build time precisely so the simulation never has to ask the engine.
- **Read life state through `AsActorState()`, not the inherited `GetLifeState()`.** The `ActorState` base sits
  at a different offset on AE (0xC0) than SE (0xB8); calling the inherited method directly reads the wrong
  bytes on one runtime. `GossipSim` carries the same note.

`THREADING_MODEL.md` used to list `RE::Actor::IsInCombat()` and `TESForm::LookupByID` among the calls that
"require the main thread", which shipped code in `GossipSim.cpp` already contradicted. That has been corrected
ahead of this phase: see **The exception: plain field reads on persistent forms** in
[`THREADING_MODEL.md`](../THREADING_MODEL.md), which records the two preconditions (guaranteed form lifetime,
and the call actually being a field read) and names `GetDisplayFullName()` as the counterexample. The diary
sweep is an application of that rule, not an exception to it.

#### The dialogue gate

`PublicIsActorBusy` is weaker than its name suggests and must not be relied on alone. It is a **cooperative
advisory flag** that only ever reflects a `PublicSetActorBusy` call from some plugin, and its own header says
"the plugin is responsible for calling `PublicClearActorBusy()` when done". SkyrimNet does not set it for its
own conversations, and NarrativeEngine has never called it. Take the gate — it is one cheap call and it is the
sanctioned "another plugin has claimed this actor" channel — but expect the dialogue check to do the work.

Use `GetRecentEvents(formId, n, filter)` rather than `GetRecentDialogue`. A non-zero `formId` returns events
*involving* that actor, and the filter takes the four real dialogue type strings — read off live payloads in
`SkyrimNetEvents.cpp`, not from a doc comment:

```text
dialogue, dialogue_background, dialogue_player_text, gamemaster_dialogue
```

`dialogue_background` is NPC-to-NPC, which `GetRecentDialogue` structurally cannot see — its header scopes it
to exchanges "between the player and the given NPC". Since the question is "has this actor spoken at all", the
event stream is the right source.

One known caveat, already documented at `src/VisitConclusionPoll.cpp:159`: the event stream appends
continuation speech onto the same event's `text` rather than firing new events, so an ongoing conversation
keeps its *starting* timestamp. That disqualified it for turn-by-turn polling. For a coarse "spoke within the
last N in-game minutes" gate it is fine, and the error direction is safe for any window longer than a typical
exchange.

`gameTime` on both endpoints is in-world game-seconds on the same clock as `EventLogUtil::NowGameTimeSeconds()`,
which the letter and visit composers have relied on since Phase 4.

#### Overdraft

Gating happens after ranking, so a sweep in which most finalists are mid-fight would otherwise produce one
entry instead of `N`. The sweep takes `N x fDiaryFinalistOverdraft` candidates and walks the ranked list until
`N` dispatches have landed or the list is exhausted. A gated NPC's watermark does not move, so they are
naturally near the front of the next sweep — no bookkeeping required.

#### Dispatch

`GenerateDiaryEntryByUUID` needs a UUID, so `FormIDToUUID` is resolved here rather than in the scan. It is not
an eligibility gate: generation works for any actor that has accumulated memories, and an actor with no
memories scores zero and never reaches this point. A zero UUID is skipped defensively and the walk continues
to the next candidate.

The `N` generations fire together with no spacing. SkyrimNet's own hotkey already does exactly this — the
*nearby actors* and *pinned actors* scopes of `TriggerGenerateDiaryBio()` submit a diary generation for every
matching actor at once — so `DiaryManager` handling concurrent submissions is established behaviour rather
than something this phase needs to be careful about.

### Threading

The sweep gets its own worker for the same reason gossip did, and the reasoning is worth writing down because
this is the fourth serial dispatcher in the plugin:

- **Not `AsyncDispatch`.** It is the cadenced queue. `Tick::PollOnPluginThread` drives `CombatEventLog`,
  `WeatherEventLog`, `TravelEventLog`, `EventHistoryWriter`, `FineRoads` and `GossipTick` off it every 500 ms.
  A sweep making hundreds of DLL round trips would stall all of them.
- **Not `EvalDispatch`.** Sharing it would let a diary sweep delay a Director evaluation.
- **Not `GossipDispatch`.** A gossip tick blocks on a synchronous LLM round trip for its whole duration.

`DiaryThread::Token` follows `GossipThread::Token` exactly, including the property that `MainThread::Run` /
`FireAndForget` do not accept it — the sweep touches no engine state that needs the main thread, so the
deadlock surface stays closed by construction.

The one main-thread need is the Papyrus dispatch itself, which **does** require main. The sweep therefore ends
by handing its finalist list to `AsyncDispatch::EnqueueWork`, whose job holds a `PluginThread::Token` and can
reach main. That handoff is the sweep completing, not the sweep pausing: the scan runs start to finish on the
diary thread and enqueuing the finalists is its last act.

The dispatch step is bounded — a handful of SkyrimNet calls and one `MainThread::FireAndForget` per entry — so
it does not stall the cadenced queue the way the scan would. `FireAndForget` rather than `Run` because nothing
needs the result: the native returns "submitted" and is asynchronous on SkyrimNet's side regardless.

### Scheduling: one persisted timestamp, compared against the game clock

The whole scheduler is one comparison against one co-saved `double`:

```text
if (NowGameTimeSeconds() - lastSweepGameTime >= interval) {
    lastSweepGameTime = NowGameTimeSeconds();
    DiaryDispatch::EnqueueWork(sweep);
}
```

Real time does not enter into it at any point. The only question is how much *in-game* time has passed since
the last sweep, so there is no unpaused-seconds accumulator, no separate poll-rate throttle, and no
`iDiaryTickIntervalSeconds`. `Tick::PollOnPluginThread` already runs every 500 ms and the comparison is two
field reads; throttling it would be optimizing nothing.

This is a deliberate departure from `GossipTick`, and the departure is what buys the simplicity:

**Persisting the timestamp deletes the rebase logic.** `GossipTick` keeps `g_nextDueGameDay` in memory, so it
needs an explicit "clock went backwards further than drift explains → re-base" branch and an `OnSessionStart`
hook, both of which exist to survive a load into an older world. A co-saved timestamp *travels with the save*:
load a game from ten hours ago and the restored timestamp is the one that was current then, so the comparison
is consistent with no special case at all. The rollback that breaks an in-memory schedule is exactly what
makes a persisted one correct.

**Coalescing is emergent, not implemented.** Gossip never coalesces — passing 24 hours with `T` crosses two
harvest boundaries and runs two stamped ticks, because each simulates a distinct slice of elapsed time. A
diary sweep has no such property: it reads the world as it is *now* — current watermarks, current memories,
current actor state — so running it twice back to back would sweep a world the first pass just consumed. One
comparison against one timestamp gives exactly one sweep no matter how many intervals were skipped, with no
backlog cap, no queue of owed sweeps, and no stamped `asOf`. A sweep is always "now".

Two details worth fixing in the design rather than discovering in play:

- **Overwrite with `now`, not with `lastSweep + interval`.** After a 72-hour `T` skip the next sweep should be
  a full interval away, not immediate.
- **Write the timestamp when the sweep is enqueued, not when it completes.** A sweep cancelled by a load would
  otherwise never advance the clock — and the co-save record is rolled back by that same load anyway, so the
  two stay consistent either way.

On a save with no record — a new game, or first install on an existing one — the timestamp initializes to the
current game time rather than to zero, so the first sweep is one interval away instead of firing immediately
into a world the player has not touched yet.

#### Why the interval is short

The default is **4 in-game hours**, which is deliberately far more often than any individual NPC will produce
an entry.

The threshold, not the cadence, is what decides whether someone writes. A given actor will rarely accumulate
`fDiaryImportanceThreshold` worth of new memories inside four hours, so most sweeps will not pick them. What a
frequent sweep buys is *turnover in the ranking*: the notable and player-adjacent NPCs are consuming their own
accumulated memories every time they journal, which drops them down the ranking, and a short interval means
the sweeps that happen while they are climbing back up go to background NPCs who have quietly crossed the
threshold. A long interval would spend each of its few slots on whoever is at the top at that moment, which is
persistently the same handful of people.

So the cadence is the mechanism by which the province gets reached at all, without needing any explicit
fairness machinery of the kind gossip's bucket sampling provides.

### What this deliberately does not touch

- **Alpha Canon.** A diary entry modifies no vanilla world state. It is a row in SkyrimNet's store.
- **`LLMTextSanitizer`.** We never handle the entry text — `DiaryManager` composes it and stores it, and no
  free-form LLM string crosses into NarrativeEngine. If a later phase renders diary rows in the dashboard,
  sanitization applies at that point, in that phase.
- **The ESP.** No quest, no alias, no record, no `.psc` change. The Papyrus bridge is a `DispatchStaticCall`
  against SkyrimNet's own script object.

---

## Settings

`[Diary]`

| Key                         | Proposed default | Purpose                                                                  |
| --------------------------- | ---------------- | ------------------------------------------------------------------------ |
| `bDiaryEnabled`             | `false`          | Master switch. Ships off, as gossip did.                                 |
| `bDiaryLogEnabled`          | `true`           | Dedicated trace at `SKSE/NarrativeEngine_Diary.log`.                      |
| `fDiaryIntervalGameHours`   | `4.0`            | In-game hours between sweeps. Short on purpose — see the scheduling note. |
| `iDiaryEntriesPerSweep`     | `3`              | How many generations a sweep dispatches at most.                         |
| `fDiaryFinalistOverdraft`   | `3.0`            | Rank this many times `iDiaryEntriesPerSweep` so gates trim, not gut.      |
| `fDiaryImportanceThreshold` | `1.5`            | Minimum summed importance to qualify at all.                             |
| `iDiaryMemoriesPerActor`    | `20`             | The K in the top-K sum, and the query's `maxCount`.                       |
| `fDiaryQuietMinutes`        | `30.0`           | In-game minutes of dialogue silence required before dispatch.             |
| `bDiaryRespectActorBusy`    | `true`           | Honour `PublicIsActorBusy`.                                               |
| `sDiarySkipTags`            | `ne_gossip`      | Comma-separated `excludeTags` for the source-memory query.                |

---

## File map

```text
include/DiaryThread.h     zero-sized proof-of-diary-thread token
include/DiaryDispatch.h   serial worker + cancellation handle
src/DiaryDispatch.cpp
include/DiaryTick.h       scheduler API
src/DiaryTick.cpp         game-clock comparison, co-save record for the sweep timestamp
include/DiarySweep.h      the job
src/DiarySweep.cpp        per-actor watermark + memory scan, score, rank, gate, dispatch
include/DiaryLog.h        dedicated trace
src/DiaryLog.cpp
```

Changes to existing files: three new wrappers in `SkyrimNetAPI` (`GetDiaryEntries`, `IsActorBusy`, the Papyrus
bridge); a `[Diary]` block in `Settings`; `DiaryDispatch::Start()` at `kDataLoaded` and `DiaryTick::Poll` in
`Tick::PollOnPluginThread`; one new co-save record alongside the existing ones, plus `DiaryDispatch::CancelAll`
on the `OnLoad` / `OnRevert` paths that already cancel gossip. (The `THREADING_MODEL.md` correction noted
above is already landed and is not part of this phase's diff.)

---

## Prerequisite — capture a live `PublicGetDiaryEntries` payload

**Step 1 is a data-gathering step, not a coding one, and nothing downstream can be written honestly without
it.** The whole watermark rests on a row shape nobody here has seen. The header documents `startTime` /
`endTime` as "seconds since epoch" and says rows carry "all fields from `DiaryEntry::ToJson()` plus an
`actor_name` field" — which names nothing. Three things have to come out of one captured payload:

- **The field names, and which field is the timestamp.** `age_hours` on the memory endpoint turned out to be
  wall-clock while `game_time` is in-world; the diary row's could plausibly be either.
- **The row ordering.** The design passes `maxCount = 1` and assumes that yields the *newest* entry. If the
  ordering is ascending it yields the oldest, and every watermark is then wrong in the most damaging possible
  direction — an actor's whole history counts as new, forever.
- **Which FormID keys the diary table.** Memories are keyed on the reference (`Participant::actorRef`) and
  diary entries are assumed to match. That is an assumption.

Every one of these fails **soft**. `nlohmann::value()` returns the default for a missing key, so a wrong name
reads as `0`, every watermark reads as "never journalled", and the sweep dispatches for whoever has the
longest history in the save — while the log reports nothing wrong. Capture the payload, dump it verbatim
(`GossipHarvest` does exactly this once per session under `debugMode`), and write the parser against what came
back.

---

## Implementation plan

Not yet written. The design above is the contract; steps land once it is agreed, beginning with the payload
capture above.

---

## Open questions

None outstanding.

Resolved during design:

- ~~**Does the Papyrus dispatch need the main thread?**~~ Yes. Settled, and the threading section is written
  against it rather than hedging.
- ~~**What interval reads best?**~~ 4 in-game hours, configurable. The reasoning is a design point rather than
  a tuning default and now lives under *Why the interval is short*.
- ~~**How does `DiaryManager` behave when handed several generations at once?**~~ Not our problem — the
  *nearby* and *pinned* scopes of SkyrimNet's own `TriggerGenerateDiaryBio()` hotkey already submit many at
  once. The `N` generations fire together with no spacing.
- ~~**Does generation work for an actor SkyrimNet has never seen loaded?**~~ It works for any actor that has
  accumulated memories, and an actor with none scores zero and never reaches dispatch. `FormIDToUUID` is
  consequently no longer a scan gate — it is just the argument resolution at dispatch, with a defensive skip.
- ~~**Extract a generic `SerialDispatch<Token>`?**~~ Out of scope; moved to *Deferred*.
- ~~**Is the `PublicGetDiaryEntries` row shape verified?**~~ Not a question — a prerequisite. Promoted to its
  own section above.
