# Main-thread stutter audit — Director tick and beat dispatch

## Context

Playtesters report micro-stutters coinciding with:

1. The Director's evaluation tick firing (default cadence: every 60 s of unpaused
   play — `iTickIntervalSeconds`).
2. The Director deciding to dispatch a beat (the "beat-select" branch inside
   `BeatSystem::ConsiderBeat`, which only runs when tension eval doesn't already
   want to advance the phase and the global cooldown has elapsed).

This document catalogues work currently pinned to the main thread on those two
paths that does not intrinsically need to be there. Each finding lists what
runs, why it's expensive, why it's on the main thread today, and what a
plausible fix looks like. Findings are ordered by rough stutter impact.

The audit is scoped to the two paths named above. Other main-thread work
(dashboard interop, save/load, cosave, sink registration, hotkey capture, MCM
event pumps) is out of scope because it doesn't correlate with the reported
stutters.

## Architectural baseline — where we already are

The engine already has a decent bones for off-loading work:

- `AsyncDispatch::EnqueueWork(fn)` runs `fn` on a single background worker
  thread (`AsyncDispatch.cpp:23`). Producer-consumer queue, one job at a time,
  exceptions swallowed.
- `AsyncDispatch::MarshalToMainThread(fn)` uses SKSE's task interface to hop
  back to the main thread when engine touches are needed
  (`AsyncDispatch.cpp:94`).
- `BeatSystem` runs its own polling worker thread that reads a small set of
  engine bools (`EngineUtils::IsGamePaused` / `IsPlayerInCombat` /
  `IsPlayerInDialogue`) directly from a non-main thread every
  `iBeatSystemPollIntervalMs`. Precedent: **off-main reads of engine singletons
  are considered acceptable in this codebase**, provided the singleton pointer
  is stable and the read is a plain bool/pointer load (`BeatSystem.cpp:78`).
- The evaluation pipeline already correctly moves Phase B/C/D of the tick onto
  the worker: `BuildPromptContext`, the LLM round-trip, and `ParseDecision`
  all run off-main; only `BuildSnapshot` and `ApplyDecision` are pinned to main
  (`EvaluationPipeline.cpp:441-518`).

The stutters therefore come from the pieces still stuck on main, not from a
missing async substrate.

---

## Findings — highest impact first

### 1. `SenderCandidatePool::Build` / `CountViable` run on the main thread during beat-select

**Where:** `BeatSystem.cpp:1057-1122` (`ConsiderBeat`, Gates 6–7), which calls
`LetterComposer::CollectSenderCandidates()` and
`VisitComposer::CollectSenderCandidates()`, both of which delegate to
`SenderCandidatePool::Build(opts)` in `SenderCandidatePool.cpp:286`.

**What runs synchronously on main:**

- `SkyrimNetAPI::GetActorEngagement(fetchCap, …)` — DLL boundary call into
  SkyrimNet's memory system. `fetchCap = maxCandidates * 3` (36 by default).
  This walks SkyrimNet's engagement scoring across the actor set.
- For every returned entry (up to 36 per pool):
  - `RE::TESForm::LookupByID` + `As<RE::Actor>` (form-registry hash lookup).
  - `IsDead()`, `IsDisabled()`.
  - `AliasWalkFilter::IsActorStoryActive` (`AliasWalkFilter.cpp:72`) — walks
    every alias-instance the actor is plugged into via
    `ExtraAliasInstanceArray`. For each entry: fetches the parent quest, calls
    `IsRunning() / IsStopped() / IsCompleted()`, checks the alias
    `kReserves` / `kQuestObject` flags, resolves the alias's package list. Under
    a `BSReadLockGuard` on the alias array.
- For each survivor: **another** DLL boundary call to
  `SkyrimNetAPI::GetMemoriesForActor(formId, memoryFetchCap, playerName)` —
  memory DB read for that specific actor. Up to 24 memories requested.
- For every memory: JSON parse, `LLMTextSanitizer::Sanitize` over `content`,
  `emotion`, `location` (per-string Unicode normalization pass), `std::sort`
  by age, truncate, reverse.
- Optional `std::shuffle` at the end.

And this happens **twice per beat-select tick** — once for letters, once for
visits — because both `npc_letter` and `npc_visit` are enabled at Phase 10.
Together that's up to 72 engagement entries walked and up to 24 memory-DB
round-trips on the main thread while the player waits.

**Why it's on main:** `LookupByID`, `As<Actor>`, `IsDead`, `IsDisabled`, and
`ExtraAliasInstanceArray` reads all touch engine state. In principle they can
be read off-main (BeatSystem already reads engine bools off-main and calls it
"acceptable"), but `AliasWalkFilter`'s walk over the extra-list holds a
`BSReadLockGuard` on the alias array — that lock exists precisely because the
engine mutates the array on the main thread, so an off-thread read is at least
formally justified. `GetActorEngagement` / `GetMemoriesForActor` are SkyrimNet
DLL calls that the API-doc contract says are thread-safe once
`PublicIsMemorySystemReady()` returns true (and we already call them from the
AsyncDispatch worker inside `EvaluationPipeline::EnqueueWork`'s
`BuildPromptContext`).

**Why it stutters:** measured cost dominated by the two SkyrimNet DLL calls (the
memory DB does a semantic-search pass per actor) plus the O(candidates × alias-
entries) walk. In the debug logs the "walk complete" summary line shows this
is not trivial work.

**Fix sketch:** move both `CollectSenderCandidates()` calls out of the
main-thread body of `ConsiderBeat`. The cleanest structure mirrors what
`EvaluationPipeline::BeginEvaluation` already does: perform the main-thread
gate walk (Gates 1–5, which are cheap bool checks + one
`CheckGlobalBeatPreconditions`), then `AsyncDispatch::EnqueueWork` a lambda
that runs Gate 6's `AvailableMatching` and Gate 7's sender-candidate collection
off-main, then marshals back to fire the beat-select LLM. `AliasWalkFilter`'s
`BSReadLockGuard` and the `TESForm::LookupByID` calls are the same shape the
existing off-main callback paths already trust.

If that's judged too aggressive, a smaller win is available: today the pool is
built even when `npc_letter` / `npc_visit` end up dropped by other gates. The
`npcLetterPresent` / `npcVisitPresent` guards at `BeatSystem.cpp:1086-1087`
already avoid building a pool the beat won't use — but the pool is still built
whenever the beat *is* a candidate, even though the *result* only gates on
`size < min`. Caching the last build for a few seconds (they're already
shuffled per-call, so cache-hit shuffling is fine) would cut cost during
back-to-back near-misses.

### 2. `EvaluationPipeline::BuildSnapshot` runs on the main thread every firing tick

**Where:** `EvaluationPipeline.cpp:123-191`, called from
`BeginEvaluation` before the async enqueue.

**What runs synchronously on main:**

- `SkyrimNetAPI::GetRecentEvents(0, eventTail, "")` — DLL boundary call.
  `eventTail` defaults to a nontrivial value (`iSkyrimNetEventTailSizeForPrompt`);
  under normal play this returns a JSON blob of dozens of events **as a single
  serialized string** that then has to be re-parsed by the worker.
- `DecisionLog::Tail(n)` — deque copy under `shared_lock`. Cheap; not a
  problem.
- `PlayerCharacter::GetSingleton` reads, `GetCurrentLocation()`,
  `GetParentCell()`, `GetFullName()` on each — genuinely main-thread-only.
- `AlphaCanon::EvaluateAll()` — `IsInCombat`, `IsInDialogue` (menu check),
  `IsInScriptedScene` (walks `GetCurrentScene`), `IsInDoNotDisturbCell`
  (`GetParentCell`, `GetFormEditorID`, CSV match). All main-thread-appropriate;
  none is a stutter driver individually.

**Why it stutters:** `GetRecentEvents` is the load-bearing cost here. Fetching
the tail requires SkyrimNet to serialize its event ring to a JSON string,
cross the DLL boundary, then we discard the fetch (we don't even parse it in
`BuildSnapshot` — the worker re-parses it in `BuildPromptContext`). Everything
else in the snapshot function is a cheap pointer/int read.

**Why it's on main today:** the surrounding engine reads (`GetCurrentLocation`,
`GetParentCell`, `IsInCombat`, `IsInScriptedScene`) require the main thread.
But `GetRecentEvents` does not — it's a SkyrimNet DLL call gated on
`PublicIsMemorySystemReady()` (`SkyrimNetAPI.cpp:85`), and we already call it
from the AsyncDispatch worker path in
`DashboardUIManager::ComposeFullStateJSON` and `EventHistoryWriter`.

**Fix sketch:** split the snapshot into a tiny main-thread half (player/cell/
location/calendar/AlphaCanon) and a worker-thread continuation that populates
`skyrimNetEventsJSON`. `EnqueueWork` already runs the whole `BuildPromptContext`
step off-main — just move the fetch there too and delete the field from the
main-thread `Snapshot` struct. Net cost saved on main: one full DLL call per
tick.

### 3. `EventHistoryWriter::Poll` does file I/O on the main thread every 5 s of unpaused play

**Where:** `Tick.cpp:99` calls `EventHistoryWriter::Poll(elapsedSec)` from
inside `PollOnMainThread`. When the accumulator crosses
`iEventHistoryFlushIntervalSeconds` (5 s default), `FlushLocked` runs.

**What runs synchronously on main:**

- `SkyrimNetAPI::GetRecentEvents(0, 200, "")` — a **200-event** DLL fetch.
- `nlohmann::json::parse` of that string.
- `SkyrimNetEvents::FormatEventsText` over the whole array (per-event string
  synthesis; loops through every event and rebuilds `text` from `type` +
  `data`).
- Drain of combat / weather / travel pending-history queues, each of which
  renders bodies at drain time.
- `std::stable_sort` of the combined batch by localTime.
- `std::ofstream` write per entry, `flush()` at end.

**Why it stutters:** this is by far the most expensive thing that runs on the
main thread on a *fixed cadence*, unrelated to the Director tick. Because
`iTickIntervalSeconds = 60` and `iEventHistoryFlushIntervalSeconds = 5`, the
history flush hits roughly 12× more often than the tick — so it will produce
a periodic stutter the player perceives as a soft rhythm even when the
Director tick isn't running. On combat-heavy sessions the 200-event fetch is
worst-case: `FormatEventsText` allocates many small strings, and the writer
does synchronous file I/O with an explicit `flush()`.

**Why it's on main today:** `SKSE::log::log_directory()` and `std::ofstream`
have no thread affinity — this can be a pure worker-thread task. The event
sources it drains (`CombatEventLog::DrainHistoryTail`, etc.) are all
`std::mutex`-guarded and already thread-safe.

**Fix sketch:** replace the direct call at `Tick.cpp:99` with an
`EnqueueWork` — the writer needs no main-thread state. The unpaused-elapsed
accumulator can stay main-thread-side (compute the elapsed increment on main,
then hand the flush to the worker whenever it crosses). Alternatively the
writer could own its own thread with its own accumulator, similar to
`AsyncDispatch`.

### 4. `DashboardUIManager::PushFullState()` runs after every applied decision, even when the dashboard is hidden

**Where:** `EvaluationPipeline::ApplyDecision` calls
`DashboardUIManager::PushFullState()` at `EvaluationPipeline.cpp:431`, which
runs `ComposeFullStateJSON()` at `DashboardUIManager.cpp:1066`.

**What runs synchronously on main:**

- Another full `SkyrimNetAPI::GetRecentEvents(0, 20, "")` DLL fetch.
- `nlohmann::json::parse`, `std::reverse`, `FormatEventsText` over all 20.
- `CombatEventLog::GetRenderedTail`, `WeatherEventLog::GetRenderedTail`,
  `TravelEventLog::GetRenderedTail` — each takes a `scoped_lock`, copies a
  vector of internal events, sorts by `localTime`, renders `text` for each
  entry.
- `SkyrimNetEvents::BuildMergedTimeline` — merge + stable-sort + condensation
  walk that groups combat hits into "trades blows" summaries.
- `LetterPool::GetSlotSnapshots` per-slot snapshot + body-preview strip of
  `<font>` tags.
- `nlohmann::json::dump` on the whole payload.
- `PrismaUI_API::InvokeJS(g_view, "updateFullState", json)` — the InvokeJS is
  the only cheap part; the JSON compose above dominates.

**Why it's a stutter:** the whole payload is composed **even when the dashboard
is hidden**. `PushFullState` early-outs on `!PrismaUI_API::IsAvailable() ||
g_view == kInvalidView` but does **not** check `g_visible.load()`. In a normal
session where the player rarely opens the dashboard, this is pure waste on the
main thread at exactly the moment we're already running Phase D of the tick
(`ApplyDecision`).

Two independent problems:

1. Should not run at all when the dashboard is hidden.
2. Even when visible, the compose is doing 3× the work it needs to (three
   DLL calls + full merged-timeline recompute is the same work `BuildSnapshot`
   already did seconds ago on the same tick).

**Fix sketch:**

- Guard the whole body of `PushFullState` on `g_visible.load()` — cheap and
  immediate.
- When visible, the compose can happily run on the AsyncDispatch worker as
  long as the LetterPool + BeatSystem readers it hits are mutex-guarded
  (they are). `PrismaUI_API::InvokeJS` is itself documented as async by our
  comment at `EvaluationPipeline.cpp:429`, so no marshaling loss.
- If we want to be surgical, `BuildSnapshot` and `PushFullState` already
  fetch the same event tail and internal logs. Have `ApplyDecision` reuse the
  merged JSON the pipeline already built — or at least share the timestamp
  and event tail so the second `GetRecentEvents` DLL call goes away.

### 5. Every enabled beat's `IsAvailable(ctx)` runs on the main thread during Gate 6

**Where:** `BeatSystem.cpp:1057` calls `BeatRegistry::AvailableMatching`, which
at `BeatRegistry.cpp:176` invokes each beat's `IsAvailable(ctx)` under
`g_mutex`.

**What runs synchronously on main per beat:**

- `NPCLetterBeat::IsAvailable` (`NPCLetterBeat.cpp:541`) —
  `SkyrimNetAPI::GetActorEngagement(5, …)` DLL fetch + JSON parse.
- `NPCVisitBeat::IsAvailable` (`NPCVisitBeat.cpp:1276`) —
  `SenderCandidatePool::CountViable(&VisitViabilityFilter_ForCountViable, min)`.
  This is the *same* engagement + alias-walk pipeline as Finding 1, though it
  exits early via `stopWhen` once `min` viable actors are found.

Even the "short-circuit" path is a `GetActorEngagement` DLL call + several
`LookupByID` + `IsActorStoryActive` walks. And this runs *before* Gate 7 does
its own full `SenderCandidatePool::Build` — so today, on any tick where the
gates open, we do the engagement/DB work at least twice for each of letters
and visits.

**Fix sketch:** most direct is to fold this into the async continuation from
Finding 1 — `AvailableMatching` moves off-main, and the pool-build result
becomes the input to Gate 6 rather than a duplicate query. As an intermediate
step, `NPCLetterBeat::IsAvailable` and `NPCVisitBeat::IsAvailable` could cache
their engagement result for a few seconds so a rejected tick doesn't repeat
the same query on the next tick.

### 6. `CombatEventLog::Poll` walks the bleedout table under a mutex every main-thread poll cycle (≈500 ms)

**Where:** `Tick.cpp:79` → `CombatEventLog::Poll` at `CombatEventLog.cpp:540`,
which runs `PollPlayerCombatLocked` + iterates `g_bleedingOut` doing
`TESForm::LookupByID`, `IsDead()`, `AsActorState()->IsBleedingOut()`,
`WithinRadius(actor->GetPosition(), radius)`.

**Why it's on the main thread:** the poll interval is the `Tick.cpp` driver's
500 ms cycle, which is the marshaled main-thread task. Cost per call is small,
but this cost is paid **regardless of whether the tick fires** — it's the
per-500ms background hum of the main-thread poll body.

**Why it's a minor stutter contributor, not a major one:** the bleedout table
is typically empty (only populated by the `BleedoutSink` when the player
witnesses a collapse). The per-500ms `PollPlayerCombatLocked` is a single
`IsInCombat()` read plus a change diff. So the *routine* cost is fine — it's
only during / just after a big fight that the `g_bleedingOut` iteration adds
up.

**Fix sketch:** the singleton reads here (`RE::PlayerCharacter::GetSingleton`,
`TESForm::LookupByID`, `IsInCombat`, `IsBleedingOut`, `GetPosition`) are all
things BeatSystem already reads off-main, so this poll is a candidate to move
off the marshaled `PollOnMainThread` and into a worker-thread cadence — but
the payoff is small compared to Findings 1–4.

### 7. `TravelEventLog::Poll` runs `Region::ForPlayer()` on the main thread every 500 ms

**Where:** `Tick.cpp:94` → `TravelEventLog::Poll` → `BuildFreshSnapshot`
(`TravelEventLog.cpp:221`) → `Region::ForPlayer` (`Region.cpp:145`).

**What runs:** `HoldGrid::LookupPlayer` (fast-path hash lookup, cheap), and on
miss a `GetCurrentLocation()` + `BGSLocation::parentLoc` walk up to depth 16
looking for `LocTypeHold`. Every 500 ms of unpaused play. Under the
`TravelEventLog` mutex, and even if the cell / location did not change.

**Why it's a minor contributor:** the HoldGrid fast-path is O(1) and covers
most exterior cells. The parent walk hits only interior cells and unmapped
exteriors. So on 95% of ticks it's a hash lookup — trivial. The concern is
that we do this every 500 ms whether or not the location changed. A cheaper
change gate (compare `player->GetParentCell()` FormID against last-seen) would
early-out the majority of these calls.

**Fix sketch:** in `TravelEventLog::Poll`, first read `pc->GetParentCell()` and
compare to a cached FormID. If unchanged, return immediately. Only build a
fresh `TravelSnapshot` on cell change. This preserves fast-travel-flag
consumption logic (which the sink still sets independently) and avoids the
per-500ms hold walk.

### 8. Miscellaneous smaller items on the main-thread poll body

Not individually stutter-scale, but worth mentioning:

- `Tick::PollOnMainThread` calls `RE::UI::GetSingleton()->GameIsPaused()`,
  `CombatEventLog::Poll`, `WeatherEventLog::Poll`, `TravelEventLog::Poll`, and
  `EventHistoryWriter::Poll` **every 500 ms** whether or not the tick fires.
  Only the pause read genuinely needs to be main-thread. The Combat / Weather
  / Travel pollers all touch engine singletons that BeatSystem's worker
  reads off-main today. Consolidating them onto the BeatSystem worker (or a
  dedicated poll worker) would let the main-thread task be a one-line
  `elapsedSec` accumulator plus a killswitch check.
- `PhaseTracker::Tick()` (`Tick.cpp:127`) is called on every firing tick.
  Cheap, but bundled with the LLM eval kick — worth confirming it does no
  event-log I/O.
- `LLMTextSanitizer::Sanitize` is called on lots of small strings in the
  candidate-pool build path. Per-string cost is small, but pool build does
  it hundreds of times per tick. Not a bug — just amplifies Findings 1 and 5.

---

## Summary — recommended order of attack

If we ship one fix, it should be Finding 4 (skip `PushFullState` when the
dashboard is hidden) — it's a one-line change, zero risk, and eliminates a
guaranteed stutter that happens once per firing tick.

If we ship a batch, the biggest reduction in per-tick main-thread time comes
from Findings 1, 2, and 3 together:

1. Move `EventHistoryWriter::Poll`'s flush body onto the AsyncDispatch worker
   (Finding 3). Kills the biggest fixed-cadence stutter.
2. Move `GetRecentEvents` out of `BuildSnapshot` and into the worker
   continuation (Finding 2). Halves the per-tick DLL work.
3. Move `LetterComposer::CollectSenderCandidates` /
   `VisitComposer::CollectSenderCandidates` off main by folding Gates 6–7 into
   an async continuation of `ConsiderBeat` (Finding 1). Kills the dispatch-
   moment stutter that playtesters describe.

Finding 5 is a natural consequence of the Finding 1 fix. Findings 6–8 are
polish once the big three are done.
