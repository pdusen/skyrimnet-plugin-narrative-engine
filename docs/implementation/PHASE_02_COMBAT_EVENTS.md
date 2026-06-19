# Phase 02 — Internal Combat Event Source

The first feature beyond MVP. Adds an internally-tracked combat event stream that fills the gap SkyrimNet's event
log leaves around moment-to-moment combat, and merges it into the same `recent_events` array the Director's prompt
and the dashboard already consume.

---

## Why this phase exists

SkyrimNet's event log captures dialogue lines and character deaths well, but is silent on combat dynamics — who
started swinging, who's actively trading hits, when a fight begins and ends. During Phase 01 verification we saw a
concrete consequence: a brawl played out for nearly a minute with no signal reaching the Director until the killing
blow, at which point a single `"X killed Y"` line was the only combat input the LLM had to judge tension on.

This phase plugs that hole with a NarrativeEngine-owned event source. It hooks SKSE's combat-related events,
stores them in a small in-process log, prunes them on phase boundaries the same way the rest of the pipeline
respects, and merges them into the existing recent-events stream. To keep the LLM from drowning in repetitive
"X strikes Y" lines, runs of individual hits between non-hit events get **condensed into a single summary entry**
before the merge surfaces them.

---

## Scope of capture

Five internal event kinds — kept deliberately narrow so the log stays signal, not noise:

- **`combat_start`** — Player's `IsInCombat` flips false → true. **Detected by polling**
  `player->IsInCombat()` on the existing main-thread tick (see "Player combat-state poll" below); the
  `TESCombatEvent` sink path does not reliably fire with `actor == player`.
- **`combat_end`** — Player's `IsInCombat` flips true → false. Detected by the same poll.
- **`hit`** — Any actor-on-actor hit within ~6000 engine units (~90 ft) of the player. Captured from
  `TESHitEvent`.
- **`collapse`** — Any actor within ~6000 units of the player drops to the engine's bleedout state — visually,
  they slump and stop fighting. Captured from `TESEnterBleedoutEvent`. Despite the engine's "bleedout" naming,
  this is the recovery posture, not a fatal one — essential actors enter it instead of dying, and non-essential
  actors can enter it too if their `IsEssential` / `IsProtected` / `NoBleedoutRecovery` flags align. Rendered
  as `"X collapses"`.
- **`regain_footing`** — A previously-collapsed actor stands back up and re-enters the fight. There is no
  dedicated SKSE exit-bleedout event; we detect this by maintaining a small "currently bleeding out" actor set
  populated from `TESEnterBleedoutEvent` and polled on each Director tick (which already runs a 500 ms main-
  thread poll in `src/Tick.cpp`). When `actor->AsActorState()->IsBleedingOut()` returns false for a tracked
  actor, we drop it from the set and emit `regain_footing` rendered as `"X regains their footing"`. Actors that
  die from the collapsed state are observed via `actor->IsDead()` on the same poll and dropped silently — the
  death itself is already in SkyrimNet's event log as a `death` entry.

Hit / collapse / regain-footing filtering:

- Skip if the **target** is null, not an actor, or has no usable display name — we still need *somebody* getting
  hit. (For collapse: same rule applied to `event->actor`.)
- The hit `cause` is **not** required to be a named actor. Environmental damage (traps, falling boulders,
  poison, ambient hazards) is narratively meaningful — those events stay in the log; see "Damage source
  resolution" below.
- Skip hits where `cause` is non-null and `cause == target` (self-damage from concentration spells etc.).
- Skip collapses and hits where the target is farther than ~6000 units from the player. For hits where the
  cause is a named actor, *either* party being in range is enough.
- `regain_footing` is subject to the same distance gate, checked at recovery time: if the previously-collapsed
  actor is now outside ~6000 units of the player, drop them from the bleedingOut set silently and don't emit
  the event. The player can't witness the recovery, so it isn't narratively significant.

### Damage source resolution

For each `TESHitEvent`, resolve a best-effort `sourceLabel` string using this cascade. Stop at the first match:

1. **Named actor cause** — `event->cause` resolves to an `Actor*` with a non-empty display name. Use the actor
   name (e.g., `"Hans"`). This is the common case for melee / ranged combat and is handled exactly as today.
2. **Identifiable form via `event->source`** — `event->source` resolves through `RE::TESForm::LookupByID`:
   - `MagicItem` (spell, ingested poison effect, ench. proc) whose name or any effect mentions "poison" /
     "venom" → `"poison"`.
   - `TESObjectACTI` (activator — common for falling traps and hazards) → use the activator's display name,
     lowercased and pluralized if singular (`"boulders"`, `"spikes"`, `"the spike wall"`). Heuristic — fine to
     ship a small lookup of well-known editor IDs and fall back to the raw name otherwise.
   - `BGSExplosion` / `TESObjectWEAP` with no living wielder (the cause was null but the source is a weapon
     form, typical for dart/swinging-blade/pressure-plate traps) → `"a trap"`.
   - Other `MagicItem` (a spell-trap without a poison hint) → `"a spell trap"`.
3. **Total fallback** — none of the above succeed. Emit the hit with no source label; the renderer drops the
   `"from ..."` clause entirely (see "Condensation rules" below for the worked examples).

The resolver is a small free function in `src/CombatEventLog.cpp`; the keyword and editor-ID lookups can grow
over time as we observe what actually shows up in logs. Initial implementation should at least cover the
worked examples below — traps, boulders, poison — and let everything else fall through to the bare `"X took
damage"` form rather than guessing.

Combat-state events are player-only on purpose — per-NPC combat-state churn is high-volume and not narratively
load-bearing. Bleedout events, by contrast, are inherently dramatic and rare enough to be worth tracking for
any nearby actor.

---

## New module: `CombatEventLog`

Files: `include/CombatEventLog.h`, `src/CombatEventLog.cpp`.

Public surface:

```cpp
namespace NarrativeEngine::CombatEventLog
{
    void Initialize();   // register TESCombatEvent + TESHitEvent sinks
    void Shutdown();     // best-effort sink deregistration

    // Called by PhaseTracker after AdvanceTo/Reset commits the new phase.
    // Drops events older than the current encounter's start (if the player
    // is in combat) or wipes the log entirely (if not).
    void OnPhaseAdvanced();

    // Returns a JSON array of currently-retained internal events shaped like
    // SkyrimNet events: { type, localTime, gameTime, originatingActorName,
    // targetActorName, text }, oldest-first. `text` is pre-rendered with the
    // same "[N ago]" relative-game-time prefix FormatEventsText produces, so
    // the merger can interleave with SkyrimNet events transparently.
    nlohmann::json GetRenderedTail(double currentGameTimeSeconds);

    // SKSE co-save persistence (record type 'NECE'). OnSave prunes-then-writes
    // so the on-disk payload mirrors in-memory state — no resurrection of
    // already-purged encounters across save/load.
    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
}
```

Private state:

- `std::mutex g_mutex` — event sinks fire on a non-main thread, so every read/write goes through the mutex.
  Same threading discipline already used by `PhaseTracker.cpp` and `DashboardUIManager.cpp`.
- `std::vector<InternalEvent> g_events` — bounded ring (cap ~256) so a pathological capture rate can't grow
  unbounded. Drop oldest on overflow.
- `double g_currentEncounterStartRealTime` — Unix-epoch `localTime` of the most recent player `combat_start`,
  or `0.0` when the player isn't in combat. Set on `combat_start`, cleared on `combat_end`. Drives pruning.
- `std::unordered_map<RE::FormID, std::string> g_bleedingOut` — actors currently in bleedout, keyed by FormID,
  value holds the cached display name so `regain_footing` can render even if the actor's `TESForm*` is no
  longer resolvable. Populated by the bleedout sink, drained by the per-tick poll. Not persisted directly;
  rebuilt on load (see Persistence).
- `struct InternalEvent { Kind kind; double localTime; double gameTime; std::string actorName; std::string
  targetName; bool actorIsNamedActor; };` — no FormIDs persisted on the event itself; the event is consumed
  for text rendering soon after capture and pruned out within a phase or two. For `hit` events, `actorName`
  holds the resolved damage-source label (named actor, `"poison"`, `"a trap"`, or empty for the bare-fallback
  case), and `actorIsNamedActor` distinguishes a real attacker from an environmental source — the merge /
  condensation step uses this to decide whether the entry can ever appear "two-sided". For `collapse` /
  `regain_footing`, `targetName` is unused and `actorIsNamedActor` is always true.

---

## Event sinks

Two sinks, both registered against `RE::ScriptEventSourceHolder::GetSingleton()` at plugin init in
`src/Plugin.cpp`'s `kDataLoaded` handler:

```cpp
auto* src = RE::ScriptEventSourceHolder::GetSingleton();
src->AddEventSink<RE::TESHitEvent>(GetHitSink());
src->AddEventSink<RE::TESEnterBleedoutEvent>(GetBleedoutSink());
```

The implementation pattern follows the existing `HotkeySink` in `src/DashboardUIManager.cpp:35-103`: static
singleton accessor, `ProcessEvent` does the minimum work synchronously (capture names + timestamps, push under
mutex), and no main-thread marshalling is needed because storage is mutex-protected and the sink doesn't touch
engine APIs that require main-thread context.

`HitSink` does the distance check against `RE::PlayerCharacter::GetSingleton()->GetPosition()` before capturing.

`BleedoutSink` resolves `event->actor` to an `RE::Actor*`, runs the same null/name/distance gates as `HitSink`,
emits a `collapse` event, and inserts the actor's FormID + display name into `g_bleedingOut`.

Player combat-state changes are intentionally *not* on a sink — see "Player combat-state poll" below.

### Player combat-state poll

`TESCombatEvent` fires reliably for NPCs but not for the player as `actor` — observed empirically in playtest
where hits and bleedouts were captured fine but `combat_start` / `combat_end` never appeared. Cleanest fix:
detect player combat-state via polling instead. The same main-thread `Poll()` that drives bleedout recovery
(below) reads `player->IsInCombat()`, compares against a `g_playerInCombatLast` bool, and emits a
`combat_start` or `combat_end` (and updates `g_currentEncounterStartRealTime`) on flip.

500 ms granularity is plenty for narrative pacing — the player won't notice that the Director sees their
combat start half a second late, and the SkyrimNet event stream's own granularity is coarser than that.

### Bleedout recovery poll

There is no SKSE `TESExitBleedoutEvent`, so recovery is detected by polling. The same main-thread
`CombatEventLog::Poll()` call from `src/Tick.cpp` (which runs every ~500 ms) walks `g_bleedingOut` and for
each entry:

- If `TESForm::LookupByID(formID)` returns nullptr or the form is no longer an `Actor`, drop the entry silently
  (the actor was unloaded / despawned — no recovery to report).
- Else if `actor->IsDead()`, drop the entry silently — the death is already captured by SkyrimNet's event log.
- Else if `actor->AsActorState()->IsBleedingOut()` is false:
  - If the actor is now more than ~6000 units from the player, drop the entry silently (recovery happened
    out of witnessing range; not narratively significant).
  - Otherwise drop the entry and emit a `regain_footing` event using the cached display name.
- Else leave the entry in place for the next poll.

500 ms granularity is plenty for a narrative-pacing signal; the recovery doesn't need frame-accurate timing.
The poll runs main-thread, so the engine API calls (`LookupByID`, `IsBleedingOut`) are safe.

---

## Phase-change hook

`PhaseTracker` has no callback mechanism today, and Phase 02's needs don't justify building a general one. The
simplest cut: `PhaseTracker::AdvanceTo` and `PhaseTracker::Reset` call `CombatEventLog::OnPhaseAdvanced()`
directly **after** they release `PhaseTracker::g_mutex` (CombatEventLog takes its own lock; we don't want to
hold two at once).

Use a forward declaration inside `PhaseTracker.cpp` rather than including the header — keeps the new dependency
one-way and out of `PhaseTracker.h`. If a second listener ever needs notification we can graduate to a callback
list, but that's YAGNI for now.

Pruning logic inside `OnPhaseAdvanced`:

```cpp
std::scoped_lock lock(g_mutex);
if (g_currentEncounterStartRealTime > 0.0) {
    // Keep the in-progress encounter; drop everything older.
    const auto cutoff = g_currentEncounterStartRealTime;
    std::erase_if(g_events, [cutoff](const auto& e) { return e.localTime < cutoff; });
} else {
    g_events.clear();
}
```

This satisfies the "current encounter survives a phase change, prior encounters do not" rule from the design
discussion.

---

## Merge + condensation

New helper, alongside the existing event helpers (most natural home is `src/SkyrimNetEvents.cpp` extending the
existing namespace):

```cpp
// Merges SkyrimNet's event tail (already filtered + reversed + FormatEventsText'd) with CombatEventLog's tail,
// sorted ascending by localTime, condensing runs of consecutive `hit` internal events between non-hit events
// into one entry.
nlohmann::json BuildMergedTimeline(
    nlohmann::json skyrimNetEvents,
    nlohmann::json combatEvents,
    double currentGameTimeSeconds);
```

Algorithm:

1. Concatenate both arrays; **stable** sort ascending by `localTime` (stable so equal timestamps preserve input
   order).
2. Walk linearly with a `std::vector<HitRef> pendingHits` buffer.
3. For each event:
   - If it's a `hit` (internal type tag): push into `pendingHits`.
   - Otherwise: if `pendingHits` is non-empty, emit one condensed summary built from the pending buffer (see
     rules below), clear the buffer, then emit the current event as-is.
4. After the loop: if `pendingHits` is non-empty, emit one final condensed entry.

`combat_start`, `combat_end`, `collapse`, and `regain_footing` are all treated as non-hit events — they stay as
their own discrete entries (per the clarified design decision; they're narrative beats, not noise to fold
away). Only `hit` events get condensed.

Condensation rules for a run of hits:

- Group **actor-vs-actor** hits by unordered pair `{A, B}` of `(actorName, targetName)` — these are the only
  ones that can ever be "two-sided." Group **environmental** hits (where `actorIsNamedActor` is false) by
  `(sourceLabel, targetName)` and always treat them as one-sided.
- Single actor-vs-actor hit: `"A strikes B"`.
- Single environmental hit with a resolved source: `"B took damage from <sourceLabel>"`
  (e.g., `"Hans took damage from boulders"`, `"Hans took damage from a trap"`, `"Hans took damage from
  poison"`).
- Single environmental hit with no resolved source: `"B took damage"` (e.g., `"Hans took damage"`).
- One dominant actor-vs-actor pair, one-sided: `"A attacks B"`.
- One dominant actor-vs-actor pair, both sides traded hits: `"A and B trade blows"` (optionally with an
  exchange count if it reads well during testing).
- Multiple actor pairs: `"Combat continues: A attacks B; C attacks D"` — cap at the top 3 pairs and suffix
  `"and others"` if more.
- Repeated environmental damage to one target: `"B took repeated damage from <sourceLabel>"` (or just
  `"B took repeated damage"` if no source).
- Mixed actor + environmental in the same run: emit the actor-vs-actor summary first, then the
  environmental summaries on subsequent lines (or join with `"; "` — pick whichever reads cleaner in test).
- Prefix with `"[N ago]"` derived from the most recent hit's `gameTime` (simpler than computing a median and
  good enough for narrative framing).

The condensed entry's `type` is `"combat_event"` (the same value every individual combat event uses, so the
dashboard and prompt see a single uniform label across the whole combat family). `ne_kind` carries the finer
discriminator (`"combat_summary"`, `"hit"`, `"collapse"`, etc.) for code paths that need it. The LLM ignores
the discriminator anyway — it reads `text`.

---

## Wiring into existing pipelines

Two call sites already build a `recent_events` JSON array from `SkyrimNetAPI::GetRecentEvents` +
`SkyrimNetEvents::FormatEventsText`. Both get extended to also pull `CombatEventLog::GetRenderedTail(...)` and
run both through `BuildMergedTimeline(...)`:

- `src/EvaluationPipeline.cpp` — inside `BuildPromptContext`, right after the existing
  `phaseEnteredAtRealTime`-cutoff filter / reverse / `FormatEventsText` block. The final
  `parsed = std::move(...)` becomes `parsed = BuildMergedTimeline(std::move(parsed),
  CombatEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds), snapshot.player.gameTimeSeconds)`.
- `src/DashboardUIManager.cpp` — inside `ComposeFullStateJSON`, mirror the same change so the dashboard and the
  prompt always see identical merged streams.

PhaseTracker's `localTime >= phaseEnteredAtRealTime` filter continues to apply to the SkyrimNet half. The combat
half is already phase-pruned in memory by `OnPhaseAdvanced`, so no extra filter pass is needed for it.

---

## Persistence

New SKSE co-save record:

- Type ID: `'NECE'` (NarrativeEngine Combat Events). Add to the dispatcher in `src/Plugin.cpp` alongside the
  existing `'PHTR'` and `'DECL'`.
- Version: `1` to start. Bump on any wire-format change, per the convention `PhaseTracker.cpp` uses.
- Payload: `uint32_t count`, then per-event `{ uint8_t kind; double localTime; double gameTime; uint16_t
  actorNameLen; <bytes>; uint16_t targetNameLen; <bytes>; }`.
- `OnSave` runs the same prune step as `OnPhaseAdvanced` before writing — guarantees on-disk content matches the
  in-memory rules and never re-introduces purged encounters across a save/load.
- `OnLoad` deserializes; on unknown version, log and clear. On a `kNewGame` revert, `OnRevert` clears.
- `g_currentEncounterStartRealTime` is recoverable from the loaded events (scan for newest player `combat_start`
  with no subsequent `combat_end`), so it does not need its own persisted field.
- `g_bleedingOut` is also rebuilt rather than persisted: on `SKSE::MessagingInterface::kPostLoadGame`, walk
  `RE::ProcessLists::GetSingleton()->highActorHandles` and seed the set with any loaded actor where
  `actor->AsActorState()->IsBleedingOut()` is true. Worst case: actors that entered bleedout while unloaded
  won't get a `regain_footing` event paired with their `collapse`; acceptable since the `collapse` event
  itself was already pruned by the same OnSave path if it occurred before the current encounter.

---

## Settings (optional)

If they fall out cheaply, add to `Config` in `include/Settings.h`:

- `int combatEventsHitRadiusUnits = 6000;` — distance gate tunable.
- `int combatEventsMaxStored = 256;` — ring-buffer cap.

INI section: `[CombatEvents]` in `statics/SKSE/Plugins/NarrativeEngine.ini`. Nice-to-have for debugging; do not
block the implementation on them.

---

## Verification

End-to-end test in game:

1. Build via `pwsh -File build.ps1 build`. DLL auto-deploys to the mod folder.
2. Launch a save with the player somewhere calm. Open the dashboard (default `F7`); confirm `recent_events`
   matches the existing baseline (no combat entries, no regressions).
3. Trigger combat — attack a guard, or use the console to spawn and aggro a hostile (`player.placeatme
   <bandit>`, set relationship rank to hostile).
4. Confirm in the dashboard:
   - A `combat_start` entry appears (`"Player enters combat"` or similar).
   - As hits land, a single condensed entry appears (not one entry per swing), e.g. `"Player and Whiterun Guard
     trade blows"`. The entry's text changes / updates as more hits arrive between SkyrimNet events.
   - After death or disengage, a `combat_end` entry appears.
5. Trigger a bleedout — beat an essential NPC (a named follower, or a quest-essential like Lydia) down past
   their bleedout threshold without finishing them, or use `setessential <baseId> 1` on a generic enemy first.
   Confirm in the dashboard:
   - A `collapse` entry appears (`"Lydia collapses"`).
   - Wait for the actor to recover (a few seconds of being left alone). A `regain_footing` entry appears
     (`"Lydia regains their footing"`).
   - If you finish them while collapsed instead, no `regain_footing` fires — SkyrimNet's existing `death` event
     is the only thing that lands.
6. Wait for (or force via console / log inspection) a Director phase advance. Confirm via log that
   `CombatEventLog::OnPhaseAdvanced` ran and that older combat entries dropped from the next dashboard refresh.
7. Trigger a second combat encounter *during* a phase, then save and reload while still in combat. Confirm the
   in-progress encounter survives the round-trip and that no events from before that encounter's start were
   re-loaded.
8. Watch the next `BuildPromptContext` log block and confirm `recent_events` now interleaves SkyrimNet and
   combat entries in correct chronological order, with hits condensed.

Smoke-test failure modes to watch for:

- Sink registration silently failing — check for the init log lines on plugin startup; if missing, check init
  ordering in `Plugin.cpp::OnMessage(kDataLoaded)`.
- Threading hang in `BuildPromptContext` from lock contention with a sink-side push — the mutex window in the
  sink should be tiny, but worth monitoring.
- LLM prompt-size bloat from too many combat entries surviving merge — if observed, lower the SkyrimNet
  `eventTail` count or the combat ring-buffer cap.
