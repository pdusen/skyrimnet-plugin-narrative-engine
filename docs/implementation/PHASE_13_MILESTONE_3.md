# Phase 13 — Milestone 3: The Gossip Simulation Thread

Milestone 2 made rumors real and gave the simulation enough to do that the shape of *where* it runs has
become the problem. Gossip currently shares a single worker thread with every other subsystem and every
foreign-thread handoff in the plugin, and it is throttled to an 8-millisecond slice per tick so that it
does not delay its neighbours. This milestone gives gossip its own thread, so that the throttle can be
deleted rather than tuned.

Prerequisites:

- [`PHASE_13_GOSSIP_PROPAGATION.md`](PHASE_13_GOSSIP_PROPAGATION.md) — the social graph and the SIR model.
- [`PHASE_13_MILESTONE_2.md`](PHASE_13_MILESTONE_2.md) — harvesting, the claim ledger, content generation.
- [`../THREADING_MODEL.md`](../THREADING_MODEL.md) — the three roles, the two tokens, and the enforcement
  table this milestone extends.

> **Doc status: planned, not built.** The design is settled and the implementation plan below is ordered;
> no step has been started.

---

## The problem

`AsyncDispatch` owns exactly one worker thread. Everything the plugin does off the main thread that is not
a Director LLM round-trip or a beat tick runs on it, in FIFO order:

```text
Tick driver (every 500ms) ──► AsyncDispatch queue ──► the one worker ──► CombatEventLog::Poll
SkyrimNet LLM callbacks   ──►                                            WeatherEventLog::Poll
Engine event sinks        ──►                                            TravelEventLog::Poll
Dashboard state compose   ──►                                            EventHistoryWriter::Poll
                                                                         FineRoads::Poll
                                                                         GossipHarvest::Poll
                                                                         GossipSim::Poll     ◄── the drain
                                                                         GossipLog::Poll
```

Gossip is the only occupant whose cost grows without bound. A carrier-step can make two `AddMemory` calls
into SkyrimNet's vector database, and per-event cost is dominated by those — so the work scales with the
number of live carriers *and* with the size of a database that only ever grows. Measured across a single
16-day run: 15 rumors, 203 transmissions, 891 conversations, and one rumor (`r01`) that reached 37 carriers
over 11 days.

Because that work sits in the shared queue, it is capped:

```cpp
const auto budgetStart = std::chrono::steady_clock::now();
const std::chrono::milliseconds budget{std::max(1, cfg.gossipMaxMillisecondsPerTick)};   // 8ms
...
if (processed > 0 && (processed % 4) == 0 && std::chrono::steady_clock::now() - budgetStart > budget) {
    outOfTime = true;
    break;
}
```

The cap does its job — the queue drains fully a hundred times over a session, in bursts of one to five
polls. But it exists **entirely to protect gossip's neighbours**, not gossip. Its cost is paid by the
simulation: a rest that advances the clock a full day defers carrier-steps across several polls, and the
per-event budget check itself reveals how uneven the work is. A poll that stops at 4 events spent over 2 ms
each; one that stops at 56 did 52 in under 8 ms. That is a 10× spread driven by whether the events were
transmissions, and it is not something this side can predict.

The throttle is therefore a tax on the wrong party. Gossip is slowed down so that the weather log is not.

---

## Scope

### In scope

1. **`GossipDispatch`** — a dedicated worker thread with a serial FIFO queue, one job at a time, in order.
2. **A whole gossip tick becomes one job**, running start to finish: harvest, evaluate, compose, seed,
   simulate, prune, flush. Including the LLM calls, which become **blocking** rather than callback-driven.
3. **Each tick is stamped with the game time it was due**, and reads the world as of that time. Ticks are
   queued one per crossed interval, never merged.
4. **The per-tick time budget is deleted.** The drain runs to completion inside that job.
5. **Cross-thread reads stop taking the simulation's lock** — the dashboard reads a published snapshot.
6. **Co-save serialisation reads the same snapshot the dashboard does.** No callback ever blocks on the
   gossip thread, in either direction.
7. **Queued and running work is cancellable**, so a load does not leave a tick writing into the world it
   just replaced.

### Deferred (explicitly out)

- **Parallelism inside the simulation.** One thread, one job at a time. The model is a discrete-event
  simulation over shared state; making it concurrent with itself would be a different project with no
  demonstrated need.
- **Any change to the model.** Weights, probabilities, band text, verdicts, claim semantics — all untouched.
  This milestone moves where code runs, not what it computes.
- **Moving other subsystems off `AsyncDispatch`.** Only gossip has the unbounded-growth problem.

---

## Design

### `GossipDispatch` is a third instance of an existing pattern, not a new invention

The plugin already runs three plugin-role workers, and the second one exists for precisely this reason:

```cpp
// include/EvalDispatch.h
// Dedicated worker for the Director's per-tick LLM round-trip so it
// can't stall AsyncDispatch's cadenced queue.
```

`GossipDispatch` is the same shape with the same justification. Its worker loop declares
`ScopedThreadRole(ThreadRole::Plugin)` at entry and holds it for the thread's lifetime, exactly as the other
three do. No new thread *role* is introduced — the roles stay Main / Plugin / Foreign.

### But it does introduce a fourth token, and that is the substantive decision

The invariant worth having is stronger than "gossip runs somewhere else":

> **Every byte of mutable gossip state is touched only from the gossip thread.**

If that holds, the ten `std::scoped_lock` sites in `GossipSim.cpp`, the three in `GossipHarvest.cpp` and the
nine in `GossipClaims.cpp` can all be **deleted**, because there is no longer a second thread to race with.
Single-threaded ownership is a stronger and cheaper guarantee than mutual exclusion.

An invariant that load-bearing should be enforced the way this project enforces the others — in the type
system, not in review. That means a `GossipThread::Token`, minted only by `GossipDispatch`'s job
dispatcher, and gossip's internal functions taking it instead of `PluginThread::Token`:

```cpp
namespace NarrativeEngine::GossipThread { class Token { /* friend GossipDispatch::JobDispatcher */ }; }
```

This extends the enforcement table in [`THREADING_MODEL.md`](../THREADING_MODEL.md) with a seventh row:
*"Gossip state touched from a non-gossip thread → compile error, no `GossipThread::Token` to pass."*

The cost is real and should be stated plainly: every signature in five gossip translation units changes,
and the threading-model doc needs a fourth box in its diagram. The alternative — reuse
`PluginThread::Token` and keep the invariant as convention — is cheaper to write and gives up the one
property that makes deleting the mutexes safe. The mutexes are not free either: `GossipSim::g_mutex` is
currently taken by the main thread during `OnSessionStart` and by SKSE's serialisation thread during
`OnSave`, and those are exactly the acquisitions that get dangerous once the drain is unbounded (see
below).

**Recommendation: add the token.** The mutex deletion is what makes the unbounded drain safe, and the token
is what makes the mutex deletion provable.

### A worker-token concept, gating exactly one function

The blocking `SkyrimNetAPI::SendCustomPromptToLLM` takes a `PluginThread::Token`, because until now its only
caller was `EvalDispatch`. Gossip needs it too, so the requirement becomes "any of our worker threads"
rather than "the plugin worker specifically".

The blast radius is one function. SkyrimNet's data wrappers — `GetMemoriesForActor`, `GetActorEngagement`,
`AddMemory`, `FormIDToUUID` — take no token at all today, because the underlying calls are documented
thread-safe; they need nothing. The asynchronous `SendCustomPromptToLLM` keeps its `PluginThread::Token`,
since gossip stops using it entirely and its other callers are all plugin-thread. Only the blocking overload
changes.

The recommended mechanism is a trait plus a concept, kept **additive** so the existing token is not touched:

```cpp
namespace NarrativeEngine
{
    template <typename T> struct is_worker_token : std::false_type {};
    template <> struct is_worker_token<PluginThread::Token> : std::true_type {};
    template <> struct is_worker_token<GossipThread::Token> : std::true_type {};

    template <typename T>
    concept WorkerToken = is_worker_token<std::remove_cvref_t<T>>::value;
}
```

The function then becomes a thin header-side forwarder onto the existing untagged implementation — the token
discarded, exactly as `EngineUtils`' token-gated wrappers already discard theirs:

```cpp
LLMResult SendCustomPromptToLLM(const WorkerToken auto&, const std::string& prompt,
                                const std::string& variant, const std::string& ctx);
```

Two properties make this the right shape over a shared base class:

- **`PluginThread::Token` is not modified.** It keeps its private constructor and its single `friend`.
  Retrofitting a base class onto it would mean either a protected base constructor — forgeable by anyone
  willing to write `struct Fake : WorkerToken {};` — or a friend list on the base that has to name every
  token type, which is the coupling the concept avoids.
- **Registering a future worker is one specialisation**, in one place, with no edit to any existing type.

**What deliberately does NOT take a `WorkerToken`:** `MainThread::Run` and `MainThread::FireAndForget`.
They keep demanding a `PluginThread::Token`, which makes the prohibition in the risk table below a compile
error rather than a convention — gossip code physically cannot reach the main thread, and so cannot
construct the one deadlock this design is exposed to.

### The gossip tick is one linear job

Everything a gossip tick does today is spread across the shared plugin thread and stitched together with
asynchronous callbacks. On its own thread it becomes a single unit of work that runs beginning to end:

```text
GossipDispatch job: "gossip tick", stamped with the game time it was SUPPOSED to fire at
  0.  asOf = the tick's scheduled game time. Every step below reads the world as it stood then.
  1.  harvest — rank actors, fetch memories, DISCARD anything stamped after asOf, qualify,
                build the candidate pool, shuffle it
  2.  for each candidate until one seeds or the pool is exhausted:
        a.  evaluate       ── BLOCKING LLM call (director variant)
        b.  on `seed`:
              compose      ── BLOCKING LLM call (composer variant)
              seed rumor, dated asOf
        c.  settle the claim, dated asOf
  3.  simulate — advance the simulation clock to asOf and drain every carrier-step due by then
  4.  prune — reap dead rumors, expire claims as of asOf
  5.  flush the trace
  6.  publish the dashboard snapshot
```

No callbacks, no continuation chain, no re-entrancy. Read top to bottom.

### Blocking LLM calls are the point, and there is precedent

`SkyrimNetAPI` already offers a synchronous overload alongside the async one, and its header states exactly
when it may be used:

> Blocking cost: this holds the plugin thread for the full duration of the LLM call (typically several
> seconds). Only use from single-flighted contexts where the plugin thread has nothing useful to do
> concurrently […] In particular do NOT use from contexts where other plugin-thread work would meaningfully
> suffer from the stall.

A dedicated gossip thread satisfies that condition by construction — nothing else runs there, and nothing
else is waiting on it. `EvaluationPipeline` already does this from `EvalDispatch`'s dedicated worker for the
Director's per-tick round trip. Gossip becomes the second instance of the same arrangement, not a novel one.

### What this deletes

The asynchronous walk exists solely because the calls were non-blocking. Make them blocking and it
evaporates:

| Deleted | Replaced by |
| ------------------------------------------------------ | -------------------------------------- |
| `struct Walk` and its `shared_ptr` lifetime             | locals in a function |
| `Advance` / `Evaluate` mutual recursion via callbacks   | a `for` loop over the pool |
| Two-stage claim settlement split across callback bodies | straight-line code after each verdict |
| ~22 `scoped_lock` sites across three translation units  | single-thread ownership |
| `kMaxGameDayDeltaPerPoll` and its catch-up clamp        | one stamped tick per interval |
| `iGossipMaxMillisecondsPerTick` and the budget check    | nothing — the drain runs to completion |

It also removes an entire **class** of bug rather than an instance. The double-claim defect found last run —
memory 1356 evaluated twice by two concurrent walks, seventeen seconds apart, for two director calls and one
answer — was possible only because two owed sweeps could have walks in flight simultaneously. With one job
per tick on a serial queue, two sweeps cannot overlap at all. The `IsClaimed` re-check stays, but it drops
from load-bearing to belt-and-braces.

### One consequence worth naming

A rumor seeded in step 2 is live before step 3 runs, so it takes part in the same tick's simulation. Today
the seed lands ten to twenty seconds later via a callback and joins some later drain. The new ordering is
the more coherent of the two — a rumor exists, then the world talks — but it is a behavioural change, not
just a scheduling one.

### What moves onto the gossip thread

| Operation                            | Entered from today                        | Trigger after                              |
| ------------------------------------ | ----------------------------------------- | ------------------------------------------ |
| `GossipHarvest::Poll` — the sweep    | `AsyncDispatch` worker, via `Tick`        | step 1 of the tick job                     |
| `GossipContent` evaluate / compose   | `AsyncDispatch` worker, from LLM callback | step 2, inline and blocking                |
| `GossipSim::SeedRumor`               | `AsyncDispatch` worker, from a walk       | step 2                                     |
| `GossipSim::Poll` — the event drain  | `AsyncDispatch` worker, via `Tick`        | step 3                                     |
| `GossipClaims` mutation and expiry   | wherever the caller happened to be        | steps 2 and 4                              |
| `GossipLog::Poll` — the trace flush  | `AsyncDispatch` worker, via `Tick`        | step 5                                     |
| `OnSessionStart`                     | **main thread** (SKSE messaging)          | enqueued; the main thread does not wait    |
| `OnSessionEnd`                       | **main thread** (SKSE messaging)          | enqueued **and waited on** — see below     |
| `GossipLog` lifecycle                | **main thread** (SKSE messaging)          | follows `GossipSim`'s, same job            |

### What stays on the plugin thread

The cadence check. `Tick` keeps accumulating unpaused real seconds and comparing against
`iGossipTickIntervalSeconds`; when the accumulator crosses, it enqueues a gossip tick job and returns
immediately. That check touches no gossip state and costs nothing, which is exactly why it does not need to
move.

### Deleting the time budget, and what bounds the work instead

`iGossipMaxMillisecondsPerTick` goes away. The drain runs until the due queue is empty.

Nothing waits on it, but "nothing waits" is not the same as "unbounded", so the actual bound should be
written down. Work in one drain is:

```text
events per tick ≤ (interval game-days / fGossipStepDays) × live carriers
                = (0.5 / 0.25) × live carriers
                = 2 × live carriers
```

Stamping each tick is what makes that tight. A tick advances the simulation by exactly one interval no
matter how late it runs, so the work per job is fixed by configuration rather than by how long the player
held down the wait key. `iGossipMaxEventsPerTick` (currently 5000, never observed binding) stays as a
runaway backstop — a count cap costs nothing and is the difference between a bug being slow and a bug being
a hang.

### Ticks are scheduled and stamped, never coalesced

A tick is due every `fGossipHarvestIntervalGameHours` of in-world time. Passing 24 hours with `T` crosses
two of those boundaries, and **two ticks must run** — not one that happens to have twice as much to do.
That is how the harvest accumulator already behaves, and it is the behaviour to preserve.

So the plugin-thread check does not ask "is gossip busy?". It asks "how many scheduled tick times have gone
by that I have not enqueued yet?", and enqueues one job per boundary crossed:

```text
every iGossipTickIntervalSeconds of unpaused real time, on the plugin thread:
    while (nextScheduledGameDay <= now && outstanding < kMaxOutstandingTicks) {
        GossipDispatch::Enqueue(GossipTick{ .asOf = nextScheduledGameDay });
        nextScheduledGameDay += intervalGameDays;
        ++outstanding;
    }
```

The queue itself is the backlog, and because `GossipDispatch` is serial and ordered the two ticks run in
schedule order with the first one's rumors already seeded before the second one looks at the world.

`kMaxOutstandingTicks` inherits the existing `kMaxOwedSweeps = 4`. Beyond that the schedule advances without
enqueuing and the drop is logged — the same policy as today, where `g_gameHoursSinceSweep` is clamped to
four intervals so a console time jump cannot queue a year of simulation.

### Each tick carries the time it was supposed to fire

This is what makes a delayed tick still a *correct* tick. A job stamped `asOf` reads the world as it stood
at `asOf` and **ignores anything timestamped later**, so it does not matter whether it ran on schedule or
forty seconds late behind two LLM calls. Whatever it declined to look at is not lost; it is simply the next
tick's business.

Concretely:

| Step | Filtered how |
| ------------------------- | -------------------------------------------------------------------------- |
| Memory qualification      | Rows whose `game_time` exceeds `asOf` are discarded before ranking          |
| Memory age                | Computed against `asOf`, not against the clock at execution time            |
| Claim stamping and expiry | `asOf` is the claim date and the expiry cursor                              |
| Simulation clock          | `g_simGameDay` is **set** to `asOf` rather than sampled from the game clock |
| Event drain               | Every carrier-step with `dueGameDay <= asOf`, processed at its own due time  |
| Rumor seeding             | The new rumor's `seededOnGameDay` is `asOf`                                 |

The simulation clock line is the substantial one. `g_simGameDay` stops being "sample the game clock, clamp
the delta, hope the pacing works out" and becomes "the scheduled time of the tick being executed". The
`kMaxGameDayDeltaPerPoll` clamp can then go: it existed to stop an unbounded burst after a time jump, and
the outstanding-tick cap now does that job at the scheduling layer, where it is expressible in ticks rather
than in days.

**Where it cannot be exact, and why that is acceptable.** `GetActorEngagement` reports engagement as of now
and offers no time filter, so the *ranking* of which actors to examine is unavoidably current even when the
memories examined are not. The effect is bounded: it can pick an actor whose engagement rose after `asOf`,
but every memory it then reads is still filtered, so it can only mis-prioritise, never leak future content
into a rumor. Milestone 4 deletes that call outright, at which point the horizon is exact.

### A tick advances the simulation in one step, and that is fine

Today `GossipSim::Poll` runs every couple of real seconds and advances the simulation by whatever slice of
game time has passed. Under this design the simulation advances once per scheduled tick — twelve game hours
at a stride.

The *model* is unaffected. Carrier-steps are already processed at their own `dueGameDay` rather than at the
current clock, which is precisely what makes catch-up correct today:

> Process AT the event's scheduled time, not at the current simulated time. This is what makes catch-up
> actually catch up: a carrier due on day 5.2 when the clock has jumped to day 6.0 reschedules from 5.2, is
> immediately due again, and works through its backlog inside this same drain loop.

So a twelve-hour stride produces exactly the same sequence of transmissions as a hundred small ones. What
changes is *when the memories get written*: in one batch per tick rather than trickled continuously. For a
system whose output is NPCs recalling that they heard something, batching is if anything the better fiction
— but it is a real change and worth watching in the first run.

### One snapshot, serving the dashboard and the co-save alike

`DashboardUIManager` calls `GossipSim::GetStats()`, `GossipSim::GetRumorViews()`, `GossipHarvest::GetStats()`
and `GossipClaims::Count()` from the `AsyncDispatch` worker. `OnSave` reads live state from SKSE's
serialisation thread. All of them take gossip mutexes today, and with the mutexes gone all of them need
another route.

They get the same one. The gossip thread publishes an **immutable snapshot** — a
`std::shared_ptr<const GossipState>` swapped atomically — **at the end of each tick job, and only there.**
Every outside reader loads the pointer and reads it with no lock and no wait.

Publishing only at job boundaries is deliberate. A snapshot taken mid-drain would show a half-advanced
simulation: some carriers stepped to the new game-day and some not, transmission counts that do not match
the carrier set they came from. Nobody needs to watch a tick happen. What a reader needs is a series of
consistent states, and a completed tick is exactly that.

**The snapshot is the full state, not a display projection.** Today's `RumorView` carries counts and
strings for the dashboard; the snapshot has to carry everything the co-save record needs — the carrier map
with each carrier's `heardOnGameDay`, `infectiousUntilGameDay`, generation and recovered flag, the pending
event queue, `g_nextRumorId`, `g_simGameDay`, the per-rumor conversation counters, and the claim ledger. The
dashboard's view becomes a projection *of* the snapshot rather than a separate thing.

The claim ledger is part of it. `GossipClaims` keeps its own co-save record — `'NEGC'` is frozen and there
is no reason to disturb it — but the record is written from the same snapshot as the simulation's, so the
two are always an image of the same instant. Split them and a reload could restore a rumor whose source
memory is no longer claimed, and the memory would be free to seed a second rumor about a happening already
going round.

The copy cost is not a concern at this scale: the largest run so far held 15 rumors, a maximum of 37 carriers
on any one of them, and ~55 queued events. That is a few hundred small trivially-copyable structs, once per
twelve game hours.

### Nothing blocks on the gossip thread

With a full-state snapshot in hand, the serialisation callbacks stop needing the gossip thread at all.

| Callback    | What it does |
| ----------- | -------------------------------------------------------------------------------------------- |
| `OnSave`    | Serialises the latest published snapshot. Never touches live state, never waits.                |
| `OnLoad`    | Deserialises into a fresh state object, publishes it as **pending**, cancels outstanding work.  |
| `OnRevert`  | Publishes an empty state as pending, cancels outstanding work.                                  |

The gossip thread adopts a pending state at the top of its next job, before doing anything else. So the
handoff is one-way in both directions — the gossip thread publishes snapshots outward, the serialisation
thread publishes states inward, and neither ever waits on the other.

### Cancellation tokens, because side effects leave our custody

Every enqueued tick carries a **cancellation token**. Loading a save cancels every token then outstanding —
queued and running alike. A queued job that finds itself cancelled when it pops off the queue does nothing
at all; a running job checks at each operation boundary, throws away everything it has accumulated, and
terminates without publishing.

Checking only at the *end* of a job would be enough to keep our own state consistent — the results would be
discarded and the loaded state would stand. It is not enough for anything else, and that is the point:

> **A carrier-step writes two memories into SkyrimNet's database, and those writes cannot be taken back.**

SkyrimNet's memory store is a SQLite file outside our co-save, unaffected by loading an earlier game. A tick
that keeps running after a load keeps writing memories, and every one of them lands in a world that has no
record of the rumor that produced it. Discarding our own results at the end does not help; the damage was
done at each `AddMemory` along the way. Cancelling at operation boundaries stops the writes from happening.

The same argument applies to the LLM calls. A tick that continues past a load spends a director call and
possibly a composer call producing a rumor for a world that no longer exists.

**Where the checks go:**

| Checkpoint | Stops |
| ------------------------------- | -------------------------------------------------------------- |
| Job entry, before anything      | A queued tick for the old world from running at all             |
| After each blocking LLM call    | An evaluation or composition result being acted on              |
| Between drain events            | Further `AddMemory` writes — the important one                  |
| Before publishing the snapshot  | A superseded state overwriting a freshly loaded one             |

**What cannot be cancelled**, stated so the bound is known: an HTTP request already in flight completes and
its response is discarded, and an `AddMemory` already issued has already landed. So the escape is at most one
carrier-step's pair of memories plus one wasted LLM round trip — against an uncancelled tick, which could
spend a full drain writing into the wrong world.

The token also gives shutdown a clean answer. `GossipDispatch::Stop()` cancels before joining, so a tick
blocked in an LLM call unwinds at its next checkpoint rather than holding the shutdown for the request
timeout.

This deletes the yield point along with the rendezvous — but not the ability to stop. The drain has no
mechanism for *pausing* on behalf of a waiting caller, because nothing waits any more; it does have a
mechanism for *abandoning*, because abandoning is sometimes necessary and end-of-job detection was too late
to be useful.

### What a snapshot-based save actually costs

A save captures the world as of the **last completed tick**, so up to one tick interval of simulation can be
lost on reload. That is a clean loss rather than a messy one: a tick's effects are either wholly in the
snapshot or wholly absent, so rumors, carriers and claims can never disagree with each other about what
happened.

The one genuine wrinkle is that SkyrimNet's memory database is **not** part of our co-save and is not rolled
back with it. A transmission in a tick whose snapshot never published has already written its two memories
into SkyrimNet's SQLite store, and reloading will not remove them. The listener can therefore be told the
same rumor a second time and end up with two memories of hearing it.

This is worth naming and not worth solving:

- It is bounded by one tick's transmissions.
- It already happens today for any reload at all — gossip state lives in the co-save, SkyrimNet's memories
  live in a database that persists across loads, and the two have never been transactional with each other.
- The symptom is a duplicate "I heard a rumor that…" memory, which is indistinguishable from an NPC being
  told the same thing twice by two different people — something the simulation does on purpose.

### The census writes from the snapshot too

`GossipSim::OnSessionEnd` fires at `kPreLoadGame` and writes the live-rumor census into the trace before
`GossipLog` closes its file. It reads the published snapshot and formats it on the calling thread — no
enqueue, no wait, and no possibility of `OnRevert` wiping the state out from under it, because the snapshot
it holds is immutable.

That does mean `GossipLog` keeps its mutex. Its writes come from the gossip thread during ticks and from the
main thread at session boundaries, and it is a file writer rather than simulation state — a mutex on a log
write is not what this milestone is trying to remove. The single-writer claim narrows accordingly: it covers
the simulation, not the trace.

### Ordering and shutdown

`GossipDispatch::Start()` at `kDataLoaded`, alongside the other dispatchers. Shutdown must stop it **after**
`AsyncDispatch`, because an in-flight LLM callback on the `AsyncDispatch` worker can still be trying to hop
onto the gossip queue; stopping gossip first would drop that job and leave a claim unsettled. Jobs enqueued
after `Stop()` are dropped with a warning, as `AsyncDispatch` already does.

Game pause needs no special handling: `Tick` already returns early when the game is paused, so nothing is
enqueued. A drain that began before the pause runs to completion, which is correct — it is simulating time
that has already passed.

---

## What this does not fix

Worth stating so the milestone is not credited with more than it does.

- **It does not make the simulation faster.** The same events are processed at the same per-event cost, and
  the same LLM calls take the same seconds. They simply stop being spread across polls, stop being stitched
  together with callbacks, and stop delaying anything else.
- **It does not reduce `AddMemory` pressure.** Two writes per transmission into a growing vector database
  remains the dominant cost and remains unaddressed.
- **It does not help the main thread.** Gossip never touched the main thread; the only engine reads in the
  simulation are `LookupByID` plus a life-state read, both already off-main. If a future stutter is traced to
  gossip, this milestone is not where the fix lives.

---

## Risks

| Risk                                         | Why it matters                                                                                     | Mitigation                                                                                                                                    |
| -------------------------------------------- | -------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| A tick outlives the world it started in      | It writes memories into SkyrimNet for a rumor the loaded save has no record of                     | Cancellation tokens checked at every operation boundary, above all between drain events                                                        |
| An LLM call hangs without timing out         | The gossip thread stops draining and outstanding ticks pile up to the cap, then start being dropped | SkyrimNet's request timeout is the backstop; the outstanding-tick counter must be decremented on the job's exit path, not on its success path   |
| The snapshot omits a field the co-save needs | Silent state loss across a save/load, visible only much later                                      | The snapshot IS the save format — one struct, serialised directly, rather than two parallel descriptions of the same state that can drift apart |
| A snapshot publish is missed                 | The dashboard silently shows old numbers, and staleness is hard to notice                          | One publish site at the end of the tick job, not scattered per-mutation                                                                       |
| Token churn introduces a mis-signed function | A gossip function taking `PluginThread::Token` compiles and quietly breaks the invariant           | The invariant is only worth having if it is complete; a mixed-token module is worse than either extreme                                        |
| A future caller reintroduces a blocking wait | The gossip thread holds an LLM call for seconds; anything that waits on it inherits that stall      | Nothing waits on it today, by construction. `MainThread::Run` keeps demanding a `PluginThread::Token`, so gossip cannot even reach the main thread — a compile error, not a rule |
| A save loses the in-flight tick              | Up to one interval of simulation is rolled back, while SkyrimNet's memories are not                | Accepted. Bounded by one tick, and the two stores were never transactional with each other anyway                                               |
| Two threads writing the trace                | Interleaved or lost gossip-log lines                                                               | `GossipLog` moves with everything else, becoming single-writer rather than mutex-guarded                                                       |

---

## Decisions taken

The questions this design opened have been answered; they are recorded here rather than deleted, because
each one had a live alternative.

1. **`GossipLog` moves onto the gossip thread.** Trace order then matches execution order, and its mutex
   goes with it. Its lifecycle calls ride along with `GossipSim`'s in the same job so a session's trace can
   never be opened or closed out of step with the state it describes.
2. **The snapshot is published once per gossip tick, at the end.** Never mid-job — see above.
3. **`AvailableContactShare` stays in the seeding loop.** Moving it up to qualification would test every
   candidate rather than the handful about to be evaluated: roughly 90 candidates times ~100 contacts per
   sweep instead of ~500 lookups total. Losing the mutex makes it cheaper, not cheap, and the gate is
   already in the right place — it exists to avoid *spending an LLM call*, and that is precisely where it
   sits.
4. **The whole tick is one job**, LLM calls included and blocking, and **ticks are never coalesced**. Two
   crossed interval boundaries enqueue two stamped jobs, because a tick is a unit of simulated time rather
   than a request to catch up to the present.
5. **`iGossipTickIntervalSeconds` is unchanged**, in meaning and in value. It gates how often the driver
   checks whether a scheduled tick time has gone by. Only the accumulator behind it relocates, from inside
   `GossipSim::Poll` to the `Tick` side, so that deciding *whether* to enqueue never touches gossip state.

6. **The blocking LLM call is gated on a `WorkerToken` concept**, not on `PluginThread::Token` and not on a
   per-worker overload. Exactly one function changes; `MainThread::Run` pointedly does not.
7. **Every tick carries the game time it was scheduled for, and ignores data stamped after it.** This is
   what decouples *when* a tick runs from *what* it processes, and it is why the queue can back up behind a
   slow LLM call without the simulation drifting.
8. **Serialisation goes through the snapshot, not through the gossip thread.** `OnSave` writes the last
   published state, and `OnLoad` / `OnRevert` publish a pending state inward. This replaced an earlier design
   in which all three blocked on a rendezvous and the drain carried yield points to shorten the wait; both
   are gone.
9. **One snapshot covers every piece of gossip state**, the claim ledger included. It keeps its own co-save
   record — `'NEGC'` is frozen and there is no reason to disturb it — but both records are written from the
   same snapshot, so claims and rumors can never be saved from different instants. A ledger snapshotted
   separately could let a reload resurrect a memory whose rumor still exists.
10. **Work items carry cancellation tokens, checked at every operation boundary** — not an epoch compared at
   the end. End-of-job detection keeps our own state consistent but cannot unwrite the memories an
   abandoned tick has already pushed into SkyrimNet's database, which is the only part of a tick's output
   that a load cannot roll back.

## Module structure

| Module            | Role                                                                                                                                                                              |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GossipDispatch`  | **New.** The dedicated worker and its serial FIFO queue. Mints `GossipThread::Token`. Owns the cancellation-token registry.                                                        |
| `GossipThread`    | **New.** The token type, mirroring `PluginThread` exactly.                                                                                                                        |
| `WorkerToken`     | **New.** The trait and concept naming which tokens may hold a blocking SkyrimNet call.                                                                                            |
| `GossipState`     | **New.** One struct owning every piece of mutable gossip state — rumors, queue, counters, claims. The unit that is snapshotted, serialised and swapped.                            |
| `GossipTick`      | **New.** The single linear job: harvest, evaluate, compose, seed, simulate, prune, flush, publish. Carries its `asOf` stamp and its cancellation token.                            |
| `GossipSim`       | Loses its mutex, its `Poll`, `kMaxGameDayDeltaPerPoll` and the per-tick time budget. Becomes the drain and the model, operating on a `GossipState` handed to it.                   |
| `GossipHarvest`   | Loses its mutex and its `Poll`. Becomes the sweep, called as step 1 of a tick with an `asOf` horizon.                                                                              |
| `GossipContent`   | Loses `Walk`, `Advance`, `Evaluate` and the whole continuation chain. Becomes a loop over the shuffled pool making blocking calls.                                                 |
| `GossipClaims`    | Loses its mutex. Its maps move into `GossipState`; it keeps `'NEGC'` and the ledger logic.                                                                                        |
| `GossipLog`       | Keeps its mutex — it is a file writer, and session-boundary writes still come from the main thread. Its `Poll` becomes step 5 of the tick.                                         |
| `Tick`            | Keeps the cadence check. Reduced to: accumulate, compare, enqueue one stamped job per crossed boundary, return.                                                                    |

---

## Implementation plan

Ordered so that **the thread moves last among the risky parts**. Steps 2–4 restructure ownership and
serialisation while everything is still single-threaded on `AsyncDispatch`, which means any bug they
introduce is a plain logic bug rather than a logic bug wearing a race as a disguise. Only once state has one
owner and one publication point does the work change threads.

Step 1 is pure addition and touches nothing existing. Step 7 is pure subtraction and is safe precisely
because everything before it has landed.

---

### Step 1 — `GossipDispatch`, `GossipThread::Token`, and the worker-token concept

- [x] Complete

**Goal:** The thread exists and can be given work. Nothing uses it yet.

1. `GossipThread::Token` mirroring `PluginThread::Token` — private constructor, non-copyable, non-movable,
   one `friend` for `GossipDispatch`'s job dispatcher.
2. `GossipDispatch` with `Start()` / `Stop()` / `EnqueueWork()`, modelled directly on `EvalDispatch`. One
   `std::thread`, one `std::deque`, one condition variable. `ScopedThreadRole(ThreadRole::Plugin)` at the
   top of the worker loop, held for the thread's lifetime. Exceptions swallowed and logged so a bad job
   cannot kill the worker.
3. The `is_worker_token` trait and `WorkerToken` concept, with the blocking
   `SkyrimNetAPI::SendCustomPromptToLLM` overload retargeted onto it.
4. `Start()` at `kDataLoaded` beside the other dispatchers; `Stop()` in shutdown **after**
   `AsyncDispatch::Stop()`.

`MainThread::Run` and `MainThread::FireAndForget` are deliberately left demanding `PluginThread::Token`.

**Verification:** enqueue a job that logs and confirm it runs on a distinct thread id from `AsyncDispatch`'s.
Confirm a job enqueued after `Stop()` is dropped with a warning rather than crashing. Confirm that a
deliberate `MainThread::Run` from inside a gossip job **fails to compile**, and delete the experiment.

Done. Three throwaway translation units were compiled against the real build flags:

| Probe | Result |
| ------------------------------------------------------ | ------------------------------------------------------- |
| Both tokens satisfy `WorkerToken`                       | compiles, exit 0                                        |
| `MainThread::Run` with a `GossipThread::Token`          | `error C2672: no matching overloaded function found`    |
| `static_assert(WorkerToken<Fake>)` on a hand-rolled type | `error C2338: static_assert failed`                     |

The second is the one that matters: gossip code cannot reach the main thread, so the deadlock in the risk
table has no expressible form. Note there is no shutdown path in `Plugin.cpp` today — `AsyncDispatch` and
`EvalDispatch` are never stopped either — so `Stop()`'s cancel-before-join ordering is written for whenever
one appears rather than exercised now.

---

### Step 2 — `GossipState`: one owner for every mutable field

- [x] Complete

**Goal:** Collect gossip's scattered file-static state into a single copyable struct, with no behaviour
change and no thread change.

Everything currently living as a `g_`-prefixed static in `GossipSim.cpp` and `GossipClaims.cpp` moves into
one struct: the rumor map, the event queue, `g_nextRumorId`, `g_simGameDay`, `g_lastGameDaySample`, the
session counters, the memory-claim map and the event-claim map. `GossipHarvest`'s pacing state stays out —
it belongs to the scheduler, not to the world.

The functions in all three modules take a `GossipState&` rather than reaching for statics. A single
file-static instance remains for now, still mutex-guarded, still on `AsyncDispatch`.

This is the step where the copy cost gets checked: the struct must be cheaply copyable, which means no
`std::function`, no owning pointers, and containers of trivially-copyable elements.

**Verification:** a full session behaves identically — same seeds, same transmissions, same trace, same
co-save round-trip. This step should be invisible in every log.

Done. `Rumor`, `Carrier`, `QueueEntry`, `EventQueue` and `EventClaim` moved from two anonymous namespaces
into `include/GossipState.h`, and the six `GossipSim` globals plus the two `GossipClaims` maps became
references into a single `GossipState`. Every function body is untouched — the references keep each name
spelled the way it was — so the diff is a relocation rather than a rewrite.

The instance is a **function-local static** behind `GossipSim::MutableState()`, not a namespace-scope
object. `GossipClaims` binds references into it during its own static initialisation, and the order between
two translation units is unspecified; construct-on-first-use removes the question rather than leaving it to
be reasoned about.

`GossipSim::Stats` survives as a projection built in `GetStats()`, since it carries derived fields
(`liveRumors`, `totalCarriers`, `queuedEvents`) that are counted rather than stored.

---

### Step 3 — Publish the snapshot; the dashboard reads it

- [ ] Complete

**Goal:** One `std::shared_ptr<const GossipState>` published at a single point, and the first reader off the
lock.

1. A published snapshot pointer, swapped atomically at the end of the combined gossip work in `Tick`.
2. `GossipSim::GetStats`, `GossipSim::GetRumorViews`, `GossipHarvest::GetStats` and `GossipClaims::Count`
   become projections **of a snapshot** rather than lock-taking reads of live state.
3. `DashboardUIManager` loads the pointer once per compose and derives everything it shows from that one
   image, so its numbers cannot disagree with each other.

`RumorView` survives as the dashboard's projection type; it stops being produced by walking live state.

**Verification:** the Gossip tab shows the same values it does today. Confirm the rumor list, carrier counts
and claim count are mutually consistent — they now come from one instant by construction, which is a
guarantee the current code does not make.

---

### Step 4 — Serialisation through the snapshot

- [ ] Complete

**Goal:** `OnSave`, `OnLoad` and `OnRevert` stop touching live state.

- `OnSave` serialises the published snapshot. Both `'NEGS'` and `'NEGC'` are written from **the same
  snapshot**, so claims and rumors are always an image of the same instant.
- `OnLoad` deserialises into a fresh `GossipState` and publishes it as **pending**.
- `OnRevert` publishes a default-constructed `GossipState` as pending.
- The pending state is adopted at the top of the next unit of gossip work, before anything else runs.

Record ids and versions are untouched. `'NEGS'` stays at version 5 unless the state struct's layout forces a
bump, in which case bump it and skip-and-log on mismatch as the existing code already does.

**Verification:** save with rumors in flight, reload, confirm carriers, generations, queued events and
claims all restore. Confirm a revert with no load leaves an empty world rather than a half-cleared one. Save
twice in a row without any gossip work between and confirm byte-identical records.

---

### Step 5 — Scheduled, stamped, cancellable ticks on the gossip thread

- [ ] Complete

**Goal:** The work changes threads. This is the step the milestone exists for.

1. **`Tick` stops calling gossip directly.** It accumulates against `iGossipTickIntervalSeconds`, samples the
   game clock, and enqueues one `GossipTick{ asOf }` per crossed `fGossipHarvestIntervalGameHours` boundary,
   up to `kMaxOutstandingTicks` (inheriting the existing `kMaxOwedSweeps = 4`). Beyond the cap the schedule
   advances without enqueuing and the drop is logged.
2. **The tick job runs harvest, drain, prune and flush in order**, on the gossip thread, against the single
   `GossipState`. Content generation still uses the asynchronous LLM path for now, with its callbacks hopping
   from `AsyncDispatch` onto `GossipDispatch` — Step 6 removes that.
3. **`asOf` is the horizon.** `g_simGameDay` is *set* to `asOf` rather than sampled; memory rows stamped
   after `asOf` are discarded before ranking; memory ages, claim dates and claim expiry all measure against
   `asOf`.
4. **Cancellation tokens.** Every enqueued tick carries one. `OnLoad` and `OnRevert` cancel all outstanding
   tokens; `GossipDispatch::Stop()` cancels before joining. Checks at job entry, after each LLM call returns,
   between drain events, and before publishing.
5. **Deletions:** `iGossipMaxMillisecondsPerTick` and the budget check; `kMaxGameDayDeltaPerPoll` and its
   catch-up clamp; the `catch-up: processed …` trace line, which described a mechanism that no longer exists.
   `iGossipMaxEventsPerTick` stays as a runaway backstop.

**Verification:** pass 24 hours with `T` and confirm **two** ticks run, in schedule order, with the first
one's rumors present in the second's `active_rumors`. Confirm the plugin thread's gossip work is now a
handful of microseconds per poll. Load a save mid-tick and confirm the trace shows the tick abandoning, with
no `TELL` lines after the load stamped for the old world. Confirm `Stop()` during an in-flight tick unwinds
promptly rather than waiting out the LLM timeout.

---

### Step 6 — Blocking LLM calls and the linear tick

- [ ] Complete

**Goal:** Delete the continuation machinery.

`GossipContent` switches to the blocking `SendCustomPromptToLLM` overload and becomes a loop:

```text
for each candidate in the shuffled pool:
    if cancelled: return
    claim it
    verdict = evaluate(candidate)          // blocks
    if cancelled: return
    settle the claim per the verdict
    if verdict == seed:
        bands = compose(candidate)         // blocks
        if cancelled: return
        seed the rumor, dated asOf
        break
```

`struct Walk`, its `shared_ptr`, and the `Advance` / `Evaluate` mutual recursion go. The `IsClaimed` and
`AreEventsClaimed` re-checks stay, demoted from load-bearing to belt-and-braces: with one job per tick on a
serial queue, two walks can no longer overlap, so the double-claim defect they were added for is now
unrepresentable.

**Verification:** a sweep that refuses its first candidate and accepts its second produces the same trace as
today. Confirm the `content:` lines for one tick are contiguous — no interleaving with another tick's, which
was the symptom that exposed the concurrent-walk bug. Confirm a forced LLM failure releases the claim and
moves to the next candidate.

---

### Step 7 — Delete the mutexes

- [ ] Complete

**Goal:** Collect the guarantee the previous six steps bought.

Every `std::scoped_lock` in `GossipSim.cpp`, `GossipHarvest.cpp` and `GossipClaims.cpp` goes, along with the
mutexes themselves. Their internal functions take `GossipThread::Token const&` instead of
`PluginThread::Token const&`, which is what makes the deletion provable rather than hopeful.

`GossipLog` keeps its mutex and its `PluginThread`/main-thread callers. It is a file writer, not simulation
state.

Add the seventh row to the enforcement table in [`../THREADING_MODEL.md`](../THREADING_MODEL.md), and a
fourth box to its diagram.

**Verification:** builds clean; a deliberate call to a `GossipSim` internal from `AsyncDispatch` **fails to
compile**, and the experiment is deleted. A long session produces identical behaviour to Step 6.

---

### Step 8 — In-game validation

- [ ] Complete

**[USER]**

Run on a save with live rumors already spreading, and pass a lot of time.

1. **Two ticks for 24 hours.** The count of `HARVEST` lines must match the number of intervals crossed, not
   the number of times you pressed `T`.
2. **Late ticks are still correct ticks.** With several ticks queued behind a slow LLM call, confirm each one
   reports memories and ages consistent with its own `asOf` rather than with the wall clock.
3. **Nothing stutters.** The whole point. Watch for hitches while a large drain runs.
4. **Saves are instant** regardless of what gossip is doing, including mid-drain and mid-LLM-call.
5. **Load mid-tick is clean.** Load an earlier save while a tick is running and confirm no rumor, claim or
   memory from the abandoned tick appears afterwards.
6. **The dashboard stays consistent**, and updates once per tick rather than continuously.
7. **Propagation is unchanged.** Reach, depth, settlement and hold counts should match the pre-refactor runs
   within noise. The model did not move; if the numbers move, something else did.
8. **Batched memory writes read acceptably.** Transmissions now land in bursts of one tick rather than
   trickling. Confirm this reads as news arriving rather than as a glitch.

---

## Done condition

Milestone 3 is complete when:

- All 8 steps are checked off and Step 8 passes.
- No mutex remains in `GossipSim`, `GossipHarvest` or `GossipClaims`.
- No non-gossip thread ever blocks on the gossip thread, in either direction.
- Two crossed interval boundaries produce two ticks, each reading the world as of its own `asOf`.
- A load cancels outstanding work, and nothing from an abandoned tick reaches SkyrimNet.
- `iGossipMaxMillisecondsPerTick`, `kMaxGameDayDeltaPerPoll` and the `Walk` continuation chain are gone.
- Propagation metrics match the pre-refactor runs within noise.

---

## Open questions

None. The design is settled and the plan above is ordered.
