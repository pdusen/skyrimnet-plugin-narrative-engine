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

1. New CSV setting alongside the existing exclusion lists — `sBlacklistedSendersCSV` under `[Beats]`, empty by
   default, whitespace around commas trimmed. Parse once into a set in `Settings.cpp` and expose a membership
   query the way `Settings::IsSpellNameBlocked` already does for `sSpellNameBlocklist`.
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

- [ ] Complete

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

**Verify [CLAUDE]:** `pwsh -File build.ps1 test` passes. A debug-mode compose run on a sender the player has
spoken to in a crowded room renders only the two-party lines.

---

## 3 — The ambush narration must not contain dialogue

- [ ] Complete

**[CLAUDE]**

**Goal:** The ambush beat's `narration_prose` comes back as narration only. No quoted speech, no lines put in
an attacker's mouth.

**Files:** `src/AmbushBeat.cpp` — the `narration_prose` paragraph of `PromptContribution`, around line 1273.

**Sub-tasks:**

1. Minimal edit to the existing parameter description — one added sentence, in the voice of the surrounding
   text, forbidding quoted or attributed speech and saying what to do instead (report what is said as
   narration, or leave it out). Do not restructure the paragraph or touch the other two parameters.
2. No parser change, no rejection path, no new setting. This is a prompt wording fix; if it proves
   insufficient in play, that is a separate piece of work.

**Verify [USER]:** several ambush dispatches in game, with the narration read in the event log.

---

## 4 — Dashboard timing indicators on every tab

- [ ] Complete

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

**Verify [CLAUDE]:** `pwsh -File build.ps1 build`, `pwsh -File build.ps1 test`, and the dashboard build all
pass.

**Verify [USER]:** open the dashboard, close it, play for a stretch, and reopen — the game-time and
unpaused-real-time readouts have moved by roughly the elapsed amount, and the active-play one has moved by
less if any of that stretch was spent in combat or dialogue.

---

## Done condition

All four boxes checked, `pwsh -File format.ps1` clean, and a test build packaged per
`.claude/rules/test-build-naming.md` for a run through the verifications that need a running game.
