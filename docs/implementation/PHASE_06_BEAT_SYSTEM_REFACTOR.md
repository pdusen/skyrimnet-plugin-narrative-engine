# Phase 06 — Narrative Beat System Refactor

Phases 03 through 05 shipped three actions (`AmbushAction`,
`NPCLetterAction`, `NPCVisitAction`) and a dashboard. Each
action brought its own dispatch verification, its own
watchdog threads, its own async pollers, its own retry /
rollback branches, and its own definition of "how do I know
I'm still running?" The result is a working but sprawling
collection of ad-hoc lifecycles — no two actions treat time
the same way, and none of them resume cleanly across
save/load. `PollUntilOrTimeout` watchdogs run on private
worker threads, watchdog-abort counters race with actual
gameplay events, and cosave state lags in-memory state
because each action decided independently what to persist
and when.

This phase replaces all of that with a single, uniform
lifecycle. The plugin gets **one** driver — a 250ms master
poll that owns every timer, every waiting state, and every
advancement decision. Individual beats become
`Tick(mode)`-only state machines whose entire behavior is a
function of their cosave-recorded state and their quest's
current stage. Watchdog threads, `PollUntilOrTimeout`, per-
beat async lifecycles, and the "how do I resume mid-visit?"
class of bug all go away.

The phase also renames the system: **Narrative Beat System**
replaces "Action System", and individual dispatches are
**Narrative Beats** ("beats" for short) instead of "actions".
The old terminology collided with SkyrimNet's own "Action"
concept and the engine's ModEvent surface, and neither name
was distinctive enough to log about or grep for cleanly.
See **Naming: from Actions to Narrative Beats** below for
the full rename map.

No new gameplay features. This phase is scoped strictly to
the beat system and its constituent beats. The Director's
evaluation pipeline, phase tracking, tension scoring, and
everything outside the beat machinery is untouched.

---

## Why this phase exists

Three concrete pain points from Phases 03–05, in order of
severity:

1. **Watchdog timers are not resumable.** Every existing
   beat spins up its own `PollUntilOrTimeout` worker
   thread that holds a wall-clock deadline in process
   memory. If the player saves mid-visit and reloads, the
   thread is gone but the quest is still running; the visit
   continues but with no watchdog, no failure recovery, and
   no way to reason about its state. This has bitten us
   directly (hard-abort at 900s; Discuss watchdog firing
   during load screens); each fix has been local to one
   beat.

2. **State lives in too many places at once.** The visit
   beat splits its state across the quest stage,
   `VisitState` in-memory snapshot, three separate watchdog
   threads, and the `NEVS` cosave record. Each of these has
   its own idea of "where we are" and each can lag or drift.
   Reasoning about "is this beat running?" requires reading
   three sources and hoping they agree.

3. **No two beats look alike.** `AmbushAction`,
   `NPCLetterAction`, and `NPCVisitAction` implement their
   lifecycles in three visibly different shapes. Adding a
   fourth beat means either picking one to imitate or
   inventing a fourth pattern; neither scales.

The unifying observation: all three problems come from
per-beat async machinery. A single-threaded, single-poll,
state-machine-in-cosave model resolves all three, and does
so with much less code than the current combined footprint.

The rename is secondary but pairs naturally with a
demolition of the old surface — old symbol names go away
whether we rename or not, so renaming them costs almost
nothing on top.

---

## Scope

### In scope

- A new **`BeatSystem`** module owning the master poll
  loop, the top-level state, and the per-beat dispatch. It
  supersedes the current `ActionDispatcher`'s in-flight and
  lifecycle-tracking responsibilities. (The Director's
  handshake into it — "please start beat X" — is preserved;
  see **Director / BeatSystem handshake** below.)
- A new **`IBeat` contract** built around `Tick(context,
  mode, state)` plus the four-state lifecycle described
  below. `Start()`, `DetectAndRollbackFailedStart()`, and
  `DetectCompletion()` are removed from the interface;
  their responsibilities are absorbed into `Tick` and its
  state-machine arms.
- **Rename sweep across the codebase.** Every C++ symbol,
  cosave record ID, INI key, ModEvent name, LLM prompt-
  facing terminology, log message, code comment, and
  dashboard label carrying "Action" or "action" in the
  Narrative Engine's sense gets renamed to "Beat" /
  "beat". The full map lives in **Naming: from Actions to
  Narrative Beats** below.
- Reshape of **all three existing beats** (`AmbushBeat`,
  `NPCLetterBeat`, `NPCVisitBeat`) onto the new contract.
  Every existing per-beat worker thread, watchdog, and
  `PollUntilOrTimeout` call is removed; every equivalent
  behavior is re-expressed as a counter and stage check
  inside the beat's `Tick`.
- A new **top-level cosave record** (`'NBSY'`,
  "NarrativeEngine Beat SYstem") carrying the two-state
  top-level state and the global-cooldown counter.
- Restructure of the **three per-beat cosave records** —
  new type IDs (`'NBAM'`, `'NBLP'`, `'NBVS'`), new schema
  carrying the beat's `BeatState` enum (NOT_RUNNING /
  COMPOSE / RUNNING / CLEANUP) plus only the residual data
  that materially outlives an async op — composed text,
  sender FormID, return-teleport pose, per-beat cooldown
  counters, etc. Any state that only exists during an in-
  flight async op is dropped; on reload, that op is
  restarted from scratch.
- Removal of `VisitState`'s per-visit intermediate counters
  and every `PollUntilOrTimeout` call site. The
  `VisitConclusionPoll` module collapses into a single
  `Tick` arm on `NPCVisitBeat`.
- Removal of `AsyncDispatch::PollUntilOrTimeout` entirely —
  no remaining caller once the three beats are reshaped.
  `MarshalToMainThread` is preserved (still needed for LLM
  callbacks and worker→main handoff).
- The `IBeat::IsAvailable` contract is preserved unchanged
  (parameter type is `BeatContext`, otherwise identical to
  today's `IAction::IsAvailable`). Availability gates
  (per-beat cooldowns, precondition checks, sender-pool
  counts) stay inside each beat; the Director's beat-select
  LLM candidate build still calls `IsAvailable` on each
  registered beat to filter the choose-from set.

### Deferred (explicitly out)

- **The Director's evaluation pipeline.** Tension scoring,
  Freytag phase tracking, and the beat-select LLM prompt
  are all untouched. The Director's tick continues to run
  on its own cadence; only its final handshake with the
  beat side changes shape.
- **New beats.** The three existing beats are reshaped and
  re-verified. No fourth beat lands in this phase.
- **New settings or new INI keys** beyond one needed for
  the master poll's cadence.
- **Dashboard restructuring.** The dashboard's in-flight
  query surface changes trivially (the top-level state
  moves from `ActionDispatcher` to `BeatSystem`, and every
  visible label with "Action" becomes "Beat"), but no UI-
  visible rework beyond that.
- **Papyrus refactor.** The quests, their stages, their
  fragments, their alias fills, and their packages are all
  preserved as-is. The C++ side changes; the ESP does not.
- **Historical phase docs.**
  `PHASE_03_ACTION_TOOLBOX.md` etc. are frozen artifacts
  and are not swept for terminology.

---

## Core concepts

### Naming: from Actions to Narrative Beats

The old term "Action" collided with SkyrimNet's own Action
concept (invoked via `SkyrimNetApi.ExecuteAction`) and the
old term "Event" collided with the engine's ModEvent
surface. Neither was distinctive enough to grep for or log
about cleanly — "action started" in the log could mean our
dispatch, SkyrimNet's action, or something upstream. This
phase renames the whole surface. From here forward:

- The system is the **Narrative Beat System**.
- Individual dispatches are **Narrative Beats**, or "beats"
  for short.

Concrete rename map. New name in every case; the old name
is preserved only in this table for reference during the
rename sweep and in the transitional cosave-load fallback.

| Category         | Old                                | New                              |
|------------------|------------------------------------|----------------------------------|
| Module           | `ActionSystem` (proposed)          | `BeatSystem`                     |
| Module (deleted) | `ActionDispatcher`                 | *(rolled into `BeatSystem`)*     |
| Interface        | `IAction`                          | `IBeat`                          |
| Registry         | `ActionRegistry`                   | `BeatRegistry`                   |
| Context struct   | `ActionContext`                    | `BeatContext`                    |
| Polarity enum    | `ActionPolarity`                   | `BeatPolarity`                   |
| Per-beat state   | *(new)*                            | `BeatState`                      |
| Tick mode enum   | *(new)*                            | `TickMode`                       |
| Beat: ambush     | `AmbushAction`                     | `AmbushBeat`                     |
| Beat: letter     | `NPCLetterAction`                  | `NPCLetterBeat`                  |
| Beat: visit      | `NPCVisitAction`                   | `NPCVisitBeat`                   |
| Cosave: system   | `'NEAC'`                           | `'NBSY'`                         |
| Cosave: ambush   | `'NEAB'`                           | `'NBAM'`                         |
| Cosave: letter   | `'NELP'`                           | `'NBLP'`                         |
| Cosave: visit    | `'NEVS'`                           | `'NBVS'`                         |
| INI section      | `[Actions]`                        | `[Beats]`                        |
| INI: cooldown    | `iActionCooldownSeconds`           | `iBeatCooldownSeconds`           |
| INI: repetition  | `iActionRepetitionWindowSeconds`   | `iBeatRepetitionWindowSeconds`   |
| INI: stale lock  | `iActionStaleLockTimeoutSeconds`   | *(removed; per-beat counters)*   |
| INI: ambush CD   | `iAmbushPerActionCooldownGameHours`| `iAmbushPerBeatCooldownGameHours`|
| INI: letter CD   | `iLetterActionCooldownGameHours`   | `iLetterBeatCooldownGameHours`   |
| DecisionRecord   | `actionSelected`                   | `beatSelected`                   |
| DecisionRecord   | `actionParametersJSON`             | `beatParametersJSON`             |
| ModEvent         | `_ne_ActionCompleted`              | *(removed; C++ sink deleted)*    |
| LLM prompt       | "action-select"                    | "beat-select" *(in code refs)*   |
| Dashboard tab    | Actions / Dispatch                 | Beats / Dispatch                 |
| Dashboard label  | "Action in flight"                 | "Beat in flight"                 |

Preserved as-is (not renamed): SkyrimNet-side prompt names
(`narrative_engine_visit_compose`, etc.), ESP form editor
IDs (`_ne_VisitQuest`, `_ne_VisitSenderFaction`, etc.),
Papyrus script class names on the ESP side, and the
individual beat-specific settings that don't say "action"
(`iAmbushDefaultBanditCount`, `iVisitBriefingMinWords`,
etc.).

Historical `PHASE_NN_*.md` docs are not swept — they are
frozen artifacts of what shipped at the time.

### Two-layer state model

The system has a top-level state machine (owned by
`BeatSystem`) and a per-beat state machine (owned by each
`IBeat`). Both live in the SKSE cosave; neither has any in-
memory state that must survive save/load.

**Top level — two states:**

- **`NO_BEAT_RUNNING`** — no beat is currently in flight. A
  single global cooldown counter ticks upward (see below);
  the poll is otherwise idle. Transitions to `BEAT_RUNNING`
  when the Director asks the system to start a beat.
- **`BEAT_RUNNING`** — a beat is in flight. The record
  carries the name of the running beat. Each poll cycle,
  the master poll calls that beat's `Tick`; the beat's own
  per-beat state machine drives the actual work.
  Transitions back to `NO_BEAT_RUNNING` when the beat's
  per-beat state returns to `NOT_RUNNING`.

That is the entire top-level model. There is no `EVAL`
state, no `COOLDOWN` state, no `PENDING_COMPOSE` state.
The beat-select LLM cadence stays where it lives today
(inside the Director's own tick); the cooldown is a
counter, not a state; and compose is a per-beat state,
not a top-level one.

**Per-beat — four states:**

- **`NOT_RUNNING`** — the beat's baseline. The master poll
  does not call `Tick` in this state; it's the value the
  record has when the beat isn't in flight at all.
- **`COMPOSE`** — the beat is running its pre-quest work.
  For letter and visit this covers the compose LLM call
  (draft the letter body / draft the visit briefing) plus
  any preparatory work that can't be encoded as a quest
  stage — chiefly the marker-faction promote and the pre-
  dispatch position snapshot. When the pre-work completes,
  `Tick` starts the beat's quest and moves the per-beat
  state to `RUNNING`.
- **`RUNNING`** — the beat's quest is live. All internal
  progression is quest-stage-driven: `Tick`'s job in this
  state is to read `GetCurrentStageID()`, evaluate the
  stage's advancement conditions (distance, counter, LLM
  poll response, whatever the stage cares about), and call
  `SetStage` when the condition trips. When the quest
  reaches its terminal stage (or is stopped for any
  reason), `Tick` advances to `CLEANUP`.
- **`CLEANUP`** — the beat's quest has stopped, but there
  is post-quest work the plugin still needs to do. For
  visit this covers the return teleport and cosave state
  clear; for letter this covers the courier-container
  cleanup; for ambush this may be empty. When cleanup is
  done, `Tick` sets the per-beat state to `NOT_RUNNING`,
  which the master poll observes on its next cycle and
  uses to transition the top level back to
  `NO_BEAT_RUNNING`.

The four-state contract is the same for every beat; what
each beat *does* inside each state is beat-specific.

### The master poll

A single dedicated worker thread wakes every **250ms**
(configurable; see **Settings**) and runs one tick. The
tick body proceeds as follows:

1. **Read the gates** (worker thread, no marshal). Poll
   reads the three coarse world-state flags from stable
   engine singletons: `RE::UI::GameIsPaused()`,
   `player->IsInCombat()`, and dialogue-menu-open. These
   are not officially thread-safe, but they are stable-
   pointer bool reads and CommonLibSSE-NG's off-thread
   usage of them is routine and safe in practice. From
   these three the poll derives a `TickMode` enum:
   - `Normal` — none of the three are active.
   - `Paused` — `GameIsPaused()`.
   - `Combat` — player is in combat and not paused.
   - `Dialogue` — player is in vanilla dialogue and not
     combat and not paused.
   (Only one applies per tick; `Paused` wins over `Combat`
   wins over `Dialogue`.)

2. **Read the top-level state** from the cosave record
   snapshot (worker thread, mutex-guarded).

3. **Dispatch based on top-level state:**

   - **`NO_BEAT_RUNNING`.** Increment the global cooldown
     counter by the tick interval (250ms) if `mode ==
     Normal`; otherwise leave it alone. No marshal. No
     other work. The Director's tick, running on its own
     cadence, will call `BeatSystem::StartBeat(name)` when
     its beat-select LLM lands; that call is what
     transitions the top level to `BEAT_RUNNING` — it does
     not happen from inside the master poll.

   - **`BEAT_RUNNING`.** Look up the running beat by name
     in the `BeatRegistry`, then call `beat->Tick(context,
     mode, state)`. The beat receives the mode and its own
     per-beat state; it decides what to do (or not) and
     returns a `TickResult` indicating whether its per-
     beat state should transition. The master poll applies
     the transition. If the transition lands on
     `NOT_RUNNING`, the master poll clears the running-
     beat name from the top-level record and switches back
     to `NO_BEAT_RUNNING`, resetting the global cooldown
     counter to zero.

4. **Marshal only if the tick needs main-thread work.** The
   worker thread does its own tick decisions using only
   cosave-owned state and the three gate reads. When
   `Tick` decides it needs to actually mutate the engine —
   fire an LLM call, advance a quest stage, read an
   actor's position, teleport the sender — it marshals
   that specific block of work to the main thread via
   `AsyncDispatch::MarshalToMainThread`. The overwhelming
   majority of ticks — waiting for a threshold, watching a
   counter climb, gate-blocked — do no marshaling at all.

Per the ~16ms main-thread queue latency at 60fps, marshaled
work lands roughly one game frame after the poll decides
it's needed. That's invisible at the 250ms cadence.

### Tick modes and per-beat behavior under each

The `mode` argument passed to `Tick` lets the beat decide
what a paused / combat / dialogue tick means for it. The
convention across all three beats:

- **`Normal`** — full behavior. Increment counters,
  evaluate transitions, fire LLM calls, mutate quest
  stages.
- **`Paused`** — freeze. `Tick` returns immediately without
  incrementing any counter or evaluating any transition.
  Real-time watchdogs pause; game-time watchdogs also
  pause because game time doesn't advance while paused.
- **`Combat`** — beat-specific. `NPCVisitBeat` uses this as
  the trigger for the OnHold detour and its combat-stuck
  counter; other beats treat it as `Paused`-equivalent for
  their own purposes.
- **`Dialogue`** — treat as `Paused` for advancement
  purposes: the player is already occupied by an engine
  system, and driving state transitions on top of that has
  historically been the source of visible bugs (dispatch
  during vanilla dialogue, LLM calls landing while the
  player is speaking to a shopkeeper). Counters do not
  advance in `Dialogue`.

Both the master poll's own logic and the beat's `Tick`
receive the same mode. The top-level state's cooldown
counter also freezes in `Paused` / `Combat` / `Dialogue`.

### Restart-on-reload principle

The state persisted to cosave is only what materially
survives a save/load cycle. Everything else — including
every "async operation currently in flight" — is not
persisted, and is restarted from scratch on reload if the
state we come back into implies it.

Concretely:

- **LLM calls in flight are not persisted.** If the game
  crashes or the player saves and reloads mid-compose, the
  beat's cosave state says `COMPOSE`. On the first tick
  after reload, `Tick` sees `COMPOSE`, sees no live LLM
  request from this session, and fires a fresh compose
  call. The previous session's response (if it was on the
  wire) is discarded when it lands, because the beat's
  callback checks against a per-request nonce that no
  longer matches.
- **Watchdog counters are not persisted.** A "how long has
  the sender been closing distance in Salutation?"
  counter that ticks on every `Normal` poll is entirely
  in-memory; on reload it resets to zero. This is correct:
  the wall-clock context that made the counter meaningful
  (that we've been waiting for the sender) is itself only
  real inside a single play session.
- **Multi-step async sequences may persist intermediate
  data on the cosave record** if the sequence has already
  produced work that can't be redone. Compose is the
  canonical case: once the compose LLM call returns, its
  output (briefing text, topic, mood) is written to the
  beat's cosave record, and the beat transitions to
  `RUNNING`. If the player saves *after* that write,
  reload lands directly in `RUNNING` with the composed
  text preserved; the compose call is not re-fired.

The rule that falls out of this: the cosave record's
schema is exactly the set of fields that must survive a
reload to preserve the player's experience. Anything the
plugin can regenerate freshly on reload does not go in
the cosave.

### Director / BeatSystem handshake

The Director's existing beat-select pathway is preserved
in shape but its target changes. Today:

- `EvaluationPipeline::ProcessTick`
  → `ActionDispatcher::ConsiderAction`
- `ConsiderAction` walks gates, fires the action-select LLM,
  and on response calls `chosen->Start(ctx, params)`.
- `Start` returns synchronously with a `StartResult`; the
  dispatcher writes `actionInFlight` and starts polling
  `DetectAndRollbackFailedStart` / `DetectCompletion`.

After this phase:

- `EvaluationPipeline::ProcessTick`
  → `BeatSystem::ConsiderBeat` (entry point moves modules
  and renames).
- `ConsiderBeat` walks gates, fires the beat-select LLM,
  and on response calls `BeatSystem::StartBeat(name,
  params)`.
- `StartBeat` (main thread) sets the top-level state to
  `BEAT_RUNNING`, writes the beat's per-beat state to
  `COMPOSE` (with the LLM-supplied parameters preserved on
  the beat's cosave record), and returns. The master poll
  picks it up on its next 250ms cycle.

The Director does no polling of the running beat; it does
no in-flight tracking beyond "the `BeatSystem` told me a
beat is running, so don't fire another beat-select LLM
right now." The master poll owns the entire lifecycle from
`StartBeat` through the terminal `NOT_RUNNING` transition.

The completion signal that used to flow back through the
`_ne_ActionCompleted` ModEvent is no longer needed for
top-level lifecycle tracking — the master poll already
knows the beat has ended because it saw the per-beat state
hit `NOT_RUNNING`. The ModEvent is removed on both the C++
sink side and, where the Papyrus quest fragments were
sending it purely to close the loop with C++, on the
Papyrus side as well. (The ESP changes needed for that are
trivial fragment edits, and are still bookmarked as part
of the deferred "no Papyrus refactor" scope — the
fragments just stop calling `SendModEvent`.)

### The `IBeat` contract

The new interface, on every registered beat:

```cpp
enum class BeatState : std::uint8_t {
    NOT_RUNNING,
    COMPOSE,
    RUNNING,
    CLEANUP,
};

enum class TickMode : std::uint8_t {
    Normal, Paused, Combat, Dialogue,
};

struct TickResult {
    std::optional<BeatState> transitionTo;
    // future: extension points for logging metadata
};

class IBeat {
public:
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual BeatPolarity Polarity() const = 0;

    virtual bool IsAvailable(const BeatContext& ctx) const = 0;

    // The whole per-beat lifecycle. Called by BeatSystem's
    // master poll every 250ms while this beat is the running
    // beat (top-level state == BEAT_RUNNING and this beat's
    // name matches the record).
    virtual TickResult Tick(const BeatContext& ctx,
                            TickMode           mode,
                            BeatState          state) = 0;

    // Called by BeatSystem::StartBeat to seed the beat's
    // per-beat cosave record from the LLM-supplied parameters
    // BEFORE the first Tick lands. The beat stores whatever
    // params it needs (typically none — most beats compose
    // fresh in Tick's COMPOSE arm).
    virtual void OnStart(const BeatContext&    ctx,
                         const nlohmann::json& parameters) = 0;

    virtual double RemainingCooldownGameHours() const { return 0.0; }
};
```

`Start`, `DetectAndRollbackFailedStart`, and `DetectCompletion`
are gone. Their responsibilities are:

- The old `Start`'s "kick off the quest / send the ModEvent"
  work moves inside `Tick`'s `COMPOSE` → `RUNNING`
  transition arm.
- The old `DetectAndRollbackFailedStart` becomes a counter
  in `Tick`'s `COMPOSE` or early-`RUNNING` arm that ticks
  during `Normal` mode and, if it exceeds a threshold
  without seeing the expected progress signal, transitions
  to `CLEANUP` with a `failure_reason` written to the
  cosave record.
- The old `DetectCompletion` becomes the `RUNNING` →
  `CLEANUP` transition condition in `Tick`, driven by the
  beat's own quest-stage read.

### Cosave layout

Two record types touch the beat system.

**`'NBSY'` — top-level BeatSystem state.** New. Owned by
`BeatSystem`.

- `topLevelState` (enum) — `NO_BEAT_RUNNING` /
  `BEAT_RUNNING`.
- `runningBeatName` (string) — populated iff state ==
  `BEAT_RUNNING`. Names the beat whose `Tick` gets called
  each cycle.
- `globalCooldownMs` (uint32) — ticks upward while state
  == `NO_BEAT_RUNNING` in `Normal` mode. Compared against
  `iBeatCooldownSeconds * 1000` to decide when the
  Director's next beat-select LLM call is allowed.

**Per-beat records** — one per beat, all with new type IDs
because the schema is breaking anyway:

- `'NBAM'` — ambush
- `'NBLP'` — letter pool
- `'NBVS'` — visit

Each per-beat record now carries:

- `state: BeatState` — the beat's current four-state
  position.
- Residual data that must survive reload — composed text,
  sender FormID, return-teleport pose, per-beat cooldown
  expiration times, per-sender cooldown maps, etc.

What's dropped from the pre-refactor per-beat records:

- Intermediate-async-op counters — `ignoreNudgeCount`,
  `consecutivePollFailures`, and friends. These reset to
  zero on reload because the counter they belong to only
  means something within one active session's context.
- `dispatchedAtRealSeconds` retained on records where the
  dashboard displays it, otherwise dropped.

`OnLoad` for the new type IDs treats a missing record as
"beat wasn't running when the save was made" and
initializes to `NOT_RUNNING`. The old type IDs (`'NEAB'`,
`'NELP'`, `'NEVS'`, `'NEAC'`) are recognized as pre-
refactor records: their content is skipped and the beat
is initialized to `NOT_RUNNING`. This means loading a
save that had an in-flight beat from a pre-refactor build
silently drops that beat on the floor — the same behavior
every prior schema bump has taken.

### What goes away

For clarity on the demolition scope:

- **`AsyncDispatch::PollUntilOrTimeout`** — every existing
  caller (there are ~six across the three beats) is
  reshaped to counter-in-`Tick` form. The helper itself is
  removed.
- **`VisitConclusionPoll`** as a standalone module — its
  gate-tick logic collapses into `NPCVisitBeat::Tick`'s
  `RUNNING` arm during the Discuss stage.
- **`VisitState`'s in-memory counter fields** —
  `ignoreNudgeCount`, `consecutivePollFailures`,
  `dispatchedAtRealSeconds` (kept only if the dashboard
  still displays it) — dropped or moved into the per-beat
  record.
- **`NPCLetterAction`'s per-slot verify-delay worker
  thread** — reshaped to a counter in `NPCLetterBeat::Tick`'s
  `RUNNING` arm gating the "give up on this slot"
  transition.
- **`NPCVisitAction`'s three separate watchdog worker
  threads** (OnHold, ReEngage, Discuss) — gone. All three
  behaviors become gate-checks inside the single
  `NPCVisitBeat::Tick`.
- **`AmbushAction`'s hard-timeout branch** — reshaped to a
  counter in `AmbushBeat::Tick`.
- **The `_ne_ActionCompleted` ModEvent, C++ sink and
  Papyrus sender** — the top-level lifecycle no longer
  needs it; Papyrus fragments stop calling
  `SendModEvent` for it.
- **`ActionDispatcher::OnTick`'s stale-lock recovery** —
  per-beat `Tick` handles its own stuck-state detection
  under the same counter pattern; the top-level watchdog
  is not needed.
- **The old `IAction` interface**, including `Start`,
  `DetectAndRollbackFailedStart`, `DetectCompletion`, and
  `StartResult`.

What stays: `AsyncDispatch::MarshalToMainThread`, the beat
registry (renamed but same shape), the `IsAvailable`
contract (renamed context type, otherwise identical), every
quest and package and alias in the ESP, every LLM prompt
template, every INI-tunable setting except the handful of
watchdog-timeout ones that no longer have a consumer.

---

## Settings

New keys:

- `[BeatSystem] iBeatSystemPollIntervalMs` — master poll
  cadence. Default `250`.

Renamed keys (schema-breaking, but consistent with the
system rename):

- `iActionCooldownSeconds`         → `iBeatCooldownSeconds`
- `iActionRepetitionWindowSeconds` → `iBeatRepetitionWindowSeconds`
- `iAmbushPerActionCooldownGameHours`
  → `iAmbushPerBeatCooldownGameHours`
- `iLetterActionCooldownGameHours` → `iLetterBeatCooldownGameHours`

INI section `[Actions]` becomes `[Beats]`.

Removed keys (their consumers no longer exist):

- `iActionStaleLockTimeoutSeconds` — replaced by per-beat
  counter thresholds.
- `iVisitHardTimeoutSeconds` — the watchdog it protected
  is gone.
- Any `PollUntilOrTimeout`-consumed timeout that no longer
  has a poller.

Preserved unchanged: every other beat-related setting that
didn't carry "action" in its name, including sender
cooldowns, poll gate thresholds, dispatch verification
windows, beat-specific tuning (ambush bandit counts, visit
briefing word counts), etc.

---

## Persistence

Cosave records touched: `'NBSY'` (new top-level), plus
three new per-beat records (`'NBAM'`, `'NBLP'`, `'NBVS'`).
The old type IDs (`'NEAC'`, `'NEAB'`, `'NELP'`, `'NEVS'`)
are recognized by the new `OnLoad` handlers as pre-refactor
records and skipped, with the corresponding beat initialized
to `NOT_RUNNING` and the top-level state initialized to
`NO_BEAT_RUNNING`.

The Papyrus save data is untouched — the quests still carry
their own stage / alias / property state through the vanilla
save as they do today.

Loading a save that had an in-flight action from a
pre-refactor build silently drops that action on the floor.
This is the same behavior every prior schema bump has
taken, and the alternative (writing translation logic that
reconstructs a mid-flight state under the new schema) is
not worth the effort for a dev-branch refactor.

---

## Pipeline integration

The Director's tick continues to run on its existing
cadence (`iTickIntervalSeconds`, default 30s dev / 90s
ship). On each Director tick:

1. `EvaluationPipeline` builds the snapshot and runs the
   tension-score LLM as it does today.
2. `EvaluationPipeline` calls
   `BeatSystem::ConsiderBeat(snapshot, rec, onFinalized)`
   instead of `ActionDispatcher::ConsiderAction`.
3. `ConsiderBeat` walks its gates. The first gate now
   reads the top-level `'NBSY'` state — if it's
   `BEAT_RUNNING`, skip; if `NO_BEAT_RUNNING`, check the
   global-cooldown counter against `iBeatCooldownSeconds`.
4. If gates pass, fire the beat-select LLM with the same
   candidate-build and prompt structure as today.
5. On response, call `BeatSystem::StartBeat(name, params)`.
   That call transitions `'NBSY'` to `BEAT_RUNNING` and
   seeds the per-beat record; the master poll takes over
   from there.

The Director does not observe the running beat's progress.
It does not know about `BeatState`. It learns the beat has
ended by seeing `'NBSY'` back at `NO_BEAT_RUNNING` on its
next tick.

`DecisionLog::DecisionRecord`'s `actionSelected` /
`actionParametersJSON` fields are renamed to `beatSelected`
/ `beatParametersJSON` in the same sweep; the decision log
is a session-local ring buffer with no persistence, so no
translation is needed.

---

## Dashboard

The "current in-flight" panel currently reads from
`ActionDispatcher::GetInFlightInfo`; that accessor moves to
`BeatSystem::GetInFlightInfo` with the same shape. The
panel adds a `BeatState` display alongside the beat name —
so during a visit compose the panel shows "visit — COMPOSE";
during Discuss it shows "visit — RUNNING (stage 20)".

The dashboard tab currently labeled "Dispatch" (which
surfaces per-beat enable toggles and force-dispatch
buttons) is relabeled where it says "Action" — the tab
itself keeps the "Dispatch" label, but every "Action X"
row label becomes "Beat X". No layout changes.

---

## File map

New:

- `include/BeatSystem.h` — public API for the master poll
  driver, `ConsiderBeat`, `StartBeat`, `GetTopLevelState`,
  `GetInFlightInfo`, cosave callbacks.
- `src/BeatSystem.cpp` — the 250ms worker thread, gate
  reads, dispatch loop, top-level cosave record.
- `include/IBeat.h` — the new interface, `BeatContext`,
  `BeatPolarity`, `BeatState`, `TickMode`, `TickResult`.
- `src/BeatRegistry.cpp` — beat-registry implementation
  (renamed from `ActionRegistry`).
- `include/BeatRegistry.h` — beat-registry declaration.

Renamed:

- `include/AmbushAction.h`  → `include/AmbushBeat.h`;
  class `AmbushAction`     → `AmbushBeat`.
- `src/AmbushAction.cpp`    → `src/AmbushBeat.cpp`.
- `include/NPCLetterAction.h`
  → `include/NPCLetterBeat.h`; class
  `NPCLetterAction` → `NPCLetterBeat`.
- `src/NPCLetterAction.cpp` → `src/NPCLetterBeat.cpp`.
- `include/NPCVisitAction.h`
  → `include/NPCVisitBeat.h`; class
  `NPCVisitAction` → `NPCVisitBeat`.
- `src/NPCVisitAction.cpp` → `src/NPCVisitBeat.cpp`.

Reshaped (in place, no rename):

- `include/EvaluationPipeline.h`,
  `src/EvaluationPipeline.cpp` — redirect `ConsiderAction`
  call site to `BeatSystem::ConsiderBeat`.
- `include/DecisionLog.h` — rename
  `DecisionRecord::actionSelected` /
  `actionParametersJSON` to `beatSelected` /
  `beatParametersJSON`. Update all readers.
- `include/DashboardUIManager.h`,
  `src/DashboardUIManager.cpp` — swap in-flight query
  source, relabel action → beat everywhere in the UI.
- `include/VisitState.h` — kept as the visit beat's
  residual-data record; the counter fields and
  intermediate-async fields drop. May be moved into
  `NPCVisitBeat.cpp` outright as a private type; leave as
  a header if the dashboard still reads it, drop if not.

Deleted:

- `include/ActionDispatcher.h`, `src/ActionDispatcher.cpp`
  — every responsibility either moves to `BeatSystem` or
  disappears.
- `include/IAction.h` — replaced by `include/IBeat.h`.
- `include/ActionRegistry.h`, `src/ActionRegistry.cpp` —
  replaced by `BeatRegistry`.
- `include/VisitConclusionPoll.h`,
  `src/VisitConclusionPoll.cpp` — absorbed into
  `NPCVisitBeat::Tick`.
- `AsyncDispatch::PollUntilOrTimeout` (declaration and
  implementation) — no remaining caller.

Preserved unchanged:

- `include/AsyncDispatch.h` minus `PollUntilOrTimeout` —
  `MarshalToMainThread` and friends stay.
- `include/VisitComposer.h`, `include/LetterComposer.h` —
  LLM prompt build code unchanged; only the caller moves
  (from `Start` into `Tick`'s `COMPOSE` arm).
- All Papyrus scripts (edits limited to removing
  `SendModEvent("_ne_ActionCompleted", ...)` calls where
  they exist), all ESP forms.

---

## Implementation plan

Sequential. Each step is **entirely Claude's work (C++ /
INI / build)** or **entirely the user's work (Creation Kit

- Papyrus)**. Every step has a self-contained verification
and builds on the previous ones. After every step the
plugin builds cleanly and boots; steps that reshape a beat
also require an in-game force-dispatch pass.

Step order:

1. **Neutralize existing beat sources** — the three actions
   are removed from `ActionRegistry::Register` at plugin
   init so no beat can be dispatched. The action `.cpp`
   files themselves stay live (their utility namespaces —
   `NPCLetterAction_QuestControl`, `NPCLetterAction_Cooldowns`,
   `NPCVisitAction_Cooldowns` — have external callers in
   `LetterPool.cpp` and `VisitComposer.cpp` and can't be
   wrapped without breaking the link). The old files get
   deleted when each beat is rewritten in Steps 8, 10,
   and 11.
2. Settings + INI rename.
3. `IBeat` interface + `BeatRegistry` skeleton.
4. `BeatSystem` module — master poll worker thread.
5. `BeatSystem` cosave layer.
6. `EvaluationPipeline` redirect + `DecisionLog` rename.
7. Dashboard rename + query-source swap.
8. Reshape and rename Ambush → `AmbushBeat`.
9. `BeatSystem::ConsiderBeat` — full beat-select LLM
   handshake. Unblocks Director-normal-cadence dispatch
   for every registered beat.
10. Reshape and rename NPC Letter → `NPCLetterBeat`.
11. Reshape and rename NPC Visit → `NPCVisitBeat`.
12. Delete `ActionDispatcher`, `IAction`, `ActionRegistry`,
    `PollUntilOrTimeout`. (Deferred until every beat has
    been rewritten, at which point nothing references
    them.)
13. Papyrus fragment sweep — remove
    `SendModEvent("_ne_ActionCompleted", ...)` calls.

**Note on step reordering.** The initial plan put the old-
module deletion at Step 8 (before the beat rewrites) and
assumed the action `.cpp` files could be wrapped in
`#if 0` at Step 1. That doesn't work: `LetterPool.cpp`
depends on `NPCLetterAction_QuestControl` /
`_Cooldowns`, and `VisitComposer.cpp` depends on
`NPCVisitAction_Cooldowns`. Deferring the deletion until
after the beat rewrites (where each old file is deleted
by its replacement) resolves this without any extra
surgery on `LetterPool` or `VisitComposer`.

**Note on the LLM handshake (Step 9).** Step 6 wired
`EvaluationPipeline` to call `BeatSystem::ConsiderBeat`,
but the beat-select LLM round trip inside `ConsiderBeat`
was left as a TODO. That meant Ambush (registered at
Step 8) was reachable via dashboard force-dispatch but
not via Director-normal-cadence dispatch. Step 9 ports
the LLM handshake in — after which every registered beat
becomes fully reachable end-to-end and the Letter/Visit
rewrites in Steps 10–11 land onto a working dispatch
pipeline.

---

### Step 1 — Neutralize existing beat sources

- [x] Complete

**[CLAUDE]**

**Goal:** Take the three existing `IAction` implementations
out of the dispatch pool so no beat can fire, and disable
their init hooks that reach into CK content. The `.cpp`
files stay live because their utility namespaces have
external callers (see the reordering note above).

**Files:**

- `src/Plugin.cpp` — comment out the three
  `ActionRegistry::Register(...)` calls at `kDataLoaded`,
  plus `NPCVisitAction_Init::Initialize()` and
  `NPCLetterAction_Init::Initialize()`. Leave a
  `// PHASE-06: temporarily disabled; restored in
  Step 9/10/11` comment above each so the reason is
  obvious to a future reader.

Files intentionally NOT touched:

- `src/AmbushAction.cpp`, `src/NPCLetterAction.cpp`,
  `src/NPCVisitAction.cpp`, `src/VisitConclusionPoll.cpp` —
  left intact. The classes exist and compile, but nothing
  instantiates them. Their utility namespaces continue to
  serve `LetterPool.cpp` and `VisitComposer.cpp`.
- Every corresponding header — untouched.
- Cosave `OnSave` / `OnLoad` / `OnRevert` calls in
  `Plugin.cpp` for `AmbushAction_Persistence` /
  `NPCLetterAction_Persistence` /
  `NPCVisitAction_Persistence` — left in place. The
  records will remain empty (no beat is running), and
  the skip handlers from Step 5 will not conflict
  because these handlers reference the *pre-refactor*
  record IDs directly.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- The build emits an `unused-includes` warning for
  `<memory>` in `Plugin.cpp` — expected; Step 3 restores
  the include's use.
- On boot the `ActionRegistry: registered '{}'` info
  line does not fire for any action. `ActionRegistry`'s
  internal state stays empty.
- Wait past the Director's `iTickIntervalSeconds`
  interval; log shows the action-select gate skipping
  with an empty candidate list.

---

### Step 2 — Settings + INI rename

- [x] Complete

**[CLAUDE]**

**Goal:** Rename every setting whose name carried "Action"
in the Narrative Engine's sense, add the new
`iBeatSystemPollIntervalMs` key, and drop the two settings
whose consumers are being removed. Preserve every other
setting.

**Files:**

- `include/Settings.h` — struct field renames.
- `src/Settings.cpp` — INI read renames; new key read;
  removals.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — section
  rename, key renames, new key, removals, comment
  updates. (This file is under `statics/`; per project
  convention it only reaches the runtime mod folder via
  `build.ps1`, so a rebuild is required to see effects
  in-game.)

**Sub-tasks:**

1. In `Settings.h`, rename the affected struct fields per
   the naming table in **Naming: from Actions to
   Narrative Beats**:
   - `actionCooldownSeconds` → `beatCooldownSeconds`
   - `actionRepetitionWindowSeconds` →
     `beatRepetitionWindowSeconds`
   - `ambushPerActionCooldownGameHours` →
     `ambushPerBeatCooldownGameHours`
   - `letterActionCooldownGameHours` →
     `letterBeatCooldownGameHours`
2. `actionStaleLockTimeoutSeconds` and
   `visitHardTimeoutSeconds` are **retained transitionally**:
   `actionStaleLockTimeoutSeconds` is still read by
   `ActionDispatcher` (deleted in Step 12);
   `visitHardTimeoutSeconds` is still read by
   `NPCVisitAction` (rewritten in Step 11). Both are
   removed at their consumer's death point. Marked with
   `// TODO PHASE-06` comments in `Settings.h` /
   `Settings.cpp` / `NarrativeEngine.ini`.
3. Add `int beatSystemPollIntervalMs = 250;` under a new
   `// [BeatSystem]` grouping comment.
4. Update `Settings.cpp` INI reads to match — `Get...`
   calls for each renamed key move to the new `[Beats]`
   section (or the new `[BeatSystem]` section for the
   poll interval), reading the new key names.
5. In `NarrativeEngine.ini`:
   - Rename `[Actions]` → `[Beats]`.
   - Rename the four keys.
   - Add `[BeatSystem]` section with
     `iBeatSystemPollIntervalMs=250` and an inline
     comment describing what it controls.
   - `iActionStaleLockTimeoutSeconds` and
     `iVisitHardTimeoutSeconds` retained with transitional
     comment (see sub-task 2).
   - Update inline comments that mention "action" in the
     Narrative Engine sense to say "beat" instead.
6. Sweep readers of the renamed fields elsewhere in the
   codebase (`ActionDispatcher.cpp`, `AmbushAction.cpp`,
   `NPCLetterAction.cpp`) to the new field names.

**Specifics:**

- Comments in `Settings.h` for the removed fields should
  be deleted along with the fields; leaving orphaned
  comments is confusing.
- The new `iBeatSystemPollIntervalMs` comment should note
  it drives the `BeatSystem` master poll worker thread's
  sleep interval; default 250, unit ms.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot Skyrim; the existing "Settings loaded from…" log
  line shows the new field names with the configured
  values. Bump `iBeatCooldownSeconds` via the INI,
  rebuild, reload — confirm the log reflects the change.

---

### Step 3 — `IBeat` interface + `BeatRegistry` skeleton

- [x] Complete

**[CLAUDE]**

**Goal:** Land the new interface and its registry as inert
headers/source. Nothing calls into them yet; they exist
only to be extended by Step 4 and consumed by Steps 8, 10,
and 11.

**Files:**

- `include/IBeat.h` — new. `BeatContext`, `BeatPolarity`,
  `BeatState`, `TickMode`, `TickResult`, `IBeat`.
- `include/BeatRegistry.h` — new. Registration and lookup
  surface.
- `src/BeatRegistry.cpp` — new. Vector of registered
  beats; `Initialize()` as a no-op (Steps 8, 10, 11 add
  registrations); `LookupByName`; `EnumerateAvailable`.
- `src/Plugin.cpp` — wire `BeatRegistry::Initialize()`
  into the existing `kDataLoaded` handler *alongside* the
  current `ActionRegistry::Initialize()` (not replacing).

**Sub-tasks:**

1. Author `IBeat.h` matching the shape shown in **The
   `IBeat` contract** section above.
2. Author `BeatRegistry.h`/`.cpp` — same registration
   pattern as the current `ActionRegistry` (a static
   vector guarded by the plugin lifecycle), just with the
   new element type.
3. `BeatRegistry::Initialize()` body: log
   `BeatRegistry: registered N beats` where N is the
   registered count (0 at this stage).
4. Wire the init call.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot Skyrim; log shows both
  `ActionRegistry: registered 0 actions` and
  `BeatRegistry: registered 0 beats` at `kDataLoaded`.

---

### Step 4 — `BeatSystem` module + master poll

- [x] Complete

**[CLAUDE]**

**Goal:** Stand up the `BeatSystem` module and get its
250ms master poll worker thread running. Implements the
in-memory shell of the two-state top-level machine and
the gate detection. Cosave persistence and Director
wiring come in Steps 5 and 6.

**Files:**

- `include/BeatSystem.h` — public API.
- `src/BeatSystem.cpp` — implementation.
- `src/Plugin.cpp` — call `BeatSystem::Initialize()` at
  `kDataLoaded` after `BeatRegistry::Initialize()`; call
  `BeatSystem::Shutdown()` at plugin unload / game exit.

**Sub-tasks:**

1. Declare `BeatSystem::Initialize()`, `Shutdown()`,
   `GetTopLevelState()`, `GetRunningBeatName()`,
   `GetGlobalCooldownMs()`. `ConsiderBeat` and
   `StartBeat` are declared but implemented in Step 6.
2. In `.cpp`, implement the master poll worker thread:
   spawn a `std::thread` at `Initialize`; the thread
   loops on a `sleep_for(kInterval)` with `kInterval =
   Settings::Get().beatSystemPollIntervalMs`. Loop exits
   on a `std::atomic<bool> stopRequested` set by
   `Shutdown`.
3. Each iteration builds a `TickMode` from the three
   gates (`RE::UI::GetSingleton()->GameIsPaused()`,
   `PlayerCharacter::GetSingleton()->IsInCombat()`,
   dialogue-menu-open). Compute mode precedence:
   `Paused` > `Combat` > `Dialogue` > `Normal`.
4. Read the top-level state (mutex-guarded). If
   `NO_BEAT_RUNNING` and `mode == Normal`, add
   `kInterval` to the cooldown counter (clamped to some
   ceiling like `UINT32_MAX`). If `BEAT_RUNNING`, look
   up the beat by name in the registry, call
   `beat->Tick(ctx, mode, state)`. Apply any
   `TickResult::transitionTo`. If the transition lands
   on `NOT_RUNNING`, clear the running-beat name and
   reset the cooldown counter to zero.
5. Emit a heartbeat log line every 40 ticks (~10s at
   250ms) summarizing state, mode, and cooldown counter.
   Guard this behind `Settings::Get().debugMode`.
6. `Shutdown` sets `stopRequested` and joins the thread.

**Specifics:**

- The worker thread must not read engine state that
  isn't known-safe off-thread. The three gates above use
  stable singletons plus bool reads; that's fine. When
  `Tick` needs anything else (positions, quest stages,
  etc.), the beat's own implementation is responsible
  for marshaling via
  `AsyncDispatch::MarshalToMainThread`.
- The top-level state is protected by a mutex; every
  read and write inside the poll loop takes the lock.
- No cosave persistence yet — top-level state is in
  process memory only. Step 5 wires cosave.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot with `bDebugMode=1`; log shows the master-poll
  heartbeat every ~10s reporting
  `state=NO_BEAT_RUNNING mode=Normal cooldown=<ms>`.
- Pause the game (open the menu); next heartbeat shows
  `mode=Paused` and `cooldown` frozen. Unpause; cooldown
  resumes.

---

### Step 5 — `BeatSystem` cosave layer

- [x] Complete

**[CLAUDE]**

**Goal:** Add cosave persistence for the top-level state
under `'NBSY'`, and install the recognized-and-skipped
handlers for the four pre-refactor record IDs.

**Files:**

- `include/BeatSystem.h` — add `OnSave`, `OnLoad`,
  `OnRevert`.
- `src/BeatSystem.cpp` — implement.
- `src/Plugin.cpp` — register `'NBSY'` with the SKSE
  serialization interface; register the four pre-refactor
  IDs (`'NEAC'`, `'NEAB'`, `'NELP'`, `'NEVS'`) with skip
  handlers that read length bytes and discard.

**Sub-tasks:**

1. `BeatSystem::OnSave` — write version byte, top-level
   state enum, running-beat name (empty if none),
   cooldown counter.
2. `BeatSystem::OnLoad(intfc, version, length)` — read
   the version, top-level state, running-beat name,
   cooldown counter under the mutex. If the running-beat
   name is set but doesn't resolve in the current
   `BeatRegistry`, log a warning and reset to
   `NO_BEAT_RUNNING` (defensive — should never happen in
   practice but survives a beat being removed between
   builds).
3. `BeatSystem::OnRevert` — reset to `NO_BEAT_RUNNING`,
   empty name, zero cooldown.
4. Skip handlers for the four old IDs: each reads the
   record's length bytes from the serialization interface
   into a scratch buffer and discards, then logs a
   one-line `"pre-refactor record 'XXXX' found at load;
   discarding"`.

**Specifics:**

- Cosave record IDs are declared as
  `inline constexpr std::uint32_t kRecordTypeId = 'NBSY';`
  in `BeatSystem.h`, matching the pattern the existing
  `VisitState`/`ActionDispatcher` records use.
- Version byte starts at `1`. `OnLoad` treats any other
  version as an error and resets to `NO_BEAT_RUNNING`
  after logging.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot Skyrim; make a save; log shows `'NBSY'` written.
- Load the save; log shows `'NBSY'` read with the
  expected state.
- Load a save made in Phase 05 (pre-refactor); log shows
  `pre-refactor record 'NEAC'/'NEAB'/'NELP'/'NEVS' found
  at load; discarding` for each present record; the
  plugin continues to boot cleanly.

---

### Step 6 — `EvaluationPipeline` redirect + `DecisionLog` rename

- [x] Complete

**[CLAUDE]**

**Goal:** Redirect the Director's beat-select handshake
from `ActionDispatcher::ConsiderAction` to
`BeatSystem::ConsiderBeat`; rename the two
`DecisionRecord` fields that carried "action" in their
name.

**Files:**

- `include/DecisionLog.h` — rename `actionSelected` →
  `beatSelected`, `actionParametersJSON` →
  `beatParametersJSON`.
- `src/DecisionLog.cpp` — sweep readers and writers.
- `include/BeatSystem.h` — declare `ConsiderBeat` and
  `StartBeat`.
- `src/BeatSystem.cpp` — implement.
- `include/EvaluationPipeline.h`,
  `src/EvaluationPipeline.cpp` — redirect the
  `ConsiderAction` call site to `ConsiderBeat`.
- Any other consumer of the renamed `DecisionRecord`
  fields (dashboard state builder, decision-log tail
  formatter, LLM prompt context builder).

**Sub-tasks:**

1. Rename `DecisionRecord` fields; sweep all readers.
2. Author `BeatSystem::ConsiderBeat(snapshot, rec,
   onFinalized)`. Gate walk: (a) top-level state is
   `BEAT_RUNNING` → skip; (b) global cooldown counter
   `< iBeatCooldownSeconds * 1000` → skip; (c)
   `BeatRegistry::EnumerateAvailable(ctx)` yields
   zero → skip.
3. If the gates pass, fire the beat-select LLM call
   (same prompt template, same candidate-building
   pattern as the current `ActionDispatcher` uses). On
   response, call `StartBeat(name, params)`.
4. Author `BeatSystem::StartBeat(name, params)` — main
   thread. Sets top-level state to `BEAT_RUNNING`, sets
   `runningBeatName = name`, calls
   `registeredBeat->OnStart(ctx, params)` so the beat
   can seed its own per-beat cosave record from the
   LLM-supplied params, and sets the beat's state to
   `COMPOSE`. Returns.
5. `EvaluationPipeline` change — replace the single
   `ActionDispatcher::ConsiderAction` call with
   `BeatSystem::ConsiderBeat`. Preserve the
   `onFinalized` signature and semantics.

**Specifics:**

- The candidate-list build inside `ConsiderBeat` reuses
  the same LLM prompt template and the same
  `SkyrimNetApi.SendCustomPromptToLLM` call the current
  `ActionDispatcher` makes; only the caller module
  changes. If the code lives inside `ActionDispatcher.cpp`
  today, move the relevant helper into `BeatSystem.cpp`
  (or a small shared helper in
  `include/BeatSelectHelpers.h` if it's more than a few
  lines).
- The old `ActionDispatcher::ConsiderAction` code path
  stops being reachable at this step; the module and its
  file get deleted in Step 8.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot with `bDebugMode=1`; wait for the Director's
  `iTickIntervalSeconds` interval; log shows
  `BeatSystem::ConsiderBeat` running the gate walk and
  skipping with `"no candidates"` (registry is still
  empty until Step 9).

---

### Step 7 — Dashboard rename + query-source swap

- [x] Complete

**[CLAUDE]**

**Goal:** Move the dashboard's in-flight query source from
`ActionDispatcher` to `BeatSystem`; relabel every "Action"
surface to "Beat"; add a `BeatState` display next to the
beat name.

**Files:**

- `include/DashboardUIManager.h`,
  `src/DashboardUIManager.cpp`.
- Any dashboard state-builder / tab-model source file
  that pulls in-flight info or per-action state.

**Sub-tasks:**

1. Replace `ActionDispatcher::GetInFlightInfo()` calls
   with `BeatSystem::GetInFlightInfo()`, which returns
   the same `{name, startedAt}` shape plus the current
   `BeatState`.
2. Sweep the dashboard's UI strings — every "Action"
   label becomes "Beat" (tab labels, panel headings,
   status text). Keep short forms terse ("Beat" not
   "Narrative Beat" in space-constrained UI).
3. Add a `BeatState` display next to the running-beat
   name in the in-flight panel — e.g. `"visit — COMPOSE"`
   or `"visit — RUNNING (stage 20)"`.
4. Update the Dispatch tab's per-beat enable toggles and
   force-dispatch buttons to display beat names (empty
   list at this stage; registrations come back in
   Steps 8, 10, 11).

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot; open dashboard; every tab label reads "Beat"
  where it used to read "Action". In-flight panel shows
  "no beat running". Dispatch tab shows zero registered
  beats.

---

### Step 8 — Reshape and rename Ambush → `AmbushBeat`

- [x] Complete

**[CLAUDE]**

**Goal:** Reshape the ambush action onto the `IBeat`
contract. Ambush is the simplest of the three
lifecycles — no compose LLM call, no follow-up state
machine, just "spawn some bandits at a marker and let
combat play out."

**Files:**

- `include/AmbushAction.h` → `include/AmbushBeat.h`;
  class rename; interface swap.
- `src/AmbushAction.cpp` → `src/AmbushBeat.cpp`; unwrap
  the `#if 0`; rewrite for the new contract.
- `src/BeatRegistry.cpp` — add
  `Register(std::make_unique<AmbushBeat>())` in
  `Initialize()`.
- `src/Plugin.cpp` — register `'NBAM'` cosave record.
- CMake file list — swap old filename for new.

**Sub-tasks:**

1. Rename files. Class rename. Interface swap: inherit
   from `IBeat`; implement `Name`, `Description`,
   `Polarity`, `IsAvailable`, `Tick`, `OnStart`,
   `RemainingCooldownGameHours`.
2. `OnStart(ctx, params)` — validate + clamp the LLM
   params (bandit_count, spawn_distance_units) against
   the existing settings-defined ranges; write the
   resolved values into a private `AmbushBeat`
   per-instance struct; write per-beat cosave state to
   `COMPOSE` via a small `SetState` helper.
3. `Tick(ctx, mode, state)` implementation:
   - `COMPOSE` arm — marshals to main thread to spawn
     the bandit cluster (via the existing quest / alias
     mechanism the old `Start()` used); on success,
     `transitionTo = RUNNING`; on failure,
     `transitionTo = CLEANUP` with a `failure_reason`
     written to cosave.
   - `RUNNING` arm — read the ambush quest's stage each
     poll; when it hits the terminal stage (quest
     complete / all bandits dead / player fled),
     `transitionTo = CLEANUP`.
   - `CLEANUP` arm — tear down: stop the quest, clear
     alias fills. `transitionTo = NOT_RUNNING`.
     `NOT_RUNNING` is where the master poll's top-level
     transition triggers.
4. Cosave record `'NBAM'` — schema and OnLoad/OnSave.
   Persist `BeatState`, per-beat cooldown expiry (in
   game hours), any residual data the beat needs across
   a save/reload (e.g. the resolved parameter values so
   the reload doesn't need to re-clamp).
5. Under `Paused` / `Combat` / `Dialogue` mode, `Tick`
   returns `{}` (no transition, no work) — ambush has no
   combat-stuck detection or paused-behavior of its own.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot; open dashboard; Dispatch tab shows `ambush` as a
  registered beat.
- Click "Force Dispatch → ambush"; bandits spawn at the
  correct distance; the in-flight panel shows
  `ambush — COMPOSE` briefly, then `ambush — RUNNING`,
  then clears when the encounter resolves.
- Save mid-encounter; reload; the encounter continues
  cleanly.
- Confirm the per-beat cooldown persists across a save
  cycle (dashboard shows the cooldown counter running).

---

### Step 9 — `BeatSystem::ConsiderBeat` — full beat-select LLM handshake

- [x] Complete

**[CLAUDE]**

**Goal:** Replace the placeholder "candidate path not
implemented yet — skipping" branch in `ConsiderBeat` with
the full beat-select LLM round trip. After this step,
every registered beat is reachable via both dashboard
force-dispatch AND Director-normal-cadence dispatch. This
unblocks Steps 10 and 11 (letter, visit) from having to
add their own dispatch plumbing.

The logic ported here mirrors what `ActionDispatcher::ConsiderAction`
does today — the difference is the interface (`IBeat` /
`BeatContext` / `BeatRegistry` / `BeatSystem::StartBeat`)
and the top-level state model (BeatSystem's cosave record
replaces ActionDispatcher's `g_actionInFlight`). The prompt
template name (`narrative_engine_action_select`) is
preserved as-is — the SkyrimNet-side rename is out of scope
here.

**Files:**

- `include/BeatSystem.h` — declare
  `ForceDispatchBeat(std::string_view name)` for the
  dashboard's force-dispatch path (bypasses the LLM
  candidate build but still runs prompt + response through
  the same LLM to satisfy validation and land parameters
  on the beat's OnStart).
- `src/BeatSystem.cpp` — flesh out `ConsiderBeat`. Add
  helper functions for the ported logic:
  - `BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta)`
    — main-thread ctx builder, analogous to
    `BuildActionContextFromSnapshot`.
  - `CheckGlobalBeatPreconditions(ctx)` — port of
    `CheckGlobalActionPreconditions` (combat / dialogue /
    scripted scene / DND cell gates).
  - `BuildBeatSelectPromptContext(...)` — port of
    `BuildActionPromptContext`; assembles candidates,
    per-candidate letter/visit sender lists, player
    context, recent-events tail.
  - `TrimRecentlyFiredLocked(now)` and
    `WasFiredRecentlyLocked(name, now)` — port of the
    anti-repetition ring. Owned by `BeatSystem` (its own
    mutex + deque; not shared with ActionDispatcher).
  - `FinalizeWithFailure(rec, reason, onFinalized)` — port
    of the failure-path helper; stamps
    `rec.beatSelected = "(failed: <reason>)"`, applies
    global cooldown (resets `g_globalCooldownMs = 0`),
    calls ApplyDecision + onFinalized.
  - `FinalizeWithLLMResponse(snapshot, rec, candidateNames,
    chosenBeat, parameters, narrativeNote,
    parameterJustification, direction, tensionDelta,
    onFinalized)` — port of the LLM-response finalizer.
    Validates name, re-checks global preconditions,
    populates rec, calls `StartBeat(chosenBeat,
    parameters)`.
- `include/EvaluationPipeline.h` — no changes; the
  `ApplyDecision` accessor was already used from Step 6.
- `src/DashboardUIManager.cpp` — swap the force-dispatch
  handler from `BeatSystem::StartBeat(name, {})` to
  `BeatSystem::ForceDispatchBeat(name)` so force-dispatch
  runs through the same LLM validation path (this matches
  what the old `ForceDispatchAction` did — single-element
  candidate list, LLM invoked, response validated, then
  StartBeat).

**Sub-tasks:**

1. Add the anti-repetition ring state to `BeatSystem.cpp`
   (mutex + `std::deque<RecentlyFired>`). Cap and window
   sourced from `Settings::Get().beatRepetitionWindowSeconds`.
2. Author `BuildBeatContextFromSnapshot` — copy from
   `ActionDispatcher::BuildActionContextFromSnapshot`,
   swap `ActionContext` → `BeatContext`.
3. Author `CheckGlobalBeatPreconditions` — port verbatim.
4. Author `BuildBeatSelectPromptContext` — port
   `BuildActionPromptContext`. Preserve the special-case
   letter/visit sender-candidate collection (the branch
   guarded on `a->Name() == "npc_letter"` /
   `"npc_visit"`). Note: at Step 9 completion, only
   `ambush` is registered — the letter/visit special-
   cases sit dormant until Steps 10/11 land the beats.
5. In `ConsiderBeat`, after Gate 5 (candidates
   non-empty), continue with:
   - Anti-repetition filter pass on candidates.
   - Sender-candidate pre-collection for letter/visit
     beats.
   - Assemble prompt context via
     `BuildBeatSelectPromptContext`.
   - Fire `SkyrimNetAPI::SendCustomPromptToLLM`
     (`narrative_engine_action_select` / `narrative_engine_director`).
   - Callback: parse the response on the SkyrimNet
     thread, marshal to main thread, call
     `FinalizeWithLLMResponse`.
   - Handle the `!queued` failure path with
     `FinalizeWithFailure`.
6. Author `FinalizeWithLLMResponse`. Key differences from
   the old ActionDispatcher version:
   - Chosen-beat lookup via `BeatRegistry::Find`.
   - No `action->Start(ctx, parameters)` call — instead,
     hand off to `BeatSystem::StartBeat(chosenBeat,
     parameters)`. StartBeat's OnStart + top-level state
     transition replace the old Start's role.
   - After a successful StartBeat, push to the recently-
     fired ring and call `ApplyDecision(rec)` +
     `onFinalized`.
7. Author `ForceDispatchBeat(name)` — mirror the
   ActionDispatcher force-dispatch shape: build a
   one-element candidate list, bypass every gate, fire the
   LLM with the single-candidate prompt so parameter
   validation still runs through the same code path, then
   land on `FinalizeWithLLMResponse`. Refuses cleanly if
   another beat is in flight.
8. Swap `DashboardUIManager::OnDispatchAction` to call
   `ForceDispatchBeat` instead of `StartBeat({})`.

**Specifics:**

- The prompt template stays
  `narrative_engine_action_select` — its input schema is
  driven by the same JSON we're building. A SkyrimNet-side
  rename to `narrative_engine_beat_select` is deferred
  and would just be a file rename plus one string
  constant flip here.
- The recently-fired ring lives inside `BeatSystem` and
  is session-only (not persisted). Same shape as
  `ActionDispatcher`'s ring; the cap is the same
  `kRecentlyFiredCap = 32`.
- The global cooldown counter (`g_globalCooldownMs`)
  is what `ConsiderBeat`'s Gate 3 already reads. Landing
  in `StartBeat` sets it to 0 (reset), and it ticks up
  again while `NO_BEAT_RUNNING`. Nothing new to wire.
- `FinalizeWithFailure` mirrors what the old dispatcher
  does — writes `rec.beatSelected = "(failed: <reason>)"`
  and applies the cooldown gate as if the beat had
  completed. The design intent is "we tried to fire, so
  wait the normal cooldown before trying again."
- The Ambush registration from Step 8 stays intact; this
  step just makes it reachable via the Director cadence
  in addition to force-dispatch.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Boot Skyrim; observe an ambient wilderness area.
- Wait for the Director's tension-eval to fire (30s
  intervals at dev settings). When the phase gate is
  open and the environment is ambush-viable, log shows
  `BeatSystem::ConsiderBeat` firing the LLM, receiving
  a response, calling `StartBeat("ambush", ...)`, and
  the beat runs end-to-end.
- Dashboard's "Recent Decisions" table now shows
  populated `beatSelected` values instead of empty.
- Dashboard's "Dispatch → ambush" button still works
  (routes through `ForceDispatchBeat`, LLM validates the
  single candidate, ambush fires).
- Trigger two ambushes in short succession by rewinding
  cooldowns from the console; confirm the anti-
  repetition window blocks the second selection with a
  logged "gate recently_fired" line.

---

### Step 10 — Reshape and rename NPC Letter → `NPCLetterBeat`

- [x] Complete

**[CLAUDE]**

**Goal:** Reshape the letter action onto the `IBeat`
contract. Letter has a compose LLM call and a
single-slot courier-dispatch flow; medium complexity.

**Files:**

- `include/NPCLetterAction.h` → `include/NPCLetterBeat.h`.
- `src/NPCLetterAction.cpp` → `src/NPCLetterBeat.cpp`;
  unwrap; rewrite.
- `src/BeatRegistry.cpp` — register `NPCLetterBeat`.
- `src/Plugin.cpp` — register `'NBLP'` cosave record.
- CMake.
- `include/LetterComposer.h`, `src/LetterComposer.cpp` —
  no interface change; called from `Tick`'s `COMPOSE`
  arm instead of from `Start`.

**Sub-tasks:**

1. Rename files. Class rename. Interface swap.
2. `OnStart` — validate params; write per-beat state
   `COMPOSE`; nothing else.
3. `Tick`:
   - `COMPOSE` arm — if no compose LLM is in flight this
     session, fire it via `LetterComposer`. On response
     (via `AsyncDispatch::MarshalToMainThread` from the
     callback), write the composed text / topic / mood
     / tags to the cosave record; look up the pool slot
     for dispatch; promote the sender via the marker
     faction; write the letter body to the book; start
     the courier quest. On success,
     `transitionTo = RUNNING`. On any failure
     (bad-sender, LLM parse error, pool empty),
     `transitionTo = CLEANUP` with `failure_reason`.
   - `RUNNING` arm — poll the letter's pool-slot state.
     A verify-delay counter ticks each `Normal` poll;
     when it exceeds
     `iLetterDispatchVerifyDelaySeconds * 1000`,
     confirm the courier is holding the letter; if not,
     `transitionTo = CLEANUP` with `failure_reason =
     dispatch_verify_failed`. When the courier's
     delivery ModEvent fires (still Papyrus-driven),
     the beat detects it via a small delivery-flag
     field on the cosave record (written from the
     ModEvent sink, read by `Tick`) and
     `transitionTo = CLEANUP` with success.
   - `CLEANUP` arm — clear the pool slot back to
     `Available`; write per-beat + per-sender
     cooldowns. `transitionTo = NOT_RUNNING`.
4. Cosave `'NBLP'` — persist `BeatState`, composed
   letter body / topic / mood / tags, sender FormID,
   pool-slot index, per-beat cooldown expiry map,
   per-sender cooldown expiry map, delivery flag.
5. Restart-on-reload: `COMPOSE` reload with no composed
   text stored → re-fire compose LLM. `COMPOSE` reload
   with composed text stored → skip directly to
   slot-promotion. `RUNNING` reload → resume verify
   counter from zero (the counter is not persisted).

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Force-dispatch a letter; the compose LLM fires; the
  courier delivers; dashboard shows the state
  transitions.
- Save mid-`COMPOSE` (before the LLM lands); reload; the
  compose call re-fires from scratch.
- Save mid-`RUNNING` (after the courier is dispatched
  but before delivery); reload; the beat continues to
  wait for delivery cleanly.
- Confirm both the per-beat and per-sender cooldowns
  persist across saves.

---

### Step 11 — Reshape and rename NPC Visit → `NPCVisitBeat`

- [x] Complete

**[CLAUDE]**

**Goal:** Reshape the visit action onto the `IBeat`
contract. The largest of the three lifecycles — full
Salutation → Discuss → Valediction → ReturnHome state
machine, plus the natural-conclusion poll gate absorbed
from `VisitConclusionPoll`, plus OnHold / ReEngage
detours during combat.

**Files:**

- `include/NPCVisitAction.h` → `include/NPCVisitBeat.h`.
- `src/NPCVisitAction.cpp` → `src/NPCVisitBeat.cpp`;
  unwrap; rewrite.
- `include/VisitState.h`, `src/VisitState.cpp` — retire
  as a standalone module; move the residual-data struct
  into `NPCVisitBeat.cpp` as a private type used only by
  the beat, or keep the header if the dashboard is still
  reading it. Prefer folding into the beat unless a
  separate reader survives.
- `include/VisitComposer.h`, `src/VisitComposer.cpp` —
  called from `Tick`'s `COMPOSE` arm; no interface
  change.
- `src/BeatRegistry.cpp` — register `NPCVisitBeat`.
- `src/Plugin.cpp` — register `'NBVS'` cosave record;
  drop the old `'NEVS'` explicit handler (the skip
  handler from Step 5 covers it).
- CMake.

**Sub-tasks:**

1. Rename files. Class rename. Interface swap.
2. `OnStart` — validate params; write per-beat state
   `COMPOSE`.
3. `Tick`:
   - `COMPOSE` arm — same shape as letter's compose
     arm. Fire `VisitComposer`; on response write
     briefing / topic / mood / tags to cosave, snapshot
     the sender's pre-dispatch position + cell, promote
     the sender via the marker faction, start
     `_ne_VisitQuest`. `transitionTo = RUNNING`.
   - `RUNNING` arm — dispatch by quest stage:
     - Stage 10 (Salutation): tick approach-timeout
       counter each `Normal` poll; if it exceeds
       `iVisitApproachTimeoutSeconds`, roll back to
       `CLEANUP` with `failure_reason =
       approach_timeout`; if sender-player distance
       drops below
       `iVisitSalutationApproachDistanceUnits`, fire
       the Salutation `ExecuteAction` on the sender and
       `SetStage(20)`.
     - Stage 20 (Discuss): tick the three poll-gate
       counters (turns-since-poll, silence-since-turn,
       game-minutes-since-poll). When any gate trips
       under `Normal` mode, fire the natural-conclusion
       poll (marshaling as needed). On response, either
       advance to Valediction or continue. Ignore-nudge
       counter is per-session, not persisted (matches
       the pre-refactor intent for that counter).
     - Stage 25 (OnHold): entered when
       `mode == Combat` was hit during Discuss and
       combat persists past the threshold. Tick the
       combat-stuck counter under `Combat` mode; if it
       exceeds `iVisitOnHoldCombatMaxSeconds`, roll to
       `CLEANUP` with `failure_reason = combat_stuck`.
     - Stage 27 (ReEngage): tick approach counter
       toward `iVisitReEngageApproachDistanceUnits`.
     - Stage 30 (Valediction): tick valediction-dwell
       counter; when it exceeds
       `iVisitValedictionDwellSeconds`, `SetStage(50)`.
     - Stage 50 (ReturnHome): tick return-timeout
       counter; if sender-player distance exceeds
       `iVisitReturnHomeExitDistanceUnits`, or cell
       unloads, or counter exceeds
       `iVisitReturnHomeTimeoutSeconds`, teleport the
       sender to their pre-dispatch position + cell,
       release the marker faction, and
       `transitionTo = CLEANUP`.
     - Stage 60 (Rolled back — Salutation timeout):
       terminal quest state; `transitionTo = CLEANUP`.
     - Stage 200 (Hard abort): terminal quest state;
       `transitionTo = CLEANUP`.
   - `CLEANUP` arm — stop the quest, clear cosave
     residuals, write per-beat + per-sender cooldowns
     (only if the terminal outcome warrants them per the
     existing rules). `transitionTo = NOT_RUNNING`.
   - `Paused` / `Dialogue` mode: no counter advance, no
     transitions.
   - `Combat` mode: only the combat-stuck counter
     (Stage 25) advances; all other counters freeze.
4. Cosave `'NBVS'` — persist `BeatState`, briefing /
   topic / mood / tags, sender FormID, return-teleport
   pose (position + angleZ + cell FormID + anchor
   FormID), per-beat cooldown expiry, per-sender
   cooldown expiry map.
5. Persist history-ring for dashboard — same shape as
   pre-refactor; on-save write, on-load read, in-memory
   append during `CLEANUP`.
6. Delete `VisitConclusionPoll.h/.cpp` residues (Step 8
   already removed them; this step just ensures nothing
   references them post-rewrite).

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Force-dispatch a visit; the compose LLM fires; sender
  warps; Salutation line plays; Discuss begins; wait for
  the natural-conclusion poll to conclude; sender walks
  away and teleports home.
- Dashboard shows the state transitions live:
  `visit — COMPOSE` → `visit — RUNNING (stage 10)` →
  `... (stage 20)` → `... (stage 30)` →
  `... (stage 50)` → cleared.
- Save mid-`COMPOSE`; reload; compose re-fires.
- Save mid-Discuss (stage 20); reload; the beat resumes
  waiting for the natural-conclusion poll gates from
  zero-count (matching the restart-on-reload principle).
- Enter combat during Discuss; the beat transitions to
  Stage 25 (OnHold) and combat-stuck counter starts
  ticking; end combat quickly and the beat returns to
  Discuss cleanly.
- Confirm the per-sender cooldown persists across a save
  cycle.

---

### Step 12 — Delete old modules

- [x] Complete

**[CLAUDE]**

**Goal:** Physically remove every module the refactor
made unreachable.

**Files:**

- `include/ActionDispatcher.h`,
  `src/ActionDispatcher.cpp` — delete.
- `include/ActionRegistry.h`,
  `src/ActionRegistry.cpp` — delete.
- `include/IAction.h` — delete.
- `include/VisitConclusionPoll.h`,
  `src/VisitConclusionPoll.cpp` — delete (absorbed into
  `NPCVisitBeat` in Step 11; no remaining callers).
- `include/AsyncDispatch.h`, `src/AsyncDispatch.cpp` —
  remove the `PollUntilOrTimeout` declaration and
  implementation, plus its supporting constants/types if
  they exist only for it. Keep `MarshalToMainThread` and
  every other helper.
- `src/Plugin.cpp` — remove any remaining reference to
  `ActionRegistry::Initialize()`,
  `ActionDispatcher::Initialize`, and their cosave
  registrations (`'NEAC'` — replaced by the skip handler
  installed in Step 5, so the registration line can be
  removed cleanly).
- CMake target file list — remove entries for the four
  deleted `.cpp` files.

**Sub-tasks:**

1. Delete the files.
2. Search the codebase for any remaining reference to
   the deleted symbols (`IAction`, `ActionDispatcher`,
   `ActionRegistry`, `PollUntilOrTimeout`,
   `VisitConclusionPoll`). No matches should remain —
   Steps 8, 10, and 11 already replaced each beat's file
   with an `IBeat`-based rewrite, so nothing includes
   the old headers.
3. Update CMake.

**Verify:**

- `pwsh -File build.ps1 build` succeeds with the four
  files gone.
- Boot Skyrim; log shows no init failures; dashboard
  still opens cleanly.
- Grep `src` and `include` for any of the deleted symbol
  names returns no matches.

---

### Step 13 — Papyrus fragment sweep

- [x] Complete

**[USER]**

**Goal:** Remove `SendModEvent("_ne_ActionCompleted",
...)` calls from every quest fragment that fired them.
These calls were signaling completion to the C++
`ActionDispatcher` sink, which was deleted in Step 8;
the ModEvent is now sent-but-unheard. Removing them is
cosmetic (a sent-but-unheard ModEvent is harmless), but
the fragments should reflect current C++ reality.

**Files (Creation Kit — quest fragment scripts):**

- `_ne_AmbushQuest` — any stage fragment sending the
  ModEvent.
- The letter-pool quests — any stage fragment sending
  the ModEvent.
- `_ne_VisitQuest` — any stage fragment sending the
  ModEvent (most likely the ReturnHome / shutdown
  stage).

**Sub-tasks:**

1. In the CK, open each quest above.
2. In each stage fragment, search for
   `SendModEvent("_ne_ActionCompleted", ...)`.
3. Delete the call and any local vars only used to build
   its arguments.
4. Compile the fragment.
5. Save the ESP.

**Verify:**

- Every affected fragment compiles cleanly in the CK.
- Load Skyrim; force-dispatch each beat once (via the
  dashboard); log shows no `_ne_ActionCompleted`
  ModEvent traffic.

---

## Done condition

- Every step above has its checkbox marked complete.
- `pwsh -File build.ps1 build` succeeds cleanly with
  `IAction.h`, `ActionDispatcher.h/.cpp`,
  `ActionRegistry.h/.cpp`, `VisitConclusionPoll.h/.cpp`,
  and `AsyncDispatch::PollUntilOrTimeout` deleted.
- No current-generation source or config file references
  any of `IAction`, `ActionDispatcher`, `ActionRegistry`,
  `PollUntilOrTimeout`, `VisitConclusionPoll`, or
  `_ne_ActionCompleted` (matches in
  `docs/implementation/PHASE_0{1..5}_*.md` are expected
  and preserved as historical).
- All three beats fire end-to-end under both
  force-dispatch (dashboard) and normal Director cadence.
- Save/reload mid-beat resumes cleanly for each beat
  according to its restart-on-reload contract (compose
  restarts fresh; RUNNING resumes from quest stage with
  session counters at zero; CLEANUP idempotent).
- Dashboard displays the correct `BeatState` alongside
  the beat name through a full lifecycle for all three
  beats.
- No `_ne_ActionCompleted` ModEvent traffic remains in
  the log during a normal play session.
