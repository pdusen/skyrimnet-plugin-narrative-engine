# Phase 14 — Faction Plots

The first tier-3 background simulation: unique NPCs adopt objectives, plan multi-step schemes toward them,
delegate the steps to people who owe them something, and adapt when a step goes wrong. Steps resolve over
in-world days whether or not the player is anywhere nearby, and what they produce is memory — on the
mastermind, on whoever did the work, and on whoever it was done to.

This phase covers **Phases A, B and C** of the feature's implementation staging: the headless simulation, the
LLM authoring that replaces its stub content, and the memory and world-effect output that makes it worth
playing. **Player delegation (Phase D) is not in this document** — no offer pool, no errand beat, no quest, no
ESP surface. Everything here is C++ plus prompts, observable from the log and the dashboard.

> **Doc status: planned, not built.** No step is checked off. Numbers in the settings table are proposed
> starting points, not measurements: **Step 7 replaces them** with figures from an offline harness, and Step 17
> checks those against real play. Two design-level questions (the conspicuousness split, and how a step gets
> caught) are still open in the design doc and are deliberately built behind switches here rather than blocked
> on.
>
> Every step is owned by either `[CLAUDE]` or `[USER]` and carries its own standalone verification; see
> **How steps are verified** at the head of the implementation plan.

---

## Why this phase exists

Gossip (Phase 13) spreads whatever high-notability memories already exist, and today those come almost
entirely from the player's own vicinity. The propagation model was tuned for a province-wide supply of notable
events and has been starved of one. Plots generate exactly that supply, with no player nearby.

Secondarily, this is the first system in the plugin whose output is a *pretext that already had a reason to
exist*. Every beat so far invents its own justification at compose time. A plot step is something the world
decided on its own, days earlier, because a specific person wanted it done. Phase D eventually hands those to
the player; Phases A–C are what make there be something to hand.

---

## Scope

### In scope

- The plot and step object model, the step manifest, and the occupancy table with per-role cooldowns.
- A dedicated worker thread and its token, mirroring `GossipDispatch` / `GossipThread`.
- The in-world tick scheduler: stamped, never coalesced, backlog-capped.
- Casting masterminds and step actors from the existing `GossipGraph` population.
- The progress-race resolution model, its typed outcomes, and the conspicuous-step presence gate.
- Co-save persistence and a dedicated trace log.
- The dashboard's **Plots tab**, shipping with the machinery it observes rather than trailing it.
- LLM-authored objectives, plans and adaptations, against pre-resolved target menus.
- Memory writes at step dispatch and step resolution, and the bounded world mutations.
- Gossip-seeding eligibility for plot output.

### Deferred (explicitly out)

- **Everything in Phase D.** Player delegation, the offer pool, the errand `IBeat`, the reusable CK quest, the
  disclosure budget, and the player-side standing consequences. The `PlotState` fields the offer pool will need
  are reserved in Step 2 so that adding it later is not a migration, but nothing populates or reads them.
- **MCM exposure.** INI only, per the design doc's "Later, unscheduled."
- Everything the design doc's own Deferred list rules out of the feature entirely.

---

## Design summary

The authoritative design is [`../design/FACTION_PLOTS.md`](../design/FACTION_PLOTS.md). It is a living document
and it wins over this one wherever they disagree; this section exists so a reader can follow the implementation
plan without holding both open, not to restate it.

In brief:

- **One mastermind per plot**, holding one of at most `iPlotMaxConcurrent` slots. Birth is opportunistic: any
  tick with a free slot picks a mastermind first, then generates a plot from *that person's* ambitions and
  memories. See Part 2.
- **A plan is a ladder of steps** drawn from a fixed **manifest** of eight step types, which is also the
  vocabulary the objective itself comes from — the last step of a plan is the one that accomplishes it. The
  objective is fixed for the plot's life; adaptation rewrites the path, never the destination. See Part 1.
- **Resolution is a race, not a coin flip.** A step is dispatched with a `budget` in ticks (from travel
  distance and step scale) and a `threshold` of work (from step type and target importance), independently
  derived. Progress accrues per tick on a roll modified by the actor; the step succeeds on reaching the
  threshold and fails when the budget runs out. See Part 6.
- **The LLM authors, dice resolve.** Calls happen at plot birth, at each adaptation, and at step dispatch and
  resolution for memory text — never per tick. Every noun the model can name comes from a pre-resolved menu;
  an off-menu answer rejects the plot rather than being repaired. See Parts 3 and 4.
- **A conspicuous step makes no progress while its actor or target is loaded near the player**, and `elapsed`
  advances anyway — so it fails by running out of time like any other step, with no special case. See Part 6.
- **Threading is the existing background-sim pattern, fourth instance.** The plugin thread does a cadence check
  and nothing else; a dedicated serial worker runs whole ticks and may block on LLM calls; a `PlotThread::Token`
  makes the state unreachable from anywhere else. See Part 11.

---

## Settings

New `[Plots]` block. `bPlotsEnabled` ships **false** through Phases A–C and flips when Phase D lands — the
subsystem writes memories into a save from Step 14 onward and should be opt-in until it has been played.

| Key                            | Proposed default | Meaning                                                        |
| ------------------------------ | ---------------- | -------------------------------------------------------------- |
| `bPlotsEnabled`                | false            | Master switch for the whole subsystem                          |
| `bPlotLogEnabled`              | true             | The dedicated trace at `NarrativeEngine_Plots.log`             |
| `fPlotTickIntervalGameHours`   | 12.0             | In-world hours between simulation ticks                        |
| `iPlotMaxOutstandingTicks`     | 4                | Backlog cap; past it the schedule advances without working     |
| `iPlotMaxConcurrent`           | 10               | The plot budget — slots drawn against by the birth rule        |
| `fPlotMastermindCooldownDays`  | 5.0              | In-world days before an NPC may mastermind again               |
| `fPlotActorCooldownDays`       | 1.5              | In-world days before an NPC may take another step              |
| `iPlotMaxAdaptations`          | 3                | Hard cap on re-plans per plot, regardless of what the LLM says |
| `iPlotStepHistoryCap`          | 12               | Steps retained in a plot's history                             |
| `fPlotTerminalRetentionDays`   | 7.0              | How long a finished plot stays before reaping                  |
| `fPlotProgressRollMin`         | 0.05             | Floor on an unblocked tick's progress, as a fraction of threshold |
| `fPlotProgressRollMax`         | 0.45             | Ceiling, so no step clears its threshold in one tick           |
| `bPlotMishapEnabled`           | true             | The caught-in-the-act roll; off while its shape is argued      |
| `fPlotMishapChanceBase`        | 0.04             | Per-tick mishap chance before conspicuousness and competence   |
| `iPlotRandomSeed`              | 0                | Seeds the plot RNG stream; 0 = nondeterministic                |

`fPlotProgressRollMin` and `fPlotProgressRollMax` are the two clamps Part 6 requires. They are expressed as
fractions of the step's threshold so that changing threshold sizing does not silently change step duration.

---

## Module structure

| Module               | Role                                                                                                      |
| -------------------- | --------------------------------------------------------------------------------------------------------- |
| `PlotThread`         | **New.** The token type, mirroring `GossipThread` exactly.                                                 |
| `PlotDispatch`       | **New.** The dedicated worker and its serial FIFO queue. Mints `PlotThread::Token`. Cancellation registry. |
| `PlotModel`          | **New.** `Plot`, `Step`, the step manifest and its per-type traits. Pure data and predicates, no engine.   |
| `PlotState`          | **New.** One struct owning every mutable field — plots, occupancy, counters, RNG. Snapshotted and saved.   |
| `PlotTick`           | **New.** The plugin-thread scheduler, and the linear job: birth, dispatch, resolve, adapt, prune, publish. |
| `PlotCasting`        | **New.** Mastermind and actor selection over `GossipGraph`; occupancy, cooldowns, rejection reasons.       |
| `PlotResolution`     | **New.** Budget/threshold sizing, the progress and mishap rolls, the presence gate, typed outcomes.        |
| `PlotContent`        | **New.** The LLM junctions — birth, adaptation, memory composition — plus menu construction and validation.|
| `PlotItemPool`       | **New.** The curated `Acquire` item pool, loaded and validated from its own INI.                           |
| `PlotEffects`        | **New.** The bounded world mutations, each marshalled through `MainThread`.                                |
| `PlotLog`            | **New.** The dedicated trace, flushed per line so it stays readable while a tick blocks on an LLM call.    |
| `Tick`               | Gains one `PlotTick::Poll(pt)` call. No elapsed-seconds argument.                                          |
| `DashboardUIManager` | Gains the plots block in `ComposeFullStateJSON`.                                                           |
| `dashboard/src`      | Gains `tabs/PlotsTab.tsx`, a `PlotChain` widget component, a `plots` `TabId`, and its `types.ts` slice.    |

---

## Persistence

One new SKSE co-save record, `kRecordTypeId = 'NEPL'`, `kRecordVersion = 1`. Written from the **published
snapshot** rather than the live state, so the saved image is one consistent instant — the argument
`GossipSim` makes for its own record.

It holds the active and not-yet-reaped plots (with their plans, histories and live step state), the occupancy
table including per-role cooldown stamps, the tick schedule's last-fired stamp, and the RNG stream position.
It does not hold anything derived from `GossipGraph`, which is rebuilt every session.

---

## Implementation plan

Ordered so that **the simulation is fully observable before anything authored reaches it**. Steps 1–9 build and
validate a complete plot lifecycle whose objectives and plans come from a hardcoded stub table — which means
the casting distribution, the progress-race arithmetic and the terminal-state bookkeeping can all be argued
with at zero LLM cost before a single prompt exists.

That is the Phase 13 precedent and it is deliberate: gossip built a validation harness first, and that ordering
is what caught a propagation model tuning could not have fixed. The failure mode this guards against here is a
resolution model that never fails, or never succeeds, or quietly casts the same six NPCs forever — none of
which is visible once plausible LLM-written text is draped over it.

Steps 10–13 replace the stubs. Steps 14–17 attach output. Every step from 4 onward leaves the plugin runnable.

### How steps are verified

Every step is a standalone unit with its own verification, and every step is owned by exactly one party:

- **`[CLAUDE]`** — implemented and verified without launching Skyrim. Verification is a clean
  `pwsh -File build.ps1 build`, a throwaway translation unit compiled against the real build flags and then
  deleted (the Milestone 3 precedent, including **negative**-compile probes that must fail with a named
  error), a probe driving a pure function over synthetic inputs, a committed fixture, an offline Python
  harness, or a lookup against the Spriggit export. A `[CLAUDE]` step's verification never says "run the game
  and look."
- **`[USER]`** — verified by actually playing. These exist only where the question genuinely requires the
  running game and real gameplay: does the world feel right, do NPCs talk about plots coherently, does the
  presence gate fire in normal play. There are three of them, one closing each staged phase.

Anything that needs the game but *not* gameplay — "launch Skyrim and confirm two threads have different ids" —
is a design smell, not a verification. Where such a check matters, the thing being checked is extracted into a
pure function or a compile-time property so a probe can settle it instead.

### How steps are executed

**Closing out a step.** A step is not finished when the code is written. Once both the implementation *and* the
step's own verification are done, two things happen before anything else is started:

1. **Mark the step Complete in this document** — tick its `- [ ]` to `- [x]`, and write up what the
   verification actually showed underneath it, including anything that turned out differently from what the
   step predicted. That write-up is the record; a checked box with nothing under it is worth nothing.
2. **Commit.** One commit per completed step, containing the implementation, the doc update, and any probe or
   fixture the verification produced. Throwaway compile probes are deleted before committing, with their
   result recorded in the doc instead.

**Working through the plan.** When told to implement, work **one step at a time, in order**, fully completing
each — implementation, verification, doc update, commit — before starting the next. Do not run steps in
parallel and do not start a step while an earlier one is unfinished. Keep going until one of these stops you:

- **The next step is `[USER]`.** Stop there and hand it over; those steps need the running game and cannot be
  done from here. Say which step is waiting and what it is asking the user to judge.
- **The plan as written is wrong or impossible**, in a way that cannot be resolved independently from what is
  already written down here and in the design doc. Stop and ask. Do not silently redesign around it, and do
  not implement a version of the step that is not what the plan describes — the plan is the contract, and a
  step that cannot be built as specified is information the design needs, not an obstacle to route around.

A step whose verification *fails* is not one of those stopping conditions: fix it and finish the step. Stop
only when the plan itself is at fault.

---

### Phase A — the simulation, headless

#### Step 1 — `PlotThread::Token`, `PlotDispatch`, and the settings surface

- [x] Complete

**[CLAUDE]**

**Goal:** The worker exists and can be given work, and the token discipline is enforced by the compiler.
Nothing uses it yet.

1. `PlotThread::Token` mirroring `GossipThread::Token` — private constructor, non-copyable, non-movable, one
   `friend` for the dispatcher's job dispatcher.
2. `PlotDispatch` with `Start()` / `Stop()` / `EnqueueWork()`, modelled directly on `GossipDispatch`. One
   `std::thread`, one `std::deque`, one condition variable. `ScopedThreadRole(ThreadRole::Plugin)` at the top
   of the worker loop, held for the thread's lifetime. Exceptions swallowed and logged so a bad job cannot
   kill the worker.
3. The `CancellationToken` / `CancellationHandle` registry, same shape as gossip's. `Stop()` cancels every
   outstanding token **before** joining.
4. Register `PlotThread::Token` with the `is_worker_token` trait so the blocking
   `SkyrimNetAPI::SendCustomPromptToLLM` overload accepts it. Nothing calls it until Step 11.
5. The `[Plots]` block in `Settings`, in `statics/SKSE/Plugins/NarrativeEngine.ini`, and in the INI's
   documented comment block. `bPlotsEnabled` default **false**.
6. `Start()` at `kDataLoaded` beside the other dispatchers; `Stop()` in shutdown after `AsyncDispatch::Stop()`.

**Verification:** `build.ps1 build` is clean. Three throwaway translation units compiled against the real build
flags and then deleted, with the expected result recorded here:

| Probe                                                          | Must                                  |
| -------------------------------------------------------------- | ------------------------------------- |
| `static_assert(WorkerToken<PlotThread::Token>)`                 | compile                               |
| `MainThread::Run` called with a `PlotThread::Token`             | **fail** to compile                   |
| `PlotDispatch::EnqueueWork` reached from a `PluginThread::Token`-only context | **fail** to compile     |

Plus a probe that default-constructs `Settings` and asserts every new `[Plots]` key holds the default this
document's table states — so the doc and the code cannot drift apart silently.

Done. `include/PlotThread.h`, `include/PlotDispatch.h`, `src/PlotDispatch.cpp`, the `[Plots]` block in
`Settings.h` / `Settings.cpp` / the shipped INI, the `is_worker_token` specialisation, and
`PlotDispatch::Start()` beside the other dispatchers in `Plugin.cpp`. `build.ps1 build` is clean.

**One probe in the table above was wrong as written and has been replaced.** The third row asked that
`PlotDispatch::EnqueueWork` fail to compile when reached from a `PluginThread::Token`-only context. That is
not a property the design has or wants: the scheduler enqueues plot work *from the plugin thread*, exactly as
`Tick.cpp` does for gossip, so making it uncallable there would break Step 4 before it was written. The
property that row was reaching for — plot state being unreachable without the plot token — belongs to
`MutableState` and is verified in Step 2. Substituted the Milestone 3 probe that does belong here: a
hand-rolled type must not satisfy `WorkerToken` merely by existing.

Three throwaway translation units were compiled against the real build flags and then deleted:

| Probe                                                       | Expected | Result                                            |
| ----------------------------------------------------------- | -------- | ------------------------------------------------- |
| `static_assert(WorkerToken<PlotThread::Token>)` (+ gossip, plugin) | compile  | compiled                                    |
| `MainThread::Run` called with a `PlotThread::Token`          | reject   | `error C2672: no matching overloaded function`    |
| `static_assert(WorkerToken<FakeToken>)` on a fabricated type  | reject   | `error C2338: static_assert failed`               |

The second is the one that matters: plot code cannot reach the main thread, so the deadlock this design could
otherwise suffer has no expressible form. Its consequence for Phase C is real and is now written into
`WorkerToken.h` — the world effects in Step 15 need the main thread, so they cannot be called from plot code
and must be marshalled by a `PluginThread::Token` holder.

Two pieces of tooling came out of this and are kept rather than thrown away, because Steps 2 and 15 both need
negative-compile probes again:

- `docs/implementation/tests/faction-plots/compile-probe.py` — compiles a probe TU against the flags pulled
  from the build's own `compile_commands.json`. It requires a negative probe to fail with the **specific**
  diagnostic asserted, not merely to fail. That mattered immediately: the first run of these probes reported
  two passes that were really `C1083 cannot open include file`, because the probes had not been given the
  build's forced-PCH include. A negative probe that accepts any failure silently stops testing anything.
- `docs/implementation/tests/faction-plots/compile-probe.ps1` — supplies the VS Developer environment, the
  way `build.ps1` does. Without it the toolchain cannot find `<type_traits>`.

For the settings check, a **three-way** consistency check turned out to be worth more than the probe this step
specified. A default lives in three places — `Settings.h`, the shipped INI, and this document's table — and
nothing in the build makes them agree; INI drift is the worst of the three, because a player reading it sees a
number the plugin is not using. `docs/implementation/tests/faction-plots/check-plot-settings.py` asserts all
three carry the same value for all 15 keys, compares by meaning rather than spelling (`12`, `12.0`, `12.0f`),
and fails on a key present in one place and missing from another. Currently passing.

`PlotDispatch::Start()` is called unconditionally, even with `bPlotsEnabled=false`. The worker is idle at rest
— nothing is enqueued unless the scheduler runs — and starting it unconditionally keeps the enable flag a
question about the simulation rather than about thread lifetime.

---

#### Step 2 — The object model, `PlotState`, and display derivation

- [ ] Complete

**[CLAUDE]**

**Goal:** Plots and steps exist as data with exactly one owner, and can render themselves as text without
touching the engine.

1. `PlotModel`: a `StepType` enum covering the manifest's eight entries, with `Deliverable`, `Conspicuous` and
   the display verb phrase as `constexpr` trait tables beside it rather than scattered `switch`es. `Step`
   carries type, target, agent, state, `budget`, `elapsed`, `threshold`, `progress`, and its typed outcome.
   `Plot` carries id, mastermind, ambition, objective, plan, cursor, history, state, and adaptation count.
2. `PlotState`: the plot array, the occupancy table (NPC FormID → `{plotId, role, availableAt}` per role), the
   counters the dashboard reads, and the RNG stream seeded from `iPlotRandomSeed`.
3. `PlotState& MutableState(const PlotThread::Token&)` and `std::shared_ptr<const PlotState> Snapshot()`,
   mirroring `GossipSim`'s pair. No mutex anywhere — the token is the whole safety argument.
4. Names cached as `std::string` when a plot is born — the mastermind, every step's actor, **and every step's
   target** — so the log and the dashboard never touch the engine to render a line. The
   `GossipGraph::Participant` precedent, and what makes the trace safe to write from the worker.
5. The two display derivations: a plot title (objective verb + target name) and a step label (step verb +
   target name). Pure string functions over cached names, taking no engine pointer.
6. Reserve the `PlotState` fields Phase D's offer pool will need (offer list, per-step offer stamp) so adding
   it later is not a co-save migration. Nothing writes or reads them.
7. A console command that hand-seeds a plot from the stub table, so later steps have something to act on.

**Verification:** `build.ps1 build` is clean. A negative-compile probe confirms `MutableState` cannot be called
with a `PluginThread::Token`, then is deleted. A probe constructs one synthetic plot per `StepType` and asserts
the derived title and every step label match an expected string table — which is what actually proves the
phrasing table is complete rather than defaulted for six of the eight types.

---

#### Step 3 — Co-save persistence

- [ ] Complete

**[CLAUDE]**

**Goal:** State round-trips losslessly, and a nonsense record degrades cleanly instead of corrupting the run.

1. `'NEPL'` at version 1, registered beside the other records.
2. Serialise **from the published snapshot**, not the live state.
3. Revive FormIDs through the serialization interface's resolver. A plot whose mastermind no longer resolves
   is dropped with a log line naming it, and its slot freed.
4. `OnSessionStart` (`kNewGame` / `kPostLoadGame`) rebases the tick schedule onto the current game clock, so a
   load does not read as an enormous backlog. The `GossipTick::OnSessionStart` precedent.
5. A load cancels every outstanding and running `PlotDispatch` job before the record is read.
6. Factor the read and write halves so they take a byte sink / source rather than reaching for
   `SKSE::SerializationInterface` directly — that is what makes the round trip testable off-SKSE.

**Verification:** `build.ps1 build` is clean. A probe writes a synthetic `PlotState` — several plots, mixed
states, a populated occupancy table with cooldown stamps, a live step mid-progress — into an in-memory buffer
and reads it back, asserting field-for-field equality. A second probe feeds a record whose mastermind FormID
does not resolve and asserts the plot is dropped and its slot freed rather than reviving half-formed. A third
feeds a truncated buffer and asserts the reader fails without leaving partial state behind.

---

#### Step 4 — The tick schedule, as a pure function

- [ ] Complete

**[CLAUDE]**

**Goal:** Ticks fire on the right in-world cadence with the right stamps, and the arithmetic that decides so is
testable without a game clock.

1. The schedule is a **pure function**: given the last fired stamp, the current game-hours reading, the
   interval and the outstanding cap, it returns the list of stamps to enqueue and the new last-fired value. It
   takes no engine pointer and no wall clock. This is the shape that makes the step verifiable at all, and it
   costs nothing.
2. `PlotTick::Poll(const PluginThread::Token&)` — **no elapsed-seconds argument.** It samples the game clock,
   calls that function, and enqueues what it returns. No real-seconds accumulator and no pause handling: a
   paused game does not advance the game clock.
3. Each job carries its `asOf` stamp and its cancellation handle, and reasons as of that moment rather than as
   of when it happens to run. Cancellation checked at every operation boundary, not only before publish.
4. Publish the snapshot at the end of a job, never during one.
5. `PlotTick::Poll(pt)` added to `PollOnPluginThread` in `Tick.cpp`. The job body logs its stamp and returns.
6. Two console commands on the existing `ConsoleCommand` surface: force one tick immediately, and force N
   ticks in sequence. Phase A's validation is entirely a question of watching many ticks go by, and making
   that a command rather than an hour of waiting is what keeps Step 9 cheap enough to repeat after a tuning
   change. They enqueue through the normal scheduler so a forced tick is stamped and cancellable like any
   other.

**Verification:** `build.ps1 build` is clean. A probe drives the schedule function over synthetic clock
readings and asserts each case:

| Clock advance since last fire | Expected                                                    |
| ----------------------------- | ------------------------------------------------------------ |
| 0 h (paused)                  | no stamps, last-fired unchanged                              |
| 11.9 h                        | no stamps                                                    |
| 12 h                          | exactly one stamp                                            |
| 24 h                          | exactly two, consecutive, 12 h apart — never one coalesced   |
| 30 days                       | exactly `iPlotMaxOutstandingTicks`, and last-fired jumped to now |
| clock moved backwards         | no stamps, and a rebase rather than a negative backlog       |

That last row is the one worth having: a save loaded from an earlier point moves the game clock backwards, and
without a case for it the subtraction underflows.

---

#### Step 5 — Casting

- [ ] Complete

**[CLAUDE]**

**Goal:** Plots and steps get people, by rules that can be checked against a synthetic population before they
ever meet the real one.

1. `PlotCasting` reads `GossipGraph::Participant` directly. **No second index of unique NPCs.**
2. Mastermind weighting by faction rank within the size- and name-filtered prominent-faction set gossip
   already builds. Independent NPCs eligible at a low but non-zero weight.
3. Agent selection ladder, in order: a subordinate with a personal edge to the mastermind → any subordinate →
   a personal edge without shared faction → the mastermind themselves.
4. Occupancy and cooldown are **one lookup**: the row that says who is busy carries the per-role
   "available again at" stamp, so the two cannot disagree.
5. Every rejected candidate and its reason (occupied, on cooldown, dead, no tie) recorded on the step.
6. The selection functions take a population view and a clock reading as parameters rather than reaching for
   `GossipGraph` and the engine internally, so a probe can hand them a fabricated population.

**Verification:** `build.ps1 build` is clean. A probe over a fabricated population asserts: an occupied NPC is
never returned for either role; an NPC inside its cooldown window is never returned, and is returned again one
tick after it expires; the agent ladder falls through each rung in order and lands on the mastermind when the
population offers nobody else; a rank-weighted draw over 10,000 trials puts high-rank NPCs ahead of
independents without ever returning an independent zero times. The real-population question — does casting
actually spread across Skyrim — is Step 7's harness, not this step.

---

#### Step 6 — The progress race

- [ ] Complete

**[CLAUDE]**

**Goal:** Steps dispatch, accrue progress, and reach typed terminal outcomes, with the arithmetic isolated
enough to test.

1. A stub plan table — hardcoded objective-plus-ladder shapes exercising every step type and both
   conspicuousness values. Step 11 deletes it.
2. Budget sizing from travel distance (via `TravelGraph` / `HoldGrid`) plus the step type's inherent scale.
   Threshold sizing from step type plus target importance. **They must share no input.**
3. The per-tick progress roll, modified by the actor's relevant skills and attributes and their suitability for
   the step type, clamped to `[fPlotProgressRollMin, fPlotProgressRollMax]` of the threshold.
4. The mishap roll behind `bPlotMishapEnabled`, weighted up by conspicuousness and down by competence.
5. The conspicuous-step presence gate: no progress on any tick where the actor or target is 3D-loaded near the
   player. `elapsed` advances regardless.
6. Typed terminal outcomes: `Succeeded`, `FailedTimeout` (carrying how far the actor got), `FailedCaught`.
7. Cursor advance on success; adaptation on failure against `iPlotMaxAdaptations` (stubbed here as "drop to
   the next hardcoded ladder"); plot `Succeeded` when the plan is exhausted; plot `Failed` on concession, on
   the cap, or when the mastermind dies. Slot released on any terminal state.
8. Resolved steps — **failed as well as succeeded** — appended to `history` in resolution order, capped at
   `iPlotStepHistoryCap`, retaining their roll history and sizing inputs rather than only their outcome. The
   dashboard renders `history ++ live ++ remaining plan` as one chain, so a failed step has to stay in the
   record at the position it occupied.
9. Sizing and rolling are free functions over plain inputs — no `PlotState`, no engine — so they are testable
   directly.

**Verification:** `build.ps1 build` is clean. Probes assert the design's own constraints, each of which is a
real bug if it fails:

1. **Independence.** Varying every threshold input leaves the budget unchanged, and vice versa. If this fails
   the race is theatre and every step has identical odds.
2. **Clamps hold.** Over 100,000 rolls, no unblocked roll is 0 and none reaches the threshold in one tick.
3. **A held tick advances `elapsed` and not `progress`,** and a step held for its whole budget terminates as
   `FailedTimeout` rather than living forever.
4. **`FailedTimeout` carries a progress fraction** that matches what actually accumulated.
5. **The adaptation cap terminates the plot** and releases its slot, no matter what the stub adaptation
   returns.

---

#### Step 7 — Offline validation harness, and tuning the numbers

- [ ] Complete

**[CLAUDE]**

**Goal:** Argue with the model's *behaviour over a long run* before it ever runs in-game, and replace this
document's proposed settings with measured ones.

This is the `docs/implementation/tests/gossip-spread/` precedent applied to plots. Step 6's probes prove
individual rules hold; this proves the system built from them produces a world worth playing.

1. `docs/implementation/tests/faction-plots/build-plot-population.py` — reads the Spriggit export at
   `C:\Projects\spriggit-output\` and emits the real unique-NPC population with faction ranks and
   relationship edges, mirroring `build-social-graph.py`.
2. `docs/implementation/tests/faction-plots/simulate-plots.py` — a Monte Carlo of the birth rule, casting,
   the progress race and adaptation over that population, at the settings this document proposes.
3. Report, over a simulated in-world year: the outcome mix (succeeded / timed out / caught), the distribution
   of plot durations and step counts, how many distinct NPCs mastermind and how many act, budget occupancy
   over time, adaptations per plot, and **the implied LLM call rate per in-world day**.
4. Commit the run as `PHASE_14_PLOT_VALIDATION_LOG.md` beside the scripts — the
   `PHASE_13_SIR_VALIDATION_LOG.md` precedent.
5. **Update this document's settings table with the tuned values**, and record what moved and why.

**Verification:** the harness runs to completion and its report is committed. The tuned settings satisfy: no
outcome class below ~10% or above ~70% of resolutions; at least a few hundred distinct masterminds over a
simulated year rather than dozens; budget occupancy neither pinned at 10 nor starving; and a call rate the
design doc's Part 3 estimate can be checked against. Any figure that cannot be brought into range by tuning is
a model problem, and finding that here rather than in Step 9 is the entire point of the step.

---

#### Step 8 — The Plots tab and the step-chain widget

- [ ] Complete

**[CLAUDE]**

**Goal:** Every decision the simulation made is on screen, summarised as a chain and expandable to the
arithmetic behind it.

1. A plots block in `ComposeFullStateJSON`, read from the published snapshot. Per plot: the derived title, the
   mastermind and ambition, the adaptation count, and **the chain** — one entry per node with its number,
   derived label, state, and, for the live node, `progress / threshold`. The expandable detail (roll history,
   sizing inputs, cast and rejects) rides along per node.
2. A `plots` `TabId`, `tabs/PlotsTab.tsx`, and its `types.ts` slice. Hidden via `TabBar`'s existing `hidden`
   set when `bPlotsEnabled` is false — the gossip-tab precedent.
3. The tab is a budget header above a **vertically scrolling list of plot cards**: active plots first, then a
   *recently ended* section for terminal plots retained until reaping.
4. `PlotChain` as its own component. A horizontal row of numbered circular nodes joined by connectors, a
   two-line label under each, a legend beneath the row. Four node states — completed (filled green), failed
   (muted, number legible under a red ✕), in progress (progress-ring arc plus percentage), pending (muted
   outline) — with connectors coloured to match. Draw the ring as an **SVG arc with `stroke-dasharray`**
   rather than a conic gradient.
5. **Colour is never the only signal:** the failed node carries a glyph, the live node its percentage.
6. The chain is built from `history ++ live ++ remaining plan`, numbered 1..N over that concatenation — **not**
   from the `plan` array, which adaptation rewrites. A chain wider than its card scrolls horizontally within
   the card; nodes keep their size.
7. Clicking a node expands that step's detail in place, following `GossipTab`'s expand-row pattern including
   its accessibility discipline — a real `<button>` for the toggle, the expanded panel outside it.
8. The delegation row is not built; the Phase D field is reserved and simply not rendered.
9. A committed fixture at `dashboard/src/fixtures/plots.sample.json`, hand-built to exercise every node state,
   a plot that has adapted, a 15-node chain, and an empty list.

**Verification:** `npm run build` in `dashboard/` succeeds with no TypeScript errors. A `renderToString` probe
renders `PlotsTab` against the fixture and asserts: nine nodes for a nine-step chain; the failed node keeps its
number and carries the ✕; the live node's arc `stroke-dasharray` matches its progress fraction; the nodes left
of the cursor are numbered identically before and after an adaptation replaces the tail; the 15-node chain
emits a horizontally scrollable container rather than shrinking nodes; and the empty list renders the empty
state rather than throwing. Whether it *looks* right is Step 9's business.

---

#### Step 9 — Phase A in-game validation

- [ ] Complete

**[USER]**

**Goal:** Judge the model against the real population, the real map and a real save, before any content is
attached to it. This is what Step 7's harness was a model *of*.

**This does not need real play — skipping time is the intended method.** Nothing in Phase A reads player
activity: births draw from the whole `GossipGraph` population, casting is population-wide, and the race is
actor skills against travel distance. Waiting, sleeping, or Step 4's force-tick command all advance the
simulation identically, and they are far faster than playing to the same tick count. Use the console commands.

Two things about where to stand:

- **Somewhere populated.** The one player-coupled rule is the presence gate, and it can only fire against
  actors whose 3D is loaded. Standing in a city holds a set of NPCs loaded for the whole run, which is the
  sharpest form of the case the design cares about — the "player parks in Riften for a week" scenario — and it
  is *better* isolated than wandering, where the loaded set changes constantly for reasons you are not
  tracking.
- **Move once, late.** Fast-travel somewhere else near the end and confirm a step stalled by your presence
  starts progressing again. That is the only observation the run needs you to change position for.

Note that the standing caveat about wait-driven test runs — that skipping time starves the memory supply and
decays the candidate pool — **does not apply to Phase A.** That caveat is about subsystems whose input is
memories generated by play; Phase A has no memory dependency at all, because objectives and plans come from
the stub table. It starts applying at Step 13, and Step 13 says so.

Run with `bPlotsEnabled=true` and no LLM involvement anywhere, and judge from the Plots tab and the trace:

1. **Plots start, advance, adapt and terminate at sane rates** against the real population — and the rates
   match what Step 7's harness predicted. A divergence here means the harness models something the game does
   not, and that is worth more than either number alone.
2. **The budget behaves.** Occupancy stays healthy, and a spell of long plots visibly suppresses births.
3. **Casting spreads.** The masterminds are people you recognise from around the province, not the same six —
   and the mix reflects the real faction data rather than the harness's reconstruction of it.
4. **The presence gate fires, matters, and releases.** Conspicuous steps involving the NPCs around you stall
   while you stand there and resume once you leave. Confirm a step held for its whole budget terminates as
   `FailedTimeout` and the mastermind adapts, rather than the plot hanging.
5. **The chain reads at a glance.** Plan lengths are legible, derived labels identify a step without
   expanding, and the expanded detail answers "why did that fail" without going to the log.
6. **A save/load mid-run resumes cleanly.** Step 3 proved the round trip in a buffer; this proves it against
   a real co-save, with plots mid-step and cooldowns pending.
7. **No stutter,** including while forcing many ticks in a row. Nothing waits on the plot thread, so there
   should be none; note tick durations regardless.

Record findings under this step, including any tuning applied on top of Step 7's numbers.

---

### Phase B — LLM authoring

#### Step 10 — Target menus and the item pool

- [ ] Complete

**[CLAUDE]**

**Goal:** Every noun the model can name is a resolved `TESForm` before the prompt is built.

1. Candidate actors from `GossipGraph`, filtered by relevance to the mastermind — shared faction, personal
   edge, same hold — rather than the whole province.
2. Candidate locations from `HoldGrid` / `TravelGraph`; candidate factions from the prominent set.
3. `PlotItemPool`: the curated `Acquire` pool in its own INI, following the `GossipFactions.ini` precedent —
   validated at load, failing loudly and by name on an unresolvable EditorID, with a shipped default roster.
4. Menus built per birth, carried into the prompt with stable indices.

**Verification:** `build.ps1 build` is clean. **Every EditorID in the shipped default item pool is confirmed to
exist in the Spriggit export** at `C:\Projects\spriggit-output\` — looked up, never recalled, per
`docs/VANILLA_RECORD_REFERENCE.md`; the roster is worthless if a third of it is invented. A probe feeds the
loader a file with one bad EditorID and asserts it fails naming that line. A probe builds menus for two
fabricated masterminds in different holds and asserts the candidate sets differ and neither is empty.

---

#### Step 11 — The birth prompt and its response handling

- [ ] Complete

**[CLAUDE]**

**Goal:** Real objectives and plans replace the stub table, and every way a response can be wrong is handled.

1. `narrative_engine_plot_birth.prompt` under `statics/SKSE/Plugins/SkyrimNet/prompts/`, following
   [`../CUSTOM_PROMPTS.md`](../CUSTOM_PROMPTS.md).
2. Handed the mastermind's identity, **their memories** via the SkyrimNet filtered memory query, the step
   manifest verbatim, and the target menus.
3. Returns an objective and an ordered plan in one response, every target given by menu index.
4. Validation is a **membership test**. Anything off-menu rejects the plot at birth with a log line and frees
   the slot.
5. `LLMTextSanitizer::Sanitize` on `ambition` and every other free-form field, at the point of extraction.
6. The call is **synchronous on `PlotDispatch`**. No async continuation chain.
7. Delete the Step 6 stub table.
8. Response parsing is a free function from JSON string plus menus to a plot-or-rejection, so it is testable
   without an LLM.

**Verification:** `build.ps1 build` is clean. A committed fixture set at
`docs/implementation/tests/faction-plots/fixtures/` drives the parser, one file per case:

| Fixture                  | Must produce                                                        |
| ------------------------ | -------------------------------------------------------------------- |
| well-formed              | a plot whose every target resolves to the menu entry it named        |
| target index out of range | rejection, slot freed, one log line                                  |
| step type not in manifest | rejection                                                            |
| malformed JSON            | rejection, no partial plot                                           |
| markdown-fenced JSON      | parsed — `StripMarkdownFences` already exists for this                |
| smart quotes, em-dashes, NBSP, accented Latin | plot text that is pure ASCII after the sanitizer     |
| empty plan                | rejection                                                            |

Whether the *content* is any good is Step 13's business; this step proves it cannot corrupt state.

---

#### Step 12 — Adaptation

- [ ] Complete

**[CLAUDE]**

**Goal:** A failed step re-plans instead of ending the plot, and cannot re-plan forever.

1. `narrative_engine_plot_adapt.prompt`, handed the plan so far, the typed failure, how far the actor got, and
   the still-valid menu.
2. Returns a revised remaining plan **or** a concession. The concession must be an explicit option in the
   response schema — without it the model invents alternatives forever.
3. The objective is **not** in the writable set.
4. `iPlotMaxAdaptations` backstops regardless of what the model returns.

**Verification:** `build.ps1 build` is clean. Fixture-driven probes assert: a revision replaces only the tail
and leaves `history` and the objective untouched; a concession terminates the plot and frees the slot; a
response attempting to change the objective is rejected rather than applied; a plot fed nothing but revisions
terminates at `iPlotMaxAdaptations` rather than looping.

---

#### Step 13 — Phase B in-game validation

- [ ] Complete

**[USER]**

**Goal:** Judge whether the authored content is worth attaching output to.

Run with authoring live and memory writes still off, on a save with an established memory corpus — plots are
generated *from the mastermind's memories*, so a thin corpus cannot show what this step is asking about.

1. **Do plots read as things that specific NPC would want,** given who they are and what they know? Read a
   dozen from the Plots tab.
2. **Do different masterminds produce different plots,** or has the prompt collapsed into one shape?
3. **Does the memory grounding show?** A mastermind who heard a rumor should sometimes be scheming about it.
4. **Does adaptation concede when it should,** or does it spiral until the cap catches it every time?
5. **Has anything off-menu ever been accepted?** The log answers this; it should be silent.

Record findings under this step.

---

### Phase C — memories and world effects

#### Step 14 — The memory calls

- [ ] Complete

**[CLAUDE]**

**Goal:** The feature starts producing its actual product.

1. `narrative_engine_plot_memory.prompt`: **one call composing every party's memory together**, not a call per
   person. The accounts have to be consistent with each other to be worth anything.
2. At **dispatch**, for the mastermind and the step actor. At **resolution**, for the same two plus the
   **target** conditionally on the outcome type.
3. The prompt is told it may let the two accounts diverge — a subordinate who failed has reason to shade the
   report.
4. Written via `SkyrimNetAPI::AddMemory` with a plot-output tag of their own — the
   `GossipHarvest::kOwnOutputTag` precedent.
5. `LLMTextSanitizer::Sanitize` on every string before it is written or stored.

**Verification:** `build.ps1 build` is clean. Fixture-driven probes over the response parser assert: a dispatch
response yields exactly two memories, addressed to the mastermind and the actor; a resolution response with an
outcome type saying the target noticed yields three, and one saying they did not yields two; every emitted
memory carries the plot tag; every emitted string is ASCII after the sanitizer; and a response naming a party
who is not in the step is rejected rather than written to whoever it named.

---

#### Step 15 — World effects

- [ ] Complete

**[CLAUDE]**

**Goal:** The two mutations the design allows, and provably nothing else.

1. `PlotEffects`, with every mutation marshalled through `MainThread::Run` / `FireAndForget`.
2. A successful `Acquire` puts an item from the curated pool into the actor's inventory.
3. A relationship-rank change between two NPCs, clamped by **named level**.
   `RE::BGSRelationship::RELATIONSHIP_LEVEL` runs **worst-is-highest** (`kLover = 0` through
   `kArchnemesis = 8`), the inverse of the CK's -4..+4 scale — clamping by sign would invert floor and ceiling.
   There is **no C++ setter**: `level` lives on a relationship *record* and `SetRelationshipRank` is a
   condition/Papyrus function, so budget for a boundary crossing here.
4. Establish what happens when no `BGSRelationship` record exists between the pair — the common case for two
   arbitrary NPCs. Either skip the rank half and write only the memory, or create the record. Decide before
   building, because "silently did nothing" is the default failure.
5. An explicit allow-list, asserted in code. `Discredit` and `Sabotage` resolve entirely in the memory layer.

**Verification:** `build.ps1 build` is clean. A probe drives the clamp over the full enum range and asserts it
saturates at the named floor and ceiling and never crosses into `kEnemy` / `kArchnemesis`, in both directions,
including when handed an already-out-of-range starting value. A probe asserts the allow-list rejects a
mutation kind not on it. A negative-compile probe confirms `PlotEffects`' mutating entry points cannot be
called with a `PlotThread::Token` — they require main — then is deleted.

---

#### Step 16 — Gossip seeding

- [ ] Complete

**[CLAUDE]**

**Goal:** Close the loop the feature was justified by.

1. Plot memories become eligible for `GossipHarvest`'s candidate set, with caught failures weighted as the
   high-notability events they are.
2. Confirm the tag interaction: plot output seeds gossip, gossip output does not seed plots, and plot output
   does not re-seed itself.

**Verification:** `build.ps1 build` is clean. A probe over synthetic memory rows asserts a plot-tagged memory
passes the harvest filter, a gossip-tagged one does not reach plot birth, and a plot-tagged memory is excluded
from the plot subsystem's own candidate set. The tag predicates are pure and this needs no game.

---

#### Step 17 — Phase C in-game validation and call-volume measurement

- [ ] Complete

**[USER]**

**Goal:** Confirm the feature is worth playing on its own, and find out what it actually costs.

Real play, subsystem fully live, over enough hours to see plots run end to end.

1. **Talk to an NPC who was cast in a plot.** Can they discuss it coherently — what they were asked, by whom,
   how it went? This is the whole feature; if it fails here nothing else matters.
2. **Check the two accounts diverge** where they should: find a subordinate who failed and ask both them and
   their mastermind about it.
3. **Confirm the target's memory is conditional.** Someone cleanly surveilled should not know; someone
   surveilled badly should.
4. **Watch a caught failure become a rumor** and reach another hold.
5. **Measure the real LLM call rate** over a session and compare against Step 7's harness figure and the
   design doc's Part 3 estimate. Record the measurement.
6. **Confirm nothing outside the allow-list moved.** Inventories and relationships changed as expected; no
   other world state did.

If the call rate is too high, the lever is step duration — lengthening steps costs nothing narratively — not
fewer memories. Record findings and any tuning under this step.

---

## Done condition

This phase is complete when:

- All 17 steps are checked off. The three `[USER]` steps — 9, 13 and 17 — have their findings recorded in this
  document, and Step 7's validation log is committed.
- A plot can be born, dispatch steps, adapt around a failure, and reach a terminal state without any
  intervention, over a normal play session.
- No LLM call happens on a per-tick basis; the junctions are exactly the five the design doc's Part 3 lists,
  minus the player-offer row.
- No off-menu target has ever been accepted, and a rejected birth frees its slot.
- `MutableState` is unreachable from any token but `PlotThread::Token`, and no plot module carries a mutex.
- Sleeping 24 in-world hours produces two stamped ticks; a console time jump is capped and logged.
- No NPC is ever double-booked, and both cooldowns are observed.
- The Plots tab renders one step-chain card per plot, expandable per node to the arithmetic behind it, and
  disappears entirely when the subsystem is disabled.
- A failed step stays in its position in the chain with the plot continuing past it, and an adaptation does not
  renumber the nodes behind the cursor.
- Every free-form LLM string reaching state, a memory, or the log has passed through `LLMTextSanitizer`.
- No engine state outside Step 15's allow-list is mutated.
- A caught failure is observed propagating as a rumor.

---

## Open questions

The feature's open questions are tracked in the design doc rather than duplicated here; two of them land inside
this phase and are built around rather than blocked on:

1. **Is the conspicuousness split right?** Built in Step 6 as a per-type constant from the manifest. If
   Step 7's harness or Step 9's play shows it needs to be a per-step property decided at dispatch, that is a
   change to one function's inputs, not to the model.
2. **How does a step get caught?** Step 6 implements the proposed per-tick mishap roll behind
   `bPlotMishapEnabled` precisely so the phase does not wait on the answer. If the shape changes, the switch
   and one roll are what move.

One question belongs to this document alone:

1. **Should `bPlotsEnabled` default true when Phase D lands, or stay opt-in?** Gossip defaults on. Plots write
   more consequential memories and mutate engine state, and the honest answer probably depends on what
   Step 17 measures.
