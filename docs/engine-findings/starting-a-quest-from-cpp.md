# Starting a quest from C++ (with FMR alias evaluation)

## TL;DR

To start a Skyrim quest from C++ with the same semantics as the console
`startquest <EditorID>` command — i.e. the engine actually promotes the
quest, runs the stage-0 fragment, AND evaluates Find-Matching-Reference
alias conditions to bind nearby references — use:

```cpp
auto* quest = /* RE::TESQuest* lookup */;
bool engineResult = false;
const bool callOk = quest->EnsureQuestStarted(engineResult, /*a_startNow=*/true);
```

Verified working in `src/AmbushAction.cpp` against
`_ne_BanditAmbushQuest`. No `SetEnabled(true)` call is needed beforehand;
`EnsureQuestStarted` handles enabling internally.

## The other paths we tried (and what they actually do)

We burned a lot of time on this before landing on `EnsureQuestStarted`.
Recording every failed path so they don't get re-tried:

| Path | Stage advances? | FMR aliases fill? | Notes |
| --- | --- | --- | --- |
| `SetEnabled(true) + Start()` | ❌ stage stays 0 | ❌ | Sets the `kEnabled` flag (which `IsRunning()` reads — see below) but never runs the engine's promotion pass. |
| `SetEnabled(true) + ResetAndUpdate()` | ✅ to stage 10 | ❌ | Runs the stage-0 fragment, but the FMR conditions never get evaluated. Quest ends up "running at stage 10" with empty alias slots. |
| Papyrus VM dispatch to `Quest.Start()` | ❌ | ❌ | Same outcome as the direct `Start()` call. |
| `EnsureQuestStarted(engineResult, true)` (no `SetEnabled` first) | ✅ | ✅ | **The working path.** Matches console `startquest`. |
| `ConsoleCommand::Run("startquest <EditorID>")` | ✅ | ✅ | Works, but is a workaround — actually compiles and runs a `Script` form through the console's compiler. Use `EnsureQuestStarted` instead. |

## Why the difference matters

If the quest only "starts" by flag flip without the FMR pass, all
reference aliases (XMarker spawners, target NPC bindings, etc.) stay
empty. Stage scripts that reference those aliases silently no-op, and to
an outside observer the quest looks "running" while doing nothing.

`sqv <EditorID>` in the console will report `Enabled: Yes, State:
Running, Current stage: 10` for an `SetEnabled + ResetAndUpdate` start
even though no aliases filled — which made debugging this very
confusing.

## API signature

```cpp
// CommonLibSSE-NG: RE/T/TESQuest.h
bool EnsureQuestStarted(bool& a_result, bool a_startNow);
```

- **return value**: whether the call itself was dispatched OK.
- **`a_result` (out)**: the engine's reported success/failure of the
  start. Both `callOk` and `engineResult` came back `true` in our
  working case.
- **`a_startNow` (in)**: pass `true` to run the promotion pass
  synchronously / immediately. We have not tested `false` — if you need
  a deferred start (queue and let the engine pick it up later), try
  that, but `true` is what console-startquest seems to do under the
  hood.

## `IsRunning()` caveat — do not gate on it

While digging into this we also confirmed:
`RE::TESQuest::IsRunning()` reads the same `kEnabled` flag bit as
`IsEnabled()`. It returns `true` for any enabled quest, including ones
that have never been promoted (e.g. a "Start Game Enabled" quest at
game load). It is **not** a reliable "is the quest currently running"
signal — gating on it produces false-positive blocks for never-started
quests.

For "is this quest really in flight," use
`GetCurrentStageID() > 0 && !IsCompleted()` instead. The stage-0
fragment reliably advances the stage when the engine promotes the
quest, and `Reset()` drops it back to 0 during cleanup, so this
self-clears across the lifecycle.

`IsCompleted()` IS reliable — it reads the `kCompleted` flag set by
Papyrus `CompleteQuest()` and is independent of `kEnabled`.

## Stopping / cleaning up

For symmetry: to return the quest fully to its game-load baseline after
it completes, run `Stop() + Reset() + SetEnabled(false)` in that order.
`Stop()` alone leaves the current stage advanced; `Reset()` clears
stage / alias state back to baseline; `SetEnabled(false)` flips the
enabled flag off so the next `EnsureQuestStarted` begins from the same
baseline the engine has at game-load.
