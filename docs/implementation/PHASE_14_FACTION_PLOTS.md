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
> starting points, not measurements: **Step 9 replaces them** with figures from an offline harness, and Step 19
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
- A **faction roster** (`PlotFactions.ini`) declaring, per faction, how seniority inside it is determined. It
  is the source of truth for leadership, not an allowlist for participation.
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
subsystem writes memories into a save from Step 16 onward and should be opt-in until it has been played.

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
| `PlotFactionRoster`  | **New.** The participating-faction roster from `PlotFactions.ini`, and the three ranking methods.          |
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

Ordered so that **the simulation is fully observable before anything authored reaches it**. Steps 1–11 build
and validate a complete plot lifecycle whose objectives and plans come from a hardcoded stub table — which means
the casting distribution, the progress-race arithmetic and the terminal-state bookkeeping can all be argued
with at zero LLM cost before a single prompt exists.

That is the Phase 13 precedent and it is deliberate: gossip built a validation harness first, and that ordering
is what caught a propagation model tuning could not have fixed. The failure mode this guards against here is a
resolution model that never fails, or never succeeds, or quietly casts the same six NPCs forever — none of
which is visible once plausible LLM-written text is draped over it.

Steps 12–15 replace the stubs. Steps 16–19 attach output. Every step from 4 onward leaves the plugin runnable.

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
  presence gate fire in normal play. There are three of them — Steps 11, 15 and 19 — one closing each staged
  phase.

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
   `SkyrimNetAPI::SendCustomPromptToLLM` overload accepts it. Nothing calls it until Step 13.
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
`WorkerToken.h` — the world effects in Step 17 need the main thread, so they cannot be called from plot code
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

- [x] Complete

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

Done. `include/PlotModel.h` + `src/PlotModel.cpp` (the manifest, `Step`, `Plot`, and the display derivation),
`include/PlotState.h` + `src/PlotState.cpp` (`PlotState`, occupancy, counters, the RNG, and the
`MutableState` / `Snapshot` / `PublishSnapshot` trio). `Plots::Initialize()` runs at `kDataLoaded` before
`PlotDispatch::Start()`. Build clean.

**The console command in item 7 does not exist and cannot, as written.** `ConsoleCommand` is an *issuer* —
it compiles and runs a command as if the player typed it — not a registration surface, and this codebase has
no way to add a console command of its own. The established debug-action surface here is the dashboard's
JS→C++ bridge (`DashboardUIManager::OnDispatchAction` and its siblings), so the manual trigger becomes a
bridge action, wired up in Step 10 with the tab. The seeding itself exists now as `Plots::SeedDebugPlot`, which
is what Steps 3–6 actually need — all four are probe-verified and call it directly. Nothing is weakened: the
only step that needs a *human-usable* trigger is Step 11, and Step 10 lands first.

**Step 4's item 6 rests on the same mistaken premise** and is corrected there in the same way: force-tick
becomes a bridge action, not a console command.

Two decisions worth recording:

- **The objective is stored on the plot, not read off the plan's last step.** They duplicate each other, which
  is the point: adaptation rewrites the plan tail but may never change the destination, and holding the
  objective outside the rewritable structure makes that an invariant instead of a convention. It also keeps
  the card title stable across a re-plan.
- **The RNG lives in `PlotState`, unlike gossip's**, and is a bare `std::uint64_t` splitmix64 stream rather
  than a generator object. Gossip excludes its RNG as "a generator, not world state"; plots need reproducibility
  from a seed for the Step 9 harness and for bug reports, and a stream position that survives the co-save is
  what makes a reloaded save continue a run rather than silently reroll it. A `std::mt19937` would have put
  2.5 KB into every snapshot copy; one 64-bit field costs nothing.

`Conspicuousness` is a three-valued enum (`No` / `Sometimes` / `Yes`) rather than a bool, because open
question 1 is about exactly that distinction. `IsConspicuous` currently treats `Sometimes` as conspicuous —
the conservative reading, since the failure mode of the other choice is a scheme resolving implausibly under
the player's nose. When that becomes per-step, only the table and that one predicate move.

Probes, all deleted afterwards:

| Probe                                                    | Expected | Result                              |
| --------------------------------------------------------- | -------- | ----------------------------------- |
| `MutableState` with a `PlotThread::Token`                  | compile  | compiled                            |
| `MutableState` with a `PluginThread::Token`                | reject   | `error C2664: cannot convert arg 1` |
| Label/title derivation across all 8 manifest types         | run, 0   | passed                              |

The positive half of that pair is there deliberately: without it, the negative probe cannot distinguish "the
gate works" from "I misspelled the function".

The derivation probe checks more than the step asked for, because the extra cases were free once the harness
existed: every manifest id round-trips through `ParseStepType` (the membership test Step 13's validation will
rest on), off-manifest ids — including `"Locate"` with the wrong case and `"locate "` with a trailing space —
are rejected rather than guessed at, a step with no target renders the bare verb rather than a trailing space,
and `ProgressFraction` returns 0 on an unsized step rather than dividing by zero into the dashboard's progress
ring.

**No articles are inserted into derived text.** "Acquire Amulet of Kings" reads slightly stiff, but the
alternative is a grammar problem with no correct answer — "Silence the Maven Black-Briar" is worse than stiff,
and nothing in a Skyrim display name distinguishes a proper noun from a common one.

A third piece of tooling came out of this step and is kept:
`docs/implementation/tests/faction-plots/run-probe.{py,ps1}` compiles, links and **runs** a probe against real
plugin sources. Steps 4, 5 and 6 all turn on pure functions whose verification is what the code *does*, not
whether it compiles, and there is no test harness in this repo. It only works for translation units that are
genuinely engine-free — a source that calls into CommonLibSSE fails to link — and that failure is informative
rather than an obstacle, because it means the code under test is not the pure function the plan says it should
be. `PlotModel.cpp` links standalone, which is the first evidence that the model layer really is separable.

---

#### Step 3 — Co-save persistence

- [x] Complete

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

Done. `include/PlotSerialize.h`, `src/PlotSerialize.cpp` (the format), `src/PlotCoSave.cpp` (the SKSE half),
`Plots::StageLoadedState` / `TakePendingState`, and the save / load / revert wiring in `Plugin.cpp`. Build
clean, round-trip probe passing at 559 bytes for a two-plot state.

**Item 6 did not go far enough as I first wrote it, and the probe is what said so.** The reader and writer took
a byte sink and source as specified, but they still lived in a translation unit that included `logger.h`, and
`logger.h` needs the plugin's SKSE plumbing. The probe failed to build with a wall of `C2039: 'log': is not a
member of 'SKSE'` — which is precisely the outcome `run-probe.py`'s own documentation predicts for a source
that is not as pure as the plan claims. The fix was a real split rather than a workaround:

- `PlotSerialize.cpp` — the byte format and nothing else. No SKSE, no logging, no engine.
- `PlotCoSave.cpp` — the SKSE adapters and the save/load/revert entry points, which may log.

`ReadState` therefore had to stop logging its own version-mismatch, so it returns silently and `OnLoad`
reconstructs the reason for the log line. `PlotState::FindPlot` and `ActivePlotCount` moved into the header as
inline definitions for the same reason: they are operations on the struct's own data, and leaving them in an
engine-bound `.cpp` dragged the plugin's plumbing into every probe that wants the format.

That is worth stating plainly because it generalises: **"takes an interface" is not the same as "is testable",
and only running the probe distinguishes them.**

Probe results, one probe covering all four cases, then deleted:

| Case                                              | Asserted                                                            |
| ------------------------------------------------- | ------------------------------------------------------------------- |
| Identity resolver                                 | every scalar, plot, step and occupancy row equal field-for-field     |
| Resolver that drops the mastermind's FormID       | that plot dropped, count reported, the other plot intact             |
| Truncation at 7 cut points                        | rejected, and state left EMPTY rather than half-populated            |
| A record claiming a future version                | discarded, not guessed at                                            |

The truncation case is cut at several offsets rather than one, because the failure that matters stops halfway
through a plot rather than at a tidy field boundary.

Three format decisions, each made because the alternative fails quietly:

- **Step types are written as their manifest id, not the enum's numeric value.** Reordering `StepType` must not
  silently reinterpret every saved step in every existing save.
- **Counters are not saved.** They are session-scoped diagnostics; carrying them across a load would make
  "plots born" a number nobody can reason about.
- **An occupancy row pointing at a plot that was dropped is freed rather than restored.** Otherwise an NPC
  stays engaged forever in a plot that no longer exists — a leak that would be invisible until casting starved.

**Item 4's schedule rebase is Step 4's, not this step's.** There is no tick schedule to rebase yet; the only
clock this step owns is `PlotState::simGameDay`, which comes out of the save and is correct as saved. Step 4
already specifies the rebase in its own items 1–2, so nothing is lost — and a stub `OnSessionStart` that
rebased nothing would have been dead code pretending to be coverage. Until the tick exists, loaded state waits
in the pending slot; nothing reads live state before then, so it has nowhere to be wrong.

One rename landed here: `PlotModel::PlotState` (the enum) became `PlotModel::PlotStatus`, because it collided
with `NarrativeEngine::PlotState` (the struct) the moment a probe pulled both namespaces into scope. Qualified
code compiled fine either way, which is exactly why it was worth fixing before more code depended on it.

---

#### Step 4 — The tick schedule, as a pure function

- [x] Complete

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
6. Two manual triggers: force one tick immediately, and force N ticks in sequence. Phase A's validation is
   entirely a question of watching many ticks go by, and making that a button rather than an hour of waiting
   is what keeps Step 11 cheap enough to repeat after a tuning change. They enqueue through the normal
   scheduler so a forced tick is stamped and cancellable like any other.

   *(Corrected in Step 2: this said "console commands on the existing `ConsoleCommand` surface". There is no
   such surface — `ConsoleCommand` issues commands into the engine rather than registering them. These are
   dashboard bridge actions, like every other debug affordance in this plugin, and are wired to buttons in
   Step 10.)*

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

Done. `include/PlotSchedule.h` + `src/PlotSchedule.cpp` (the pure decision), `include/PlotTick.h` +
`src/PlotTick.cpp` (the poll and the job), `PlotTick::Poll(pt)` in `Tick.cpp`'s `PollOnPluginThread`, and
`Initialize` / `OnSessionStart` in `Plugin.cpp`. Build clean, probe passing every row.

Making the schedule a free function over plain numbers paid for itself immediately — three cases the plan did
not list turned out to matter, and all three were free to add once the harness existed:

- **The cap must account for work already in flight.** `Advance` takes `currentOutstanding` rather than
  assuming an empty queue, so a backlog that is already at the cap enqueues nothing while the schedule still
  advances.
- **A misconfigured interval must stall, not spin.** `fPlotTickIntervalGameHours` of 0 or negative yields no
  stamps and leaves the schedule untouched, rather than dividing by zero or emitting a boundary per poll.
- **An absurd clock reading must be capped before the cast, not after.** A `set timescale` stunt or a very
  long wait produces a boundary count that would overflow `std::size_t` on its way to being clamped.

The subtlest property is one the probe asserts explicitly: **the schedule advances past every boundary
crossed, including the ones whose work was skipped.** Advancing only past the enqueued ones would leave the
backlog permanently owed — the next poll would see the same overdue boundaries again and enqueue another
capped batch, forever. "30 days" is therefore checked for both `stamps.size() == cap` *and*
`newLastFired > now - interval`.

Two implementation notes:

- **The rebase is deferred to the next poll rather than done in `OnSessionStart`.** That hook runs on the main
  thread at `kNewGame` / `kPostLoadGame`, and the clock reading that matters is the one the plugin thread sees
  when it next looks. Setting a flag keeps every game-clock read on one thread. This is also where Step 3's
  item 4 lands, as recorded there.
- **A game-clock reading of 0.0 is treated as "no clock yet", not as a time.** `EngineUtils::GetCurrentGameHours`
  returns 0.0 before the `Calendar` singleton exists, and taking that as a real value would make the first
  genuine reading look like an enormous backlog.

`PlotTick::ForceTicks` is implemented and unwired, per the correction recorded in Step 2: it is a dashboard
bridge action, wired to buttons in Step 10. Forced ticks go through the same queue with the same stamps and
cancellation handles as scheduled ones, so nothing observed through them is an artefact of how they were
triggered — and the schedule is advanced to match, or the next real poll would re-run the same in-world time.

The job body is deliberately still almost empty: it installs any pending loaded state (which is where a load
that landed while the job sat in the queue takes effect, before anything reads live state), advances
`simGameDay`, logs, and publishes. Steps 5 and 6 fill in the middle. Cancellation is checked at three
boundaries in that short body already, because the reason for checking early is not how long the body is — it
is that anything the tick does outside our co-save cannot be un-done by the load that cancelled it.

---

#### Step 5 — Casting

- [x] Complete

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
actually spread across Skyrim — is Step 9's harness, not this step.

Done. `include/PlotCasting.h` + `src/PlotCasting.cpp` (pure: the ladder, the weighting, occupancy and
cooldowns) and `include/PlotPopulation.h` + `src/PlotPopulation.cpp` (engine-bound: builds the population from
`GossipGraph` plus faction ranks, and supplies the liveness predicate). `PlotPopulation::Build()` runs at
`kDataLoaded` after `GossipGraph::Initialize`. Build clean, probe passing.

The split is what made the step verifiable, and it is sharper than "takes parameters": **`PlotCasting.cpp`
does not include `Settings.h`, `logger.h`, `GossipGraph.h` or anything engine-shaped.** It even carries its own
six-line copy of splitmix64 rather than calling `Plots::NextRandom`, because that one lives in an engine-bound
translation unit and using it would have dragged the plugin's plumbing into every casting probe. That is a
deliberate duplication of a named constant and four lines of arithmetic, and it is worth it.

**`GossipGraph` does not expose its admitted-faction set**, only the fact that a personal edge is a
shared-faction one and the FormID of the faction behind it. The set is therefore recovered from the edges
themselves rather than by inventing a second prominence filter — one answer to "which organisations matter",
not two that can drift. Faction *ranks* are not in the graph at all and are read once at build time from each
`TESNPC`'s own faction list, on the main thread; doing it per tick would be an engine read from the wrong
thread as well as a waste.

Probe results, one probe, then deleted:

| Group                       | Asserted                                                                  |
| --------------------------- | ------------------------------------------------------------------------- |
| The agent ladder            | falls through rungs 1→2→3→4 in order as each rung is emptied               |
| Subordinate test            | an EQUAL-rank colleague is never a subordinate; a tie-less stranger never cast |
| Rung 4                      | the mastermind casts themselves, and their own occupancy does not block it |
| Occupancy                   | engagement blocks BOTH roles — one uniqueness constraint, not two          |
| Cooldowns                   | expire exactly on the stamp; the two roles' stamps are independent          |
| Liveness                    | nobody alive → nobody chosen; a dead mastermind still casts a live subordinate |
| Weighting (20,000 draws)    | rank outdraws no-rank, every member drawn, no NPC takes more than half     |
| Determinism                 | the same seed reproduces the same casting                                  |
| Rejects                     | occupied and cooling-down candidates recorded with their reason            |

Two things worth recording from the run:

- **The first failure was in the probe, not the code.** It asserted that an *actor* cooldown would show up
  when screening for the *mastermind* role. It correctly does not — that independence is the entire reason
  there are two stamps — so the assertion was rewritten to set the cooldown for the role being screened, and a
  converse assertion added: an NPC serving a mastermind cooldown is still castable as an actor. A probe that
  had "passed" here would have been asserting the bug.
- **The weighting curve is deliberately gentle** (base 1.0, +0.5 for any admitted membership, +0.75 per rank).
  A jarl should be likelier than a guard, not a hundred times likelier; steeper and the same handful of
  high-rank NPCs would scheme continuously with the cooldown as the only thing spreading the work. The probe
  pins both ends: an independent is still drawn more than 1% of the time, and no single NPC takes half. Step 9
  is where those numbers meet the real population.

`Release` starts only the cooldown for the role the NPC was actually holding. Starting both would bench
someone from masterminding because they ran an errand, which is exactly the conflation the two stamps exist to
prevent.

The ladder walks rung by rung and stops at the first rung with anyone on it, rather than drawing across all
rungs at once. Pooling them would let a large set of distant acquaintances drown out the one subordinate who is
the obvious choice.

---

#### Step 6 — The progress race

- [x] Complete

**[CLAUDE]**

**Goal:** Steps dispatch, accrue progress, and reach typed terminal outcomes, with the arithmetic isolated
enough to test.

1. A stub plan table — hardcoded objective-plus-ladder shapes exercising every step type and both
   conspicuousness values. Step 13 deletes it.
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

Done. `include/PlotResolution.h` + `src/PlotResolution.cpp` (pure: sizing, the progress and mishap rolls,
`AdvanceStep`), the tick orchestration in `PlotTick.cpp` (birth, dispatch, advance, adapt, reap), the skill
profile and presence check on `PlotPopulation`, and `TickRecord` plus the four sizing inputs on
`PlotModel::Step`. Co-save bumped to v2 to carry them. Build clean, probe passing.

**The probe found a real bug, and it is the kind that would never have surfaced in play.**

The per-tick ceiling caps any single tick at `fPlotProgressRollMax` (0.45) of the threshold, so a step needs at
least `ceil(1 / 0.45) = 3` ticks before success is even *arithmetically possible*. `SizeBudget` was returning
2 for a `Deliver` step at zero travel distance. That step could never succeed — not "was hard", could not
succeed — and it would have failed as a perfectly ordinary-looking timeout every single time. In a log full of
timeouts it is invisible; in the Step 9 harness it would have shown up as an inexplicably low success rate for
one step type and cost hours to trace.

The fix is `MinimumViableBudget(rollMaxFraction)` and a floor under `SizeBudget`, which now takes the ceiling
as a third parameter. That does **not** compromise the budget/threshold independence the design turns on: the
ceiling is a tuning constant, not a property of the target, so `SizeBudget` still cannot see
`targetImportance` and `SizeThreshold` still cannot see travel. The probe now sweeps every step type at every
travel distance and asserts every budget is winnable.

The probe's own first version was also wrong in an instructive way: it tried to verify "a step that finishes on
its last tick succeeds" with a one-tick budget, which the ceiling makes impossible by construction. The
tightest expressible case is a budget of exactly `MinimumViableBudget` with an actor rolling at the top of the
band, and that is what it asserts now.

Probe results, one probe, then deleted:

| Group           | Asserted                                                                             |
| --------------- | ------------------------------------------------------------------------------------ |
| Independence    | travel moves only the budget, importance only the threshold; both deterministic       |
| Winnability     | every type at every distance gets a budget in which the threshold is reachable        |
| Clamps          | over 100,000 rolls across the whole competence range, no roll is 0 and none clears the threshold |
| Held ticks      | `elapsed` advances, `progress` does not; held to exhaustion fails as a plain TIMEOUT   |
| Terminal steps  | are not advanced again                                                                |
| Timeouts        | carry the fraction actually reached, and one roll record per tick lived                |
| Last-tick finish| succeeds rather than expiring — the success test runs before the budget test           |
| Mishap          | disabled never fires; conspicuous raises it; competence lowers it but never to zero    |
| Held + mishap   | a held step is never caught — being caught while waiting for the player to leave is nonsense |

Design notes worth keeping:

- **The actor shifts the centre of the band they roll in, rather than replacing the roll.** A good actor still
  has bad days and a poor one still has good ones, which is what keeps a plot's outcome from being readable
  off its cast list.
- **A step dispatched this tick starts working next tick.** Sizing and rolling in the same tick would make the
  first tick of every step worth double.
- **Birth runs before any plot is advanced**, so a slot freed by a plot ending on this tick is not refilled
  until the next one. The budget check then reads the world as the tick found it.
- **The mastermind acting for themselves is not re-engaged.** They are already engaged in this plot as its
  mastermind; engaging them again would overwrite the role on their occupancy row and make the release start
  the wrong cooldown.

Two proxies are in place where the design wants real values, both marked in the code as such: travel distance
is same-hold / different-hold rather than a road-graph query, and target importance is faction rank. They
exist to make distance and importance *matter* so Step 9 can judge whether they matter by the right amount;
replacing them is a change to two small functions.

---

#### Step 7 — `PlotFactions.ini`: the faction roster

- [ ] Complete

**[CLAUDE]**

**Goal:** A faction's internal hierarchy is declared, not inferred. The roster says which factions plots know
the shape of, and how seniority inside each is determined.

This exists because the data does not support a general prominence heuristic. Step 5 weighted masterminds by
authored faction rank; measured against the export, only the College of Winterhold populates a rank ladder out
of the 857 unique NPCs in the population. Cell ownership turned out to measure property rather than authority
(innkeepers outrank jarls), and faction nesting fires for 65% of the population once location and occupant
factions are counted. What the data *does* support is a per-faction answer, declared once.

**The roster is the source of truth for faction LEADERSHIP, not an allowlist for participation.** A plot can
involve anyone; what the roster adds is knowing who outranks whom inside the factions it names. Everything
outside it falls back to what already exists — the gossip-derived admitted-faction set still makes an NPC
count as "in an organisation" and still feeds the agent ladder, it just carries no gradient, so its members are
peers. NPCs in no faction at all remain eligible independents at the base weight.

1. `statics/SKSE/Plugins/NarrativeEngine/PlotFactions.ini`, following `AttackerGroups.ini` in form and in
   failure behaviour: `[Faction:<id>]` sections, aligned `Key = Value`, **per-section validation** so one bad
   section is skipped with a named reason in the log while every other section still loads, and an unknown key
   warned about rather than treated as an error so a file written for a newer build still works on an older
   one.
2. Keys on every section:

   | Key           | Required | Meaning                                                                    |
   | ------------- | -------- | -------------------------------------------------------------------------- |
   | `Faction`     | yes      | EditorID of the **primary** faction — `CompanionsFaction`, not `CompanionsHarbingerFaction` |
   | `DisplayName` | yes      | Readable name, for the dashboard and for LLM context                        |
   | `RankMethod`  | yes      | `Rank`, `Marker` or `Explicit` — how seniority is derived                    |
   | `Enabled`     | no       | Default true; switch a faction off without deleting its section             |
   | `Member`      | no       | `<NpcEditorID>, <rank>` — a manual override. Repeatable. **Valid under every method**, not just `Explicit` |

3. `PlotFactionRoster`, the loader: resolves every EditorID through the same lookup the item pool will use,
   fails a section loudly and by name on an unresolvable `Faction`, and exposes the roster to the plot worker
   as immutable session state.
4. The shipped default roster covers the factions worth plotting inside, with at least one section per ranking
   method so the file documents itself.

**Verification:** `build.ps1 build` is clean. **Every EditorID in the shipped roster is confirmed to exist in
the Spriggit export** — looked up, never recalled, per `docs/VANILLA_RECORD_REFERENCE.md`. A probe feeds the
loader a file with, in turn: an unresolvable `Faction`, a missing `RankMethod`, an unknown `RankMethod`, an
unknown key, a section whose method-required parameters are absent, and a `Member` naming an NPC that does not
resolve — and asserts each produces a distinct outcome. The last is a **warn-and-skip on that line only**: a
mod that replaces General Tullius must cost you Tullius, not the whole Legion.

---

#### Step 8 — The three ranking methods, and casting through the roster

- [ ] Complete

**[CLAUDE]**

**Goal:** Seniority inside a rostered faction comes from that faction's declared method, and mastermind
weighting reads it instead of raw authored rank.

1. **`RankMethod = Rank`** — read the authored faction rank. Parameter: `MaxRank`, the top of the ladder, so
   standing can be expressed as a fraction of it.
   *The College of Winterhold: Savos Aren 6, Mirabelle 5, the masters 4, apprentices 3.*
2. **`RankMethod = Marker`** — seniority comes from membership of separate, more exclusive factions.
   Parameter: `MarkerFaction`, repeatable, **in descending order of seniority**. Everyone in the primary
   faction who is in none of them sits on the bottom rung together; an NPC in two markers takes the more
   senior.
   *The Companions: `CompanionsHarbingerFaction`, then `CompanionsCircle`, then the rest.*
3. **`RankMethod = Explicit`** — no derivation at all; the `Member` lines *are* the ladder. Anyone unlisted
   stays at the bottom.
   *The Imperial Legion: General Tullius at the top, Legate Rikke one below, everyone else unspecified.*
4. **`Member` overrides apply under every method**, layered on top of whatever the method derived. `Explicit`
   is therefore not a special case in the code — it is the degenerate one where nothing is derived and only
   the overrides remain. This is what lets a `Rank` faction correct a single NPC the authored data gets wrong
   without hand-writing its whole ladder.
5. **Standing is normalised to 0..1 per faction.** The top of a two-rung faction and the top of a seven-rung
   faction weigh the same: being the head of your organisation should mean the same thing whether that
   organisation kept a deep hierarchy or a shallow one. A single
   `PlotFactionRoster::StandingOf(npc, faction)` dispatches on the method and returns that figure, so casting
   never learns which method a faction used.
6. `PlotCasting::MastermindWeight` reads standing rather than `Member::factions[].rank`:
   - rostered faction → the roster's normalised standing;
   - non-rostered but gossip-admitted faction → the flat membership bonus, no gradient;
   - no faction → the independent's base weight.

   **In several factions, the HIGHEST standing wins** — not the sum, not the mean. Someone's weight should
   reflect the most authority they hold anywhere, and summing would make a well-connected nobody outrank a
   guild master. Note the deliberate asymmetry with item 7: weighting takes the **maximum** across factions,
   while subordinate selection takes the **union**. They are different questions — "how much can this person
   command" against "who can they command" — and unifying them would be wrong in both directions.
7. **A mastermind in several factions may draw subordinates from any of them.** The agent ladder pools
   candidates across every faction the mastermind belongs to, rostered or not, rather than picking one. The
   subordinate test becomes "lower standing than the mastermind in a faction they share" — which, inside a
   non-rostered faction where everyone is a peer, correctly yields nobody.

**Verification:** `build.ps1 build` is clean. A probe over a fabricated roster and population asserts each
method in isolation: `Rank` orders by authored rank and normalises against `MaxRank`; `Marker` puts the first
marker faction above the second and both above the unmarked, with an NPC in two markers taking the more
senior; `Explicit` honours the written ladder and leaves unlisted members at the bottom. Then the properties
that cut across methods:

- **A `Member` override wins under every method**, including on a `Rank` faction whose authored data disagrees.
- **The top of a two-rung faction and the top of a seven-rung faction score identically.**
- **A non-rostered faction produces no gradient** — its members are peers, and none is a subordinate of
  another — while still counting as membership for the weight and for the ladder.
- **An NPC in no listed faction is still drawn as a mastermind** sometimes.
- **A mastermind in two factions draws subordinates from both** (union), while their **weight comes from the
  higher-ranking of the two** (maximum) — the two rules must not be collapsed into one.
- **Membership of many low-rank factions never outweighs high standing in one.**

Then, against the **real** export rather than a fixture: assert that the shipped roster orders Ulfric
Stormcloak above Galmar Stone-Fist, Savos Aren above every other College member, and both above an NPC in no
listed faction.

---

#### Step 9 — Offline validation harness, and tuning the numbers

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
a model problem, and finding that here rather than in Step 11 is the entire point of the step.

**Item 1 is done. Items 2–5 are BLOCKED on a design decision — see below.**

`build-plot-population.py` is written and runs. It does not re-implement the Spriggit reader: Phase 13's
`build-social-graph.py` is 1,100 lines of hard-won correctness about a format with real traps in it, so it is
imported as a module and its graph reused. What this adds is the two things it has no reason to carry —
faction rank, and the authored skills `PlotResolution::Suitability` consults.

Extracting real data immediately caught three silent bugs in the extractor, each of which would have produced
a *plausible-looking but meaningless* simulation rather than an error:

| Bug                                                                                                | Would have looked like                                     |
| -------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| `PlayerSkills.SkillValues` is a LIST of `{Key, Value}`, not a mapping                                | every actor identically suited; the suitability half of the roll a constant |
| Most unique NPCs have no authored `Level` — they are `PcLevelMult` with a `CalcMinLevel`/`CalcMaxLevel` band | every actor identically competent |
| Faction rank read from `Fluff` instead of `Rank`                                                     | every membership rank 0 — see the correction below          |

The extractor now prints the distinct-value counts for skills and competence, and warns on a flat
distribution, because a flat distribution is the signature of reading the wrong field. Current output over the
vanilla export: **857 members, 454 in an admitted faction, 659 with personal ties, 31 distinct Speech values,
45 distinct competence values.**

##### Correction: an earlier version of this section claimed vanilla NPCs have no faction ranks

**That claim was wrong, and so was the evidence given for it.** It is recorded here rather than quietly
deleted because the mistake is instructive and the conclusion it supported has changed shape.

Spriggit writes Mutagen's `RankPlacement` as:

```yaml
Factions:
  - Faction: 01F259:Skyrim.esm
    Rank: 6              # OMITTED ENTIRELY when the rank is 0
    Fluff: 0x000000      # three unused bytes, ALWAYS zero
```

The extractor read `Fluff` and fell back to `Rank` only if `Fluff` was absent — which it never is. So it
returned 0 for every NPC in the game, including ones with a plain `Rank: 6` on the very next line. The
"verification" then compounded the error: it counted `Fluff` values under `Factions:` blocks, found 14,086
zeroes, and reported that as proof. It was measuring the padding.

Ranks are in fact plentiful. Across `Skyrim/Npcs`, faction memberships carry ranks distributed
`{-1: 753, 0: 11515, 1: 47, 2: 1, 3: 5, 4: 10, 5: 1, 6: 1}`.

**The C++ side was never affected.** `PlotPopulation::RankIn` reads `TESNPC::factions[i].rank`, which is the
same field Spriggit spells `Rank`. Only the offline extractor was wrong.

##### What the data actually says, and the design question that follows

With the field read correctly, the finding is narrower but still real. Within the **857 unique NPCs the plot
population actually covers**, only **14 memberships carry a rank above 0, across 2 factions**:

| Faction                      | Members in our population | Ranked | Admitted by the 3–40 size band? |
| ---------------------------- | ------------------------: | -----: | ------------------------------- |
| `CollegeofWinterholdFaction` |                        18 |     13 | yes                             |
| `CWPotentialAllyFaction`     |                         1 |      1 | no                              |

The College is a textbook hierarchy and the weighting works on it exactly as designed — Savos Aren 6,
Mirabelle Ervine 5, the masters at 4, the apprentices at 3. For everyone else in the population the rank term
is 0, so mastermind weight is `1.5` for a faction member and `1.0` for an independent.

Two things explain the gap, and they pull in opposite directions:

- **The size band excludes almost every rank-carrying faction.** The factions with real rank spreads are
  large: `JobMerchantFaction` (166), `IsGuardFaction` (463), `CWSoldierNoGuardDialogueFaction` (113),
  `CWImperialFaction` (288), `CurrentFollowerFaction` (40/40 ranked). Gossip's 3–40 band drops all of them,
  and plots inherited that band to get one answer to "which organisations matter".
- **Most of those factions are bookkeeping, not hierarchy.** `IsGuardFaction`, `GuardDialogueFaction`,
  `CWDialogueSoldierFaction`, `JobInnkeeperFaction`, `CurrentFollowerFaction` — their ranks encode a job or a
  dialogue variant, not authority. Widening the band would admit a great deal of rank that means nothing about
  commanding resources, which is worse than admitting none.

`JobJarlFaction` is the interesting exception in principle — jarls are precisely the "commands resources"
case — but no member of our population carries a rank in it.

So the design question is not "does rank exist" but **"is authored faction rank the right prominence signal,
given that in this population it describes one organisation?"** Candidates, none obviously right:

- **Keep rank, and accept that it discriminates only inside genuine hierarchies.** The College would produce
  richly-cast plots and everywhere else would fall back to the membership bit. Arguably honest: those are the
  places vanilla actually modelled a hierarchy.
- **Widen the size band for plots only**, with a hand-filtered exclusion list for the bookkeeping factions.
  Most faithful to the design's intent, and the most content to maintain.
- **Live rank rather than authored** (`Actor::GetFactionRank`), which reflects quest-set progression — but it
  is a per-actor engine read, changes during play, and is still 0 for most NPCs.
- **A different signal entirely** — faction count, membership of a small elite faction, `DispositionBase`, or
  a curated prominent-faction list.

---

#### Step 10 — The Plots tab and the step-chain widget

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
9. Wire the bridge actions Steps 2 and 4 left unwired: seed a debug plot, force one tick, force N ticks. These
   are what make Step 11 a short console-driven session rather than a play session.
10. A committed fixture at `dashboard/src/fixtures/plots.sample.json`, hand-built to exercise every node
    state, a plot that has adapted, a 15-node chain, and an empty list.

**Verification:** `npm run build` in `dashboard/` succeeds with no TypeScript errors. A `renderToString` probe
renders `PlotsTab` against the fixture and asserts: nine nodes for a nine-step chain; the failed node keeps its
number and carries the ✕; the live node's arc `stroke-dasharray` matches its progress fraction; the nodes left
of the cursor are numbered identically before and after an adaptation replaces the tail; the 15-node chain
emits a horizontally scrollable container rather than shrinking nodes; and the empty list renders the empty
state rather than throwing. Whether it *looks* right is Step 11's business.

---

#### Step 11 — Phase A in-game validation

- [ ] Complete

**[USER]**

**Goal:** Judge the model against the real population, the real map and a real save, before any content is
attached to it. This is what Step 9's harness was a model *of*.

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
the stub table. It starts applying at Step 15, and Step 15 says so.

Run with `bPlotsEnabled=true` and no LLM involvement anywhere, and judge from the Plots tab and the trace:

1. **Plots start, advance, adapt and terminate at sane rates** against the real population — and the rates
   match what Step 9's harness predicted. A divergence here means the harness models something the game does
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

Record findings under this step, including any tuning applied on top of Step 9's numbers.

---

### Phase B — LLM authoring

#### Step 12 — Target menus and the item pool

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

#### Step 13 — The birth prompt and its response handling

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

Whether the *content* is any good is Step 15's business; this step proves it cannot corrupt state.

---

#### Step 14 — Adaptation

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

#### Step 15 — Phase B in-game validation

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

#### Step 16 — The memory calls

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

#### Step 17 — World effects

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

#### Step 18 — Gossip seeding

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

#### Step 19 — Phase C in-game validation and call-volume measurement

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
5. **Measure the real LLM call rate** over a session and compare against Step 9's harness figure and the
   design doc's Part 3 estimate. Record the measurement.
6. **Confirm nothing outside the allow-list moved.** Inventories and relationships changed as expected; no
   other world state did.

If the call rate is too high, the lever is step duration — lengthening steps costs nothing narratively — not
fewer memories. Record findings and any tuning under this step.

---

## Done condition

This phase is complete when:

- All 19 steps are checked off. The three `[USER]` steps — 11, 15 and 19 — have their findings recorded in this
  document, and Step 9's validation log is committed.
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
- No engine state outside Step 17's allow-list is mutated.
- A caught failure is observed propagating as a rumor.

---

## Open questions

The feature's open questions are tracked in the design doc rather than duplicated here; two of them land inside
this phase and are built around rather than blocked on:

1. **Is the conspicuousness split right?** Built in Step 6 as a per-type constant from the manifest. If
   Step 9's harness or Step 11's play shows it needs to be a per-step property decided at dispatch, that is a
   change to one function's inputs, not to the model.
2. **How does a step get caught?** Step 6 implements the proposed per-tick mishap roll behind
   `bPlotMishapEnabled` precisely so the phase does not wait on the answer. If the shape changes, the switch
   and one roll are what move.

One question belongs to this document alone:

1. **Should `bPlotsEnabled` default true when Phase D lands, or stay opt-in?** Gossip defaults on. Plots write
   more consequential memories and mutate engine state, and the honest answer probably depends on what
   Step 19 measures.
