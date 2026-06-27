# Phase 03 — Action Toolbox Scaffolding + Ambush

The Director gains its first real lever. This phase stands up the **action toolbox** — the registry,
selection pipeline, lifecycle tracking, and dispatch path that all future actions will use — and
ships **one** fully implemented action (a wilderness/road bandit ambush, executed as a real
Skyrim quest) to exercise the whole loop end-to-end.

---

## Why this phase exists

Phases 01 and 02 gave the Director **observation** — it reads the world, scores tension, decides
phase transitions, and records its reasoning. But every decision so far has been a non-action: the
Director nudges NPC dialogue tone through decorators, nothing more. The narrative curve advances
only when the player happens to generate enough world activity to push it forward.

That breaks the design intent. From DESIGN_GOALS:

> The Director uses heuristics for desired phase duration and is willing to force progression when a
> phase has overstayed its narrative welcome — but it does so by *creating events that fit the next
> phase*, not by flipping an internal flag in isolation.

Phase 03 introduces the mechanism for those created events. After this phase, the Director can:

1. Notice that the current phase has overstayed its ideal duration.
2. Decide on a tension direction (raise or lower) based on where in the Freytag cycle it is.
3. Consult its toolbox for actions that match that direction and that the current world state
   permits — with situational and recency filtering done plugin-side.
4. Ask the LLM to pick one from the filtered candidates.
5. Execute the chosen action — which for non-trivial actions means kicking off a real Skyrim quest
   that owns the spawning, AI, and lifecycle.
6. Track the action as **in-flight** until the quest signals completion, blocking further action
   firings in the meantime.
7. Record the outcome in the decision log alongside the tension score that triggered it.

The one shipped action — **Ambush** — spawns a small group of level-scaled bandits via a dedicated
radiant quest. It is the cleanest "raise tension" proof-of-concept: high-visibility, clearly
Director-issued, testable in five minutes, exercises every layer of the new scaffolding, and uses
the standard Skyrim quest/alias/AI-package pattern for spawning rather than fragile native
`PlaceAtMe` glue.

---

## Scope

### In scope

- An `IAction` interface — name, description, polarity, availability check, **start** (the action
  may run async to completion).
- A static action registry — actions register at startup; selection iterates the registry.
- A **phase ideal duration** setting per phase (real-time seconds).
- An `ActionDispatcher` that:
  - Decides each tick whether to attempt action selection (dwell-time + cooldown + in-flight
    gates).
  - Owns plugin-side candidate filtering (`IsAvailable` + recency window).
  - Runs the action-selection LLM call.
  - Starts the chosen action.
  - Tracks the in-flight action name + start time.
  - Listens for completion ModEvents and clears in-flight state.
  - Handles a stale-lock timeout in case the completion signal never arrives.
- A second LLM prompt — `narrative_engine_action_select` — given the filtered candidate manifest +
  thin context, returns the chosen action name + a free-form parameters JSON object.
- One implemented action: `AmbushAction` — implemented as a thin C++ wrapper around a new
  `_ne_BanditAmbushQuest` (ESP + Papyrus).
- Co-save persistence of in-flight action state.
- Logging: each action firing records into the existing `DecisionRecord` and surfaces in the
  dashboard's `last_evaluation`.
- A global action cooldown that **starts at completion**, not at firing — an action that takes
  four minutes to resolve plus a two-minute cooldown means ~six minutes between firings.

### Deferred (explicitly out)

- Per-action cooldowns or per-action rate limits.
- Suppression-window plumbing (no sims to suppress yet).
- Parameter schemas as a formal type system — actions validate their own JSON.
- Dashboard introspection into the full available-action set, or "why action X was *not* picked."
- Any second action. The toolbox manifest will, post-Phase-03, contain exactly one entry; the
  selection prompt + dispatch path are nonetheless built for N.
- Quest-side polling diagnostics (alias-stuck detection, etc.) beyond the stale-lock timeout.

---

## Core concepts

### `IAction` interface

```cpp
// include/IAction.h
namespace NarrativeEngine
{
    enum class ActionPolarity : std::uint8_t { Raise, Lower, Either };

    struct ActionContext
    {
        RE::Actor*  player           = nullptr;
        bool        playerInCombat   = false;
        bool        playerInDialogue = false;
        bool        playerInInterior = false;
        std::string locationName;   // current location, may be empty
        std::string cellName;       // current cell, may be empty
    };

    struct StartResult
    {
        // True when the action's start signal has been dispatched
        // successfully (e.g. the quest start ModEvent was sent and the
        // quest was confirmed running). It does NOT mean the action has
        // completed — completion arrives asynchronously via the
        // _ne_ActionCompleted ModEvent. False means the action could
        // not even begin.
        bool        started = false;
        std::string detail;   // one-line outcome for the log
    };

    class IAction
    {
    public:
        virtual ~IAction() = default;

        // Stable snake_case identifier. Used as the value of
        // DecisionRecord::actionSelected and as the discriminator the LLM
        // returns in the selection response. Also the value the
        // _ne_ActionCompleted ModEvent carries when this action resolves.
        virtual std::string Name() const = 0;

        // One-paragraph description for the LLM.
        virtual std::string Description() const = 0;

        virtual ActionPolarity Polarity() const = 0;

        // Cheap synchronous check: does current world state permit this
        // action to fire right now? Main thread.
        virtual bool IsAvailable(const ActionContext& ctx) const = 0;

        // Start the action. Main thread. The action owns parameter
        // validation — unknown / missing fields should fall back to
        // defaults. The action does NOT block until completion; it kicks
        // off whatever long-running process it owns (quest start, ModEvent
        // send, etc.) and returns. The dispatcher tracks the in-flight
        // state until the action sends back _ne_ActionCompleted carrying
        // this action's Name().
        virtual StartResult Start(const ActionContext& ctx,
                                  const nlohmann::json& parameters) = 0;
    };
}
```

Three things to note about the lifecycle shift from "Execute" to "Start":

1. The dispatcher is responsible for in-flight tracking — actions don't self-manage their
   "running" state. Actions just start things and report success-of-start.
2. Completion arrives through a single shared ModEvent (`_ne_ActionCompleted`) that all actions
   send when they finish. The dispatcher's listener matches the event's action-name field against
   the current in-flight name.
3. Trivial actions (e.g. a future suppression-window toggle) that genuinely complete
   synchronously should call the completion path themselves at the end of `Start` rather than
   getting special-cased. From the dispatcher's perspective every action is async.

### Action registry

```cpp
// include/ActionRegistry.h
namespace NarrativeEngine::ActionRegistry
{
    void Register(std::unique_ptr<IAction> action);
    const std::vector<std::unique_ptr<IAction>>& All();
    IAction* Find(std::string_view name);
    std::vector<IAction*> AvailableMatching(const ActionContext& ctx,
                                            ActionPolarity        desired);
}
```

### Phase ideal duration

A new per-phase setting: how long each phase *should* last in unpaused real-time seconds before the
Director starts looking for ways to advance. Defaults match the design narrative — Exposition and
Resolution sit longer; Climax is brief.

```ini
[Director]
iIdealDurationExposition    = 330   ; 5.5 min
iIdealDurationRisingAction  = 225   ; 3.75 min
iIdealDurationClimax        = 90    ; 1.5 min
iIdealDurationFallingAction = 225   ; 3.75 min
iIdealDurationResolution    = 330   ; 5.5 min
```

Total ideal cycle: 1200 s (20 min). Proportions follow the design narrative — Exposition and
Resolution sit longer; Climax is brief.

A new helper on PhaseTracker derives the **desired tension direction** for the current phase:

```cpp
namespace NarrativeEngine::PhaseTracker
{
    enum class Direction : std::uint8_t { Raise, Lower };
    Direction OutgoingDirection(Phase p);   // E/R/Res → Raise; C/F → Lower
}
```

### Action dispatcher

A new module sequenced after `ApplyDecision`. Pseudocode for the per-tick check:

```cpp
void ActionDispatcher::ConsiderAction(const Snapshot& snapshot,
                                      const DecisionLog::DecisionRecord& provisional)
{
    // Action already in flight from a previous tick — wait for it.
    if (!g_actionInFlight.empty()) return;

    // Tension eval already advanced the phase this tick.
    if (provisional.advancedToPhase) return;

    // Global cooldown — measured from previous action's COMPLETION, not start.
    if (RealTimeNow() - g_lastActionCompletedAt < cooldownSeconds) return;

    // Phase-dwell-time gate.
    if (snapshot.timeInPhaseSeconds < IdealDurationFor(snapshot.currentPhase)) return;

    // Direction + tension delta for the LLM.
    const auto direction = PhaseTracker::OutgoingDirection(
                               PhaseTracker::PhaseFromName(snapshot.currentPhase).value());
    const ActionPolarity desired =
        (direction == Direction::Raise) ? ActionPolarity::Raise : ActionPolarity::Lower;
    const int tensionDelta = ComputeTensionDelta(snapshot, provisional, direction);

    // Plugin-side candidate filtering:
    //   1. IAction::IsAvailable(ctx)  — situational fit (city / combat / DND cell / etc.)
    //   2. Recency / anti-repetition — drop any action fired within the last
    //      iActionRepetitionWindowSeconds, measured from its completion.
    ActionContext ctx = ContextFromSnapshot(snapshot);
    auto candidates = ActionRegistry::AvailableMatching(ctx, desired);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](IAction* a) { return WasFiredRecently(a->Name()); }),
                     candidates.end());
    if (candidates.empty()) return;

    // Async LLM selection call. Callback marshals back to main thread for Start.
    SkyrimNetAPI::SendCustomPromptToLLM(
        "narrative_engine_action_select",
        "narrative_engine_director",
        BuildActionPromptContext(snapshot, candidates, direction, tensionDelta),
        [snapshot, candidates](std::string response, bool success) { /* ... */ });
}
```

**State the dispatcher owns:**

- `g_actionInFlight` (string) — name of the currently-running action, or empty. Persisted.
- `g_actionStartedAt` (real-time seconds, Unix epoch) — start time for stale-lock detection.
  Persisted.
- `g_lastActionCompletedAt` (real-time seconds, Unix epoch) — drives the global cooldown.
  Persisted.
- `g_recentlyFiredActions` — small ring buffer of `{name, completedAt}` entries for the
  recency filter. Per-process; not persisted.

**Completion handling.** The dispatcher registers a SKSE ModEvent sink on `_ne_ActionCompleted`.
On receipt:

1. Validate that the event's action-name field matches `g_actionInFlight`. Mismatch → log
   warning, ignore.
2. Push `{actionInFlight, now}` into `g_recentlyFiredActions`.
3. Set `g_lastActionCompletedAt = now`. Clear `g_actionInFlight`.
4. Push a fresh dashboard state so the UI reflects the resolution.

**Stale-lock timeout.** A small per-tick check: if `g_actionInFlight` non-empty and
`now - g_actionStartedAt > iActionStaleLockTimeoutSeconds` (default 900 s = 15 min real-time),
auto-clear. Log a warning naming the action. Future work: also force-stop the quest. For
Phase 03 a logged warning is sufficient — the player will notice their abandoned bandits and we
can investigate from the log.

`ComputeTensionDelta`: looks up `cfg.advanceThreshold<CurrentPhase>` and subtracts in the
appropriate direction. Always positive (the dispatch gate guarantees we didn't just advance, so
tension is on the "wrong side" of the threshold for the desired direction).

### The action-select LLM prompt

`statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_action_select.prompt`. The prompt stays
deliberately small — the plugin has already done the heavy filtering (situational, recency). The
LLM gets the *what* and a thin context block.

Receives:

- `desired_direction` — `"raise"` or `"lower"`.
- `tension_delta` — positive integer 0..100. "Nudge the current tension by approximately this
  much in `desired_direction`."
- `candidates` — array of `{ name, description }`. Already filtered; every entry is a valid pick.
- A thin flavor block — player location name, cell name, interior/exterior, and a short tail of
  recent events. Enough texture for the LLM to pick *which* of the candidates fits the moment, but
  no decision-log history, no time-in-phase numbers, no phase enumeration.

Output: a JSON object with three keys.

- `action` — string, must match one of the `candidates[].name` values. Validated; mismatch → no
  action fires, logged, cooldown applies anyway.
- `parameters` — object. Free-form; the chosen action validates its own shape. May be `{}`.
- `narrative_note` — one-sentence rationale. Stored in the `narrativeNote` field on the
  DecisionRecord (overrides the eval-prompt note for ticks where an action fires).

This prompt follows `docs/CUSTOM_PROMPTS.md`.

### Decision-record integration

`DecisionRecord` already carries `actionSelected` + `actionParametersJSON` from Phase 01. The
change is in build/append timing:

- Provisional record is built from the tension call as today, but NOT appended immediately if
  action selection is going to fire.
- Action-select callback populates `actionSelected = action->Name()`,
  `actionParametersJSON = parameters.dump()`, and replaces `narrativeNote` with the action prompt's
  note.
- Append happens once, on the main thread, after `Start` returns. On `Start` failure:
  `actionSelected = "(failed: <detail>)"` and the append still happens so the dashboard surfaces
  the failure.

The record records the **start** of the action, not the resolution. Completion is a state
transition (clears in-flight, starts cooldown) but does not produce its own DecisionRecord.

One record per tick remains the invariant.

---

## ESP content

The NarrativeEngine `.esp` (empty since MVP) gains its first real content.

### `_ne_BanditAmbushQuest` (Quest record)

- **Run Once:** OFF
- **Allow Repeated Stages:** ON
- **Type:** None (radiant misc; not part of the journal)
- **Stages:**
  - `0` — Init (set by `Start()`; fragment kicks off the spawn)
  - `10` — Encounter active (set after spawn completes)
  - `100` — Resolved (set when the last alias dies; fragment sends `_ne_ActionCompleted` and
    `Stop()`s the quest)
- **Aliases (Reference Aliases):** four slots
  - `_ne_BanditAlias00` through `_ne_BanditAlias03`
  - Fill Type: **Forced** (filled at runtime via `Alias.ForceRefTo` from script)
  - Flags: **Allow Reserved**, **Stores Text** OFF, **Optional** ON (so missing slots don't
    block quest start when `bandit_count < 4`)
  - Each alias carries the `_ne_BanditAttackPlayer` package on its **Packages** list
  - Each alias adds the actor to **BanditFaction** (vanilla, formID `0x000F26196` from
    `Skyrim.esm`) via its **Faction Owner** / package conditions — exact mechanism TBD during
    CK authoring; the goal is "spawned actor is recognized as a bandit by vanilla detection /
    crime systems."

### `_ne_BanditAttackPlayer` (Package record)

- **Template:** `DefaultCombat` (vanilla template — handles target acquisition + aggression).
- **Target:** Specific Reference → `PlayerRef`.
- **Conditions:** none beyond the alias-bound default.

If the vanilla `DefaultCombat` template proves too weak (bandits not aggroing reliably), fall
back to a hand-built package: Find → Combat target = PlayerRef, with detection radius generous.

### `_ne_BanditAmbushQuest` (Papyrus script attached to the quest)

```papyrus
ScriptName _ne_BanditAmbushQuest extends Quest

LeveledActor Property BanditLevList Auto   ; LCharBandit from Skyrim.esm
ReferenceAlias[] Property BanditAliases Auto   ; the four slots

int Property CurrentBanditCount Auto Hidden
int Property AliveCount Auto Hidden

Event OnInit()
    RegisterForModEvent("_ne_StartAmbushAction", "OnStartAmbushAction")
EndEvent

Event OnStartAmbushAction(string eventName, string strArg, float numArg, Form sender)
    ; strArg encodes "count|distance" e.g. "3|2000"
    ; numArg unused for now (reserved for future flags)
    ; Parse params, validate, kick off the spawn.
    ; Sets stage to 10 once aliases are filled.
EndEvent

Event OnDeath_Alias(Actor akActor)
    ; Fired by per-alias OnDeath registered after fill.
    AliveCount -= 1
    if AliveCount <= 0
        SetStage(100)
    endif
EndEvent

; Stage 100 fragment (in CK):
;   SendModEvent("_ne_ActionCompleted", "ambush", 0.0)
;   Stop()
```

The spawn implementation lives in the `OnStartAmbushAction` handler:

1. Compute spawn anchor: player position + random offset on XY plane in `[120°, 240°]` relative
   to player heading, scaled by `spawn_distance_units`.
2. Place each bandit at the anchor (with small per-bandit jitter) via
   `Game.GetPlayer().PlaceAtMe(BanditLevList)` — leveled-list resolution gives tier-scaled
   bandits automatically.
3. For each new actor: `BanditAliases[i].ForceRefTo(newActor)`. Register the alias for the
   actor's OnDeath event.
4. `SetStage(10)`.

Spawn-position fallback: if any of `PlaceAtMe` returns None (rare), shrink to the bandits that
did spawn; if zero spawned, set stage to 100 immediately with `bad_spawn` detail and let the
completion path tear down — the action "completes" as a no-op and cooldown applies.

### Other forms

- **ModEvent names** are not CK forms — they're just registered string names. Two new ones:
  - `_ne_StartAmbushAction` — C++ sender → Papyrus listener
  - `_ne_ActionCompleted` — Papyrus sender → C++ listener
- **No new factions** — uses vanilla `BanditFaction`.
- **No new leveled lists** — uses vanilla `LCharBandit` from `Skyrim.esm`.
- **No new keywords** — city/town filtering uses vanilla `LocTypeCity`, `LocTypeTown`, etc.

---

## The one action: `AmbushAction`

The C++ side is intentionally thin — almost all the work is in the quest.

### Preconditions (`IsAvailable`)

Returns `true` only when **all** of:

- Player is in an **exterior** cell.
- Player is **not** in active combat.
- Player is **not** in dialogue / a scripted scene.
- Player's current location is **not** flagged as a city / town / inn / settlement
  (vanilla location keywords).
- Player's current cell EditorID is not in `sDoNotDisturbCellEDIDsCSV`.

### Parameters

- `bandit_count` — integer 2..4. Defaults to `iAmbushDefaultBanditCount` (3).
- `spawn_distance_units` — integer 1500..3000. Defaults to `iAmbushDefaultSpawnDistanceUnits`
  (2000).

### `Start`

1. Re-validate availability (defends against state changes between selection and dispatch).
2. Clamp parameters to settings-defined ranges.
3. Look up `_ne_BanditAmbushQuest` from `NarrativeEngine.esp`. If missing, fail.
4. If the quest is already running (someone else fired it? leftover state?), fail with
   `quest_already_running` detail.
5. Send the `_ne_StartAmbushAction` ModEvent with `strArg = "<count>|<distance>"`.
6. Return `StartResult{ started = true, detail = "ambush started: <count> bandits at ~<dist>u" }`.

The quest's Papyrus side takes over from here. Completion arrives via `_ne_ActionCompleted` on
its own timeline.

### What's not in this action (yet)

- No "Director-driven encounter" keyword on the spawned actors. Once a sim cares, we add one.
- No despawn cleanup beyond vanilla cell reset. Aliases release their refs on `Stop()`; the
  actor refs themselves remain in the world.
- No dialogue. Bandits use vanilla bandit voice lines.
- No mid-fight Director intervention. Once started, the action runs to completion or stale-lock
  timeout.

---

## Settings

New keys in `[Director]`:

| Key                              | Default | Meaning                                                            |
| -------------------------------- | ------: | ------------------------------------------------------------------ |
| `iIdealDurationExposition`       | 330     | seconds; phase-dwell threshold past which actions may fire         |
| `iIdealDurationRisingAction`     | 225     | "                                                                  |
| `iIdealDurationClimax`           | 90      | "                                                                  |
| `iIdealDurationFallingAction`    | 225     | "                                                                  |
| `iIdealDurationResolution`       | 330     | "                                                                  |
| `iActionCooldownSeconds`         | 120     | wall-clock seconds after action *completion* before next can fire  |
| `iActionRepetitionWindowSeconds` | 300     | window during which the same action name is excluded from picks    |
| `iActionStaleLockTimeoutSeconds` | 900     | auto-clear an in-flight action that never sends completion         |

New `[Actions]` section:

| Key                                | Default | Meaning                                                          |
| ---------------------------------- | ------: | ---------------------------------------------------------------- |
| `iAmbushDefaultBanditCount`        | 3       | used when the LLM omits or supplies an out-of-range count        |
| `iAmbushDefaultSpawnDistanceUnits` | 2000    | "                                                                |
| `iAmbushMinBanditCount`            | 2       | clamp for LLM-supplied count                                     |
| `iAmbushMaxBanditCount`            | 4       | "                                                                |
| `iAmbushMinSpawnDistanceUnits`     | 1500    | clamp for LLM-supplied distance                                  |
| `iAmbushMaxSpawnDistanceUnits`     | 3000    | "                                                                |

---

## Persistence

New co-save record `'NEAC'` (NarrativeEngine Action Coordinator), versioned at 1. Payload:

- `string` action-name (the current in-flight action, or empty)
- `double` actionStartedAt (Unix-epoch real seconds; 0 when not in flight)
- `double` lastActionCompletedAt (Unix-epoch real seconds)

`g_recentlyFiredActions` is per-process — recency state doesn't need to survive a save/load
boundary; on reload the worst case is one anti-repetition allowance is forgotten, which is fine.

On `kPostLoadGame`:

- Read the record. Restore the three fields.
- If `action-name` is non-empty: check whether `_ne_BanditAmbushQuest.IsRunning()` matches.
  Mismatch (e.g. the save was taken between ModEvent send and quest start, or the quest was
  ended externally) → log warning, clear in-flight.
- If `actionStartedAt` is older than `iActionStaleLockTimeoutSeconds` worth of real-wall-clock,
  the stale-lock auto-clear fires on the next tick. No special-casing on load.

---

## Pipeline integration

Order of operations on a tick where an action fires:

1. Tick fires. `BuildSnapshot`. (unchanged)
2. Build prompt context. (unchanged)
3. Tension LLM call. (unchanged)
4. `ParseDecision`. (unchanged — produces provisional record)
5. `ActionDispatcher::ConsiderAction`. *(new)* Decides whether to attempt action selection.
6. If yes: build the action-select prompt, send to LLM.
7. Action LLM call returns. Parse → validate → main-thread marshal.
8. `IAction::Start` on the main thread.
9. Append `DecisionRecord` with combined fields.
10. `DashboardUIManager::PushFullState`.
11. Release `g_inFlight`.

Concurrency: the existing `g_inFlight` flag stays held across both LLM round-trips on
action-firing ticks. Two ticks cannot overlap action selection or starting. The post-Start
completion ModEvent fires on its own; it doesn't need `g_inFlight` because it just mutates the
small dispatcher state and pushes a dashboard refresh.

Latency note: on action-firing ticks the user-visible pause between "Director ticked" and
"dashboard updates" lengthens by one LLM round-trip. Acceptable — those ticks are bounded by the
phase ideal-duration check.

---

## Dashboard

Small additions to the React side:

- `LastEvaluation` panel: if the latest decision has a non-empty `actionSelected`, render an extra
  line: `"→ fired: <action_name>"`. Failed actions render as `"→ action failed: <detail>"`.
- (Optional) A small persistent indicator in the phase panel when an action is in flight:
  `"action in flight: ambush (started 1m ago)"`. Useful for debugging stale-lock issues.

Schema change on the `DirectorState` TS type: add an optional `action_in_flight: { name: string,
started_at: number } | null` field. C++ side populates it from dispatcher state on every
`PushFullState`.

---

## File map

New C++:

- `include/IAction.h`
- `include/ActionRegistry.h`           / `src/ActionRegistry.cpp`
- `include/ActionDispatcher.h`         / `src/ActionDispatcher.cpp`
- `include/AmbushAction.h`             / `src/AmbushAction.cpp`

New prompt:

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_action_select.prompt`

New ESP content (authored in CK against `NarrativeEngine.esp`):

- `_ne_BanditAmbushQuest` quest record
- `_ne_BanditAlias00..03` reference aliases (on the quest)
- `_ne_BanditAttackPlayer` AI package

New Papyrus:

- `esp/Source/Scripts/_ne_BanditAmbushQuest.psc` — authored in VS Code (with the Papyrus
  extension) against the `.ppj` project file at the repo root. Compiled `.pex` deploys directly
  to `<mod-folder>/Scripts/` via the CMake Papyrus step.

New ESP / Papyrus workflow scaffolding (Step 1):

- `setup-mod-folder.ps1` (repo root) — one-time per-machine setup; creates the mod folder and
  the `Source/Scripts/` junction.
- `sync-esp.ps1` (repo root) — bidirectional ESP sync (newest copy wins); invoked by CMake on every build.
- `NarrativeEngine.ppj` (repo root) — Papyrus project file; consumed by CK, the VS Code Papyrus
  extension, and the CMake Papyrus step.
- `esp/` — authoritative repo-side ESP location (mirrored from the mod folder).
- `esp/Source/Scripts/` — authoritative Papyrus source location (junctioned into the mod folder).

New ModEvent names (no CK forms — registered by name):

- `_ne_StartAmbushAction`
- `_ne_ActionCompleted`

Modified:

- `CMakeLists.txt` — add a `sync_esp` pre-build custom target that invokes `sync-esp.ps1`
  (bidirectional, newest copy wins), plus the Papyrus compile step.
- `CMakePresets.json` — add `PAPYRUS_COMPILER` cache var.
- `CLAUDE.md` — document the ESP / Papyrus workflow.
- `.gitignore` — add `*.pex` and CK temp/backup patterns.
- `include/Settings.h`, `src/Settings.cpp` — ideal durations, action cooldown, repetition
  window, stale-lock timeout, ambush clamps.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — document the new keys.
- `include/PhaseTracker.h`, `src/PhaseTracker.cpp` — add `Direction OutgoingDirection(Phase)`.
- `src/Plugin.cpp` — register `AmbushAction` at `kDataLoaded`; wire dispatcher's co-save
  callbacks; wire dispatcher's `_ne_ActionCompleted` ModEvent sink.
- `src/EvaluationPipeline.cpp` — invoke `ActionDispatcher::ConsiderAction` between
  `ParseDecision` and `DecisionLog::Append`; defer the append on action-firing ticks.
- `dashboard/src/types.ts`, `dashboard/src/components/*` — `action_in_flight` field, render
  fired-action line, optional in-flight indicator.

---

## Implementation plan

Sequential. Each step is **entirely Claude's work (C++ / TypeScript / prompts)** or **entirely the
user's work (Creation Kit + Papyrus)** — with the sole exception of Step 1, which is the bootstrap
of the ESP/Papyrus workflow and necessarily crosses both parties. Every step has a clear
self-contained verification.

---

### Step 1 — Workflow scaffolding for ESP and Papyrus authoring

- [x] Complete

**[CLAUDE]**

**Goal:** Stand up everything the user needs to author the ESP and Papyrus from Step 2 onward —
repo path conventions, build wiring, a one-time setup script that creates the directory junction,
and the timestamp-aware ESP sync (a standalone script the CMake build invokes automatically).
Pure infrastructure; nothing CK-side yet.

**Files:**

- `setup-mod-folder.ps1` (repo root) — one-time setup the user runs once per machine. Creates
  `$SKYRIM_MODS_FOLDER/NarrativeEngine/` if missing. Creates the NTFS junction
  `$SKYRIM_MODS_FOLDER/NarrativeEngine/Source/Scripts/` → `<repo>/esp/Source/Scripts/` via
  `New-Item -ItemType Junction` (no admin required). Verifies required env vars
  (`SKYRIM_MODS_FOLDER`, `PAPYRUS_COMPILER`) and `vswhere`-style sanity-checks
  `PapyrusCompiler.exe` exists. Idempotent — safe to re-run; skips steps already done. Does NOT
  create the ESP — that's the user's job in Step 2.
- `sync-esp.ps1` (repo root) — bidirectional ESP sync. Compares the mtime of
  `<mod-folder>/NarrativeEngine.esp` and `<repo>/esp/NarrativeEngine.esp` and copies whichever
  is newer over the older. Logic:
  - Both exist, mod newer → mod → repo (CK edit propagated into the repo).
  - Both exist, repo newer → repo → mod (pulled change propagated out for Skyrim to see).
  - Both exist, equal mtime → silent no-op.
  - Only mod exists → mod → repo (first-time sync into repo).
  - Only repo exists → repo → mod (first-time deploy to mod folder).
  - Neither exists → silent no-op (pre-Step-2 state).
  - Accept `-DryRun` for diagnostics.
- `CMakeLists.txt` — two additions:
  1. A custom target that invokes `pwsh -File <repo>/sync-esp.ps1` as a pre-build step. Wire
     via `add_custom_target(sync_esp ALL COMMAND ...)` plus `add_dependencies(NarrativeEngine
     sync_esp)`. Runs on every build, regardless of whether the build was triggered through
     `build.ps1`, a direct cmake invocation, or an IDE. Gated behind a `NE_SKIP_ESP_SYNC`
     CMake option for the rare override case. (`add_custom_command(... PRE_BUILD ...)` is
     **not** suitable — it only fires for the Visual Studio generator, not Ninja, and our
     setup is Ninja.)
  2. A Papyrus compile step driven by the project's `.ppj` file: invoke `$PAPYRUS_COMPILER
     -ppj <repo>/NarrativeEngine.ppj`, output going directly into `<mod-folder>/Scripts/`.
     `CONFIGURE_DEPENDS` on `<repo>/esp/Source/Scripts/*.psc`.
  All ESP movement happens through `sync-esp.ps1`; there is no separate ESP deploy step.
- `CMakePresets.json` — add `PAPYRUS_COMPILER` cache var documenting the expected path to the
  user's `PapyrusCompiler.exe` (typically `<CK_DIR>/Papyrus Compiler/PapyrusCompiler.exe`).
- `NarrativeEngine.ppj` (repo root) — Papyrus project file. Declares source folder
  (`<repo>/esp/Source/Scripts/`), output folder (`<mod-folder>/Scripts/`), and import paths
  (vanilla Skyrim Papyrus source, SKSE Papyrus source, SkyrimNet Papyrus source if any).
  Single source of truth that CK, the VS Code Papyrus extension, and `build.ps1` all consume.
  Lives at the repo root (not under `esp/`) because the VS Code Papyrus extension and most
  Papyrus tooling expect to discover `.ppj` files near the workspace root.
- `CLAUDE.md` — new section "ESP and Papyrus workflow" documenting: repo path conventions,
  junction setup, the `sync-esp.ps1` direction-and-semantics, the CMake pre-build hook that
  invokes it, `.ppj` purpose, recommended VS Code Papyrus extension config, and the rationale
  for junction-over-symlink (MO2 VFS robustness).
- `.gitignore` — add `*.pex`, CK temp/backup patterns (`*.esp.bak`, `*.esm.bak`), Papyrus
  compile log patterns.
- New empty directories `esp/` and `esp/Source/Scripts/` with `.gitkeep` placeholders so they
  exist on a fresh clone before the user has run CK. All ESP-related content (the `.esp` itself
  plus all Papyrus source) lives under `esp/`; only the `.ppj` project file sits at the repo
  root, for VS Code Papyrus extension discovery.

**Specifics:**

- The junction approach is what makes the `Source/Scripts/` story work: junctions are NTFS-native
  reparse points that all userspace software (including MO2's USVFS) handles transparently. No
  Developer Mode required, no admin elevation, no file-symlink robustness questions.
- The bidirectional timestamp comparison handles both common cases automatically: after a CK
  session, mod folder is newer → mod → repo. After a `git pull` that brings in an ESP change,
  repo is newer → repo → mod folder so Skyrim sees the update on next launch.
- `setup-mod-folder.ps1` must not assume `<mod-folder>/Source/Scripts/` already exists when
  creating the junction. `New-Item -ItemType Junction` requires the target to not exist; the
  script should `Test-Path` first and either delete (if empty) or refuse with an error (if
  populated).

**Verify:** `pwsh -File build.ps1 build` succeeds. The CMake `sync_esp` target runs and
`sync-esp.ps1` logs nothing (no ESP exists yet); the Papyrus compile step is a no-op (no `.psc`
files); DLL builds and deploys as today. `pwsh -File sync-esp.ps1 -DryRun` (run standalone)
reports "no action — neither file exists." `pwsh -File setup-mod-folder.ps1` runs without
error against an existing setup; running it twice in a row produces a clean no-op on the second
run.

---

### Step 2 — Mod folder setup, ESP creation, and ESL flagging

- [x] Complete

**[USER]**

**Goal:** Bring the mod folder into existence, create the empty `NarrativeEngine.esp`, flag it
as an ESL while the form table is still empty, and confirm the round-trip works (CK edit →
build sync → repo).

**Sub-tasks:**

1. **Run the setup script** to create the mod folder and the `Source/Scripts/` junction:

   ```pwsh
   pwsh -File setup-mod-folder.ps1
   ```

   Confirm it reports success creating `<mod-folder>/Source/Scripts/` as a junction. Verify via
   `(Get-Item <mod-folder>/Source/Scripts/).Attributes` — should include `ReparsePoint`.

2. **Configure Creation Kit defaults from within the CK Preferences UI** so that any new form
   or auto-generated artifact picks up our `_ne_` prefix and lands in our mod's directories —
   never in a path that could collide with vanilla or another mod's content. ESL-flagging
   means we share the flat editor-ID namespace with everything else loaded, and a collision
   there is much harder to diagnose than a missing form.

   Launch CK once (via MO2) and walk its preferences dialogs to set:

   - **Default editor-ID prefix → `_ne_`.** Whatever CK exposes for this (typically under
     File → Preferences or a Misc tab) — the goal is that the new-form dialog pre-populates
     with `_ne_` so any form created through normal flow inherits the prefix without us
     having to remember.
   - **Auto-load plugin.** If CK exposes a "startup plugin" / "active file on launch" setting
     in its preferences, point it at `NarrativeEngine.esp` so we never accidentally open a
     CK session against the wrong file.
   - Anything else you encounter in the preferences UI that affects new-form naming or
     defaults — bias toward "our mod, our prefix."

   (We don't need to touch Papyrus output paths in CK — MO2's VFS configuration already
   routes everything CK writes into our mod folder.)

   These specifics depend on which CK build you have (Bethesda has reshuffled the menus
   across SE versions). The discipline is the same regardless: walk the preferences once,
   set anything that controls defaults so the mod's prefix and paths are the path of least
   resistance.

3. **Open Creation Kit through MO2** (so the virtual `Data` folder maps to the mod folder).
   - File → Data: check `Skyrim.esm` and `Update.esm` only. OK. (Wait for the load — CK is slow.)
     `NarrativeEngine.esp` is not yet in the list since it doesn't exist; we create it next.
   - File → Save As: name `NarrativeEngine.esp`. CK writes it into the virtualized Data folder,
     which MO2 resolves to `<mod-folder>/NarrativeEngine.esp`. (Future CK sessions will
     auto-load this plugin via `sStartupPlugin`.)
   - Close CK with no forms added.

4. **ESL-flag the file** — do this now, while the form table is empty, before any content
   has assigned form IDs that might fall outside the ESL range.
   - Open the ESP in xEdit (launched via MO2).
   - In the left tree, expand `NarrativeEngine.esp` → select the `File Header` record.
   - In the right pane, locate `Record Header → Record Flags`.
   - Check the `ESL` flag (sometimes labeled `Light Master`).
   - Save (Ctrl+S) and exit xEdit.

5. **Run the build** to trigger the first-time ESP sync into the repo:

   ```pwsh
   pwsh -File build.ps1 build
   ```

   CMake's `sync_esp` pre-build target invokes `sync-esp.ps1`; the log shows `ESP: first-time
   sync from mod folder` and `<repo>/esp/NarrativeEngine.esp` now exists.

6. **Enable `NarrativeEngine.esp` in MO2's load order** — left pane: enable the mod; right
   pane: confirm the ESP shows up checked.

7. **Boot Skyrim**, load a save. SKSE log shows the plugin DLL loading cleanly with the new
   ESP present; no missing-form warnings.

**Verify:** `<repo>/esp/NarrativeEngine.esp` exists, non-empty; `git status` shows it as
untracked, ready to commit. Opening it in xEdit confirms the ESL flag is set in the File Header.
Skyrim loads with `NarrativeEngine.esp` active, no errors in the SKSE log or in MO2's load-order
warnings panel.

---

### Step 3 — Settings expansion and PhaseTracker direction helper

- [x] Complete

**[CLAUDE]**

**Goal:** Add every new setting Phase 03 needs (phase ideal durations, action cooldown, repetition
window, stale-lock timeout, ambush parameter clamps) and the small `PhaseTracker::OutgoingDirection`
helper that downstream code will read.

**Files:**

- `include/Settings.h` — add 14 new fields to `Config` per the **Settings** section above.
- `src/Settings.cpp` — INI reads for each new key in `[Director]` (durations + cooldowns +
  timeout) and the new `[Actions]` section (ambush clamps).
- `statics/SKSE/Plugins/NarrativeEngine.ini` — document each new key inline with one-line
  explanations and default values.
- `include/PhaseTracker.h` — add `enum class Direction { Raise, Lower }` and
  `Direction OutgoingDirection(Phase p)`.
- `src/PhaseTracker.cpp` — implement `OutgoingDirection` (Exposition / RisingAction / Resolution
  → `Raise`; Climax / FallingAction → `Lower`).

**Verify:** `pwsh -File build.ps1 build` succeeds. Boot Skyrim; the existing `Settings: loaded
from …` log line is still present. Optionally bump `iIdealDurationExposition=60` in the deployed
INI and reload — log reflects the new value via debug mode.

---

### Step 4 — IAction interface and ActionRegistry

- [x] Complete

**[CLAUDE]**

**Goal:** Define the action interface, supporting types, and a registry. No actions register yet;
this is pure scaffolding so later steps have something to plug into.

**Files:**

- `include/IAction.h` — `enum class ActionPolarity`, `struct ActionContext`, `struct StartResult`,
  `class IAction` interface, all per the **`IAction` interface** section above.
- `include/ActionRegistry.h` — namespace API: `Register`, `All`, `Find`, `AvailableMatching`.
- `src/ActionRegistry.cpp` — implementation. Storage is a `std::vector<std::unique_ptr<IAction>>`
  guarded by a mutex (registration happens once at startup; iteration happens on the main thread,
  but the mutex is cheap insurance).

**Verify:** Build succeeds. No runtime test possible — nothing instantiates an action yet.

---

### Step 5 — ActionDispatcher infrastructure (state, gates, completion sink, persistence)

- [x] Complete

**[CLAUDE]**

**Goal:** Stand up the dispatcher with its state, gates, completion-ModEvent sink, co-save
persistence, and stale-lock check. Hook it into the evaluation pipeline. No LLM call yet — when
all gates pass, just log a `would consider action` line.

**Files:**

- `include/ActionDispatcher.h` — namespace API: `Initialize` (registers ModEvent sink),
  `ConsiderAction(snapshot, provisional)`, `OnSave`/`OnLoad`/`OnRevert`, `IsActionInFlight`,
  `GetInFlightInfo`, `OnTick` (drives the stale-lock check).
- `src/ActionDispatcher.cpp` — implementation. Mutex-protected state: `g_actionInFlight`
  (string), `g_actionStartedAt` (double Unix epoch), `g_lastActionCompletedAt` (double),
  `g_recentlyFiredActions` (small ring buffer of `{name, completedAt}`).
- `src/Plugin.cpp` — call `ActionDispatcher::Initialize()` at `kDataLoaded`; wire its
  `OnSave`/`OnLoad`/`OnRevert` into the existing serialization callbacks.
- `src/Tick.cpp` — call `ActionDispatcher::OnTick()` from the existing tick so the stale-lock
  check runs without an extra timer.
- `src/EvaluationPipeline.cpp` — call `ActionDispatcher::ConsiderAction(snapshot, provisional)`
  between `ParseDecision` and `DecisionLog::Append`. For now the call always returns "no action,"
  so the existing flow is unaffected.

**Specifics:**

- Co-save record type ID: `'NEAC'`, version 1. Payload: length-prefixed string (action name;
  empty when not in flight), double (started-at), double (last-completed-at).
- Completion ModEvent sink: register `_ne_ActionCompleted` via SKSE's ModEvent registration.
  The sink fires off the main thread; marshal back via `AsyncDispatch::MarshalToMainThread`
  before mutating dispatcher state. Validate that the event's action-name field matches
  `g_actionInFlight`; on mismatch, log warning and ignore.
- Stale-lock: in `OnTick`, if `g_actionInFlight` non-empty and
  `now - g_actionStartedAt > Settings::Get().actionStaleLockTimeoutSeconds`, log warning and
  clear in-flight (future-phase work will also force-stop the quest from C++).
- Log every gate decision in debug mode: `ActionDispatcher: gate <name> blocked: <reason>` and
  `ActionDispatcher: all gates passed; would consider action (direction=…, dwell=…/…)`.

**Verify:** Build clean. Enable `bDebugMode=1`. Boot Skyrim; let a phase overrun its ideal
duration (or shorten it via INI for the test). Confirm the log shows the per-gate decisions and
the final "would consider action" line. Save with `g_actionInFlight` empty; reload; confirm the
`'NEAC'` record round-trips without warning.

---

### Step 6 — Action-select prompt and dispatcher LLM call

- [x] Complete

**[CLAUDE]**

**Goal:** Wire the LLM round-trip into the dispatcher. With no actions registered yet, the
candidate list is always empty and the call still won't fire — but a log line shows how close we
got, and the deferred-DecisionRecord plumbing is in place for Step 7.

**Files:**

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_action_select.prompt` — new prompt
  per the **action-select prompt** section above; follows `docs/CUSTOM_PROMPTS.md`.
- `src/ActionDispatcher.cpp` — implement `BuildActionPromptContext`, `SendCustomPromptToLLM`
  call, response parsing (`StripMarkdownFences` extracted to a shared helper if not already),
  candidate-name validation, `narrativeNote` propagation back into the provisional record, and
  the deferred `DecisionLog::Append` on action-firing ticks.
- `src/EvaluationPipeline.cpp` — refactor so that when the dispatcher fires the action-select
  LLM call, the provisional `DecisionRecord` rides on the dispatcher's continuation and the
  `Append` + `g_inFlight` release happen in the dispatcher's callback. Non-action ticks remain
  on the existing direct path.
- `include/EvaluationPipeline.h` (or a new tiny shared helper header) — expose
  `StripMarkdownFences` so the dispatcher can reuse it.

**Specifics:**

- Prompt context JSON: `{ desired_direction, tension_delta, candidates: [{name, description}],
  player_context: { location_name, cell_name, cell_is_interior }, recent_events: [<short tail>] }`.
- Failure paths populate `actionSelected = "(failed: <detail>)"` and append the record. Cooldown
  applies (i.e. update `g_lastActionCompletedAt = now`) — failed attempts shouldn't immediately
  retry next tick.

**Verify:** Build clean. Re-run the Step 5 scenario; confirm the log now ends with
`ActionDispatcher: 0 candidates after filtering; no action this tick` (since no actions are
registered). No regression to ordinary ticks.

---

### Step 7 — Ambush quest, AI package, alias slots, Papyrus scripts

- [x] Complete

**[USER — Creation Kit + Papyrus]**

**Goal:** Author all the CK-side and Papyrus-side content for the ambush. After this step the
quest is self-contained: starting it (via console `startquest _ne_BanditAmbushQuest` for now,
later via the C++ `AmbushAction` dispatch in Step 8) causes bandits to spawn from existing
world markers near the player, jog into engagement range, fight to the death, and complete the
quest. The C++/ModEvent integration is deliberately deferred to Step 8.

The design intentionally diverged from the original sketch in several places. Notes below
record both the final shape and the non-obvious findings that drove the divergence — they're
captured here because they apply to any future "spawn-and-engage" action, not just Ambush.

**Sub-tasks:**

1. **AI Package — `_ne_BanditAmbushTravel`** (in CK, on `NarrativeEngine.esp`):
   - Template: `Travel`.
   - Travel Location: Alias `PlayerRef`, Near Reference.
   - Preferred Speed: Jog.
   - **No conditions** — the script flips the bandit out of the package by raising aggression
     and starting combat at the release threshold; vanilla combat interrupt then preempts.
   - **Critical flags:** `Ignore Combat` = FALSE, `Interrupt Override` = None. With either of
     these set the way the UseWeapon experiments left them (`Ignore Combat` = TRUE, `Interrupt
     Override` = Combat) the package dominated over `StartCombat`, the bandit would jog to the
     package destination and stand idle for 5–10s after script release before vanilla combat
     AI eventually took over.

2. **Quest — `_ne_BanditAmbushQuest`** (in CK):
   - Quest Data: Run Once OFF, Allow Repeated Stages ON, Priority 90, Type None.
   - Stages: 0 (init), 100 (resolved — **Complete Quest** flag checked).
   - One **Location alias** for the player's current Location (used by the bandit aliases'
     FMR conditions to constrain marker selection to the player's loaded space).
   - One **PlayerRef** ReferenceAlias (Forced fill → PlayerRef) for use by the spawned bandits.
   - Six bandit ReferenceAliases (`Bandit01Ref` through `Bandit06Ref`). Each:
     - **Fill Type: Find Matching Reference** (not Forced — we use existing world markers
       rather than `PlaceAtMe`-style programmatic spawning).
     - **FMR conditions** select an `XMarker`/`XMarkerHeading` with a `GetDistance > PlayerRef`
       band (close enough to feel ambushy, far enough to not pop into the player's eyeline),
       `IsInInterior == 0`, and `GetInCurrentLocAlias` matching the player-Location alias
       (keeps markers to the same Location as the player so the bandit has a reachable
       navmesh).
     - Optional ON, Packages = `_ne_BanditAmbushTravel`.
     - Script: `_ne_BanditAmbushQuest_SpawnedBandit` (see sub-task 3).
   - Faction setup is handled by the leveled bandit's own faction membership at level-list
     resolution; no extra alias-level faction work needed.

3. **Papyrus scripts** (in `<repo>/esp/Source/Scripts/`):

   `_ne_BanditAmbushQuest.psc` (quest script):

   - Properties: `Bandit01Ref` through `Bandit06Ref` (ReferenceAlias each).
   - `CheckAllBanditsDead()`: early-return if `GetStage() >= 100`; otherwise count alive
     bandits across all six aliases (a `None` alias or a `None` actor counts as dead, so a
     partial FMR fill doesn't strand the quest); when count is zero, `SetStage(100)`,
     `CompleteQuest()`, `Stop()`. All three are needed: stage 100 alone marks the journal entry
     complete and runs the stage fragment, `CompleteQuest()` flips the IsCompleted flag, and
     `Stop()` actually halts the quest so its aliases release.

   `_ne_BanditAmbushQuest_SpawnedBandit.psc` (alias script on each Bandit*Ref):

   - Property: `PlayerRef` (ReferenceAlias).
   - `OnAliasInit` / `OnLoad` → `Initialize()`: guard against double-init; set Aggression to 0
     (so the bandit doesn't initiate combat with civilians/guards encountered en route — the
     Travel package handles approach, not the bandit's own AI); `DrawWeapon`;
     `EvaluatePackage`; register single update.
   - `OnUpdate` (poll every 0.5s): if not yet released and distance to player ≤ 1500 units,
     raise Aggression to 2, `EvaluatePackage`, `StartCombat(player)`, set a 3-tick post-release
     lock; otherwise re-register. During the lock window, re-assert `StartCombat(player)` if
     combat AI's target has drifted.
   - `OnDeath(akKiller)` / `OnDying(akKiller)`: cast `GetOwningQuest() as
     _ne_BanditAmbushQuest`, call `CheckAllBanditsDead()`. Both events are wired because the
     reliability of `OnDeath` on runtime-aliased actors is variable; `OnDying` fires more
     consistently, and `CheckAllBanditsDead` is idempotent (it early-returns once stage 100 is
     set), so duplicate calls are harmless.

4. **Build + deploy:**
   - Run `pwsh -File build.ps1 build`. The Papyrus compile step produces both
     `.pex` files; the sync hook copies them and the `.esp` into the mod folder.

**Non-obvious findings worth not relearning:**

- `Actor.SetGhost(true)` disables package execution. Used initially to make bandits
  non-targetable en route; effect was that the Travel package silently failed to drive any
  movement at all. Use Aggression=0 (and faction membership tricks if needed) instead.
- Pure-script movement (`PathToReference`, `KeepOffsetFromActor`) without an active AI package
  is unreliable — the engine quietly defers to the actor's base default behavior. A minimal
  Travel package is required to anchor movement.
- `GetOwningQuest()` cast to the quest's script type is preferable to an explicit
  `BanditAmbushQuest` property on the alias — the property has to be filled per-alias in CK
  (six times here), is easy to leave None, and is invisible when None (the `if quest` guard
  short-circuits silently rather than logging).

**Verify:** Boot Skyrim, load a save outside a settlement with several XMarkers nearby. Open
console, `startquest _ne_BanditAmbushQuest`. Within ~1s, up to six bandits spawn at nearby
markers, jog toward the player ignoring intervening NPCs, and engage in vanilla combat at
~1500 units. Kill them all; `sqv _ne_BanditAmbushQuest` reports `State: Stopped`, `Stage: 100`,
`Completed: Yes`, `Enabled: No`. The quest can be restarted immediately with the same command.

---

### Step 8 — AmbushAction C++ implementation and registration

- [x] Complete

**[CLAUDE]**

**Goal:** Implement the thin C++ side of the ambush and register it with the toolbox. After this
step the full end-to-end Director loop works: tension overrun → dispatcher fires action-select →
LLM picks ambush → quest starts → fight → completion clears in-flight → cooldown applies.

The shape of "C++ starts the quest, Papyrus signals completion via ModEvent" sketched in earlier
steps mostly didn't survive contact with the engine. The final shape treats the
**`RE::TESQuest` instance as the single source of truth** for ready / running / completed; both
the start path and the completion path are pure native C++ that read and mutate that instance.
Notes below record the final design and the non-obvious findings that drove the divergence —
they apply to any future quest-backed action, not just Ambush. The per-finding deep-dives live
under `docs/engine-findings/`.

**Files:**

- `include/IAction.h` — extend the interface with two new optional poll hooks the dispatcher
  drives every tick while the action is in flight:
  - `virtual bool DetectAndRollbackFailedStart(ctx, secondsSinceStart)` — return true if Start
    visibly didn't take (e.g. required alias slots never filled). The action must tear down its
    own engine-side state before returning true so the next tick's re-attempt starts clean.
  - `virtual bool DetectCompletion(ctx, secondsSinceStart)` — return true once the action has
    visibly completed (quest reached its Complete-Quest stage, etc.). The action must tear down
    engine-side state before returning true. Complementary to the `_ne_ActionCompleted`
    ModEvent path: actions whose Papyrus reliably sends that event can ignore this hook;
    actions whose completion is observable from native state can use this instead.
  Both hooks default to `return false`.
- `include/AmbushAction.h` — declares `class AmbushAction : public IAction`, overrides Start +
  the two new poll hooks, and forward-declares an `AmbushAction_Persistence` namespace that
  owns this action's co-save record (`'NEAB'`).
- `src/AmbushAction.cpp` — implementation:
  - `Name() = "ambush"`. `Polarity() = Raise`. `Description()` returns the LLM-facing
    one-paragraph blurb.
  - `IsAvailable(ctx)`:
    - Cheap snapshot gates (`playerInInterior`, `playerInCombat`, `playerInDialogue`).
    - **`AlphaCanon::IsInScriptedScene()` and `AlphaCanon::IsInDoNotDisturbCell()`** for the
      scripted-scene and DND-cell guards — re-using the shared module rather than recomputing
      keyword / cell checks inline.
    - **`LocationKeywords::IsSafe(ctx.player->GetCurrentLocation())`** for the
      city/town/inn/habitation check — also a shared module, with its own keyword caching and
      "fail open on missing keyword" policy.
    - **Quest-state gates, with the IsRunning caveat:**
      - `!quest` → unavailable (ESP not loaded or EditorID lookup failed).
      - `quest->IsCompleted()` → unavailable (still in completion-pending state).
      - `quest->GetCurrentStageID() > 0` → unavailable (stage 10+ means really running).
      - **Do NOT** gate on `quest->IsRunning()` — see specifics below.
    - **Per-action in-game-hour cooldown.** If `iAmbushPerActionCooldownGameHours > 0` and the
      stored `g_lastCompletionGameHours` is non-zero, block when
      `Calendar::GetHoursPassed() - lastCompletionGameHours < cooldownHours`.
  - `Start(ctx, params)`: re-validate `IsAvailable` (engine state may have shifted since the
    dispatcher's filter pass); clamp `bandit_count` / `spawn_distance_units` per settings
    (advisory only for Phase 03 — the FMR-filled aliases ignore them; clamping is here so the
    values land sensibly in the DecisionRecord and Phase 04 can wire them through without
    re-shaping parameter handling); look up the quest via
    `RE::TESForm::LookupByEditorID("_ne_BanditAmbushQuest")`; call
    `quest->EnsureQuestStarted(engineResult, /*startNow=*/true)`. Return `started=true` with a
    one-line detail; log a warning on engine-reported failure but still return started=true
    (the dispatcher's `DetectAndRollbackFailedStart` poll will catch a genuine non-promotion).
  - `DetectAndRollbackFailedStart(ctx, secondsSinceStart)`: after a 2s grace period, walk
    `quest->aliases`; for every required (`!FLAGS::kOptional`) `BGSRefAlias`, check
    `GetReference() != nullptr`. If not all required ref aliases are filled, run the teardown
    trio (`Stop()` → `Reset()` → `SetEnabled(false)`) and return true so the dispatcher rolls
    back in-flight and the action can re-fire next tick.
  - `DetectCompletion(ctx, secondsSinceStart)`: read `quest->IsCompleted()`; if true, run the
    same teardown trio, stamp `g_lastCompletionGameHours = Calendar::GetHoursPassed()`, and
    return true.
  - `AmbushAction_Persistence::OnSave/OnLoad/OnRevert` — read/write a single
    `double g_lastCompletionGameHours` under co-save record `'NEAB'`, version 1.
- `esp/Source/Scripts/_ne_BanditAmbushQuest.psc` — `CheckAllBanditsDead()` advances to
  **stage 200** (not 100), calls `CompleteQuest()`, sends the `_ne_ActionCompleted` ModEvent
  (kept for the dispatcher's existing sink path; the C++ `DetectCompletion` poll is an
  independent and now-primary completion signal), then `Stop()`. The stage marker move from 100
  to 200 keeps stage 100 free as a future "actively winding down" marker if a later action
  needs one.
- `include/ActionDispatcher.h` / `src/ActionDispatcher.cpp` — drive the new poll hooks from
  `OnTick`: while an action is in-flight, call `DetectAndRollbackFailedStart` first, then
  `DetectCompletion`. On a true return from completion, update `g_lastActionCompletedAt = now`
  so the global cooldown starts running. (The existing `_ne_ActionCompleted` ModEvent sink
  still works; the poll is an additional path, not a replacement.)
- `include/Settings.h`, `src/Settings.cpp` — add
  `int ambushPerActionCooldownGameHours = 24;` read from
  `[Actions] iAmbushPerActionCooldownGameHours`.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — document the new key.
- `src/Plugin.cpp` — at `kDataLoaded`, after `ActionDispatcher::Initialize()`, call
  `ActionRegistry::Register(std::make_unique<AmbushAction>())`. Wire
  `AmbushAction_Persistence::OnSave/OnLoad/OnRevert` into the central serialization callbacks
  alongside the existing per-subsystem records.

Auxiliary files created during the investigation and retained for future use:

- `include/ConsoleCommand.h` / `src/ConsoleCommand.cpp` — a small `Run(std::string_view)`
  utility that builds a transient `RE::Script`, calls `CompileAndRun(nullptr)`, and deletes it.
  Briefly used as a workaround when no native quest-start path was working; replaced by
  `EnsureQuestStarted`. Kept around because it's the obvious tool for future "run an arbitrary
  console command from C++" needs.

**Specifics — what did and didn't work:**

- **`LookupByEditorID` is the right surface.** Avoids hardcoding the local FormID CK assigned
  the quest, which would otherwise be a fragile coupling between C++ and the ESP. Requires
  powerofthree's Tweaks (or equivalent runtime EditorID retention) to be loaded.
- **`EnsureQuestStarted(result, /*startNow=*/true)`** is the C++ entry point that matches
  console `startquest` semantics — full engine promotion including the stage-0 fragment AND
  Find-Matching-Reference alias evaluation. Other native paths we tried (`Start()` alone,
  `SetEnabled(true)` + `Start()`, `SetEnabled(true)` + `ResetAndUpdate()`, VM-dispatched
  `Quest.Start`) variously flipped flags, sometimes advanced the stage, but **never triggered
  the FMR pass**, leaving alias slots empty. The full investigation lives in
  `docs/engine-findings/starting-a-quest-from-cpp.md`.
- **`TESQuest::IsRunning()` is unreliable.** It reads the same `kEnabled` bit
  `IsEnabled()` reads — returns true for any enabled quest, including ones that have never
  been promoted (e.g. a "Start Game Enabled" quest at load). Useless as a "really running"
  signal. The behavior-defined check is `GetCurrentStageID() > 0`: stage 0's fragment advances
  to stage 10 (or whatever the first encounter-active stage is) when the engine actually
  promotes the quest, and `Reset()` drops it back to 0 on cleanup. Memory:
  `feedback-tesquest-isrunning-unreliable`.
- **Completion is detected by polling `IsCompleted()`, not by the ModEvent.** Papyrus's
  CompleteQuest at stage 200 sets the `kCompleted` data-flag bit; `IsCompleted` reads it. The
  poll is independent of the Papyrus completion fragment running correctly, runs on the same
  cadence the dispatcher already drives, and gave us a single self-contained C++ teardown path
  (`Stop()` → `Reset()` → `SetEnabled(false)`) instead of two halves of the same operation
  living on opposite sides of the boundary. The Papyrus-side `_ne_ActionCompleted` send is
  kept (it's free and the dispatcher's sink still works on it) so the ModEvent path remains
  available for any future action whose completion isn't observable from quest flags.
- **Stage 200, not 100, is the Complete-Quest marker.** Stage 100 is left free for a future
  "actively winding down" intermediate marker; jumping straight to 200 also avoided an
  observed quirk where reading `IsCompleted` immediately after `SetStage(100)` returned false
  briefly even though completion had effectively fired.
- **Per-action cooldown is in game hours, not real seconds.** The dispatcher's global cooldown
  is real wall-clock and prevents back-to-back firings across all actions; this per-action
  gate prevents the same action firing twice in the same in-fiction afternoon. In-game time
  has the nice property that long real-world pauses (alt-tabbing for hours) don't accidentally
  clear it, and time spent sleeping / waiting / fast-traveling does count toward unlocking.
  Persisted via the `'NEAB'` co-save record (single `double`, version 1).
- **DetectAndRollbackFailedStart checks alias fill, not stage advancement.** We observed that
  partial promotion paths (the ones that didn't trigger FMR) still advanced the stage — so
  "stage > 0" by itself was a false-positive success signal. The reliable correlate of "the
  encounter actually exists in the world" is "every required `BGSRefAlias` has a non-null
  `GetReference()`." The check is restricted to `BGSRefAlias` because that's the only alias
  type we have a clean introspection API for; other alias types (LocationAlias, etc.) are
  skipped rather than treated as failures.

**Other engine findings from this work** (full write-ups in `docs/engine-findings/`):

- `ai-package-flags-for-script-driven-combat-handoff.md` — the approach package needs
  `Ignore Combat` unchecked and `Interrupt Override = None` for a script-driven combat handoff
  to preempt cleanly. The opposite settings produce a 5-10s stare-before-swinging gap.
  Memory: `feedback-ck-package-combat-handoff`.
- `avoid-setghost-with-ai-packages.md` — `Actor.SetGhost(true)` silently breaks AI package
  execution. Use `Aggression=0` or temporary faction membership for "ignored by NPCs"
  instead. Memory: `feedback-ck-setghost-breaks-packages`.

**Verify:** Boot Skyrim. Walk outside Whiterun. Drop `iIdealDurationExposition` to 60 for the
test. Wait through one tick past the threshold. The SKSE log shows: tension call →
action-select call → `EnsureQuestStarted` call → Papyrus log shows spawn → fight unfolds →
kill the bandits → stage 200 / `IsCompleted` fires → C++ `DetectCompletion` logs cleanup and
sets the cooldown stamp → `g_actionInFlight` cleared. Now wait under `iActionCooldownSeconds`
(2 min default) and force another tick past ideal duration; the dispatcher's global gate
blocks. Beyond cooldown but inside the **24 in-game-hour per-action** cooldown, `IsAvailable`
filters ambush out (look for the `per-action cooldown` debug log line). Inside Whiterun, the
action is also filtered out via the `LocationKeywords::IsSafe` check. Save/load mid-encounter:
the in-game-hour stamp survives via `'NEAB'`, and the in-flight quest survives via Skyrim's
own quest persistence — killing the remaining bandits still triggers completion correctly.

---

### Step 9 — Dashboard action-in-flight surfacing

- [ ] Complete

**[CLAUDE]**

**Goal:** Surface the dispatcher's in-flight state and most recent action result in the React
dashboard so the developer / curious player can see what the Director is doing without tailing
the log.

**Files:**

- `dashboard/src/types.ts` — extend `DirectorState` with optional
  `action_in_flight: { name: string, started_at: number } | null`. `last_evaluation.action` is
  already typed; no change there.
- `dashboard/src/components/LastEvaluation.tsx` — if `evaluation.action` is non-empty, render
  `→ fired: <action>`. If it starts with `"(failed:"`, render `→ action failed: <rest>`.
- A small new component (or extension of the phase panel) — when `action_in_flight` is non-null,
  render `action in flight: <name> (started Ns ago)`.
- `src/DashboardUIManager.cpp` — populate `action_in_flight` in `ComposeFullStateJSON` from
  `ActionDispatcher::GetInFlightInfo()`. Push a fresh state on every dispatcher state change
  (completion, stale-lock clear) in addition to the existing per-tick push.

**Specifics:**

- The TS type ↔ C++ JSON contract is enforced at build time via the existing TypeScript build;
  if shapes drift, `npm run build` fails.
- Rebuild bundle: `cd dashboard && npm run build` (or whatever the existing convention is).

**Verify:** Boot Skyrim, trigger an ambush (via Step 8's flow). Open dashboard with F7. The
in-flight badge appears during the fight and clears on quest completion. The "→ fired: ambush"
line under Last Evaluation persists as a historical record.

---

## Open questions

Flag for revisit during or after implementation, not blockers:

- **Cooldown reset on phase change.** Should advancing to a new phase reset the action cooldown?
  Pro: responsive Director at the start of a new phase. Con: spam risk if the cycle is rapidly
  traversing. Current plan: do *not* reset on phase change; revisit after observing.
- **`DefaultCombat` package reliability.** If template-based bandits don't aggro reliably,
  switch to a hand-built Find→Combat package. Decision deferred to in-CK testing.
- **Spawn position vs. navmesh.** Picking a random XY offset doesn't guarantee a navmeshed spot.
  In practice exterior worldspace is heavily navmeshed, but some wilderness corners are not.
  If "bandits get stuck on terrain" becomes a real failure mode, add a navmesh check (or use a
  small set of placed `XMarkerHeading` reference candidates that the script picks from). Defer
  until observed.
- **What if zero candidates are available repeatedly?** Director sits in a long phase, every
  tick filters down to an empty candidate list, no action ever fires. Acceptable for Phase 03
  with one action; becomes a real concern at 3+ actions, where zero-candidate ticks should be
  near-impossible.
