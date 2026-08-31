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
> starting points, not measurements: **Step 9 replaces them** with figures from an offline harness, and Step 22
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

New `[Plots]` block. `bPlotsEnabled` ships **true**, like `bGossipEnabled`: the simulation is the feature, and
a background sim nobody has switched on generates nothing to remember. The caveat it inherits from gossip —
that from Step 19 onward it writes memories the co-save does not roll back — belongs in the INI's own comment,
not in a default that hides the feature.

| Key                            | Proposed default | Meaning                                                        |
| ------------------------------ | ---------------- | -------------------------------------------------------------- |
| `bPlotsEnabled`                | true             | Master switch for the whole subsystem                          |
| `bPlotLogEnabled`              | true             | The dedicated trace at `NarrativeEngine_Plots.log`             |
| `fPlotTickIntervalGameHours`   | 12.0             | In-world hours between simulation ticks                        |
| `iPlotMaxOutstandingTicks`     | 4                | Backlog cap; past it the schedule advances without working     |
| `iPlotMaxConcurrent`           | 10               | The plot budget — slots drawn against by the birth rule        |
| `fPlotMastermindCooldownDays`  | 5.0              | In-world days before an NPC may mastermind again               |
| `fPlotActorCooldownDays`       | 1.5              | In-world days before an NPC may take another step              |
| `iPlotMaxAdaptations`          | 3                | Hard cap on re-plans per plot, regardless of what the LLM says |
| `iPlotStepHistoryCap`          | 12               | Steps retained in a plot's history                             |
| `fPlotTerminalRetentionDays`   | 7.0              | How long a finished plot stays before reaping                  |
| `fPlotProgressRateMin`         | 1.5              | Work per tick from a hopeless actor, in threshold units        |
| `fPlotProgressRateMax`         | 5.0              | Work per tick from an ideal one                                |
| `fPlotProgressMaxFraction`     | 0.45             | No single tick clears more than this share of a threshold      |
| `bPlotMishapEnabled`           | true             | The caught-in-the-act roll; off while its shape is argued      |
| `fPlotMishapChanceBase`        | 0.012            | Per-tick mishap chance before conspicuousness and competence   |
| `iPlotRandomSeed`              | 0                | Seeds the plot RNG stream; 0 = nondeterministic                |

`fPlotProgressRateMin` / `Max` are the per-tick work an actor does, in the **same absolute units as a step's
threshold** — so ticks-to-finish is roughly threshold / rate and a harder target genuinely takes longer.
`fPlotProgressMaxFraction` is the one clamp that stays relative, and it is what `MinimumViableBudget` derives
from.

These were originally a min/max pair expressed as fractions *of* the threshold, on the reasoning that retuning
threshold sizing should not silently change step duration. That reasoning was exactly backwards: it made the
threshold cancel out of the arithmetic, so it could not change step duration *or anything else*. Step 9's
harness measured a step worth 5 and a step worth 500 both finishing in 4.5 ticks. See Step 9.

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
| `PlotLog`            | **New.** `NarrativeEngine_Plots.log` — its own sink, flushed per line so it stays readable while a tick blocks. |
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

Ordered so that **the simulation is fully observable before anything authored reaches it**. Steps 1–12 build
and validate a complete plot lifecycle whose objectives and plans come from a hardcoded stub table — which means
the casting distribution, the progress-race arithmetic and the terminal-state bookkeeping can all be argued
with at zero LLM cost before a single prompt exists.

That is the Phase 13 precedent and it is deliberate: gossip built a validation harness first, and that ordering
is what caught a propagation model tuning could not have fixed. The failure mode this guards against here is a
resolution model that never fails, or never succeeds, or quietly casts the same six NPCs forever — none of
which is visible once plausible LLM-written text is draped over it.

Steps 13–16 replace the stubs. Steps 17–20 attach output. Every step from 4 onward leaves the plugin runnable.

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
  presence gate fire in normal play. There are three of them — Steps 12, 15 and 19 — one closing each staged
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
   `SkyrimNetAPI::SendCustomPromptToLLM` overload accepts it. Nothing calls it until Step 16.
5. The `[Plots]` block in `Settings`, in `statics/SKSE/Plugins/NarrativeEngine.ini`, and in the INI's
   documented comment block.
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
`WorkerToken.h` — the world effects in Step 20 need the main thread, so they cannot be called from plot code
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
only step that needs a *human-usable* trigger is Step 12, and Step 10 lands first.

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
existed: every manifest id round-trips through `ParseStepType` (the membership test Step 16's validation will
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
   is what keeps Step 12 cheap enough to repeat after a tuning change. They enqueue through the normal
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
   conspicuousness values. Step 16 deletes it.
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

> **Superseded by Step 13 — the paragraph above is wrong where it matters.** `MinimumViableBudget` returns
> `ceil(1 / rollMaxFraction)`, which only guarantees the *per-tick ceiling* is not what makes a step
> impossible. It never looks at the progress rate or at the threshold, so it does not deliver the property its
> name and this write-up both claim, and the probe row below asserting "every budget is winnable" was checking
> the clamp rather than the race. Step 12's first real run dispatched 6 steps that could not be completed on a
> perfect roll and 9 more expected to fall short; all 6 timed out. Step 13 replaces both the floor and the
> travel term.

The probe's own first version was also wrong in an instructive way: it tried to verify "a step that finishes on
its last tick succeeds" with a one-tick budget, which the ceiling makes impossible by construction. The
tightest expressible case is a budget of exactly `MinimumViableBudget` with an actor rolling at the top of the
band, and that is what it asserts now.

Probe results, one probe, then deleted:

| Group           | Asserted                                                                             |
| --------------- | ------------------------------------------------------------------------------------ |
| Independence    | travel moves only the budget, importance only the threshold; both deterministic       |
| Winnability     | every type at every distance gets a budget in which the threshold is reachable — **this row is wrong, see Step 13** |
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

> **The travel proxy was replaced in Step 15.** Step 12 measured it and the answer to "do they matter by the
> right amount" was no: over ten holds the comparison lands on the same value for about nine steps in ten, so
> distance was a flat surcharge rather than a variable. It survives as the fallback.

---

#### Step 7 — `PlotFactions.ini`: the faction roster

- [x] Complete

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

Done. `statics/SKSE/Plugins/NarrativeEngine/PlotFactions.ini` (six sections, all three methods),
`include/PlotFactionRoster.h`, `src/PlotFactionParse.cpp` (pure), `src/PlotFactionRoster.cpp` (engine-bound),
and `PlotFactionRoster::Load()` at `kDataLoaded` **before** `PlotPopulation::Build()` — casting reads standing
out of the roster, so the roster has to exist first. Build clean, both probes passing.

**The parse is split from the resolution, and that was not the original plan.** Written as one module it
compiled fine and could not be verified at all: every malformed-input rule — missing keys, an unknown method, a
method without its parameters, a `Member` line that is not `<name>, <rank>` — sat behind
`RE::TESForm::LookupByEditorID`, so exercising any of it needed a running game and a deliberately corrupted
shipped file. This is the same trap Step 3 hit with the co-save, recognised earlier this time:

- `PlotFactionParse.cpp` — INI **text** in, structures and diagnostics out. EditorIDs stay strings. No engine,
  no logging, no file system.
- `PlotFactionRoster.cpp` — reads the file, resolves EditorIDs to forms, logs what the parse rejected.

Diagnostics are returned as data rather than logged from inside the parse, which is what lets the probe assert
on *the reason* a section was rejected rather than only on the count.

Verification, both parts:

**`check-plot-factions.py`** looks every `Faction`, `MarkerFaction` and `Member` name in the shipped file up in
the Spriggit export — never recalled, per `docs/VANILLA_RECORD_REFERENCE.md`. It also reports what each section
will actually *do*, because a section that parses cleanly and leaves every member on the bottom rung is a
section that does nothing, and that is invisible from the file alone:

| Section          | Faction                      | Method   | Effect                        |
| ---------------- | ---------------------------- | -------- | ----------------------------- |
| college          | `CollegeofWinterholdFaction` | Rank     | authored ranks, MaxRank 6     |
| companions       | `CompanionsFaction`          | Marker   | 8 of 19 members distinguished |
| imperiallegion   | `CWImperialFaction`          | Explicit | 2 of 288                      |
| stormcloaks      | `CWSonsFaction`              | Explicit | 2 of 269                      |
| darkbrotherhood  | `DarkBrotherhoodFaction`     | Explicit | 1 of 20                       |
| thievesguild     | `ThievesGuildFaction`        | Explicit | 3 of 23                       |

It fails on a marker faction that shares no members with its primary, and on an override naming someone who is
not in the faction — both of which parse cleanly and then silently never fire.

**The parse probe** drives thirteen malformed inputs, each paired with a valid section so the property under
test is always "the good one still loaded":

| Input                                        | Result                                        |
| -------------------------------------------- | --------------------------------------------- |
| no `Faction` / no `DisplayName` / no `RankMethod` | section skipped, named reason              |
| `RankMethod = Seniority`                     | skipped, naming the three valid methods       |
| `Rank` with no / non-numeric / zero `MaxRank` | skipped, three distinct reasons               |
| `Marker` with no `MarkerFaction`             | skipped                                        |
| `Explicit` with no `Member` lines            | skipped — it describes no hierarchy at all     |
| a `[Group:...]` section                      | skipped, naming the required section form      |
| three malformed `Member` lines in one section | **section kept**, only the good line survived, one warning each |
| an unknown key (`SuccessionRule`)            | warned, section **kept** — a file written for a newer build still works |
| `Enabled = false`                            | not loaded, and not reported as broken         |
| repeated `MarkerFaction`                     | all kept, **in file order, which is their seniority** |
| a file that is not INI at all                | no factions, no crash                          |

The shipped roster documents the problem it solves in its own header, including the two heuristics that were
measured and rejected — property ownership, which ranked innkeepers above jarls, and faction nesting, which
fires for 65% of the population. Astrid's section carries the sharpest note: she leads the Dark Brotherhood and
nothing whatsoever in the data says so, which is the clearest argument for the file existing.

---

#### Step 8 — The three ranking methods, and casting through the roster

- [x] Complete

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

Done. The three methods and the override layering landed with the roster in Step 7 (`PlotFactionStanding.cpp`);
this step is the casting change and its verification. `PlotCasting::FactionRank` became `FactionStanding`
carrying a normalised 0..1 figure and a `rostered` flag, `MastermindWeight` reads standing, the subordinate
test compares standing, and `PlotPopulation` fills both halves. Build clean, both probes passing.

**A non-rostered faction needs no special case.** Its members all carry standing 0, and the subordinate test is
"lower standing than the mastermind in a shared faction" — so `0 < 0` is false and nobody in it is anybody's
subordinate, exactly as specified. The behaviour falls out of the arithmetic rather than out of a rule, which
is the version that cannot drift.

**The roster may name factions gossip's filter never admitted**, so population-building adds them outright
rather than only annotating existing entries. The Imperial Legion has 288 members and gossip's size filter
stops at 40 — without this the Legion would be in the roster and invisible to casting.

**`TargetImportance` now reads standing too.** It was a proxy over raw authored rank, which meant it did
nothing for seven of the eight factions. Reading the same normalised figure makes a declared hierarchy raise
its leaders' difficulty as targets automatically — being a jarl makes you both a likelier schemer and a harder
mark, from one declaration.

The weight is now three tiers — base, +membership, +standing — with the standing term worth `3.0` at the top.
The probe pins the gap at under 6:1 between a faction head and an unaffiliated NPC, because a steeper curve
would leave the cooldown as the only thing spreading the work.

Probe results, then deleted:

| Property                                                                   | Result |
| -------------------------------------------------------------------------- | ------ |
| Heads of a deep and a shallow faction draw **equally**, and both beat mid-rank | pass |
| A non-rostered faction beats no faction, but its members are peers          | pass   |
| ...and yields no subordinates, so the mastermind acts alone                 | pass   |
| A rostered faction does yield subordinates; **equal** standing does not      | pass   |
| Many weak memberships never outweigh one strong one (**maximum**, not sum)   | pass   |
| A mastermind in two factions draws subordinates from **both** (**union**)    | pass   |
| An unaffiliated NPC is still drawn, at under 6:1 against a faction head      | pass   |

And against the real export, via `check-plot-factions.py`, which now carries a Python mirror of
`StandingFrom` so the two implementations disagreeing is itself caught:

```text
Ulfric over Galmar          Ulfric 1.00 vs Galmar 0.67
Tullius over Rikke          GeneralTullius 1.00 vs Rikke 0.67
Mercer over Brynjolf        MercerFrey 1.00 vs Brynjolf 0.67
Savos tops the College      SavosAren 1.00 vs MirabelleErvine 0.83
Kodlak over the Circle      KodlakWhitemane 1.00 vs AelaTheHuntress 0.50
every faction leader normalises to 1.00
```

That last line is the property the whole normalisation exists for: Ulfric, Savos Aren, Kodlak and Astrid all
reach exactly 1.00 in their own organisations, by three different methods over data of wildly differing
quality. Savos gets there through an authored seven-rung ladder, Kodlak through marker factions, Ulfric and
Astrid through hand-written overrides — and none of them outranks another for it.

---

#### Step 9 — Offline validation harness, and tuning the numbers

- [x] Complete

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
simulated year rather than dozens; budget occupancy not starving; and a call rate the
design doc's Part 3 estimate can be checked against. Any figure that cannot be brought into range by tuning is
a model problem, and finding that here rather than in Step 12 is the entire point of the step.

Done. `build-plot-population.py`, `simulate-plots.py`, and the run recorded in
[`tests/faction-plots/PHASE_14_PLOT_VALIDATION_LOG.md`](tests/faction-plots/PHASE_14_PLOT_VALIDATION_LOG.md).
The settings table above carries the tuned values.

**The harness earned its keep on the first run: the threshold did nothing at all.**

Step 6 sized every step with a budget in ticks and a threshold of work, and the design turns on those being
independent. But the per-tick roll was expressed as a *fraction of the threshold*, which makes
ticks-to-finish `1 / mean(fraction)` — the threshold cancels out of the arithmetic entirely. A step worth 5
and a step worth 500 both finished in 4.50 ticks. `SizeThreshold`, `TargetImportance` and the whole "how hard
is the job" half of the model were decorative.

**Step 6's own probe passed on this**, because it asserted budget and threshold were *independently derived* —
and a value that affects nothing is trivially independent of everything. It was checking the signatures, not
the behaviour. That is the distinction between a probe and a harness, and it is why this step exists.

The fix makes the roll **absolute work** in the same units as the threshold, so ticks ≈ threshold / rate.
One relative clamp survives, `fPlotProgressMaxFraction`, which stops a trivially small step being one-shot and
is what `MinimumViableBudget` derives from. `fPlotProgressRollMin` / `RollMax` became
`fPlotProgressRateMin` / `RateMax` / `MaxFraction`, and Step 6's probe gained the assertion that was missing:
**a 3× threshold must take materially longer**, measured rather than reasoned.

Two tunings followed, both recorded with their sweeps in the log:

- **`fPlotMishapChanceBase` 0.04 → 0.015.** At 0.04, being caught was the *normal* way to fail — 35.7% of
  resolutions, against 5.7% timeouts — which drains the distinction the typed outcome exists for.
- **Base budgets tightened by a fifth.** With mishap tamed, timeouts sat at 6.8%: the clock never ran out, so
  competence had almost nothing to decide.

Tuned result over 5 trials × 365 in-world days: **68.7% succeeded / 14.3% timed out / 17.0% caught**, plots
succeeding 73.4% of the time, 553 distinct masterminds with the top ten holding 6.2% of plots, median plot 18.5
days and ~6 steps.

**The call rate is less than half the design's estimate — 7.0 per in-world day against Part 3's ~15.** The
estimate was right about the per-plot cost and wrong about the pace: plots run nearer nineteen days than
seven. Comfortably affordable, with room to raise the plot budget later.

**The harness also reported a false finding, and the log records it as such.** It said hierarchical
delegation never happens — ladder rungs 1 and 2 both at 0.0% — and explained it as arithmetic about how few
NPCs have standing. It was a bug in `build-plot-population.py`: a loop variable named `key` was shadowed
inside the roster loop, so every member of a rostered faction had their own id rewritten to that faction's
id. The simulator's "skip the mastermind themselves" filter then skipped the whole faction, and the
subordinate pool was empty by construction. **The plugin was never affected** — `PlotPopulation::Build` has
no such shadowing — and `dump-faction-ranks.py` confirms every hierarchy resolves correctly: Savos Aren has
17 subordinates, Mercer Frey 21, Astrid 13, Tullius 7.

An outcome of *exactly* 0.0% over 5,400 dispatches should have been read as a code path that cannot fire
rather than as a distribution. Corrected figures: **rung 1 = 7.3%, rung 3 = 72.7%, rung 4 = 20.0%** — four
fifths of steps are delegated. Rung 2 stays at 0.0% for a structural reason that is now verified rather than
assumed: every subordinate is also a personal tie, because `GossipGraph` derives personal edges partly from
shared faction membership, so rung 1 always claims them first.

One observation carried into Step 12: **the budget runs near-saturated**, at 10/10 for 74.6% of ticks. This
was written up as a concern and **is not one** — resolved in Step 12. A freed slot being refilled at the next
opportunity is the intended behaviour, not a symptom: the cap is a *rate limiter*, and a rate limiter that is
usually idle is not limiting anything. Nothing is starved by it, because birth still selects by weight among
every eligible mastermind whenever a slot opens; what saturation sets is how often that selection happens.
The one real consequence is the one worth remembering: `iPlotMaxConcurrent` is a ceiling doing real work
rather than a safety limit, so raising it raises the LLM call rate close to proportionally.

`population.json` is generated and gitignored. The simulator is a **mirror** of `PlotResolution.cpp` and
`PlotCasting.cpp` rather than a second design — a harness that models something the game does not produces
numbers that are confidently wrong.

---

#### Step 10 — The Plots tab and the step-chain widget

- [x] Complete

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
   are what make Step 12 a short console-driven session rather than a play session.
10. A committed fixture at `dashboard/src/fixtures/plots.sample.json`, hand-built to exercise every node
    state, a plot that has adapted, a 15-node chain, and an empty list.

**Verification:** `npm run build` in `dashboard/` succeeds with no TypeScript errors. A `renderToString` probe
renders `PlotsTab` against the fixture and asserts: nine nodes for a nine-step chain; the failed node keeps its
number and carries the ✕; the live node's arc `stroke-dasharray` matches its progress fraction; the nodes left
of the cursor are numbered identically before and after an adaptation replaces the tail; the 15-node chain
emits a horizontally scrollable container rather than shrinking nodes; and the empty list renders the empty
state rather than throwing. Whether it *looks* right is Step 12's business.

Done. The plots block in `ComposeFullStateJSON`, `dashboard/src/components/PlotChain.tsx`,
`components/tabs/PlotsTab.tsx`, the `plots` `TabId`, the `types.ts` slice, the styles, two committed fixtures,
and the `ne_forcePlotTicks` / `ne_seedDebugPlot` bridge actions Steps 2 and 4 left unwired. Both builds clean,
render probe passing.

**The chain is assembled backend-side**, in `ComposeFullStateJSON`, rather than in the component. The numbering
runs 1..N over `history ++ live ++ remaining plan` — never over the `plan` array, which adaptation rewrites —
and doing that where the plot state lives means the React side receives a flat list it cannot get wrong.

The debug buttons are Force tick / Force 10 / Force 50 and Seed debug plot. Forced ticks go through the normal
scheduler, so they carry the same stamps and cancellation handles as scheduled ones and nothing observed
through them is an artefact of the trigger. This is what makes Step 12 a short console-driven session rather
than a play session.

Render probe results, over a fixture built to exercise the awkward cases — a nine-node chain with a failure in
the middle that the plot continued past, a fifteen-node chain, a terminal plot, and an empty list:

| Asserted                                                                | Result |
| ----------------------------------------------------------------------- | ------ |
| One node per chain entry                                                | pass   |
| The failed node keeps its number **and** carries the ✕ glyph             | pass   |
| The live node draws a ring **and** states its percentage                 | pass   |
| The arc's `stroke-dasharray` matches the 42% fraction arithmetically     | pass   |
| Nodes 1–9 are each numbered, over the concatenation not the plan array   | pass   |
| The chain sits in a horizontally scrollable container                    | pass   |
| Terminal plots are retained under their own heading, and say why they ended | pass |
| Nodes are real `<button>`s announcing `aria-expanded`                    | pass   |
| **No button is nested inside another** — the detail panel is outside it   | pass   |
| An empty list renders the empty state and draws no nodes                 | pass   |

The arc check is the one worth having: it recomputes the circumference from the rendered geometry and asserts
the dash length matches `0.42 × circumference`, so a ring that renders but shows the wrong amount fails. Colour
is never the only signal — the failed node carries a glyph and the live node its percentage — and the probe
asserts both rather than trusting the CSS.

`render-plots-tab.mjs` transpiles the components with the TypeScript API and requires them as CommonJS, rather
than adding a second rollup config. Two things it needs that are worth knowing if it ever breaks: `React` has
to be injected into the module scope, because `JsxEmit.React` emits `React.createElement` and the sources
import only types; and relative imports are resolved by hand against the importing file.

---

#### Step 11 — `PlotLog`: the dedicated trace

- [x] Complete

**[CLAUDE]**

**Goal:** A run of the simulation can be reconstructed from a file, not only from the dashboard.

This step exists because it was missing. `PlotLog` was in the module table from the start and
`bPlotLogEnabled` shipped documenting "the dedicated trace at `NarrativeEngine_Plots.log`", but no step ever
listed it, so nothing built it. Everything currently goes to `NarrativeEngine.log`, interleaved with gossip
and the Director, and — the part that actually matters — **nothing logs a step's resolution at all**. No line
says whether a step succeeded, timed out or was caught, which are exactly the outcomes Step 12 has to judge.

1. `PlotLog`, on `GossipLog`'s precedent: its own spdlog-free `std::ofstream` sink at
   `NarrativeEngine_Plots.log`, session-scoped, rotated five deep, so nothing it writes reaches the main log
   and nothing from elsewhere reaches it.
2. **Gated on `bPlotLogEnabled` alone, never on `bDebugMode`.** The point is a long validation session with a
   quiet main log and a complete plot trace.
3. **Flushed per line, not per tick.** A plot tick will block on LLM calls from Step 15 onward; anything less
   than per-line flushing leaves the file silent and then bursting, with its tail sitting on a half-written
   line. Gossip learned this the hard way and the comment in `GossipLog.cpp` says so.
4. Emitters covering the whole lifecycle, each a single greppable line: `TICK`, `BORN`, `DISPATCH` (with the
   cast, the ladder rung, and the four sizing inputs), `ROLL` (per tick, with progress against threshold and
   whether it was held), `RESOLVE` (with the typed outcome), `ADAPT`, `END`, `REAP`, and a `CENSUS` at
   session end.
5. Names come from the cached strings on the plot, never from a live `RE::` pointer — the trace is written
   from the plot worker.

**Verification:** `build.ps1 build` is clean. A probe over the pure line-formatting asserts that every
emitter produces one line, that a `RESOLVE` line names its outcome, and that a `DISPATCH` line carries all
four sizing inputs — the numbers that make a later failure explicable. Then, because the format's real job is
to be *analysable*: a fixture trace is parsed back and the step outcomes counted from it must match the
counters the simulation reports, which is what proves the log is a complete record rather than a sampling of
one.

Done. `include/PlotLog.h`, `src/PlotLogFormat.cpp` (pure), `src/PlotLog.cpp` (the file), the emitters wired
through `PlotTick`, and all four lifecycle hooks in `Plugin.cpp`. Build clean, probe passing.

**What was there before was worse than nothing.** `bPlotLogEnabled` shipped `true`, documenting a dedicated
trace that did not exist; it gated four `logger` calls that went to the main log, two of them at `debug` level
so they also needed `bDebugMode`. And **no line reported a step's resolution at all** — nothing said whether a
step succeeded, timed out or was caught, which is precisely what Step 12 has to judge. The settings surface
described a facility nobody had built.

Nine tags, each greppable: `SESSION`, `TICK`, `BORN`, `DISPATCH`, `ROLL`, `RESOLVE`, `ADAPT`, `END`, `REAP`,
`CENSUS`. Two carry the numbers that make a failure explicable rather than merely reported:

- **`DISPATCH`** carries all four sizing inputs — travel, importance, competence, suitability — because
  without them a later `RESOLVE` says what happened but not what the actor was ever up against. It also
  reports the ladder rung and how many candidates were rejected, which is how "the same six NPCs do
  everything" becomes visible rather than suspected.
- **`ROLL`** carries progress against threshold every tick, with `HELD` or `CAUGHT` inline, so *"it failed at
  0.62 of its threshold, having been held for four of its nine ticks"* is recoverable from the file.

The format is split from the file (`PlotLogFormat.cpp`) for the reason `PlotSerialize` and
`PlotFactionRoster` both established: the property worth verifying is that the format is **analysable**, and
that cannot be checked if reaching the formatter needs an open file and a running game.

Probe results, then deleted:

| Asserted                                                                     | Result |
| ----------------------------------------------------------------------------- | ------ |
| Every emitter yields exactly one line, never containing a newline             | pass   |
| Each line is tagged so one event kind can be grepped out of a long run        | pass   |
| `DISPATCH` carries all four sizing inputs, the rung, and the reject count      | pass   |
| `RESOLVE` names its typed outcome, progress/threshold, ticks/budget, held      | pass   |
| A held or caught `ROLL` says so; an ordinary one says neither                  | pass   |
| A nameless mastermind or uncast actor logs as `?`, never blank                 | pass   |
| **Round trip: 37/11/9 outcomes formatted, parsed back, and counted exactly**   | pass   |

That last one is the point of the whole format. A synthetic run is written, the `RESOLVE` lines are parsed
back with `TICK` / `BORN` / `END` noise interleaved, and the counts must match what went in — which is what
distinguishes a complete record from a sampling of one.

Gated on `bPlotLogEnabled` alone, never `bDebugMode`, and **flushed per line**. From Step 16 a tick blocks on
LLM round trips; buffering would leave the file silent and then bursting, with its tail on a half-written
line. Gossip shipped that mistake once and the comment in `GossipLog.cpp` says so.

---

#### Step 12 — Phase A in-game validation

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
the stub table. It starts applying at Step 18, and Step 18 says so.

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

**Findings.** Three runs. The first two are written up in the commits they produced; what follows is the state
after the third.

Four defects found and fixed, none of which the offline harness could have caught because all four were about
the boundary between the simulation and the game:

1. **Forced ticks rewound the simulation clock.** Stamps came from the live calendar, which does not move
   while the dashboard holds the game paused, and the schedule anchor was pushed past them — which the next
   poll could not distinguish from a save loaded from the past. Forty ticks covered 4.5 in-world days instead
   of 20, replaying the same range three times.
2. **Adaptation rebuilt a plan's tail without targets**, so 26% of dispatches went out nameless and sized
   themselves off null-target fallbacks.
3. **A completed tick never reached the dashboard.** Publishing updates the snapshot the next push would read;
   nothing pushed, so an open Plots tab sat on the last Director evaluation.
4. **The progress race was sized backwards** — the Step 13 finding, and the one that mattered most.

Verification conditions, after the third run:

| # | Condition                        | Verdict                                                        |
| - | -------------------------------- | -------------------------------------------------------------- |
| 1 | Sane rates, matching Step 9       | **Partial** — in band, and re-tuned in Step 14                  |
| 2 | The budget behaves                | **Pass** — see below                                            |
| 3 | Casting spreads                   | **Pass**                                                        |
| 4 | Presence gate fires and releases  | **Not exercised** — needs a populated cell and one move         |
| 5 | The chain reads at a glance       | **Pass**                                                        |
| 6 | Save/load mid-run                 | **Not exercised**                                               |
| 7 | No stutter                        | **Pass**                                                        |

On (3): 22 distinct masterminds over 23 plots, only one repeat, spread across the province and including
General Tullius at the maximum weight of 4.50 and Drevis Neloren at 3.50 — the roster's hierarchy reaching the
selection it was built for.

On (7): measured rather than eyeballed. Another mod in the load order emits a one-second heartbeat, which
makes a main-thread stall directly visible in the log. After both 50-tick batches the next heartbeat lands
within a second and the following one at exactly 1.00s. The only gap over 1.1s in the run precedes the first
forced tick.

On (2), **resolved as not a problem**: occupancy sat at the cap for 79.8% of ticks, and a freed slot being
refilled immediately is the intended behaviour rather than a symptom. The cap is a rate limiter; one that is
usually idle is not limiting anything. Birth still selects by weight among every eligible mastermind whenever
a slot opens — saturation sets how often that selection happens, not how it is made. Step 9's criterion is
corrected accordingly.

On (4): the run was made standing in an empty room, so no conspicuous step ever had a loaded actor or target
to be blocked by, and zero holds is the correct answer rather than a silent failure. The gate is confirmed
wired — every one of the 881 participants resolves to a placed reference — but its behaviour is unobserved.
**(4) and (6) are what remain before this step can be marked complete**, and both need a run of a different
shape: parked in a city rather than an empty room, with a save and reload partway through and one move near
the end.

---

#### Step 13 — Travel as work, and a floor that means what it says

- [x] Complete

**[CLAUDE]**

**Goal:** A step's difficulty comes from where the actor has to go, not its deadline — and no step is ever
dispatched that arithmetic has already decided.

Step 12's first run found the race sized backwards. `SizeBudget` scaled the deadline by travel distance and
`SizeThreshold` did not depend on travel at all, so an errand against a neighbour got **46% fewer ticks to do
exactly the same amount of work** as the identical errand across the province. Two consequences, both bad:

- **Near targets were unwinnable.** For a strong actor at travel ≤ 0.2, six of the eight step types were
  impossible or expected to fall short. In the run, 12 of 12 such steps were; at travel 0.8, 3 of 74 were
  marginal and none impossible.
- **Distance was a bonus.** A far actor got extra ticks for unchanged work, so casting was quietly rewarded
  for picking whoever was furthest away — the reverse of the thing the travel term was added to express.

The design has been updated to match ([`FACTION_PLOTS.md` Part 6](../design/FACTION_PLOTS.md)); the
"independently derived" constraint it used to carry has been replaced by three that say what actually has to
hold.

1. **Travel moves to the threshold.** `SizeThreshold(type, importance, travel)` returns
   `base(type) × (1 + importance × 1.5) × (1 + travel × 0.4)`. Crossing the province is part of the job.
2. **The budget loses travel and keeps scale.** `SizeBudget(type, importance)` returns
   `base(type) × (1 + importance × 1.5)`. It answers "how long will the mastermind wait", which depends on how
   big the job looks and not at all on where the actor has to go.
3. **Base budgets are re-derived against the rate rather than chosen by feel.** Roughly half the base
   threshold, which is what the observed progress rate covers with margin, then perturbed per type to express
   inherent time pressure: a `Deliver` has a window, a `Surveil` can run long, `Cover Tracks` is urgent.
4. **`MinimumViableBudget` is rewritten to do what it always claimed.** It takes the threshold and the rate
   band, and returns `ceil(threshold / min(bestRateForTheWorstActor, threshold × maxFraction))` — the fewest
   ticks in which *any* actor could reach the threshold on a perfect run. Where the ceiling binds this reduces
   exactly to the old `ceil(1 / maxFraction)`, so the guarantee it used to give is kept and the one it only
   claimed is added. Applied at dispatch, where both sides are known.

Because both sides now scale identically with importance, importance no longer moves the odds — it makes a
step **longer**, and a longer step is more exposed to the mishap roll. Travel is what moves the odds. That is
the intended shape: distance is the risk, importance is the duration, and the actor is the rest.

**Verification:** `build.ps1 build` is clean, and a probe over the pure sizing and rolling functions asserts:

1. **Travel raises the threshold and never touches the budget**; importance raises both. The inverse of the
   old independence probe, and the property whose absence caused this step.
2. **Nothing is unwinnable.** Every step type × every travel distance × every importance × the worst possible
   actor gets a budget in which the threshold is reachable on a perfect run. This is the assertion Step 6's
   probe believed it was making.
3. **The floor subsumes the old one:** where the per-tick ceiling binds, `MinimumViableBudget` returns
   `ceil(1 / maxFraction)` exactly.
4. **A far step is harder than a near one,** for the same type, target and actor — asserted as a strict
   inequality on the threshold, not a simulated success rate.
5. **The Monte Carlo distribution is sane** over actor qualities sampled from the Step 12 run rather than a
   uniform actor who does not exist: overall success between 60% and 75%, and no step type below 20% at any
   distance.

Re-run `analyze-plot-log.py` against a fresh trace afterwards; its feasibility section reads budget and
threshold straight off the `DISPATCH` lines, so "0 impossible, 0 expected-short" is the check that this landed
in the game and not only in the unit test.

Done. `SizeBudget(type, importance)`, `SizeThreshold(type, importance, travel)` and
`MinimumViableBudget(threshold, rateMin, rateMax, maxFraction)` in `PlotResolution`, with the floor applied at
dispatch in `PlotTick.cpp` where both sides are known. Build clean, probe passing.

Base budgets, re-derived: `Locate` 5, `Deliver` 3, `Surveil` 6, `Acquire` 7, `Conceal` 4, `Suborn` 9,
`Sabotage` 7, `Discredit` 9. Travel multiplier **1.4** at full distance, deliberately gentler than the 3.0 it
used to apply to the budget — as a cost it compounds with importance, and at 3.0 a far errand against anyone
who mattered was hopeless.

Probe results, one probe, then deleted:

| Group            | Asserted                                                                                     |
| ---------------- | -------------------------------------------------------------------------------------------- |
| Travel isolation | `SizeBudget` is identical across every travel distance; importance raises both sides           |
| Distance costs   | threshold strictly increases with travel, every type × every importance                        |
| Winnability      | 200 (type × importance × travel) combinations, all reachable by the **worst** actor on a perfect run |
| Floor subsumption| where the per-tick ceiling binds, the floor is exactly `ceil(1 / maxFraction)` — the old value  |
| Floor bites      | where the rate binds it exceeds that; 120 work at 3.25/tick best-case returns 37 ticks          |
| Distribution     | 85.2% of steps complete within budget before the mishap roll; hardest cells `sabotage@1.0` 41%, `conceal@1.0` 43% |

The distribution figure is the one to read against Step 9's numbers with care: the probe does not model
mishaps, so 85.2% is the pure race. Applying the observed ~17% catch rate puts it near 69 / 14 / 17, which is
the band Step 6 was aiming at and the band the offline sweep predicted.

Design notes worth keeping:

- **Importance stretches a step; travel risks it.** Both sides scale with importance at the same rate, so a
  more important target does not make a step likelier to fail — it makes it *longer*, and a longer step is
  more exposed to the mishap roll. Distance is the only circumstance that moves the odds directly, which is
  what makes "who is nearest" matter to casting without a rule saying so. That is the property the old
  arrangement had exactly backwards.
- **The floor is a backstop, not a crutch.** With these curves it never engages: the base budgets already
  cover their thresholds. That is the intended relationship — if the floor were doing routine work it would
  mean the curves were wrong again, so it is worth checking that it stays quiet after any future tuning.
- **The floor reads the worst possible actor, never the one being cast.** A floor that read the actor would
  make the budget a competence figure, which the design forbids for good reason: a hopeless actor would be
  handed a longer deadline precisely because they are hopeless.

---

#### Step 14 — Road distance, and the mishap rate that followed it

- [x] Complete

**[CLAUDE]**

**Goal:** Distance is measured along the roads rather than guessed from hold membership, and the catch rate is
tuned to the step lengths the model actually produces.

Step 12's second run confirmed Step 13 landed — zero unwinnable steps — and surfaced two consequences of it.

**The travel proxy had almost no range.** Same-hold / different-hold across ten holds put **121 of 133
dispatches at the same value**, so distance was a flat surcharge, not a variable. Step 13 had just made
distance the only circumstance that moves a step's odds, which turned a tolerable approximation into the thing
carrying the whole race.

**The mishap rate no longer matched the model.** Step 13 roughly doubled the base budgets, mean step length
went 5.26 → 7.03 ticks, and since the mishap roll is *per tick*, catches went 12.2% → 23.4% of resolutions
without the rate being touched. `fPlotMishapChanceBase` was tuned by Step 9 against budgets that no longer
exist.

1. **`TravelGraph` supplies the distance.** It already reconstructs Skyrim's long-distance routing skeleton
   from the NAVI record — 622 nodes over Tamriel, about one exterior cell apart. Coarse is the right
   resolution here: the question is "roughly how far apart are these two people", and the fine road graph is
   both unnecessary and unavailable outside the loaded cell grid.
2. **Placement is per settlement, not per member.** Each member resolves to the node nearest their
   settlement's map marker, walking up `parentLoc` when a location has none of its own — a house interior has
   no marker, the city it sits in does, and the city is the right granularity anyway. Sixty-odd lookups
   instead of nine hundred.
3. **Distances are precomputed into a matrix**, one Dijkstra per occupied node, at population-build time on
   the main thread. A per-dispatch query would be a repeated Dijkstra for an answer that cannot change, and
   `FindNearestNode` needs the marker reference, which is not a plot-thread read.
4. **The scale is calibrated against the traffic, not the map.** The population-weighted median pair —
   weighting each settlement by its resident count — is defined as 0.5, and twice that is full scale. A plain
   percentile of the pair list describes the province's geometry instead, so a map whose cities happen to sit
   far apart would push every real dispatch to the top of the range and flatten the variation the term exists
   to provide: the hold proxy's failure, reached from the other side.
5. **The hold comparison survives as the fallback**, for members whose settlement has no marker and for a
   session with the graph switched off.
6. **`bTravelGraphEnabled` now ships true**, since the simulation consumes it. `bTravelGraphDebugBitmap`'s
   default is corrected to false to match the shipped INI — it defaulted true while the graph defaulted false,
   which was harmless only while nothing switched the graph on.
7. **`fPlotMishapChanceBase` 0.015 → 0.012.**

**Verification:** `build.ps1 build` is clean, `check-plot-settings.py` passes, and the offline sweep over the
new model — actor qualities sampled from the Step 12 run, travel distributed over the full range the road
graph now provides — reports **67.3% succeeded / 15.6% timed out / 17.1% caught**, against Step 9's target of
68.7 / 14.3 / 17.0.

Done. `Member::roadNode` and `PlotPopulation::RoadDistanceNorm`, the settlement-to-node resolution and the
distance matrix in `PlotPopulation::Build`, and `TravelDistanceNorm` in `PlotTick.cpp` reading them.

**A curve nudge that turned out to be unnecessary, which is worth recording.** Step 12's analysis found
`Conceal` demanding 2.97 work per tick at travel 0.8 against a mean actor rate of 2.87 — the only step type
above the line, and the reason 7 of 8 Cover Tracks steps were flagged as expected to fall short. The obvious
fix was a budget of 5 instead of 4. It is not in this step: with real road distances the typical journey is
0.5 rather than a pinned 0.8, `Conceal` demands 2.70, and the problem dissolves. Simulating the nudge anyway
showed it would have *hurt* — pushing timeouts down to 9.7%, below Step 9's 10% floor, by making an
intentionally tight step type slack. The proxy was manufacturing the symptom the nudge would have treated.

Design notes worth keeping:

- **The calibration is population-weighted on purpose.** What matters is the distribution of journeys the
  simulation will actually make, and that is settlement pairs weighted by how many people live at each end —
  not the set of pairs the map contains, most of which nobody will ever travel.
- **A negative return means "no answer", never zero.** `RoadDistanceNorm` returns a negative value when either
  member is off the graph or no route connects them, so the caller falls back rather than being handed a
  plausible-looking 0.0 that would read as "they are in the same place".
- **Per-tick rates are only meaningful against a step length.** The mishap retune is the second time a number
  tuned in one step was silently invalidated by a change in another; both times the symptom was a distribution
  drifting without anyone touching the parameter that governs it. A per-tick probability and a budget are one
  setting wearing two hats.

---

### Phase B — LLM authoring

#### Step 15 — Target menus and the item pool

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

#### Step 16 — The birth prompt and its response handling

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

Whether the *content* is any good is Step 18's business; this step proves it cannot corrupt state.

---

#### Step 17 — Adaptation

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

#### Step 18 — Phase B in-game validation

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

#### Step 19 — The memory calls

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

#### Step 20 — World effects

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

#### Step 21 — Gossip seeding

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

#### Step 22 — Phase C in-game validation and call-volume measurement

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

- All 20 steps are checked off. The three `[USER]` steps — 12, 16 and 20 — have their findings recorded in this
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
- No engine state outside Step 20's allow-list is mutated.
- A caught failure is observed propagating as a rumor.

---

## Open questions

The feature's open questions are tracked in the design doc rather than duplicated here; two of them land inside
this phase and are built around rather than blocked on:

1. **Is the conspicuousness split right?** Built in Step 6 as a per-type constant from the manifest. If
   Step 9's harness or Step 12's play shows it needs to be a per-step property decided at dispatch, that is a
   change to one function's inputs, not to the model.
2. **How does a step get caught?** Step 6 implements the proposed per-tick mishap roll behind
   `bPlotMishapEnabled` precisely so the phase does not wait on the answer. If the shape changes, the switch
   and one roll are what move.

One question belongs to this document alone:

1. ~~**Should `bPlotsEnabled` default true, or stay opt-in?**~~ Resolved: **true**, like gossip. A background
   simulation nobody has switched on produces nothing, and the memory-writing caveat belongs in the INI
   comment rather than in a default that hides the feature.
