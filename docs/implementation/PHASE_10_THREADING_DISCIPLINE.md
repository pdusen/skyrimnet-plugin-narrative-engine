# Phase 10 — Threading Discipline

A structural refactor of how NarrativeEngine handles threading. Introduces two unforgeable token
types (`MainThread::Token`, `PluginThread::Token`) that gate every plugin function by construction —
foreign threads, holding neither, are compile-locked out of everything except `AsyncDispatch::EnqueueWork`
— plus a `MainThread::Run<T>(PluginThread::Token, fn) -> T` primitive that lets plugin-thread code
synchronously ask the main thread to run a lambda and hand back its result.

No user-visible feature changes. No new beats. This phase is scoped to the threading substrate underneath
the tick loop, `AsyncDispatch`, `BeatSystem`, and the beat state machines.

---

## Why this phase exists

`docs/MAIN_THREAD_STUTTER_AUDIT.md` catalogued eight places where per-tick or per-dispatch main-thread work
is causing playtester-visible micro-stutters. Every finding traces back to the same root: **the codebase
has no discipline about which thread runs what**, and the tools we've been reaching for encourage the wrong
default.

Concretely:

- `AsyncDispatch::MarshalToMainThread(lambda)` is fire-and-forget. To get a *result* back to the worker,
  the lambda has to capture the follow-on state and re-marshal into another lambda. That chaining is
  awkward enough that we've routinely chosen the easier path — just do the whole block of work on the
  main thread — even when only one line of it genuinely required main-thread execution.
- Three thread abstractions overlap (`Tick`'s driver + `PollOnMainThread` body, `AsyncDispatch`'s worker,
  `BeatSystem`'s poller). Each has its own conventions about what runs where, and the boundaries between
  them are documented only in comments. New code has no rule to follow; author judgment fills the gap,
  and judgment has been inconsistent.
- Engine touches from off-main threads are permitted case-by-case ("stable singleton pointer + plain bool
  read is safe") without any structural guard, so the same reasoning gets reinvented — sometimes
  correctly, sometimes not — every time a new subsystem needs it.

The audit's fix sketches all boil down to "just move this work off main." That's the right destination.
This phase builds the road that gets us there safely, so that the individual fixes (in a later phase)
become mechanical rewrites rather than delicate per-case surgery.

The controlling principle for this phase: **the framework enforces the discipline, not the author.** If
the only thing stopping a mistake is "the reviewer will notice," we haven't actually fixed anything —
we've just relocated the bug from the code to the review queue. Where possible, misuse should be a
compile error. Where that's not feasible, it should be a debug-fatal assertion the first time it fires
in playtest. Convention is the last resort, not the first.

---

## Scope

### In scope

- **Two unforgeable token types.** `MainThread::Token` and `PluginThread::Token`, each zero-sized
  and constructible only by its own dispatcher's internals. These are the load-bearing pieces the
  rest of the phase is built on.
- **A new `MainThread` module** owning:
  - `MainThread::Run<T>(PluginThread::Token, fn) -> T` — blocking. Requires proof-of-plugin-thread
    from the caller; runs `fn` on the main thread with a freshly-minted `MainThread::Token`; returns
    fn's result by value.
  - `MainThread::FireAndForget(PluginThread::Token, fn)` — non-blocking counterpart. Same signature
    shape as `Run`, minus the return.
- **A signature change to `AsyncDispatch::EnqueueWork`** — jobs now receive a `PluginThread::Token`
  as their first parameter, constructed by the dispatcher when the job runs on the worker. This is
  the sole plugin API that does not require a token from the caller — it's the entry point foreign
  threads use to reach plugin-thread land in the first place. Every existing `EnqueueWork` call
  site is updated mechanically.
- **The over-arching rule** the two token types enforce: every plugin function takes either a
  `MainThread::Token` or a `PluginThread::Token` as an argument. Engine-touching wrappers demand
  the first; plugin-scope business logic and state accessors demand the second. Foreign threads,
  holding neither, are structurally locked out of everything except `EnqueueWork`.
- **A small starter set of `Token`-gated engine wrappers.** This phase introduces the pattern by
  wrapping the handful of engine calls the highest-impact audit fixes will consume: the pause/
  combat/dialogue predicates (`EngineUtils`), the sky/weather singleton read, the player location
  and cell reads, and the actor lookup + name/state reads used by the sender-pool walk. Enough to
  prove the wrapper style, not enough to touch every engine call in the tree.
- **`[[deprecated]]` marker on `AsyncDispatch::MarshalToMainThread`** — the old fire-and-forget
  entry point stays functional (its body doesn't change) but every remaining call site generates a
  build warning until the audit-fix follow-on phase migrates them to `MainThread::FireAndForget`.
- **`SkyrimNet` callback typedef change** — the user-facing callback signature for
  `SendCustomPromptToLLM` gains a `PluginThread::Token` parameter, making it compile-impossible for
  callback bodies to run on the foreign thread the callback originates from.
- **Documentation of the discipline** in `docs/THREADING_MODEL.md`: the three roles (Main Thread,
  Plugin Thread, Foreign Threads), the two tokens, the primitives, the wrapper pattern, and the
  enforcement mechanisms. Same shape as `docs/LLM_RESPONSE_HANDLING.md`.

### Deferred (explicitly out)

- **The audit findings themselves.** This phase ships the primitive; a follow-on phase applies it to
  Findings 1–8 of `MAIN_THREAD_STUTTER_AUDIT.md`. That split exists so the substrate can be reviewed
  and merged on its own without also carrying the risk of eight simultaneous behavior changes.
- **Merging `AsyncDispatch`'s worker and `BeatSystem`'s poller into a single Plugin Thread.** That's a
  sensible follow-on, but the primitive is orthogonal to it — the same `MainThread::Run` works whether
  there's one plugin thread or three.
- **Retrofit-token-gating every existing plugin function and engine call.** The token discipline
  is aspirationally universal but practically incremental. This phase gates the substrate itself
  (the two primitives, `EnqueueWork`, the SkyrimNet callback) plus a starter set of engine wrappers.
  Existing plugin functions in `BeatSystem`, `BeatRegistry`, `DecisionLog`, event logs, etc. keep
  their current tokenless signatures and continue to compile. The audit-fix follow-on phase, and
  every subsequent phase that touches an existing subsystem, gates additional functions as it goes.
  The rule that binds *now* is: any newly-authored plugin function, and any refactored one, must
  take a token in its signature.
- **A cross-thread cancellation mechanism.** `MainThread::Run` will block the worker until the main
  thread finishes the task. If a job needs to be cancellable, the cancellation flag is the caller's
  responsibility to check between `Run` calls, not something the primitive owns.
- **Any change to tick cadence, tick-firing thread, or the polling intervals.** The `Tick.cpp` driver
  keeps its 500 ms sample cadence and keeps posting `PollOnMainThread` via the SKSE task interface.
  What changes is only what `PollOnMainThread` is allowed to do (less) and where the rest of the work
  lives (the plugin thread).
- **Cosave format changes, INI changes, ESP-side changes, Papyrus changes.** None.

---

## Design overview

### The three thread roles

The design centres on making three roles explicit and giving each one a small, sharp contract:

- **Main Thread.** The game's own thread. Runs the engine, owns essentially all engine mutation, and
  runs whatever we hand it via the SKSE task interface. NarrativeEngine's rule: main-thread execution
  is a scarce resource. We use it only for work that genuinely requires it (engine mutation, engine
  reads that documented sources say are not thread-safe), and for as short a duration as we can
  arrange.
- **Plugin Thread.** Any thread NarrativeEngine itself owns and pumps. Today that's `AsyncDispatch`'s
  single worker and `BeatSystem`'s polling worker; whichever it is, the same rules apply. Code
  running here is allowed to call `MainThread::Run` to synchronously ask the main thread for
  something. Plugin-thread code is where the bulk of our logic lives.
- **Foreign Threads.** Threads NarrativeEngine does not own — SkyrimNet's callback-dispatch pool, the
  game's event-sink dispatch, any DLL-boundary continuation. Foreign-thread code is structurally
  locked out of essentially every plugin API by construction: our functions demand either a
  `MainThread::Token` or a `PluginThread::Token` as an argument, and foreign-thread code has neither.
  The one sanctioned action available to a foreign thread is `AsyncDispatch::EnqueueWork` — the sole
  entry point deliberately kept token-free. Its lambda receives a `PluginThread::Token`, at which
  point foreign work has crossed the border into plugin-thread land and everything else becomes
  available.

The reason foreign threads are locked out of the plugin surface is deadlock discipline and state-
integrity discipline. We know very little about a foreign thread's blocking guarantees, and code that
runs on unknown threads can trivially race our internal state. Enqueueing onto the plugin thread is
non-blocking and adds one hop of latency; that's the trade we accept to keep the substrate correct.

### The two token types

The type system carries the discipline. Two zero-sized token types, each unforgeable outside its own
dispatcher's internals, name the two thread contexts where NarrativeEngine's own code is legally
allowed to run:

```cpp
namespace NarrativeEngine::MainThread {
    class Token {
        Token() = default;
        friend struct RunDispatcher;
        friend struct FireAndForgetDispatcher;
    };
}

namespace NarrativeEngine::PluginThread {
    class Token {
        Token() = default;
        friend struct AsyncDispatch::JobDispatcher;
    };
}
```

The rule is exhaustive: **every plugin function takes either a `MainThread::Token` or a
`PluginThread::Token` as an argument.** Engine-touching wrappers demand `MainThread::Token`;
everything else in the plugin's own logic (business logic, state accessors, cross-module helpers)
demands `PluginThread::Token`. Some general-purpose utilities may offer overloads accepting either.

The exhaustive rule has one deliberate exception: `AsyncDispatch::EnqueueWork` is the sole plugin
API callable with no token from the caller. This is what lets foreign-thread code — SkyrimNet
callback bodies, engine event sinks — enter plugin-thread land in the first place. The lambda
passed to `EnqueueWork` receives a `PluginThread::Token` freshly constructed by the dispatcher when
the job runs on the worker.

The consequence for foreign threads: the only symbol they can name from our API is
`AsyncDispatch::EnqueueWork`, and the only thing they can put inside it is a token-taking lambda.
There is no other legal surface for foreign code to interact with the plugin. The lockout is
compile-time and total.

### The primitives: `MainThread::Run<T>(t, fn) -> T` and `FireAndForget`

```cpp
namespace NarrativeEngine::MainThread {

    // Blocking call. Enqueues `fn` on the SKSE task interface, waits for
    // the main thread to run it, returns fn's result by value.
    //
    // The caller must supply a PluginThread::Token proving they are on
    // the plugin thread. Main-thread callers, foreign-thread callers,
    // and re-entrant callers from inside another Run's lambda all fail
    // to compile because they have no PluginThread::Token to pass.
    //
    // fn signature: T(MainThread::Token). Must be self-contained — the
    // main thread does whatever fn asks and returns the result; no
    // follow-on marshaling from inside fn.
    template <typename Fn>
    auto Run(PluginThread::Token, Fn&& fn) -> std::invoke_result_t<Fn, Token>;

    // Non-blocking counterpart. Same PluginThread::Token requirement.
    // Named distinctly so the "block for a result" vs "fire and forget"
    // difference is unmissable at every call site.
    void FireAndForget(PluginThread::Token, std::function<void(Token)> fn);
}
```

Under the hood: `Run` builds a `std::promise<T>`, wraps `fn` into a task that resolves the promise
(catching exceptions and re-throwing on the caller side), submits the task via SKSE's task interface,
and blocks on the future. When the main thread runs the task, it constructs a `MainThread::Token`,
invokes `fn`, resolves the promise, and returns. The worker unblocks and returns `T` by value.

The return-by-value discipline is load-bearing: the worker cannot end up holding a pointer or reference
to main-thread-owned state that might mutate under it after `Run` returns. If the worker wants an
`RE::Actor*`, the lambda must copy out the derived fields (`FormID`, name, position, etc.) into a
plain struct and return that, not the pointer.

### How the design enforces the discipline

The user requirement is that misuse be impossible or extremely painful. The two token types collapse
what used to be a handful of runtime marker checks into compile-time barriers. Every mistake in
the list below is now a build error:

**1. Foreign threads calling `Run` or `FireAndForget`.** Compile error. Both primitives demand a
`PluginThread::Token` as their first argument. Foreign-thread code has no `PluginThread::Token` to
pass — the only way to obtain one is to be inside an `AsyncDispatch::EnqueueWork` job. Foreign code
either goes through `EnqueueWork` first (correct) or fails to compile (safe).

**2. Main-thread code calling `Run`.** Compile error. Main-thread code holds `MainThread::Token`
(delivered by `Run`'s or `FireAndForget`'s dispatcher when running the marshaled task). It does not
hold `PluginThread::Token`, so it cannot satisfy `Run`'s signature. Deadlock avoided by
construction — the blocking wait is unreachable.

**3. Re-entrant `Run` from inside another `Run`'s lambda.** Compile error, for the same reason as
2. The inner lambda receives a `MainThread::Token`, not a `PluginThread::Token`.

**4. Engine touches from unsanctioned contexts.** Compile error. Every sanctioned engine wrapper
takes a `MainThread::Token` as its first argument. `MainThread::Token` is unforgeable outside the
dispatchers' internals, so the only path that yields one is being inside a `Run` or `FireAndForget`
lambda — which by construction is running on the main thread.

**5. Plugin logic called from a foreign thread.** Compile error, by the same construction as 1.
Every plugin function demands one of the two token types, and foreign-thread code has neither.
Whole categories of race bugs are precluded by the signature system rather than by review
discipline.

The starter wrapper set (see Scope) is where this pattern gets its first proof-of-life. Existing
direct engine calls from main-thread code continue to compile — this phase does not force a global
mass-rewrite of the back catalogue. The rule is: **new code, and refactored code, must go through
`Token`-gated APIs.** Existing token-less APIs will be marked deprecated so they surface as build
warnings, and the audit-fix follow-on phase migrates the highest-value callers first.

The runtime `ThreadRole` marker mentioned in earlier drafts becomes a belt-and-braces layer only —
useful for observability and for the "am I on the main thread already?" fast path inside the
primitives, but no longer the primary enforcement mechanism. The compile-time barriers catch
misuse before the runtime marker would ever fire.

**6. Long-lived references to main-thread state escaping the lambda.** This is the one pattern the
type system can't catch cleanly (someone can always dereference a pointer stored somewhere). The
mitigation is threefold: return-by-value discipline on `Run<T>`, a documented convention that lambda
captures must be by value not by reference for engine-owned data, and a lint-style review checklist in
`docs/THREADING_MODEL.md` for the escape hatch cases. This is the one place we fall back on
convention; the design section for that doc will justify why an in-language enforcement isn't worth
the ergonomic cost.

### `AsyncDispatch` after the refactor

`AsyncDispatch::EnqueueWork` keeps its role and its name, but its signature changes:

```cpp
// Before:
void EnqueueWork(std::function<void()> work);

// After:
void EnqueueWork(std::function<void(PluginThread::Token)> work);
```

The lambda receives a `PluginThread::Token` constructed by the dispatcher when the job pops off the
queue and begins running on the plugin worker. This is the sole entry point into plugin-thread
land — no `PluginThread::Token` requirement on the caller, because callers include foreign-thread
code that has no way to prove its context.

Existing call sites to `EnqueueWork` (about a dozen, mostly in the beat state machines) need their
lambda signatures updated to accept the token parameter. This is a mechanical rewrite done as part
of Step 4 — the lambdas' bodies don't need to change if they don't yet call any token-taking
functions.

`AsyncDispatch::MarshalToMainThread(fn)` becomes deprecated. `MainThread::FireAndForget` is its
successor and lives in the `MainThread` module. The deprecated symbol stays as a forwarder — with a
`[[deprecated]]` attribute so every remaining call site generates a build warning — for the
duration of the audit-fix follow-on phase, which migrates individual callers as it touches them.

The rename matters because at the point in the refactor where a call site is considering "should I
use `FireAndForget` or `Run`," the two names sit next to each other in the same namespace, and the
name itself declares the semantics — no reader needs prior knowledge of the convention to spot the
difference.

Both `FireAndForget` and `Run` demand `PluginThread::Token` from the caller. There is no "safe to
call from either main or plugin" fast path — main-thread code that wants to marshal work to itself
was never a meaningful use case for these primitives anyway. If main-thread code needs to schedule
follow-on main-thread work, it can either call `SKSE::GetTaskInterface()->AddTask` directly or
inline the work; neither of those is what these primitives are for.

### What plugin-thread code looks like after this phase

Before (today's chained-lambda pattern from `BeatSystem::ConsiderBeat`'s LLM callback path):

```cpp
AsyncDispatch::MarshalToMainThread([snapshot = std::move(snapshot), rec = std::move(rec)]() mutable {
    BeatSystem::ConsiderBeat(std::move(snapshot), std::move(rec), [] { g_inFlight.store(false); });
});
```

After (linear code on the plugin thread that reaches into main for engine touches):

```cpp
// Running on the plugin thread with a PluginThread::Token in hand
// (delivered by AsyncDispatch::EnqueueWork's dispatcher).
void ContinueConsideration(PluginThread::Token pt, Snapshot snapshot, DecisionRecord rec)
{
    // Plugin-thread logic runs freely — our own state, our own mutexes.
    auto candidates = BuildCandidateList(pt, snapshot, rec);

    // Reach into main for the engine-owned bits, get them back as plain data.
    auto liveCtx = MainThread::Run(pt, [&](MainThread::Token mt) -> LiveBeatContext {
        return BuildBeatContextFromEngine(mt, snapshot);
    });

    // Back on the plugin thread with liveCtx in hand. Continue linearly.
    if (auto blocked = CheckGates(pt, liveCtx, candidates); blocked) {
        FinalizeWithoutBeat(pt, std::move(rec));
        return;
    }
    // ...etc.
}
```

The shape difference is the whole point of the phase. Linear code is easier to reason about, easier
to review for thread-safety mistakes, and easier to write correctly the first time. The old
capture-chained pattern will still be legal (via `FireAndForget`) for the cases that genuinely want
it, but it
stops being the path of least resistance.

### Compatibility with existing subsystems

- **`Tick.cpp`'s `PollOnMainThread`** — the SKSE-task-interface entry point that runs this becomes
  responsible for installing the "I am the main thread" thread-local marker for the duration of the
  call. The body of `PollOnMainThread` is unchanged in this phase; its contents get carved up in the
  follow-on phase that applies the audit fixes.
- **`BeatSystem`'s worker loop** — its per-tick body runs the same code as today, but each tick's
  work now runs under a freshly constructed `PluginThread::Token` (produced by the same dispatcher
  pattern `AsyncDispatch::EnqueueWork` uses). Existing off-main reads of `EngineUtils::IsGamePaused`
  etc. continue to work as-is; the phase adds `Token`-taking overloads alongside them so new callers
  can adopt the safe form without deleting the unsafe one until the follow-on phase.
- **SkyrimNet callback trampolines** in `SkyrimNetAPI.cpp` — become the sanctioned "foreign to plugin"
  bridge. The user-facing callback typedef gains a `PluginThread::Token` parameter, so callers can
  only invoke the callback with a token in hand. Inside `SendCustomPromptToLLM`, the adapter
  running on SkyrimNet's foreign thread has no token — its sole legal action is to capture the
  response/success into locals and enqueue via `AsyncDispatch::EnqueueWork`. The enqueued lambda
  receives a `PluginThread::Token`, which it then passes into the user callback. There is no legal
  path that runs the callback body on the foreign thread, and no legal path that skips the plugin-
  thread hop.
- **Engine event sinks** (`HitSink`, `BleedoutSink`, `FastTravelEndSink`) — the sinks continue to run
  on whatever thread the engine picks. Any work they currently do inline (e.g. pushing into the
  combat log's ring buffer under `std::mutex`) is fine and stays. Any new work they might want to add
  goes through the same "enqueue onto the plugin thread" bridge; the sink body itself stays minimal.

### Failure mode: what if the main thread never runs the task?

`MainThread::Run` blocks the worker until the future resolves. If the main thread is genuinely
wedged (game hang, load-screen stall, main-menu suspension), the worker blocks too. We accept this:
a wedged main thread means the game itself has stopped, and a wedged worker is a symptom, not a
new failure. The primitive will document this and offer no timeout — a timeout would add complexity
without buying anything real (there's no useful recovery from "the game froze"), and timeouts are
exactly the kind of "just in case" scaffolding the project guidance says not to build.

Legitimate long-running main-thread work (e.g. a save-load transition, an interior cell load) will
also block the worker for the duration. That's correct — the worker's job would be racing with the
engine state that's being torn down and rebuilt anyway. The plugin thread naturally quiesces during
those windows; the tick's `!IsGamePaused()` gate already covers the common case.

---

## Steps

The steps below build the substrate bottom-up: token types first, then the two primitives, then
the plugin-thread dispatcher (which mints `PluginThread::Token` for each job), then the foreign-
thread bridge, then the starter wrapper set, then documentation. Each step is separately buildable
and separately verifiable — nothing in this phase changes user-visible behaviour, so every step's
verify bar is "the build passes, the dashboard still works, ticks still fire, LLM callbacks still
round-trip." The follow-on phase that applies these tools to the audit findings is where behaviour
actually changes.

### Step 1 — `MainThread::Token` and `PluginThread::Token`

- [x] Complete

**[CLAUDE]**

**Goal:** Land the two token types with their dispatcher-friend construction gate, plus a lightweight
thread-local `ThreadRole` marker that serves as a runtime belt-and-braces sanity check (the primary
enforcement is compile-time via the token signatures, but the marker is useful for observability and
for the primitives' fast paths).

**Files:**

- `include/MainThread.h` (new).
- `include/PluginThread.h` (new).
- `src/ThreadRole.cpp` (new) — houses the thread-local marker + `ScopedRoleAssertion` shared by
  both modules.
- `include/ThreadRole.h` (new).
- `CMakeLists.txt` — add the new source file.

**Sub-tasks:**

1. Author `include/ThreadRole.h`:
   - `enum class ThreadRole : std::uint8_t { Foreign = 0, Main, Plugin };` — `Foreign` is the
     initial default so that any thread NarrativeEngine has not explicitly claimed reads as
     `Foreign`.
   - `ThreadRole CurrentRole()` — reads the thread-local marker.
   - RAII `ScopedRoleAssertion` that pushes a role on construction, restores the prior role on
     destruction. Used by the primitives' dispatchers.
2. Author `include/MainThread.h`:
   - `class MainThread::Token` — private default constructor, deleted copy/move, `friend`-declared
     with `RunDispatcher` and `FireAndForgetDispatcher` (forward-declared here, defined in Steps
     2–3).
3. Author `include/PluginThread.h`:
   - `class PluginThread::Token` — same shape as `MainThread::Token`, `friend`-declared with
     `AsyncDispatch::JobDispatcher` (forward-declared here, defined in Step 4).
4. Implement `src/ThreadRole.cpp` — the anonymous-namespace `thread_local ThreadRole g_role =
   ThreadRole::Foreign;`, `CurrentRole()`, and `ScopedRoleAssertion`'s ctor/dtor.
5. Add the file pairs to `CMakeLists.txt`.
6. Run `pwsh -File format.ps1`.

**Specifics:**

- The two token types live in separate namespaces so that overload resolution never accidentally
  matches one to the other. `MainThread::Token` and `PluginThread::Token` are distinct types by
  construction — no CRTP base, no shared parent, no implicit conversions.
- The tokens must be non-copyable AND non-movable. Copyability would let a lambda squirrel one
  away into a class member and then satisfy a signature it shouldn't; non-movability closes the
  same loophole via `std::move`. The intended lifetime is exactly "one dispatcher invocation."
- `thread_local` is C++11 and MSVC-supported on this project's compiler; no CRT gymnastics.
- Keep `MainThread.h` / `PluginThread.h` free of `<RE/Skyrim.h>` and other heavy engine headers —
  they'll be included widely.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Attempt (in a scratch branch, do not commit) to construct a `PluginThread::Token{}` from
  arbitrary code — the build should fail with an access-control error. Same for `MainThread::Token`.
  This is the foundation of every other step; confirm it's watertight before proceeding.

---

### Step 2 — `MainThread::FireAndForget` + SKSE task-entry marker installation

- [x] Complete

**[CLAUDE]**

**Goal:** Land `MainThread::FireAndForget` with its `PluginThread::Token` requirement on the caller,
install the `Main` role marker for the duration of every marshaled task, and mark
`AsyncDispatch::MarshalToMainThread` deprecated.

**Files:**

- `include/MainThread.h`, `src/MainThread.cpp` — add `FireAndForget`.
- `include/AsyncDispatch.h`, `src/AsyncDispatch.cpp` — `[[deprecated]]` on
  `MarshalToMainThread`. Keep the body as a passthrough to
  `SKSE::GetTaskInterface()->AddTask` so existing call sites still work.

**Sub-tasks:**

1. In `MainThread`, declare and define:

   ```cpp
   void FireAndForget(PluginThread::Token, std::function<void(MainThread::Token)> fn);
   ```

   Body:
   - Discard the caller's `PluginThread::Token` — its job is compile-time proof that the caller is
     on the plugin thread; no runtime use.
   - Submit to `SKSE::GetTaskInterface()->AddTask(...)` a wrapper that pushes
     `ScopedRoleAssertion(ThreadRole::Main)`, constructs a `MainThread::Token` via
     `FireAndForgetDispatcher`, invokes `fn(token)`, and swallows exceptions with
     `logger::error` (matching today's exception discipline in `src/AsyncDispatch.cpp`).
   - Null-`taskInterface` path logs and drops the task, matching today's behaviour in
     `src/AsyncDispatch.cpp:99`.
2. Introduce `FireAndForgetDispatcher` as an empty struct in the anonymous namespace of
   `src/MainThread.cpp`. This is the only type friended to `MainThread::Token` for `FireAndForget`'s
   construction path; scope is intentionally minimal.
3. Add `[[deprecated("Use MainThread::FireAndForget after obtaining a PluginThread::Token via
   AsyncDispatch::EnqueueWork.")]]` to the declaration of `AsyncDispatch::MarshalToMainThread` in
   `include/AsyncDispatch.h`. Do not remove the function — every current call site is main-thread-
   scheduled work that predates this phase, and migrating them is the audit-fix follow-on phase's
   job. Note: CommonLibSSE's build preset globally suppresses `/wd4996`, so this attribute will
   not emit visible warnings at call sites — its value here is as documentation for future readers
   of the header (and for a hypothetical future re-enablement of the warning class), not as an
   active nag.
4. Run `pwsh -File format.ps1`.

**Specifics:**

- Compile-time enforcement: any caller without a `PluginThread::Token` cannot invoke
  `FireAndForget`. That includes main-thread startup code, event sinks, and SkyrimNet callback
  bodies. Each of those must either be running under a `PluginThread::Token` (i.e. inside an
  `EnqueueWork` job) or route their marshaling through the deprecated `MarshalToMainThread`
  passthrough.
- The design section deliberately drops the "safe to call from main thread trivially" fast path
  that earlier drafts had for `FireAndForget`. That fast path only made sense when the primitive
  didn't require a token; with the token requirement, main-thread code has no way to invoke
  `FireAndForget` anyway, so the fast path is unreachable and would just be dead code.
- Existing behaviour is preserved: every current call site to `MarshalToMainThread` continues to
  compile (with a warning) and continues to run identically.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Runtime behaviour of dashboard, tick, and LLM callbacks is unchanged.
- The follow-on phase's migration list is enumerable from `grep -R "AsyncDispatch::MarshalToMainThread"
  src/` — do this scan at the top of that phase's Step 1.

---

### Step 3 — `MainThread::Run<T>` blocking primitive

- [x] Complete

**[CLAUDE]**

**Goal:** Land the synchronous request/response primitive. The three formerly-runtime-checked
misuse patterns (foreign caller, main-thread caller, re-entrant caller) all become compile errors
via the `PluginThread::Token` signature requirement.

**Files:**

- `include/MainThread.h`, `src/MainThread.cpp`.

**Sub-tasks:**

1. Declare the template in the header:

    ```cpp
    template <typename Fn>
    auto Run(PluginThread::Token, Fn&& fn) -> std::invoke_result_t<Fn, Token>;
    ```

   The `PluginThread::Token` parameter is unnamed in the signature (its value is unused; the
   parameter's job is compile-time proof of context). Provide both a returning specialization and
   a `void`-returning specialization (the latter still blocks, it just doesn't move a value back).
2. Author `RunDispatcher` in the anonymous namespace of `src/MainThread.cpp` so `MainThread::Token`'s
   friend declaration grants it construction access.
3. Template body — much simpler now that the misuse cases are compile-blocked:
   - Construct `std::promise<T>` (or `std::promise<void>`), obtain the future.
   - Submit via `SKSE::GetTaskInterface()->AddTask` a lambda that pushes
     `ScopedRoleAssertion(ThreadRole::Main)`, constructs a `MainThread::Token` via `RunDispatcher`,
     invokes `fn(token)` catching exceptions and routing them into `promise.set_exception`.
   - Block on `future.get()` and return the result. `future.get()` re-throws captured exceptions on
     the worker thread — the intended propagation shape.
   - Belt-and-braces: assert `CurrentRole() == ThreadRole::Plugin` at the top of the template. If
     someone bad-faith-forges a `PluginThread::Token` (via `friend` gymnastics or a manufactured
     subclass), the runtime marker catches it. Debug-fatal; release logs and returns a default-
     constructed `T`.
4. Do not introduce a timeout parameter. Per the design section's "Failure mode" note, a wedged
   main thread is not a recoverable condition and the primitive deliberately offers no escape hatch.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- `std::invoke_result_t<Fn, MainThread::Token>` correctly deduces `void` when `fn` returns void; an
  `if constexpr` on `std::is_void_v` inside the template body branches for the promise construction.
- Re-entrant `Run` from inside another `Run`'s lambda: the inner lambda holds `MainThread::Token`,
  not `PluginThread::Token`, so it can't satisfy `Run`'s signature. Compile error — no runtime
  guard needed.
- The template lives in the header (as any template must); the `RunDispatcher` type and the
  `SKSE::GetTaskInterface()` wire live in the .cpp behind non-template helper functions the
  template body calls.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Attempt (in a scratch branch, do not commit):
  - Calling `MainThread::Run(...)` without a token → compile error.
  - Calling `MainThread::Run(MainThread::Token{...}, ...)` from within a `Run` lambda → compile
    error (token construction blocked; also wrong type even if construction were possible).
  - Calling `MainThread::Run(PluginThread::Token{...}, ...)` from arbitrary code → compile error
    (token construction blocked).
  All three should fail to build with clear diagnostics. Restore the branch before proceeding.
- Once Step 4 lands, add a temporary test call from an `AsyncDispatch::EnqueueWork` job that fires
  a `MainThread::Run` and asserts the returned value matches what the main-thread lambda produced.
  Remove the test call before the step is marked complete.

---

### Step 4 — `AsyncDispatch::EnqueueWork` token-taking signature + plugin-thread marker

- [x] Complete

**[CLAUDE]**

**Goal:** Convert `AsyncDispatch::EnqueueWork` to deliver a `PluginThread::Token` to every job it
runs, install the `Plugin` role marker at the two worker-loop entry points, and mechanically update
every existing `EnqueueWork` call site to accept the token parameter in its lambda.

**Files:**

- `include/AsyncDispatch.h`, `src/AsyncDispatch.cpp` — signature change, `JobDispatcher` type,
  marker installation in the worker loop.
- `src/BeatSystem.cpp` — `WorkerLoop` marker installation.
- All existing `EnqueueWork` call sites (found via `grep -R "AsyncDispatch::EnqueueWork"`) — lambda
  signature update. Expect ~a dozen sites, mostly in the beat state machines and compose paths.

**Sub-tasks:**

1. Update `AsyncDispatch::EnqueueWork`'s signature to
   `void EnqueueWork(std::function<void(PluginThread::Token)> work)`. The caller-facing signature
   change is deliberately without a deprecation window — existing call sites must be updated in
   the same commit as this step because the old signature no longer exists.
2. Introduce `AsyncDispatch::JobDispatcher` as an empty struct in the anonymous namespace of
   `src/AsyncDispatch.cpp`. This is the one type friended to `PluginThread::Token` for its
   construction.
3. In `AsyncDispatch::WorkerLoop`, at the top of the function (outside the `for(;;)` loop), push a
   `ScopedRoleAssertion(ThreadRole::Plugin)`. Inside the loop, when a job pops off the queue,
   construct a `PluginThread::Token` via `JobDispatcher` and pass it into the job's invocation:
   `job(token);`.
4. In `BeatSystem::WorkerLoop`, similarly push `ScopedRoleAssertion(ThreadRole::Plugin)` at the
   top of the function. `BeatSystem`'s worker doesn't take jobs from a queue — its tick body runs
   directly — so tick-body callees that need `PluginThread::Token` receive one either by
   constructing it (they now can, via a similar friended `TickDispatcher` type) or by having the
   tick body itself take a token. Simplest is a small `TickDispatcher` inside `src/BeatSystem.cpp`
   that mints a token at the top of each tick pass and threads it into the tick body's helper
   calls. This is minor mechanical work — the tick body is small.
5. Mechanically update every existing `AsyncDispatch::EnqueueWork` call site. For each lambda,
   change the signature from `[]() { ... }` to `[](PluginThread::Token) { ... }`. Most lambdas
   don't use the token yet (they're existing code that predates the wrapper pattern), so the
   parameter is simply unnamed. Sites to update, at minimum:
   - `src/EvaluationPipeline.cpp` (LLM-callback continuation).
   - `src/BeatSystem.cpp` (the `FireBeatSelectLLM` callback continuation).
   - `src/NPCLetterBeat.cpp`, `src/NPCVisitBeat.cpp`, `src/AmbushBeat.cpp` (compose-chain
     continuations).
   - `src/DashboardUIManager.cpp` (input-listener bodies).
6. Log at `logger::info` on marker installation (once per thread lifetime) so the boot log
   confirms both threads have declared themselves.
7. Run `pwsh -File format.ps1`.

**Specifics:**

- The mechanical lambda updates in sub-task 5 are the substrate churn cost of the token discipline.
  There is no clever way to elide them — the type signature genuinely changed. Each site is a
  one-token addition to the lambda signature, no body changes required.
- The scoped assertion is placed once per thread lifetime — because the worker loop IS the entire
  thread lifetime, one push suffices. `ScopedRoleAssertion`'s destructor runs at thread exit;
  since these threads only exit at plugin shutdown, that's the right window.
- The `Tick.cpp` driver thread is deliberately not marked `Plugin`. Its only responsibility is to
  post `PollOnMainThread` via the SKSE task interface — no plugin logic runs on it, so it doesn't
  need or want plugin-thread privileges.
- The runtime marker is redundant for the token compile-time barrier but retained as belt-and-
  braces (see Step 3's assertion in the `Run` template body). No behavioural harm; nominal cost.

**Verify:**

- `pwsh -File build.ps1 build` succeeds — this is the step where the signature change bites, so
  the build catches any missed call site.
- Boot log shows the two "role installed" lines from `AsyncDispatch` and `BeatSystem` workers.
- Runtime behaviour of dashboard, tick, beat dispatch, and LLM callbacks is unchanged.
- Re-run the temporary Step 3 test call and confirm it now takes the happy path.

---

### Step 5 — Foreign-thread bridge for SkyrimNet callbacks

- [x] Complete

**[CLAUDE]**

**Goal:** SkyrimNet fires its LLM callbacks from its own worker-thread pool. Change the
`SendCustomPromptToLLM` user-facing callback signature to require a `PluginThread::Token`, and
rewrite the internal adapter to enqueue via `AsyncDispatch::EnqueueWork`. After this step, it is
compile-impossible for the user's callback body to run on the foreign thread — the signature won't
permit invocation without a `PluginThread::Token`, and the only path to one is through
`EnqueueWork`.

**Files:**

- `include/SkyrimNetAPI.h`, `src/SkyrimNetAPI.cpp`.
- Callers of `SkyrimNetAPI::SendCustomPromptToLLM` (`src/EvaluationPipeline.cpp`,
  `src/BeatSystem.cpp`, `src/LetterComposer.cpp`, `src/VisitComposer.cpp`) — callback signature
  update. Small mechanical churn; the callback bodies don't change.

**Sub-tasks:**

1. Change the `SendCustomPromptToLLM` callback typedef from
   `std::function<void(std::string response, bool success)>` to
   `std::function<void(PluginThread::Token, std::string response, bool success)>`. The
   `PluginThread::Token` proves the callback is running on the plugin thread.
2. Rewrite the internal adapter lambda in `SendCustomPromptToLLM`. Today it calls
   `cb(response, success)` inline on SkyrimNet's foreign thread. The new body:
   - Captures the response + success into local `std::string` / `bool` values (they're only valid
     for the duration of SkyrimNet's call).
   - Calls `AsyncDispatch::EnqueueWork([cb, r = std::move(response), s = success](PluginThread::Token
     pt) { cb(pt, r, s); })`.
   - The foreign-thread adapter body has no `PluginThread::Token`. Its ONLY compile-legal action is
     to enqueue — trying to invoke `cb` directly is a compile error because `cb`'s signature now
     demands a token the adapter can't produce.
3. Add a doc-comment paragraph above the adapter explaining that the compile-time gate makes the
   foreign-thread bridge structurally correct. The old "do not marshal to main here" convention
   note is unnecessary — the type system enforces it.
4. Update each caller's callback lambda signature to add the `PluginThread::Token` parameter. The
   callback bodies don't need to change (they already do plugin-thread work), the parameter is
   simply unnamed unless the body wants to pass the token onward.
5. Verify by inspection that no other places in the code inline-invoke a SkyrimNet-supplied
   callback. `PublicRegisterDecorator` takes `std::function<std::string(RE::Actor*)>` and runs on
   the main thread when the engine renders a bio — that's already main-thread work; no bridging
   needed. Do not attempt to token-gate decorator callbacks in this phase.
6. Run `pwsh -File format.ps1`.

**Specifics:**

- The one-hop enqueue introduces single-digit-millisecond latency between SkyrimNet completing
  the LLM call and the callback body running. The LLM round-trip itself is orders of magnitude
  longer, so the added hop is invisible.
- SkyrimNet's callback may fire before `AsyncDispatch::EnqueueWork` is available (e.g. during
  shutdown). The enqueue helper already logs and drops in that case (`src/AsyncDispatch.cpp:85`);
  no additional guarding required.
- The compile-time proof is worth naming explicitly: with the new typedef, there is literally no
  syntactically-valid path that runs `cb`'s body on a foreign thread. A future author who tries
  to "just call `cb` directly to save the enqueue hop" will get a build error pointing at the
  missing `PluginThread::Token`. This is the enforcement pattern the phase's design section
  promised, applied to its first substantive API.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- With debug logging on, fire a Director tick and confirm the LLM callback trace shows up on the
  plugin thread rather than a foreign thread.
- Both LLM-driven paths (tension eval in `EvaluationPipeline` and beat-select in `BeatSystem`) go
  through this adapter, so a single force-dispatch exercises both.

---

### Step 6 — Starter set of `Token`-gated engine wrappers

- [x] Complete

**[CLAUDE]**

**Goal:** Introduce the wrapper pattern with the handful of engine calls the follow-on audit-fix
phase will consume. Enough to prove the ergonomics; not a global rewrite.

**Files:**

- `include/EngineUtils.h`, `src/EngineUtils.cpp` — extend existing predicates.
- `include/MainThreadEngine.h`, `src/MainThreadEngine.cpp` (new) — housing for wrappers that don't
  belong in `EngineUtils` (player/cell/location reads, actor lookup + basic state).
- `CMakeLists.txt`.

**Sub-tasks:**

1. Extend the four existing `EngineUtils` free functions (`GetCurrentGameHours`, `IsGamePaused`,
   `IsPlayerInCombat`, `IsPlayerInDialogue`) with token-taking overloads:

    ```cpp
    bool IsGamePaused(MainThread::Token);
    bool IsPlayerInCombat(MainThread::Token);
    // ...etc.
    ```

   The token-taking overload has the same body as the untagged one; the token is discarded (its
   purpose is compile-time gating, not runtime state). The untagged overload is retained so
   existing main-thread call sites continue to compile untouched.
2. Author `MainThreadEngine` module. Contents:
   - `PlayerSnapshot ReadPlayerSnapshot(MainThread::Token)` returning a plain struct with the
     player's `formID`, current location (`RE::FormID` + display name), current cell (`RE::FormID`
     - display name + `isInterior`), and current position (`RE::NiPoint3`). This is a single-call
     replacement for the four separate engine reads that today happen inline in
     `EvaluationPipeline::BuildSnapshot`.
   - `std::optional<ActorSnapshot> LookupActor(MainThread::Token, RE::FormID)` returning a plain
     struct with the actor's display name, `IsDead`, `IsDisabled`, `IsInCombat`, `IsPlayerTeammate`,
     `IsInBleedout`, and position. Wraps `TESForm::LookupByID + As<Actor>` and the associated
     status reads.
   - `std::optional<SkySnapshot> ReadCurrentSky(MainThread::Token)` returning `sky->mode`,
     `sky->currentWeather->formID`, and the weather category derived from `currentWeather->data`.
     Replaces the direct `RE::Sky::GetSingleton()` reads in `WeatherEventLog::SampleCurrentCategory`.
3. Every wrapper returns plain, self-contained data (no `RE::*` pointers, no references into
   engine-owned memory). This is the return-by-value discipline in code.
4. Add file pairs to `CMakeLists.txt`.
5. Run `pwsh -File format.ps1`.

**Specifics:**

- The starter set is deliberately minimal. Each wrapper corresponds to a specific audit finding
  the follow-on phase will fix (`ReadPlayerSnapshot` → Finding 2; `LookupActor` → Findings 1 and 5;
  `ReadCurrentSky` → Findings 6/7's cousin). Additional wrappers land in later phases as those
  phases touch new engine surface.
- Do not remove or deprecate the untagged `EngineUtils` overloads — they're the API existing
  main-thread code uses, and that code is fine as-is. The discipline binds new off-main callers,
  not the back catalogue.
- Wrapper implementations may `assert` (`NarrativeEngine::MainThread::CurrentRole() ==
  ThreadRole::Main)` internally as a belt-and-braces check that the caller obtained the token
  legitimately. Cheap in debug, elidable in release.
- Do not add wrappers speculatively for engine calls this phase doesn't need. The wrapper surface
  should grow with demand, not ahead of it — per project guidance in `CLAUDE.md`.

**Verify:**

- `pwsh -File build.ps1 build` succeeds.
- Add a temporary demonstration via a debug-only ConsoleCommand:
  `AsyncDispatch::EnqueueWork([](PluginThread::Token pt) { auto snap = MainThread::Run(pt,
  [](MainThread::Token mt) { return MainThreadEngine::ReadPlayerSnapshot(mt); });
  logger::info("player at {}", snap.locationName); })`. Confirm the snapshot returns correct
  values. Remove the demonstration before marking the step complete.
- Attempt (in a scratch branch, do not commit) to call `MainThreadEngine::ReadPlayerSnapshot()`
  without a token — the build should fail. This is the compile-time enforcement the design
  section promised; confirm it is actually in effect.

---

### Step 7 — Threading model documentation

- [x] Complete

**[CLAUDE]**

**Goal:** Author `docs/THREADING_MODEL.md` and cross-reference it from `CLAUDE.md`. This is the
document new authors (human or LLM) will read when they need to figure out where new code should
run and how to reach across the thread boundary.

**Files:**

- `docs/THREADING_MODEL.md` (new).
- `CLAUDE.md` — add a short section pointing to `THREADING_MODEL.md`, alongside the existing
  `LLM_RESPONSE_HANDLING.md` reference.

**Sub-tasks:**

1. Author `docs/THREADING_MODEL.md` covering:
   - The three thread roles (`Main`, `Plugin`, `Foreign`) with one paragraph each on what runs
     there and why, and the load-bearing rule that every plugin function takes either a
     `MainThread::Token` or a `PluginThread::Token`.
   - The two primitives (`Run<T>`, `FireAndForget`) with a decision rubric for which to use.
   - The token-gated wrapper pattern, with an example of a new caller adding a new wrapper.
   - The `AsyncDispatch::EnqueueWork` entry point as the sole exception to the token rule, and why
     that exception is necessary (foreign-thread code can't otherwise get a token) and safe (the
     lambda it invokes DOES receive a token).
   - The six enforcement mechanisms (five compile-time, one convention-only) and what each catches.
     Be honest about mechanism #6 (long-lived references escaping the lambda) — it's the one gap
     where reviewers still need to look manually.
   - A short "what NOT to do" cheat sheet: don't try to forge a token via `friend` tricks; don't
     capture engine pointers into worker-scope state that outlives the lambda; don't reach for
     `FireAndForget` when `Run` would fit; don't add token-taking wrappers speculatively.
   - A pointer to `docs/MAIN_THREAD_STUTTER_AUDIT.md` as the case study that motivated the model.
2. In `CLAUDE.md`, add a section modelled on the existing "LLM-returned strings: always sanitize"
   section — one paragraph explaining the model exists, pointing to `THREADING_MODEL.md`, and
   stating the one-line rule ("every plugin function takes either a `MainThread::Token` or a
   `PluginThread::Token`; foreign threads enter via `AsyncDispatch::EnqueueWork`, which is the
   sole token-free entry point").
3. Run `pwsh -File format.ps1`.

**Specifics:**

- Match the tone of `docs/LLM_RESPONSE_HANDLING.md` — declarative, rule-oriented, with worked
  examples. Not aspirational prose; the doc should read as "here is how this project works," not
  "here is how it could work."
- Include a small ASCII diagram of the three roles and the sanctioned transitions between them.
  One diagram is worth several paragraphs.

**Verify:**

- Doc renders cleanly under `markdownlint` (via `pwsh -File format.ps1`).
- The one-line rule in `CLAUDE.md` correctly forwards the reader to `THREADING_MODEL.md`.
- No code changes; the phase is substrate-complete after this step.
