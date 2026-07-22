# Threading model

NarrativeEngine runs code on three kinds of threads, and the type system
enforces which code is allowed to run on which. This document is the reference
for authors adding new subsystems, new callback surfaces, or new engine
touches.

## The three thread roles

```text
       ┌──────────────────────┐
       │    Foreign threads   │   SkyrimNet callbacks, engine event sinks,
       │  (no NE code runs)   │   any DLL-boundary continuation we don't own.
       └──────────┬───────────┘
                  │
                  │  AsyncDispatch::EnqueueWork(fn)
                  │  (the sole token-free entry point)
                  ▼
       ┌──────────────────────┐
       │    Plugin thread     │   Business logic, state accessors, our own
       │ (PluginThread::Token)│   worker loops (AsyncDispatch, BeatSystem).
       └──────────┬───────────┘
                  │
                  │  MainThread::Run<T>(pt, fn) — blocking
                  │  MainThread::FireAndForget(pt, fn) — non-blocking
                  ▼
       ┌──────────────────────┐
       │    Main thread       │   Engine mutation + engine reads that need
       │ (MainThread::Token)  │   main-thread scheduling. Short and sharp.
       └──────────────────────┘
```

**Main thread.** The game's own thread. NarrativeEngine code only runs here
inside a lambda handed to `MainThread::Run` or `MainThread::FireAndForget`.
Main-thread execution is scarce; the rule is "spend as little time on it as
possible, only for work that genuinely requires it."

**Plugin thread.** Any thread NarrativeEngine itself owns. Today that resolves
to two threads: `AsyncDispatch`'s worker and `BeatSystem`'s worker. Their
worker loops declare `ScopedThreadRole(ThreadRole::Plugin)` at entry and hold
it for the thread's entire lifetime. Plugin-thread code runs under a
`PluginThread::Token` (constructed by the dispatcher when a job pops off the
queue) and is where the bulk of the plugin's logic lives.

**Foreign threads.** Threads NarrativeEngine doesn't own — SkyrimNet's
callback-dispatch pool, the game's event-sink dispatch (`TESHitEvent`,
`TESFastTravelEndEvent`, etc.), any other DLL-boundary continuation. The
only sanctioned action for foreign-thread code is to call
`AsyncDispatch::EnqueueWork` — the sole plugin API deliberately kept
token-free. Its lambda receives a `PluginThread::Token` when the job runs on
the worker, at which point the code has crossed into plugin-thread land and
the rest of the plugin API is available.

## The two tokens

Two zero-sized, non-copyable, non-movable types with private constructors,
each friend-declared with exactly one dispatcher type in the code that
mints it:

```cpp
namespace NarrativeEngine::MainThread   { class Token { /* friend RunDispatcher, FireAndForgetDispatcher */ }; }
namespace NarrativeEngine::PluginThread { class Token { /* friend AsyncDispatch::JobDispatcher */ }; }
```

The tokens carry no runtime state. Their whole job is to prove — at compile
time, in the signature — that the caller is running in the expected context.
A function that takes `MainThread::Token const&` can only be called from
inside a `Run` / `FireAndForget` lambda; a function that takes
`PluginThread::Token const&` can only be called from inside an `EnqueueWork`
job (or an inner `Run` / `FireAndForget` lambda that received one).

**The exhaustive rule:** every plugin function takes either a
`MainThread::Token const&` or a `PluginThread::Token const&` as an argument.
Engine-touching wrappers demand the first; plugin-scope business logic
demands the second. General-purpose utilities may offer overloads accepting
either. `AsyncDispatch::EnqueueWork` is the sole deliberate exception —
without it, foreign-thread code would have no way in.

## The two primitives

```cpp
// Blocking request/response.
template <typename Fn>
auto MainThread::Run(PluginThread::Token const&, Fn&& fn)
    -> std::invoke_result_t<Fn, MainThread::Token const&>;

// Non-blocking fire-and-forget.
void MainThread::FireAndForget(
    PluginThread::Token const&,
    std::function<void(MainThread::Token const&)> fn);
```

Rule of thumb for which to use:

- **`Run<T>`** when the plugin-thread caller needs a result and cannot
  continue without it. Blocks the plugin thread until the main-thread lambda
  returns. Exceptions thrown by the lambda propagate back to the caller.
- **`FireAndForget`** when the plugin-thread caller just needs to schedule
  some main-thread work and can continue immediately. Exceptions inside
  the lambda are logged, not propagated.

Both primitives require a `PluginThread::Token` from the caller. The type
system prevents you from calling either from anywhere except a plugin-
thread context.

## The `Token`-gated wrapper pattern

Engine calls (`RE::PlayerCharacter::GetSingleton()`,
`RE::Actor::IsInCombat()`, `RE::Sky::GetSingleton()`, `TESForm::LookupByID`,
etc.) require the main thread. To make that requirement enforceable, wrap
each engine read in a small function that takes a `MainThread::Token const&`:

```cpp
namespace NarrativeEngine::EngineUtils
{
    bool IsGamePaused();                          // legacy — main-thread callers
    bool IsGamePaused(MainThread::Token const&);  // go-forward — token-gated
}
```

Both overloads share a body — the token is discarded (its purpose is compile-
time gating). The untagged overload is retained so existing main-thread call
sites don't need to be touched wholesale; the token-taking overload is what
new off-main callers must use, via `Run` / `FireAndForget`.

For wrappers that group multiple engine reads into one main-thread hop, put
them in `MainThreadEngine` (see `include/MainThreadEngine.h`). Return
plain, self-contained data (no `RE::*` pointers, no references into engine-
owned memory) — that's the return-by-value discipline that keeps engine-
owned state from leaking back to a worker thread.

Example — a plugin-thread caller wanting the player's location + cell +
whether the player is in combat, all in one main-thread hop:

```cpp
// Running on the plugin thread inside an EnqueueWork job with `pt`.
auto snap = MainThread::Run(pt, [](MainThread::Token const& mt) {
    auto player = MainThreadEngine::ReadPlayerSnapshot(mt);
    const bool inCombat = EngineUtils::IsPlayerInCombat(mt);
    return std::make_pair(std::move(player), inCombat);
});
// Back on the plugin thread with `snap` in hand. Continue linearly.
```

## Enforcement mechanisms

The design catches the classic threading misuse patterns at compile time.
Five of the six are structural; the sixth remains convention-only and needs
human review.

| # | Misuse pattern                                    | Caught by                                                        |
| - | ------------------------------------------------- | ---------------------------------------------------------------- |
| 1 | Foreign thread calls `Run` / `FireAndForget`      | Compile error — no `PluginThread::Token` to pass.                |
| 2 | Main-thread code calls `Run`                      | Compile error — main-thread code holds `MainThread::Token`, not `PluginThread::Token`. |
| 3 | Re-entrant `Run` from inside another `Run` lambda | Compile error — same as 2 (inner lambda holds only `MainThread::Token`). |
| 4 | Engine touch from unsanctioned context            | Compile error — engine wrappers demand `MainThread::Token`, which only lives inside a `Run` / `FireAndForget` lambda. |
| 5 | Plugin logic called from a foreign thread         | Compile error — every plugin function demands one of the two tokens; foreign code has neither. |
| 6 | Long-lived reference to main-thread state escapes the lambda | **Convention only** — return by value from `Run<T>` lambdas; do not capture engine pointers into worker-scope state that outlives the lambda. Reviewers must look for this. |

The runtime `ThreadRole` marker (`include/ThreadRole.h`) is a belt-and-
braces layer beneath the compile-time barriers. `MainThread::Run` asserts
`CurrentThreadRole() == ThreadRole::Plugin` inside its body to catch bad-
faith `friend`-forgery of a token. `MainThreadEngine` wrappers similarly
assert `ThreadRole::Main`. In release builds these degrade to warnings +
default results, so a stray misuse in a shipped build doesn't crash a
player's game.

## What NOT to do

- **Don't try to forge a token via `friend` tricks.** The friend list on
  each `Token` is minimal by design. If you find yourself thinking "I'll
  just friend my class to `Token`," stop — you're bypassing the whole
  discipline. Write your function to take the token as a parameter and
  make callers obtain it the sanctioned way.
- **Don't capture engine pointers into lambda-external state.** If a `Run`
  lambda receives an `RE::Actor*` and squirrels it into a class member, the
  worker thread will race against the main thread's mutations of that
  actor. Copy the fields you need out into a plain struct and return that.
- **Don't reach for `FireAndForget` when `Run` would fit.** Chaining
  fire-and-forget lambdas to fake a request/response was the old anti-
  pattern that prompted this phase. If you need a result, use `Run`.
- **Don't add token-taking wrappers speculatively.** The starter set in
  `EngineUtils` and `MainThreadEngine` covers what the audit-fix work
  needs. When a later phase touches a new engine call from off-main, add
  the wrapper then — not before.
- **Don't call `MainThread::Run` from an event sink or SkyrimNet
  callback.** Those are foreign threads. Enqueue onto the plugin thread
  first, then run from there. (Actually, the type system prevents this at
  compile time — event sinks and callback trampolines have no
  `PluginThread::Token` to pass. Named here so the intent is obvious.)

## Case study

`docs/MAIN_THREAD_STUTTER_AUDIT.md` catalogued the main-thread work that
motivated this model. The audit-fix follow-on phase is where the
substrate ships in this phase gets applied to actual playtester-visible
stutters — the pattern in every fix is the same: move the work off main,
reach into main only for the specific engine touches that need it, use
`Run` / `FireAndForget` to bridge, use `MainThreadEngine` /
`EngineUtils` wrappers to keep the engine touches type-checked.
