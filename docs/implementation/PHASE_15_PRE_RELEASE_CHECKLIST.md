# Phase 15 — Pre-Release Checklist

Not a design-and-implementation phase. This is a list of four things to knock out before the next release, each
small enough to be its own commit and none of them depending on the others. Work them in any order.

The entries below carry the same per-item checkbox as the numbered steps in earlier phase docs, and the same
rule applies: a C++ change lands its tests in the same item, and `pwsh -File format.ps1` runs before the box is
checked. What they do **not** carry is a design section — where an item has a real open decision, it is called
out as one rather than resolved here in advance.

---

## 1 — Blacklist for beat senders

- [X] Complete

**[CLAUDE]**

**Goal:** A configurable list of NPCs who can never be chosen as the sender of a letter beat or the visitor of
a visit beat.

**Files:** `include/Settings.h`, `src/Settings.cpp`, `statics/SKSE/Plugins/NarrativeEngine.ini`,
`src/SenderCandidatePool.cpp`, `src/SenderCandidatePool.engine.test.cpp`.

**Sub-tasks:**

1. New delimited setting alongside the existing exclusion lists — `sBlacklistedSenders` under `[Beats]`, empty
   by default, semicolon-separated as `sSpellNameBlocklist` is, whitespace around the separators trimmed.
   Parse once into a set in `Settings.cpp` and expose a membership query the way
   `Settings::IsSpellNameBlocked` already does for `sSpellNameBlocklist`.
2. Apply the gate inside `WalkEngagement` in `src/SenderCandidatePool.cpp`, as part of the universal viability
   walk — **not** in either caller's `extraViabilityFilter`. There are four filter call sites today
   (`LetterComposer.cpp:578`, `VisitComposer.cpp:268` and `:357`, `NPCVisitBeat.cpp:467`, the last two
   explicitly documented as "kept in sync"), and the universal walk is the one place that covers all of them
   plus `CountViable`. A blacklisted NPC must not be counted by `IsAvailable` either, or the beat reports
   candidates it cannot use.
3. Roll the rejection into the walk's existing skip-reason tally (`blacklisted`) so the summary log line says
   why the pool shrank.
4. Document the setting in the deployed INI, including what an entry is matched against.
5. Tests: an actor whose identifier is on the list is dropped from `Build`, dropped from `CountViable`, and
   counted under the new skip reason; an empty setting changes nothing; matching is case-insensitive and
   tolerant of surrounding whitespace.

**What an entry names — decided.** An entry hits if it equals either the base form's EditorID or the
candidate's display name; the EditorID is tested first, so that is the arm reported when both match. EditorID
is the precise identifier and the one to prefer wherever it resolves; the display name is what keeps the list
working on an install without an EditorID-recovery mod. Two consequences carried into the implementation:

- `TESNPC::GetFormEditorID()` is empty at runtime unless powerofthree's Tweaks (or equivalent) is installed.
  On such an install the EditorID arm never fires and every match comes from the name arm, so the name arm is
  the one that has to be right — this is not a rarely-taken path.
- Display names are ambiguous across duplicates, but the pool is already restricted to unique NPCs on the
  visit side, and a name collision here fails safe: it excludes one NPC too many rather than letting a
  blacklisted one through. Log which arm matched so a surprising exclusion is diagnosable.

**Verify [CLAUDE]:** `pwsh -File build.ps1 build` and `pwsh -File build.ps1 test` pass. With a named NPC on the
list, the debug log's pool-summary line shows them rejected as `blacklisted` on both the letter and the visit
path.

---

## 2 — Dialogue history strictly filtered to player-and-sender exchanges

- [X] Complete

**[CLAUDE]**

**Goal:** The `recent_dialogue` block rendered into the letter and visit compose prompts contains only lines
that are either the player speaking **to** the sender, or the sender speaking **to** the player. Anything else
— the sender talking to a third NPC, the player talking to someone else in earshot, ambient NPC-to-NPC chatter
— is dropped before the prompt is built.

**Files:** `src/LetterComposer.cpp`, `src/VisitComposer.cpp`, and their `.engine.test.cpp` suites.

**Sub-tasks:**

1. Establish what `SkyrimNetAPI::GetRecentDialogue` actually returns. Our header claims "exchanges between the
   player and the given NPC", but `FetchRecentDialogue` in `LetterComposer.cpp` already reads an `npcName`
   field per entry and falls back to the sender's name when it is absent — which only makes sense if entries
   can name someone other than the sender. Dump one raw payload under debug mode and settle it.
2. Filter on that shape. An entry survives only if `speaker` is the player and the counterparty is the sender,
   or `speaker` is the sender and the counterparty is the player. Drop everything else, including entries
   whose counterparty cannot be determined — a line we cannot attribute is exactly the line that should not
   reach the prompt.
3. If the payload turns out to carry no counterparty at all, the fallback is the event history, which does
   record it: `SkyrimNetEvents.cpp` renders `dialogue`, `dialogue_background` and `dialogue_player_text` as
   `speaker -> listener`, and `EventEntry` carries `originatingActorName` / `targetActorName`. Say so and
   check before switching sources — it is a bigger change than the filter is.
4. Both composers get the same treatment, in the same shape. They already carry near-identical
   `FetchRecentDialogue` / `FilterDialogueByMemoryAge` / `AnnotateDialogueAges` trios; do not let them diverge
   here.
5. Tests: a third-party line is dropped on both paths, a player-to-sender line and a sender-to-player line
   both survive, an unattributable entry is dropped, and an empty result after filtering renders the prompt's
   `sender_recent_dialogue` block as absent rather than as an empty heading.

**Done: the source moved, because the old one could not answer the question.** `PublicGetRecentDialogue`
returns `{speaker, text, gameTime}` and nothing else — SkyrimNet's own `PublicAPI.h` documents that shape, and
there is no counterparty on the row. Its docstring does claim the rows are dialogue "between the player and an
NPC", but a filter resting on that claim is an assumption, not a filter: a line the sender aimed at a third
party and a line they aimed at the player are the same row.

`PublicGetRecentEvents(formId, n, "dialogue,dialogue_player_text")` carries `originatingActorName` and
`targetActorName`, so both of the conditions above are decidable from the data. The wrapper for it already
existed (`SkyrimNetAPI::GetRecentEvents`), so this added no API surface. `dialogue_background` is not
requested: it is ambient NPC-to-NPC chatter, which the filter would drop anyway.

The trio the two composers each carried is now one module, `SenderDialogue`, rather than two copies of a
filter that has to agree. Its two shaping functions take `nowGameSeconds` instead of reading `RE::Calendar`,
which makes them pure — no token, per the `RoadRoute::Route` precedent — and testable without a clock. Call
sites pass `EventLogUtil::NowGameTimeSeconds()`, the canonical helper, in place of the
`GetHoursPassed() * 3600` both composers had open-coded.

**Still reading the unfiltered endpoint, out of this item's scope:** `VisitConclusionPoll::SampleRecentLines`
and `NPCVisitBeat`'s discuss-turn sampler. Both are in-conversation turn detection during a visit rather than
compose context, and the poll already gates on `gameTime >= discussStartedAt` plus a speaker-side bystander
filter. Neither can tell who a line was addressed to, so the same gap exists there in a smaller form.

**Verify [CLAUDE]:** `pwsh -File build.ps1 test` passes. A debug-mode compose run on a sender the player has
spoken to in a crowded room renders only the two-party lines.

---

## 3 — The ambush narration must not contain dialogue

- [X] Complete — implemented; the **[USER]** in-game verification below is still outstanding

**[CLAUDE]**

**Goal:** The ambush beat's `narration_prose` comes back with no dialogue in it at all. Not just no quoted
speech and no lines put in an attacker's mouth — no narrated or paraphrased speech either. What anyone says
is left out entirely; SkyrimNet's dialogue layer has the attackers once the fight starts.

**Files:** `src/AmbushBeat.cpp` — the `narration_prose` paragraph of `PromptContribution`, around line 1273.

**Sub-tasks:**

1. Minimal edit to the existing parameter description, in the voice of the surrounding text, forbidding
   quoted speech, attributed lines, and narrated or paraphrased speech alike, and saying to omit what is
   said rather than to render it another way. Carry no example: an example of the thing being forbidden is
   the likeliest way to reintroduce it. Do not restructure the paragraph or touch the other two parameters.
2. No parser change, no rejection path, no new setting. This is a prompt wording fix; if it proves
   insufficient in play, that is a separate piece of work.

**Verify [USER]:** several ambush dispatches in game, with the narration read in the event log.

---

## 4 — Dashboard timing indicators on every tab

- [X] Complete — implemented; the **[USER]** in-game verification below is still outstanding

**[CLAUDE]**

**Goal:** Every tab shows when the next scheduled thing on it is expected to happen and how long the cooldowns
it cares about have left — each labelled in the unit that actually governs it.

**Files:** `src/DashboardUIManager.cpp`, `dashboard/src/types.ts`, `dashboard/src/App.tsx`, the six tabs under
`dashboard/src/components/tabs/`, and whichever shared component the countdown ends up in.

**There are four units, not three, and they must be distinguished on screen rather than all rendered as bare
seconds:**

- **Wall-clock real time** — the anti-repetition window (`iBeatRepetitionWindowSeconds`). The ring stores
  `NowUnixSeconds()` (`system_clock`) and compares against it, so this one advances through everything:
  pause, combat, dialogue, menus.
- **Unpaused real time** — the Director tick (`iTickIntervalSeconds`), the minimum phase duration
  (`iMinPhaseDurationSeconds`), and the gossip tick (`iGossipTickIntervalSeconds`), all driven by the
  `unpausedElapsedSeconds` accumulators in `Tick.cpp`, which bail on `EngineUtils::IsGamePaused()`.
- **Active-play real time** — the global beat cooldown (`iBeatCooldownSeconds`). This is its own unit and the
  one most likely to be got wrong: `Settings.h` describes it as "wall-clock seconds", but `RunOneTick` only
  adds `intervalMs` to `g_globalCooldownMs` under `TickMode::Normal`, and `ComputeTickMode` returns `Paused`,
  `Combat` or `Dialogue` ahead of it. So it is frozen in combat and in dialogue as well as on pause — strictly
  slower than the unpaused clock above. Fix the misleading comment in `Settings.h` while here.
- **Game time** — the per-beat and per-sender cooldowns (`iLetterBeatCooldownGameHours`,
  `iLetterSenderCooldownGameHours`, `iVisitSenderCooldownGameHours`, `iAmbushPerBeatCooldownGameHours`) and
  the gossip harvest interval (`fGossipHarvestIntervalGameHours`).

**Sub-tasks:**

1. Per tab, the indicators to add:
   - **Director** — time to the next evaluation tick; time until the current phase may advance; whether the
     global beat cooldown or the repetition window is currently blocking a pick, and for how much longer.
   - **Letters** — remaining letter-beat cooldown; the delivery-verify and pending-delivery timeouts on any
     slot currently waiting on one.
   - **Visit** — remaining visit-sender cooldown; for a visit in flight, time to the next conclusion poll and
     the remaining approach / return-home timeout.
   - **Gossip** — time to the next sim step; time to the next harvest sweep.
   - **Dispatch** — the existing `remaining_cooldown_hours` column gains its unit label, plus the global
     cooldown and repetition window as they apply to the table as a whole.
   - **Settings** — nothing scheduled; no indicators.
2. Extend the JSON contract in `dashboard/src/types.ts` and the matching composition in
   `DashboardUIManager.cpp` together — that file's own header says a schema change is a coordinated C++ change.
3. **These are static readouts, not live tickers, and that is correct.** `ToggleVisibility` shows the view
   with `PrismaUI_API::Focus(g_view, /*pauseGame=*/true, ...)`, and `PushFullState` returns early unless
   `g_visible` — so the dashboard is only ever on screen with the game paused. Three of the four clocks above
   are frozen for exactly as long as the player is looking at them: the unpaused accumulators bail, the
   active-play cooldown sits in `TickMode::Paused`, and the Calendar does not advance. Only the wall-clock
   repetition window keeps moving, and it is the least interesting of the four. So do **not** build a cadenced
   push or a client-side interval timer. Compute the remaining values in `ComposeFullStateJSON`, render them
   as of the push, and let the existing seed-on-open push in `ToggleVisibility` be the refresh. The one thing
   worth adding is a note in the UI that the values are as of the moment the dashboard was opened, since a
   number that never moves otherwise reads as a bug.
4. Tests: the C++ side's remaining-time computations get unit coverage per unit kind, including the
   already-expired case (clamp to zero, never negative) and the disabled case (a cooldown setting of 0 renders
   as "no cooldown", not as "0s remaining").

**Done: five places the subsystems did not match the plan's shape.**

- **Gossip has one scheduled tick, not two.** `GossipTick::RunTick` sets the horizon, runs the harvest sweep,
  then advances the simulation, so "next sim step" and "next harvest sweep" are the same moment. What the
  cadence does have is two *stages* on two different clocks: `Poll` only consults the schedule every
  `iGossipTickIntervalSeconds` of unpaused real time, and what it looks for is a game-day boundary
  `fGossipHarvestIntervalGameHours` apart. Both are shown; there is no second game-time figure to show.
- **The conclusion poll has no single countdown.** Three independent conditions arm it — silence
  (active-play seconds), max interval (game seconds), and turn count — and whichever trips first wins. A
  lone "time to next poll" would have to pick a winner, and the turn counter is not a clock at all, so all
  three are reported side by side.
- **Sender cooldowns are summarised, not singular.** "The remaining visit-sender cooldown" has no one
  answer: any number of senders can be held at once. `SenderCooldownTable::SummarizePending` reports how
  many are held and when the first is released, which is the question the tab is actually being asked —
  when could this beat pick somebody again. Both beats expose it, so Letters gained the same row.
- **The letter delivery-verify delay is not shown, deliberately.** `iLetterDispatchVerifyDelaySeconds` is a
  five-second grace window living inside the beat's RUNNING state rather than on a pool slot, and the
  dashboard cannot be open while it runs — opening the panel pauses the game, and the window is five
  seconds of a dispatch that has to complete for the beat to proceed. Surfacing it would have meant new
  beat-internal state for a row nobody can ever catch. The pending-delivery timeout, which lasts ten
  minutes and is visible on a slot, is shown.
- **Two settings comments were wrong about their own clock, and are fixed.** `Settings.h` called
  `iBeatCooldownSeconds` "wall-clock seconds"; the INI said the same and additionally described the
  Director tick as plain wall-clock. Both now name the real clock, and the INI says outright that the beat
  cooldown and the repetition window run at different rates.

**Shape of the implementation.** `DashboardTimers` owns the arithmetic and the four-clock enum, emits a
`timers` object into the state payload, and is where the tests live: the inactive-versus-expired distinction
and the clamp are each one function, tested once, rather than repeated per readout. `TimerPanel.tsx` renders
every row with its clock label and carries the "as of when the dashboard was opened" caveat once per panel.
Five accessors were added to reach live state — `Tick::SecondsUntilNextTick`,
`BeatSystem::GetRepetitionWindowInfo`, `GossipTick::GetScheduleInfo`, `NPCVisitBeat_Timers::Get`, and
`VisitConclusionPoll::GetGateInfo` — plus `SenderCooldownTable::SummarizePending` for the two beats.

**Verify [CLAUDE]:** `pwsh -File build.ps1 build`, `pwsh -File build.ps1 test`, and the dashboard build all
pass.

**Verify [USER]:** open the dashboard, close it, play for a stretch, and reopen — the game-time and
unpaused-real-time readouts have moved by roughly the elapsed amount, and the active-play one has moved by
less if any of that stretch was spent in combat or dialogue.

---

## Done condition

All four boxes checked, `pwsh -File format.ps1` clean, and a test build packaged per
`.claude/rules/test-build-naming.md` for a run through the verifications that need a running game.
