# Phase 05 — NPC Visit Action

The Director gains its second social lever. Where **letters** (Phase 04) cover the
low-intensity, deferred, from-a-distance beat — a piece of news, a grievance, a
thank-you — **visit** covers the high-intensity, in-person one: a piece of
information that cannot be put to paper, an issue the sender needs to actually
*talk through* rather than vent about on a page. The NPC drops what they were
doing, travels to the player, and asks for a moment.

Mechanically this is one action — `NPCVisitAction` — that picks a known NPC
from the player's SkyrimNet interaction history, generates a per-conversation
briefing via a new LLM call, warps the NPC to a nearby out-of-sight marker,
and drives a state machine — Salutation → Discuss → Valediction → ReturnHome,
with situational On-hold / Re-engage detours — that owns the whole
conversation lifecycle. A Follow package keeps the NPC near the player
throughout; a periodic LLM poll during Discuss decides when the beat has
landed and it's time to wrap up.

The sender-selection pipeline and marker-faction dispatch pattern are
reused from Phase 04's letter action. The nearby-XMarker
Find-Matching-Reference spawn-marker pattern is reused from Phase 03's
ambush action.

---

## Why this phase exists

After Phases 03 and 04 the Director has `ambush` (raise, wilderness, combat)
and `npc_letter` (either direction, safe territory, delayed delivery). Visit
covers the urgent-personal beat neither reaches: a friend, rival, or lover
tracking the player down in person with something that couldn't be put to
paper. Letters and visits are companion actions covering the same social
space at different intensities — the LLM's action-select prompt distinguishes
them by asking whether the beat can survive being written down and read
later.

---

## Scope

### In scope

- A `NPCVisitAction` (`IAction`) implementation:
  - `Polarity` = `Either` — the same action handles both directions; content
    drives direction (a "you were right, I'm sorry" heart-to-heart lowers; a
    "you crossed me and I'm here to say it to your face" confrontation
    raises).
  - Requires the player to have enough recent NPC interactions in
    SkyrimNet's memory to seed the candidate pool (same threshold as
    letters, same setting reused).
  - Requires the player to be in a location a stranger can plausibly walk
    up to them in: not in combat, not in a scripted scene, not in a
    dangerous cell (dungeons, bandit camps), not currently in dialogue,
    not in an actively-hostile encounter zone. Interior and exterior are
    both acceptable in principle; interior support may be gated on cell
    type — see **Preconditions** below.
  - Requires the *sender NPC* to be alive, unique (not a bandit /
    generic), and in a state where warping them out of their current
    situation won't cause visible ripples. See **Sender viability** below.
  - On `Start`: runs the briefing-composition LLM call, promotes the
    chosen sender to rank 4 in a new `_ne_VisitSenderFaction`
    marker faction, snapshots the sender's current
    `RE::TESObjectREFR` position and cell (for return teleport later),
    caches the briefing / topic / mood / tags in in-memory
    `VisitState` (used later as the `argsJson` payload passed to
    SkyrimNet's `ExecuteAction` when we trigger the sender's turn
    to speak, and as input to the natural-conclusion poll — see
    below), then starts the shared `_ne_VisitQuest`.
    The engine's alias-fill pass picks up the sender via a
    Find-Matching-Reference rule keyed on the faction rank, and
    picks a nearby out-of-sight XMarker via a second FMR rule
    (the same one Ambush uses, tuned for a single marker instead
    of a bandit cluster).
  - Dispatch verification (`DetectAndRollbackFailedStart` /
    `DetectCompletion` — the same `IAction` polls Phase 04 uses) checks
    that the sender warp happened and that the Salutation phase
    (see below) reached its close-approach threshold within the
    verification window.
- A **conversation phase state machine** whose authoritative state is
  carried on the stages of `_ne_VisitQuest` — Salutation → Discuss →
  Valediction, with situational On-hold / Re-engage detours and a
  hard-abort escape hatch. C++ drives *when* to transition (LLM
  callbacks, watchdogs, timers, event sinks) and *how* the world
  reacts (SkyrimNet `ExecuteAction` calls, faction manipulation,
  return teleport); Papyrus stage fragments own small per-stage side
  effects; the engine auto-persists the stage across save/load. The
  visit action stays in-flight for the entire lifetime of the
  machine; no other Director action can fire while a visit is
  running. See **Conversation phase state machine** and **Quest
  stages as source of truth** below.
- A **Follow package** pinned to the player on the Sender alias for
  the whole duration of the action — Salutation, Discuss,
  Valediction, and any On-hold / Re-engage detours in between. The
  NPC does not resume their normal AI schedule until the action
  concludes and the alias empties. This is what makes it possible
  for the player to physically stand next to the sender during
  Discuss without the sender wandering off between exchanges.
- A **periodic natural-conclusion LLM poll** during the Discuss
  phase. The poll receives the sender, the player, the discussion
  topic, the sender's stated goal, and the last N lines exchanged,
  and returns a boolean "has this conversation reached a natural
  stopping point?" The Discuss phase does not advance to
  Valediction on any other signal. See **Natural-conclusion LLM
  poll** below.
- A **ReturnHome** state after Valediction where the sender
  visibly walks away from the player toward their pre-dispatch
  position, then teleports the rest of the way once out of
  earshot. See **Cleanup and return teleport** below.
- **Memory formation delegated to SkyrimNet.** No plugin-authored
  `PublicAddMemory` calls. Because sender turns are dispatched
  through `SkyrimNetApi.ExecuteAction`, SkyrimNet's own
  conversation lifecycle handles end-of-conversation memory
  formation for both actors.
- The **briefing** the composer produces is passed as the
  `argsJson` payload to `ExecuteAction` for plugin-owned sender
  turns (Salutation / ReEngage / Valediction), so those turns
  land on-topic. Ignore-irritation nudges go through SkyrimNet's
  built-in `ContinueConversation` action with no briefing
  payload. The briefing is also the input the natural-conclusion
  poll evaluates against.
- ESP content: one `_ne_VisitSenderFaction` marker faction; one
  `_ne_VisitQuest` quest with the two reference aliases
  (`Sender` — FMR on rank ≥ 4; `SpawnMarker` — FMR nearest XMarker
  with an out-of-line-of-sight condition, ~800–2500 units away from
  the player), plus a `_ne_VisitFollow` AI Follow package attached
  to the Sender alias with the player as its target actor.
- Extension of the `LocationKeywords` predicate set to include a
  `IsVisitHostile()` helper — a cheaper wrapper around the same
  keyword tables that names the specific exclusion criteria for this
  action (dungeons, bandit camps, hostile lairs, plus tavern
  performances / scripted scene cells if we find any).
- Co-save persistence of the residual in-flight data that has nowhere
  more natural to live — briefing / topic / mood / tags, the sender's
  pre-dispatch position/angle/cell for the return teleport, and the
  ignore-nudge / poll-failure counters. The vanilla save already
  carries the state-machine phase (via `_ne_VisitQuest`'s stage
  record) and the sender / spawn-marker fills (via the quest's
  aliases), so the co-save only fills the gaps.
- Dashboard surface: a new **Visit** tab on the dashboard, with
  a "current conversation" panel (sender, topic, briefing preview,
  state) and a small recent-history list. Same tab-bar pattern
  Phase 04 introduced; adds one entry to the existing bar.

### Deferred (explicitly out)

- **Player-initiated visit.** Director owns dispatch.
- **Multi-NPC visit.** One sender, one player.
- **Sender pool.** Single-slot; only one visit runs at a time.
- **Interior-cell dispatch beyond safe interiors.** Homes,
  inns, and owned shops are allowed; dungeons and specialized
  interior types (jails, arenas) are not.
- **Cinematic camera or scripted framing.** Vanilla free-motion
  throughout.
- **Player-side input UI.** Whatever SkyrimNet already ships.
- **Sender-side rude/hostile reaction branch.** SkyrimNet's own
  dialogue system shapes tone; visit doesn't add a special
  "insulted and left" branch. Attacking the sender goes through
  the normal combat → OnHold → hard-abort path.
- **Mid-conversation goal rewrite.** Compose fires once; the
  poll evaluates against the original briefing.
- **Per-action cooldown.** Same treatment as letters; can be
  added later on the `iAmbushPerActionCooldownGameHours` pattern
  if needed.

---

## Core concepts

### What's reused, what's different from Phase 04

Reused as-is: sender-selection LLM candidate build,
`_ne_...SenderFaction` promote/demote rank-4 mechanism, FMR
alias fills, `DetectAndRollbackFailedStart` verification window,
`ActionContext` direction propagation, dashboard tab pattern.

Different:

- **Physical delivery**: warp to nearby XMarker + Follow
  package, not courier system.
- **Pool**: single-slot (one shared quest), not 20-slot.
- **Content storage**: in-memory `VisitState` briefing passed as
  `argsJson` to `ExecuteAction`, not Book FULL + hook.
- **In-flight duration**: whole state machine (Salutation →
  Discuss → Valediction → ReturnHome), not just dispatch
  verification.
- **Conversation-end signal**: periodic natural-conclusion LLM
  poll during Discuss.
- **Memory formation**: delegated to SkyrimNet, not plugin-side.
- **Cleanup**: sender walks away then teleports home, not "slot
  recycles."

### Sender viability

Letters accept a wider candidate net than visits — the letter's
sender doesn't have to physically be anywhere. Visit candidates
must additionally be:

- **Unique** (`RE::TESNPC::IsUnique()`), not leveled/templated.
- **Alive.**
- **Not in combat** — combat state == `NOT_COMBAT`.
- **Not currently in dialogue** with anyone else.
- **Not the player's active follower** (`IsPlayerTeammate`).
- **Not disabled or deleted.**
- **`GetCurrentLocation()` returns non-null** — a sanity signal
  the actor isn't in engine-limbo.

The filter runs in `VisitComposer` before the LLM sees the pool.

### Sender-selection LLM call

Same shape as Phase 04's `LetterComposer` — pull top ~12
candidates from `PublicGetActorEngagement`, viability-filter,
per-candidate memory tail via
`PublicGetMemoriesForActor(form_id, ~6, <player_name>)`, fire
`SendCustomPromptToLLM("narrative_engine_visit_compose",
"narrative_engine_director", contextJson, callback)`.

Output — JSON with five keys:

- `sender_npc_form_id` — hex string. Must match a candidate;
  mismatch fails the action with `bad_sender`.
- `briefing` — 40–120 words, first-person from the sender's
  frame, written as the "what I've come to talk about" thought
  they're holding as they approach. Passed as the `argsJson`
  briefing to every plugin-owned SkyrimNet action call and used
  as the natural-conclusion poll's `sender_goal` input.
- `topic_tag` — 2–5 word label.
- `mood` — one of `{ warm, neutral, urgent, menacing, mournful,
  contrite, businesslike }`. `contrite` is added for the
  Falling-Action apology beat.
- `tags` — 2–6 short retrieval tags. Same shape as Phase 04's
  letters `tags` field.

Prompt discipline follows `docs/CUSTOM_PROMPTS.md` — domain-
category guidance only (money / secrets / warning / apology /
grief / request / rivalry / obligation); LLM invents specifics.
No player-location context (the sender doesn't know where the
encounter will happen).

### Marker faction and per-dispatch quest

Same marker-faction dispatch pattern as Phase 04's letters.
A new marker faction `_ne_VisitSenderFaction` (empty, ranks
`-1` never touched / `0` prior candidate / `4` currently
designated) selects the sender NPC for the shared
`_ne_VisitQuest`'s `Sender` alias via a
`GetFactionRank >= 4` FMR condition. A `SpawnMarker` alias
FMR-picks a nearby XMarkerHeading out of the player's line
of sight. Full alias / stage / package config lives in the
**ESP content** section; the per-dispatch sequence lives in
**`Start`** and **Conversation phase state machine**.

### Quest stages as source of truth

The state machine's current state lives on `_ne_VisitQuest`'s
stage — C++ reads `Quest.GetCurrentStageID()` and calls
`SetStage(N)` to advance. The engine auto-persists the stage
across save/load, giving the whole plugin one authoritative
source (matching Phase 04's letter quests). The
`VisitState::Mode` helper enum in C++ is a derivation from the
current stage for logging and dashboard convenience only, not
stored.

**Composing** is the one state without a distinct quest stage —
the quest hasn't started yet during the compose LLM's in-flight
window. C++ carries that fact as an in-process flag
(`g_composingSender`) that gates re-entrancy and lets the
dashboard show the correct badge. Once `EnsureQuestStarted`
returns and the quest is at Stage 10, the flag clears.

**C++ vs. Papyrus responsibilities:**

- **C++ owns**: LLM calls (compose, natural-conclusion poll),
  the decision to trigger each sender turn (VM-dispatched to
  a Papyrus trampoline that calls `SkyrimNetApi.ExecuteAction`),
  distance watchdogs, wall-clock timers, event sinks (menu /
  combat / scene / death), `recent_lines` sampling from
  SkyrimNet, counters, dashboard payload, return-teleport
  execution, and the `SetStage(N)` calls that drive every
  transition.
- **Papyrus fragments own** the small per-stage side effects
  (Stage 0 `SetStage(10)` startup route; Stage 10 MoveTo +
  Follow-package bind; Stage 50 EvaluatePackage to swap Follow
  → Travel; Stage 60 → 200 rollback route; Stage 200
  `Shutdown()`). Fragments do not `SetStage` themselves except
  for the two mechanical routes (0 → 10, 60 → 200).
- **Papyrus quest script also owns** the `RunSenderAction`
  trampoline (`SkyrimNetApi.ExecuteAction` call site).
- **Quest aliases own** the sender REFR (via FMR fill), the
  SpawnMarker REFR (via FMR fill), and the ReturnAnchor REFR
  (via `Create Reference to Object` on `XMarkerHeading`
  positioned `At: Sender`, which spawns the marker at the
  sender's current position during alias-fill) — all
  engine-persisted, no co-save shadowing needed.
- **Co-save (VisitState) owns** everything else C++ needs that
  isn't derivable from the quest / aliases or re-samplable from
  SkyrimNet: briefing text, return position/angle/cell, return
  anchor FormID, counters, dispatched-at timestamp. See
  **Persistence** for the full schema.

The reason to route through Stage 0 rather than making Stage 10
the Startup Stage is ordering: the engine's alias-fill pass runs
*before* any fragment, so putting the MoveTo + package bind on
a stage that fires *after* alias-fill guarantees Sender and
SpawnMarker are resolved when the fragment runs.

### Follow package

A vanilla **Follow package** — `_ne_VisitFollow` — attached to
the Sender alias. Target Actor: `PlayerRef`. Follow Distance:
`iVisitSalutationApproachDistanceUnits` (default 300u).

The Follow package (Bethesda's canonical follower AI) is the
right choice because it keeps the sender physically near the
player across cell transitions, load doors, and combat
interruptions — the same handling vanilla follower NPCs get.
This is what lets the sender stay put during OnHold and follow
the player around during Discuss instead of reverting to their
base schedule between exchanges.

Package flags:

- **`Ignore Combat` = false, `Interrupt Override` = None**
  (per the CLAUDE.md memory on Ambush's approach package). If
  the sender gets attacked, combat should preempt the Follow.
- **`Must Complete` off**.
- **`Allow Idle Chatter` off** — all sender speech must go
  through SkyrimNet so the natural-conclusion poll can reason
  about it.

**Salutation-approach signal.** Vanilla Follow doesn't emit a
"reached follow distance" event, so a C++ watchdog polls
sender-to-player distance every ~250ms and fires the Salutation
opening line when the threshold trips. No Papyrus-side state,
no ambush-style `OnUpdate` loop on the alias.

### Sender speech via SkyrimNet's ExecuteAction

> **Note — "action" is overloaded.** *NarrativeEngine actions*
> (`IAction` — `AmbushAction`, `NPCVisitAction`, etc.) and
> *SkyrimNet actions* (entries in SkyrimNet's action library,
> invoked via `SkyrimNetApi.ExecuteAction`) are unrelated
> concepts. The visit's NarrativeEngine action *calls*
> SkyrimNet actions to drive sender speech; when disambiguation
> matters, the doc says "SkyrimNet action" or "NarrativeEngine
> action" explicitly.

Every sender-driven turn in a visit is dispatched by C++
VM-invoking `SkyrimNetApi.ExecuteAction` (Papyrus global
function) against the Sender alias. Two categories of
SkyrimNet action get called:

- **Plugin-owned SkyrimNet actions** for the sender's scripted
  turns (Salutation opening, ReEngage resumption, Valediction
  closing). Use a SkyrimNet action registered by this plugin at
  `kDataLoaded` (or an existing one if a fitting action exists);
  receive the briefing payload as `argsJson` so the line lands
  on-topic.
- **SkyrimNet's built-in `ContinueConversation`** for
  ignore-irritation nudges. Empty `argsJson`; `ContinueConversation`
  reasons from scene state on its own.

API signature:

```papyrus
int function ExecuteAction(string actionName, Actor akOriginator,
                           string argsJson) global native
```

`argsJson` for plugin-owned turns carries `turn_kind`
(`"salutation"` / `"reengage"` / `"valediction"`) plus the
briefing payload from `VisitState`:

```json
{
  "turn_kind": "salutation",
  "topic_tag": "brother's estate",
  "mood": "urgent",
  "briefing": "<composer's briefing verbatim>",
  "goal": "<what the sender wants from this conversation>",
  "nudge_count": 0
}
```

`nudge_count` is 0 for Salutation / ReEngage; on the
Valediction call it carries the final `ignoreNudgeCount` so
the LLM shapes the closing line as satisfied (0) or frustrated
(≥1).

`ExecuteAction` bypasses the SkyrimNet action's own cooldowns
and eligibility gates — the plugin owns when to fire, not
SkyrimNet.

**Call site is Papyrus.** `SkyrimNetApi.ExecuteAction` is a
Papyrus global function, so C++ VM-dispatches to a small
trampoline on `_ne_VisitQuest` (`RunSenderAction(actionName,
argsJson)`) that invokes it. The trampoline is the single
Papyrus call site for both call categories.

**We don't track the spoken line.** Subtitles, voice, event
log, memory — all inherited from whatever SkyrimNet does for
actions. When the natural-conclusion poll needs recent lines
for context, it samples from SkyrimNet's event history rather
than a plugin-side transcript.

### Conversation phase state machine

The whole conversation lifecycle is a small state machine whose
authoritative state is the current stage of `_ne_VisitQuest`.
C++ drives *when* to transition (LLM callbacks, watchdogs,
timers, event sinks) and *how* the world reacts (speech-line
dispatch, memory-hook trigger, faction manipulation, return
teleport); Papyrus fragments do the small per-stage side
effects; the engine auto-persists the stage across save/load.
See **Quest stages as source of truth** above.

**States** (encoded as integer stage numbers on
`_ne_VisitQuest`; the `VisitState::Mode` helper enum in C++
is a derivation from the current stage for logging and
dashboard convenience only, not stored):

| State         | Meaning                                                            | Quest stage |
| ------------- | ------------------------------------------------------------------ | ----------: |
| `Idle`        | No visit in flight. Machine is dormant.                             |         N/A |
| `Composing`   | Compose LLM call is in flight; sender hasn't been warped yet.       |         N/A |
| `Salutation`  | Sender warped to spawn marker, Follow package active, walking to player. Watchdog polls distance; on close-approach, sender speaks opening line and machine advances. | 10 |
| `Discuss`     | Free-motion conversation underway. Natural-conclusion LLM poll runs on interval; on a "not concluded" verdict combined with sustained silence, the plugin fires SkyrimNet's built-in `ContinueConversation` action as the ignore-irritation nudge. | 20 |
| `OnHold`      | Discuss suspended because the player or the sender entered vanilla dialogue (with anyone), a scene, or combat. Follow package stays on. Natural-conclusion poll pauses. Wall-clock timers accumulate but are not compared. | 25 |
| `ReEngage`    | Both parties returned to a free state after OnHold. Watchdog waits for the sender to close distance again; on close-approach, sender speaks a resumption line and machine returns to Discuss. | 27 |
| `Valediction` | Natural-conclusion poll returned true. Sender speaks a closing line (dispatched via `ExecuteAction` with `turn_kind: "valediction"`); dwell delay lets the line play out. SkyrimNet's own conversation lifecycle is expected to form end-of-conversation memories for both actors as the exchange concludes. | 30 |
| `ReturnHome`  | Follow package deactivates; a Travel package targeting the sender's snapshot position becomes active. Sender walks away from the player toward home. Watchdog polls exit conditions (distance, cell unload, timeout); when any trips, C++ teleports the sender to the snapshot and advances the quest to Stage 200. | 50 |

Numerical stage IDs are not adjacent so future intermediate states
can be inserted without renumbering existing ones.

**Transitions** (edge = event; targets = end state):

| From          | Event                                                          | To            |
| ------------- | -------------------------------------------------------------- | ------------- |
| `Idle`        | `Start` called                                                 | `Composing`   |
| `Composing`   | Compose LLM callback delivers valid briefing                   | `Salutation`  |
| `Composing`   | Compose LLM callback fails validation                          | `Idle` (roll back) |
| `Salutation`  | Sender-to-player distance <= `iVisitSalutationApproachDistanceUnits` AND opening line spoken | `Discuss` |
| `Salutation`  | Timeout: no close-approach within `iVisitApproachTimeoutSeconds` | `Idle` (roll back) |
| `Discuss`     | Natural-conclusion poll returns `true`                          | `Valediction` |
| `Discuss`     | Player enters DialogueMenu, scene, or combat; OR sender enters same | `OnHold` |
| `OnHold`      | Both parties back to free state                                 | `ReEngage`    |
| `ReEngage`    | Sender-to-player distance <= threshold AND resumption line spoken | `Discuss` |
| `Valediction` | Dwell delay (`iVisitValedictionDwellSeconds`) elapsed after closing line | `ReturnHome` |
| `ReturnHome`  | Any exit condition trips (sender-to-player distance ≥ `iVisitReturnHomeExitDistanceUnits`, sender's parent cell no longer attached, or `iVisitReturnHomeTimeoutSeconds` elapsed) → sender teleported to snapshot, quest advanced to Stage 200, Reset() releases alias | `Idle` |
| **Any**       | Sender dies                                                     | `Idle` (hard abort) |
| **Any**       | Player dies                                                     | `Idle` (hard abort) |
| **Any**       | Wall-clock time in current lifecycle exceeds `iVisitHardTimeoutSeconds` (default 900s = 15 min) | `Idle` (hard abort) |

**Hard-abort escape** is what protects the dispatcher from getting
stuck. If either participant dies or the whole lifecycle exceeds
the outer timeout, the machine unwinds immediately: Follow package
released via Quest.Stop+Reset, sender demoted from the faction,
return teleport attempted best-effort (skipped if sender is dead),
and cooldown applied. The dispatcher never waits on an in-flight
visit for longer than the outer timeout.

**Discuss-phase poll cadence (cheap-signal-gated).** The
natural-conclusion LLM poll is expensive; firing it on a
fixed real-time interval would either burn LLM calls faster
than needed (short interval) or miss the moment the beat has
landed (long interval). Instead the poll is *gated on cheap
signals*: a fast tick (default 1s wall-clock) evaluates
three thresholds, and the LLM poll fires only when at least
one of them is exceeded.

The three thresholds:

1. **Speech-turn count since the last poll**
   (`iVisitPollTurnCountThreshold`, default 4). Sampled from
   SkyrimNet's event history each tick. Roughly 30 real
   seconds of active back-and-forth at typical exchange
   pacing.
2. **In-game time since the last observed speech turn**
   (`iVisitPollSilenceGameMinutes`, default 10). Measured
   against `RE::Calendar::GetSingleton()->GetHoursPassed()`
   rather than wall-clock so the timer freezes while the
   game is paused, in a menu, or otherwise not advancing
   world time. At default timescale 20 (Skyrim's stock
   setting), 10 game-minutes is ~30 real seconds — matches
   the turn-count threshold's implied timing. Non-standard
   timescales stretch or compress this proportionally.
3. **In-game time since the last poll**
   (`iVisitPollMaxIntervalGameMinutes`, default 10). Safety
   ceiling that guarantees a poll fires at least once every
   ~30 real seconds of gameplay even during a heated
   exchange that never quiets down. Same in-game-time
   measurement as (2), same rationale for using it.

If **any** threshold is met, C++ triggers the LLM poll and
then acts on the verdict combined with a silence check:

- **Poll returned `should_conclude: true`** → C++
  VM-dispatches `Quest.SetStage(30)`; the machine advances
  to Valediction and fires the closing line via
  `ExecuteAction`.
- **Poll returned `should_conclude: false`** AND the
  silence threshold (2) is currently exceeded: fire
  SkyrimNet's built-in `ContinueConversation` action via
  `SkyrimNetApi.ExecuteAction("ContinueConversation",
  senderActor, "")`. This is the ignore-irritation nudge
  mechanism — instead of authoring our own nudge lines, we
  delegate to SkyrimNet's own "maintain an ongoing exchange"
  action, which produces an in-character prod from the
  sender (or an interjection routed appropriately per
  SkyrimNet's speaker-selection logic) without us having to
  script the words. Increment `ignoreNudgeCount`.
- **Poll returned `should_conclude: false`** AND silence
  threshold not currently exceeded (the poll was triggered
  by turn-count or safety ceiling on an actively-flowing
  conversation): do nothing further. The next threshold
  hit will re-poll.

After the LLM poll fires (regardless of verdict), reset the
turn-count and last-poll-time trackers so the gates start
fresh for the next window. The silence tracker is *not*
reset on poll fire — it resets only when a new speech turn
is observed, since it measures silence duration
independently of poll cadence.

**Ignore-irritation escalation.** If `ignoreNudgeCount`
reaches `iVisitMaxIgnoreNudges` (default 3) without the poll
ever returning `should_conclude: true` in between, C++
forces Valediction anyway — `SetStage(30)` — and the closing
line `ExecuteAction` call receives an elevated `nudge_count`
in its `argsJson`, which the LLM naturally reads as "the
sender is leaving frustrated" and shapes the valediction
line accordingly. The visit resolves as an *unsatisfied*
dispatch, tagged as such in the history.

`ignoreNudgeCount` is reset to 0 any time a fresh SkyrimNet
speech-history sample shows a new player turn after the
most recent nudge — the player re-engaging clears the
counter.

**OnHold entry conditions** (the specific "vanilla dialogue,
scene, or combat" detection):

- Player or sender enters vanilla `DialogueMenu` (with ANY
  actor). Detected via a `MenuOpenCloseEvent` sink filtered to
  `DialogueMenu::MENU_NAME` — same sink shape Phase 04 uses,
  but here it flags OnHold rather than triggering completion.
- Player or sender enters a scripted scene (`RE::TESScene`
  running with either as a participant). Detected by a scene-
  start ModEvent or a periodic scene-state poll if no ModEvent
  is available.
- Player or sender enters combat. Detected via
  `TESCombatEvent`. Combat is a hard state to transition out
  of cleanly, so combat-triggered OnHold has an additional
  guard: if combat lasts longer than
  `iVisitOnHoldCombatMaxSeconds` (default 60s), the machine
  hard-aborts rather than waiting indefinitely for the fight
  to resolve.

**ReEngage exit condition.** The player must both (a) be within
`iVisitReEngageApproachDistanceUnits` (default 400u, slightly
larger than the Salutation threshold to give the sender room
to catch up) and (b) not be in DialogueMenu / scene / combat.
Once satisfied for at least one poll interval, C++ triggers a
resumption line via another `SkyrimNetApi.ExecuteAction` call
with `"turn_kind": "reengage"` ("As I was saying — …") and the
machine returns to Discuss.
The exchanges before OnHold remain in the natural-conclusion
poll's rolling last-N-lines window, so the poll picks up where
it left off.

### Natural-conclusion LLM poll

Discuss advances to Valediction only on an LLM's read of
whether the exchange has reached a satisfying stopping point.

Prompt file:
`statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_visit_conclusion_poll.prompt`
— use the `narrative_engine_director` variant.

Cadence: gated on cheap signals per the **Discuss-phase poll
cadence** section above (turn count / silence / max-interval).
Paused during OnHold and ReEngage. Not evaluated during
Salutation or Valediction.

Context payload:

- `sender`, `player` — form IDs and display names.
- `sender_goal` — the composer's `briefing` verbatim.
- `topic_tag`, `mood`, `desired_direction` — from the
  composer's response.
- `recent_lines` — the last N exchanges (default N=8),
  chronologically, each `{ speaker: "sender" | "player",
  text }`. Sampled fresh from SkyrimNet's event history on
  each fire (sender turns land there via `ExecuteAction`;
  player turns via whatever channel SkyrimNet uses for
  replies). Not persisted plugin-side.
- `elapsed_seconds`, `nudge_count` — advisory context only.

Output:

```json
{ "should_conclude": <bool>, "rationale": "<one sentence>" }
```

The rationale is logged and surfaced on the dashboard's Visit
tab so the design intent is legible without tailing the LLM
response body.

**Prompt discipline** (`docs/CUSTOM_PROMPTS.md`): frame the
poll's job as reading the conversation as a story beat —
not counting exchanges, not enforcing a minimum length, not
gating on the sender explicitly saying "goodbye."

**Failure handling.** Malformed / missing / errored responses
count as `should_conclude=false`; the next gate trip retries.
`iVisitConclusionPollMaxConsecutiveFailures` (default 6)
back-to-back failures hard-abort the visit.

### Cleanup and return teleport

Cleanup runs in three phases so the sender's departure looks
like a vanilla NPC leaving a scene rather than a hard cut:
walk away visibly during ReturnHome, then teleport the rest of
the way once offscreen, then shutdown.

**Phase 1 — Anchor placement at Start.** C++ snapshots the
sender's position / angle / cell into `VisitState` (pre-dispatch
reference for validation and dashboard logging), then invokes
`EnsureQuestStarted` on `_ne_VisitQuest`. The alias-fill pass
runs synchronously during that call: `Sender` fills first via
faction-rank FMR, then `SpawnMarker` fills via nearest-XMarkerHeading
FMR, then `ReturnAnchor` fires its `Create Reference to Object`
against `XMarkerHeading` positioned `At: Sender` — spawning a
new XMarker REFR at the sender's current world position and
binding it into the alias. Because Sender is filled first,
`At: Sender` resolves cleanly; the Travel package's
`Alias:ReturnAnchor` destination is populated by the time
Stage 50 flips its stage-gate condition.

C++ reads `ReturnAnchor.GetReference()` post-`EnsureQuestStarted`
and stashes the resulting FormID in
`VisitState::returnAnchorFormID` for logging / dashboard
visibility. That field is not load-bearing — the alias itself
holds the ref across saves.

If `EnsureQuestStarted` fails (any alias fill misses, e.g. no
XMarkerHeading in the SpawnMarker's distance band), the whole
Start rolls back before we've promoted the sender to rank 4 or
committed to the visit; no anchor cleanup is needed because no
marker was created.

**Phase 2 — ReturnHome walk.** On Stage 30 → 50 the Sender
alias's package selection re-evaluates: Follow's `GetStage <
50` condition fails and Travel-home's `GetStage >= 50` passes,
so Travel becomes active and the sender walks toward the
anchor.

A C++ watchdog polls exit conditions every ~500ms:

- **Distance**: sender-to-player ≥
  `iVisitReturnHomeExitDistanceUnits` (default 1500u).
- **Cell unload**: sender's parent cell no longer attached to
  the loaded worldspace (walked through a load door).
- **Timeout**: wall-clock time in ReturnHome ≥
  `iVisitReturnHomeTimeoutSeconds` (default 120s).

**Phase 3 — Final teleport and shutdown.** On exit trip C++
atomically: read the ReturnAnchor alias's REFR and
`sender->MoveTo(anchor)`, set Z angle to `returnAngle`,
`EvaluatePackage()`, `DemoteSenderToCandidate`, `SetStage(200)`.
Stage 200's fragment does `Shutdown()` — Disable+Delete the
alias-created XMarker, then `Stop() + Reset()`; `Reset()`
releases both packages via the alias-empty transition.

**Hard-abort short-circuit.** On sender/player death or
outer-timeout hard-abort from any pre-ReturnHome state, skip
the ReturnHome walk and jump straight to Phase 3 (teleport
skipped if sender is dead).

**On-load reconciliation** for ReturnHome specifically: the
Travel package resumes on its own (alias fills are
engine-persisted); watchdog re-arms from load-time. See
**Persistence** for the full per-stage reconciliation.

### Player-context gate on `IsAvailable`

`IsAvailable` gates dispatch only; once running, the state
machine's OnHold / Re-engage / hard-abort transitions handle
context changes.

Returns `true` when **all**:

- Global preconditions pass (dispatcher already gates player
  not-in-combat / not-in-dialogue / not-in-scene / not-in-DND).
- `LocationKeywords::IsVisitHostile()` returns false. Same
  underlying keyword tables as `IsDangerous` (dungeons, bandit
  camps, hostile lairs) plus a small deny-list for
  stranger-walking-up-is-jarring cells (`LocTypeJail`,
  `LocTypeArena`, `LocTypeBarracks`).
- `PublicIsMemorySystemReady()` returns true.
- Candidate-pool count (top-N `PublicGetActorEngagement`
  post-viability-filter) ≥ `iVisitMinSenderCandidates`
  (default 3).

Interior safe contexts (inns, homes, owned shops) are allowed.
Interior-only cells (arena, dream sequences) excluded by the
deny-list. Player-moving vs. stationary and follower-present
vs. alone are not gated (Follow package handles both).

### Direction flow

`ActionContext::desiredDirection` and `tensionDelta` pass through
to the visit compose prompt the
same way they pass through to the letter compose prompt. The
prompt uses the direction to bias mood selection (raise → menacing
/ urgent; lower → contrite / mournful / warm) and to shape the
briefing's tone.

---

## ESP content

All new forms are ESL-safe FormIDs (FE-prefix). The `.esp` gains
one faction, one quest, and two AI packages.

- **`_ne_VisitSenderFaction`** — Faction. No reactions, no
  crime, no combat, no vendors, no relations. Pure marker.
- **`_ne_VisitQuest`** — Quest. `Run Once = false`,
  `Start Game Enabled = false`. `_ne_VisitQuest` script attached.
  - Aliases:
    - `Sender` — FMR on
      `GetFactionRank _ne_VisitSenderFaction >= 4`; "In Loaded
      Area" unchecked; Optional OFF. Carries both AI packages
      (see below); the engine's package selector uses their
      stage-conditions to activate one at a time.
    - `SpawnMarker` — FMR nearest XMarkerHeading within
      `iVisitMarkerMinDistanceUnits`..`iVisitMarkerMaxDistanceUnits`
      of the player, `GetLineOfSight PlayerRef == 0`,
      Optional OFF.
    - `ReturnAnchor` — Fill Type `Create Reference to Object`,
      Object = `XMarkerHeading`, Create = `At: Sender`.
      Optional OFF. Fills during `EnsureQuestStarted`'s
      alias-fill pass by spawning a new XMarker REFR at the
      Sender alias's current world position; **must be
      ordered after Sender in the alias list** so
      `At: Sender` resolves to a filled reference.
  - Stage map:

    | Stage | Meaning | Fragment |
    | ----: | ------- | -------- |
    | 0 | Startup (Startup Stage ON) | `SetStage(10)` |
    | 10 | Salutation entry | MoveTo sender to SpawnMarker; `EvaluatePackage` |
    | 20 | Discuss | empty |
    | 25 | OnHold | empty |
    | 27 | ReEngage | empty |
    | 30 | Valediction | empty |
    | 50 | ReturnHome entry | `Sender.GetActorReference().EvaluatePackage()` (swaps Follow → Travel) |
    | 60 | Rollback | `SetStage(200)` |
    | 200 | Terminal (Complete Quest ON) | `Shutdown()` |

- **`_ne_VisitFollow`** — First AI package on the Sender alias.
  Package type: **Follow**. Target Actor: `PlayerRef`. Follow
  Distance: `iVisitSalutationApproachDistanceUnits` (~300u
  default). Flags: `Ignore Combat = false`, `Interrupt Override
  = None`, `Must Complete = off`, `Allow Idle Chatter = off`.
  **Package condition**: `GetStage _ne_VisitQuest < 50` (Run
  On: Subject).
- **`_ne_VisitReturnTravel`** — Second AI package on the Sender
  alias. Package type: **Travel**. Destination:
  `Alias:ReturnAnchor`. Flags: `Ignore Combat = false`,
  `Interrupt Override = None`, `Must Complete = off`. **Package
  condition**: `GetStage _ne_VisitQuest >= 50` (Run On:
  Subject). Only active during ReturnHome.

The SpawnMarker FMR picks from Skyrim's own dense pool of
XMarkerHeadings. Cells lacking one in the distance band
(extreme wilderness, sparse custom-mod cells) fail the fill,
`EnsureQuestStarted` returns `engineResult=false`, and the
action rolls back — correct behavior for cells where a warp
would end badly.

Use the vanilla `PlayerRef` global constant as the Follow
package's Target Actor. If the CK insists on an alias-typed
ref, add a `PlayerRef` reference alias filled by
`Specific Reference` = `PlayerRef` (0x14).

---

## The action: `NPCVisitAction`

### Preconditions (`IsAvailable`)

See **Player-context gate on `IsAvailable`** above for the
full list.

### Parameters

- `urgency_hint` — optional string from action-select, one of
  `{ low, medium, high }`. Same shape as the letter action's
  `urgency_hint`. Used as a soft input to the compose LLM call
  to bias `mood`. Defaults to `medium` if omitted.

That's it — same discipline as letters. The compose LLM call
picks sender, topic, and briefing from the player's SkyrimNet
history using `desiredDirection` and `urgency_hint`.

### `Start`

1. Re-validate `IsAvailable`.
2. Check the machine's current state (read
   `_ne_VisitQuest->GetCurrentStageID()` + the
   `g_composingSender` flag). If either indicates in-flight,
   refuse with `already in flight`.
3. Set `g_composingSender = true` (the Composing pseudo-state;
   clears when the quest reaches Stage 10).
4. Build the compose prompt context (sender-viability-filtered
   top 12 from `PublicGetActorEngagement` with per-candidate
   memories; attach `desired_direction` and `tension_delta`).
5. Fire the async compose LLM call.
6. `Start` returns immediately with
   `StartResult{ started=true, detail="composing briefing" }`.
7. In the compose callback (main thread after marshal):
   - Validate response shape and sender FormID membership.
   - On validation failure: clear `g_composingSender` and any
     partial `VisitState` snapshot; call
     `ActionDispatcher::CompleteAction("npc_visit")`; log the
     LLM response body.
   - On success:
     - Snapshot sender position / angle / cell into
       `VisitState`.
     - Cache the composer's briefing / topic / mood / tags in
       `VisitState`.
     - Sweep stale rank-4 members of
       `_ne_VisitSenderFaction`; promote the sender to rank 4.
     - `EnsureQuestStarted` on `_ne_VisitQuest`; alias-fill
       binds Sender, SpawnMarker, and (via
       `Create Reference to Object At: Sender`) ReturnAnchor;
       Stage 0 fragment auto-advances to Stage 10; Stage 10
       fragment MoveTo's + `EvaluatePackage`.
       `g_composingSender` clears.
     - Read `ReturnAnchor.GetReference()->GetFormID()` into
       `VisitState::returnAnchorFormID` for dashboard/logging
       visibility (not load-bearing — the alias itself holds
       the ref across saves).
     - Register the Salutation distance watchdog; arm the
       DialogueMenu / TESCombat / scene-start / death sinks.

After `Start`'s callback runs, the state machine is fully
committed to running to a terminal state.

### `DetectAndRollbackFailedStart`

Reads quest stage as source of truth:

- `g_composingSender == true`: return false; the callback owns
  its own failure branch.
- Stage 10 within `iVisitApproachTimeoutSeconds`: return false.
- Stage 10 past the window: sender never approached (drew into
  combat, package didn't bind, player fast-traveled).
  VM-dispatch `Quest.SetStage(60)`, demote sender, teleport
  home immediately (skip the ReturnHome walk), return true.
- Stage ∈ {20, 25, 27, 30, 50}: return false; `DetectCompletion`
  owns termination.

### `DetectCompletion`

Returns true only when the quest is no longer running (stage 0,
not `IsCompleted()` per the CLAUDE.md memory on `IsRunning`
unreliability) AND `g_terminalCleanupDone == true` (set at the
tail of the ReturnHome shutdown chain).

Visit stays in-flight from `Start` through the ReturnHome →
Idle transition. This is the deliberate design shift from
letters/ambush: while the visit is running the Director must
not pick new actions, or the "spontaneous action interrupting
the conversation" failure mode returns.

Cooldown starts at the ReturnHome → Idle transition. Worst-case
Director-side dead time is bounded by
`iVisitReturnHomeTimeoutSeconds`.

**Memory formation** happens as part of SkyrimNet's own
conversation lifecycle when the `ExecuteAction`-initiated
exchange tears down. If a plugin-side trigger *is* needed
(verify during implementation), the Stage 50 fragment is the
site.

### What's NOT in this action

- No re-dispatch on the same sender after a hard-abort — the
  viability filter drops dead senders on the next tick.
- No mid-conversation goal rewrite — the compose call fires
  once; the poll evaluates against the original briefing.
- No custom camera or scripted framing — vanilla free-motion
  throughout.

---

## Settings

New keys in `[Director]`:

| Key                                     | Default | Meaning                                                             |
| --------------------------------------- | ------: | ------------------------------------------------------------------- |
| `iVisitMinSenderCandidates`             | 3       | min viable candidates required for `IsAvailable` to return true     |

New `[Actions]` section keys — dispatch / composition:

| Key                                     | Default | Meaning                                                                                 |
| --------------------------------------- | ------: | --------------------------------------------------------------------------------------- |
| `iVisitBriefingMaxWords`                | 120     | upper bound on LLM-generated briefing length                                            |
| `iVisitBriefingMinWords`                | 40      | lower bound                                                                             |
| `iVisitMarkerMinDistanceUnits`          | 800     | closest a spawn marker may be to the player                                             |
| `iVisitMarkerMaxDistanceUnits`          | 2500    | farthest a spawn marker may be from the player                                          |

New `[Actions]` section keys — state machine timing:

| Key                                            | Default | Meaning                                                                                                            |
| ---------------------------------------------- | ------: | ------------------------------------------------------------------------------------------------------------------ |
| `iVisitApproachTimeoutSeconds`                 | 45      | Salutation timeout: seconds after Start before rollback if the sender hasn't closed distance for an opening line   |
| `iVisitSalutationApproachDistanceUnits`        | 300     | sender-to-player distance at which the Salutation opening line fires and the machine advances to Discuss           |
| `iVisitReEngageApproachDistanceUnits`          | 400     | sender-to-player distance for the ReEngage resumption line to fire (slightly larger to give the sender room)       |
| `iVisitPollGateTickSeconds`                    | 1       | wall-clock cadence at which C++ evaluates the three cheap-signal gates that decide whether to fire the LLM poll     |
| `iVisitPollTurnCountThreshold`                 | 4       | speech turns observed since the last poll before a poll fires (rough proxy for ~30 real sec of active exchange)    |
| `iVisitPollSilenceGameMinutes`                 | 10      | in-game minutes of silence since the last observed speech turn before a poll fires (~30 real sec at Skyrim's default timescale 20; also doubles as the "silence exceeded → fire ContinueConversation" threshold when the poll returns "not concluded") |
| `iVisitPollMaxIntervalGameMinutes`             | 10      | in-game minutes since the last poll before a poll fires as a safety ceiling (~30 real sec at default timescale) — guarantees the LLM verdict refreshes even during a productive back-and-forth that never trips the other gates |
| `iVisitConclusionPollMaxConsecutiveFailures`   | 6       | consecutive poll failures (parse errors, LLM timeouts) before hard-abort                                           |
| `iVisitMaxIgnoreNudges`                        | 3       | consecutive `ContinueConversation` fires without a poll ever returning "concluded" in between; on reaching this cap, the plugin forces Valediction with `nudge_count` in the args so the closing line reads as a frustrated exit |
| `iVisitOnHoldCombatMaxSeconds`                 | 60      | how long OnHold may persist while combat is the trigger before hard-abort                                          |
| `iVisitValedictionDwellSeconds`                | 5       | wall-clock seconds between Valediction closing line and the ReturnHome transition                                  |
| `iVisitReturnHomeExitDistanceUnits`            | 1500    | sender-to-player distance during ReturnHome that triggers the final teleport + shutdown                            |
| `iVisitReturnHomeTimeoutSeconds`               | 120     | outer wall-clock cap on ReturnHome — if the sender is still walking (hasn't hit the distance threshold and hasn't left the loaded worldspace) after this many seconds, teleport them anyway |
| `iVisitHardTimeoutSeconds`                     | 900     | outer wall-clock timeout on the entire visit lifecycle (dispatch → return teleport); hard-abort past this          |

Rank-4 headroom, sweep-of-stragglers, and faction-cleanup semantics
mirror the letter action; no new settings needed for those.

---

## Persistence

Quest stages are the authoritative state carrier (engine
auto-persists). The co-save record only carries data that isn't
recoverable from the quest / aliases / SkyrimNet.

New co-save record `'NEVS'` (NarrativeEngine Visit), version 1.
Payload:

```
uint32  senderFormID                      ; via WriteRecordData; ResolveFormID on load.
                                          ; Redundant with the Sender alias's engine-persisted fill,
                                          ; but keeping it lets the reload path validate that the alias
                                          ; still resolves to the same actor and detect drift.
uint32  returnCellFormID                  ; cell the sender was in pre-dispatch
float   returnX, returnY, returnZ
float   returnAngleZ
uint32  returnAnchorFormID                ; XMarker spawned by the ReturnAnchor alias's
                                          ; `Create Reference to Object At: Sender` fill.
                                          ; Persisted for dashboard/logging visibility only —
                                          ; the alias itself holds the ref across saves, so
                                          ; ReturnHome's teleport reads from the alias. If
                                          ; ResolveFormID fails on load (external cleanup),
                                          ; skip the field; teleport uses the alias directly.
double  dispatchedAtRealSeconds           ; start of the whole lifecycle; drives iVisitHardTimeoutSeconds
uint8   ignoreNudgeCount                  ; unanswered-nudges counter
uint8   consecutivePollFailures           ; running counter for the poll-failure hard-abort
uint16  briefingTextLen
char[]  briefingText
uint16  topicTagLen
char[]  topicTag
uint16  moodLen
char[]  mood
uint16  tagsJsonLen                       ; the composer's tags array, serialized
char[]  tagsJson
```

**Not persisted:**

- Current phase (read from
  `_ne_VisitQuest->GetCurrentStageID()`).
- Per-phase entered-at timestamps — per-phase timers get a
  fresh window from load-time. `iVisitHardTimeoutSeconds` is
  measured from `dispatchedAtRealSeconds` so total lifecycle
  budget is honored.
- Discuss poll-gate state (turn count, last-poll-time,
  last-speech-turn-time) — reset to 0/now on load. First
  post-load poll waits for a fresh gate trip.
- `recent_lines` — re-sampled from SkyrimNet's event history
  on each poll fire.
- Sender / SpawnMarker / ReturnAnchor alias fills — engine-
  persisted on the aliases.

**On load, reconciliation** (dispatched from a single pass
that reads the current stage and branches):

- Quest not running / stage 0 → nothing to do; leave VisitState
  clear.
- Stage 10 (Salutation): resume; timeout re-arms from
  load-time. If the sender alias didn't refill (rare
  cross-load-order case), demote and route to Stage 60.
- Stage 20 (Discuss): resume; poll and irritation timers
  re-arm from load-time; poll re-samples `recent_lines` on
  next fire.
- Stage 25 (OnHold): resume; exit condition re-checked;
  combat-OnHold timeout re-arms from load-time.
- Stage 27 (ReEngage): resume; distance watchdog re-arms.
- Stage 30 (Valediction): closing-line dwell re-arms from
  load-time.
- Stage 50 (ReturnHome): resume the walk (Travel package +
  ReturnAnchor fill are engine-persisted); exit-condition
  watchdog and timeout re-arm from load-time. Teleport reads
  the ReturnAnchor alias directly; the `returnAnchorFormID`
  co-save field is dashboard-only and safe to skip if it
  fails to resolve.
- Stage 60 (rollback): advance to Stage 200 immediately.
- Stage 200: normally shouldn't appear mid-load; if it does,
  run `Reset()` from C++ and clear VisitState.
- **Outer timeout guard**: if
  `now - dispatchedAtRealSeconds > iVisitHardTimeoutSeconds`
  on any non-Idle reconciliation, jump straight to the
  hard-abort teardown (teleport, demote, Stage 200) — a
  save/reload doesn't earn extra lifecycle budget.

---

## Pipeline integration

- Visit registers as a normal `IAction`. Uses the C++
  `ActionDispatcher::CompleteAction` path (introduced in
  Phase 04); no new plumbing.
- **In-flight duration is the whole state machine** —
  `Start` through the ReturnHome → Idle transition. During
  that window no other Director action can fire. See
  `DetectCompletion` above for rationale.
- Cooldown timing: (action-select) + (compose LLM) +
  (Salutation) + (Discuss) + (Valediction dwell) +
  (ReturnHome walk + teleport, bounded by
  `iVisitReturnHomeTimeoutSeconds`) + `iActionCooldownSeconds`
  — all inside the in-flight window; cooldown starts at Idle.
- Hard-aborts apply cooldown normally; the recency filter
  deprioritizes visit for a window.

---

## Dashboard

Adds a third tab (**Visit**) to Phase 04's tab bar.

### Tab 3 — Visit

Three stacked sections (Current conversation / Recent poll
verdicts / Recent history).

**Current conversation** (rendered when `visit.mode != 'idle'`;
C++ derives `mode` from quest stage + `g_composingSender`
before emitting):

- Sender name, phase badge, topic tag, mood, briefing preview.
- Nudge counter `nudges: N / M` while in Discuss (hidden at 0).
- Phase-specific aux line:
  - Salutation → elapsed + live distance-to-approach.
  - Discuss → next-gate summary (turns N/M · silence Ns ·
    interval Ns).
  - On-Hold → trigger label.
  - Re-Engage → live distance.
  - Valediction → closing-dwell countdown.
  - ReturnHome → live distance + cell load state + timeout
    countdown.
- Elapsed-in-lifecycle timer vs. `iVisitHardTimeoutSeconds`.

**Recent poll verdicts** (last ~5 polls; timestamp,
`should_conclude`, rationale).

**Recent history** (last ~10 dispatches; timestamp, sender,
topic, outcome, duration). Outcomes: `completed`,
`unsatisfied`, `rolled_back`, `aborted`.

### Schema additions on `DirectorState`

```ts
visit: {
    mode: 'idle' | 'composing' | 'salutation' | 'discuss'
        | 'on_hold' | 're_engage' | 'valediction' | 'return_home';
    sender_name: string;                    // empty when idle
    topic_tag: string;                      // empty when idle
    mood: string;                           // empty when idle
    briefing_preview: string;               // empty when idle
    dispatched_at: number;                  // 0 when idle
    current_phase_entered_at: number;       // 0 when idle
    nudge_count: number;                    // 0 unless in discuss with nudges spoken
    on_hold_trigger:
        | 'player_dialogue' | 'player_combat'
        | 'sender_combat'   | 'scripted_scene'
        | null;                             // null unless mode == on_hold
    recent_polls: Array<{
        at: number;
        should_conclude: boolean;
        rationale: string;
    }>;                                     // empty unless a poll has fired this run
    recent: Array<{
        dispatched_at: number;
        sender_name: string;
        topic_tag: string;
        outcome: 'completed' | 'unsatisfied' | 'rolled_back' | 'aborted';
        duration_seconds: number;
    }>;
};
```

C++ populates this in `ComposeFullStateJSON` by dumping
`VisitState`'s current fields, the recent-poll ring buffer,
and the recent-history ring buffer (which mirrors the
Phase 03 `g_recentlyFiredActions` buffer pattern but scoped
to visit).

### Dashboard-side updates

- `TabBar.tsx` — third tab button.
- `App.tsx` — `activeTab` union + conditional render.
- `LastEvaluation.tsx` — `→ fired: npc_visit` row.
- `dashboard/styles.css` — phase badges (Composing/Salutation
  amber, Discuss green, Re-Engage light green, On-Hold
  yellow-orange, Valediction blue, ReturnHome muted) + history
  pills (completed green, unsatisfied yellow, rolled_back grey,
  aborted red).

---

## File map

New C++:

- `include/NPCVisitAction.h` / `src/NPCVisitAction.cpp` —
  IAction wrapper + state-machine transition logic.
- `include/VisitComposer.h` / `src/VisitComposer.cpp` —
  sender-selection and briefing composition LLM call.
- `include/VisitConclusionPoll.h` / `src/VisitConclusionPoll.cpp`
  — natural-conclusion poll: gate evaluation, context assembly
  (samples `recent_lines` from SkyrimNet's event history),
  response parsing, failure accounting.
- `include/VisitState.h` / `src/VisitState.cpp` — single-slot
  data module (snapshot position/angle/cell, briefing/topic/
  mood/tags, counters, dispatched-at, recent-history ring for
  the dashboard) + co-save handlers. `DerivePhase()` helper
  reads `Quest.GetCurrentStageID()`. Does not hold current
  phase or `recent_lines`.
- `include/SenderCandidatePool.h` / `src/SenderCandidatePool.cpp`
  — shared helper extracted from `LetterComposer` so both
  actions reuse the candidate build.

New prompts:

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_visit_compose.prompt`
- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_visit_conclusion_poll.prompt`

New ESP content:

- `_ne_VisitSenderFaction` marker faction.
- `_ne_VisitQuest` quest with `Sender`, `SpawnMarker`,
  `ReturnAnchor` reference aliases and the 0/10/20/25/27/30/50/60/200
  stage map. Script attached.
- `_ne_VisitFollow` and `_ne_VisitReturnTravel` AI packages on
  the Sender alias (stage-condition-gated so only one is
  eligible at a time).

New Papyrus (`esp/Source/Scripts/_ne_VisitQuest.psc`):

- `RunSenderAction(actionName, argsJson)` trampoline —
  `SkyrimNetApi.ExecuteAction` call site for both plugin-owned
  turns and `ContinueConversation` nudges.
- Stage 0 fragment: `SetStage(10)`.
- Stage 10 fragment: MoveTo sender to spawn marker;
  `EvaluatePackage()`.
- Stages 20 / 25 / 27 / 30: empty markers.
- Stage 50 fragment (ReturnHome entry):
  `Sender.GetActorReference().EvaluatePackage()` so the package
  selector swaps Follow → Travel. Also the site for a
  SkyrimNet memory-formation hook call if implementation shows
  one is needed.
- Stage 60 fragment: `SetStage(200)`.
- Stage 200 fragment: `Shutdown()` (Disable+Delete the
  ReturnAnchor REFR the alias created, then `Stop()` +
  `Reset()`).

Modified:

- `include/Settings.h`, `src/Settings.cpp`,
  `statics/SKSE/Plugins/NarrativeEngine.ini` — new keys per
  **Settings**.
- `include/LocationKeywords.h` / `src/LocationKeywords.cpp` —
  new `IsVisitHostile()` predicate.
- `include/LetterComposer.h` / `src/LetterComposer.cpp` —
  refactor to route through `SenderCandidatePool`.
- `src/Plugin.cpp` — register `NPCVisitAction`; wire
  `VisitState`'s co-save callbacks; register event sinks
  (`MenuOpenCloseEvent`, `TESCombatEvent`, scene-start,
  `TESDeathEvent`).
- `src/DashboardUIManager.cpp` — emit the new `visit` payload.
- Dashboard: `types.ts`, `TabBar.tsx`, `App.tsx`,
  `LastEvaluation.tsx`, new `tabs/VisitTab.tsx`,
  `CurrentConversationPanel.tsx`, `RecentPollsPanel.tsx`,
  `VisitHistory.tsx`, `styles.css`.

---

## Implementation plan

Sequential. Each step is **entirely Claude's work (C++ / TypeScript /
prompts)** or **entirely the user's work (Creation Kit + Papyrus)**.
Every step has a clear self-contained verification and builds on the
previous ones.

---

### Step 1 — Settings expansion

- [x] Complete

**[CLAUDE]**

**Goal:** Add every new INI key Phase 05 needs and thread them
through `Settings::Get()`.

**Files:**

- `include/Settings.h` — new fields per the **Settings** section.
- `src/Settings.cpp` — INI reads for each new key in
  `[Director]` and `[Actions]`.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — document each new
  key inline with a comment explaining what it controls.

**Sub-tasks:**

1. Add fields to `Settings` struct (matching the table in the
   **Settings** section): `visitMinSenderCandidates`,
   `visitBriefingMaxWords`, `visitBriefingMinWords`,
   `visitMarkerMinDistanceUnits`, `visitMarkerMaxDistanceUnits`,
   `visitApproachTimeoutSeconds`,
   `visitSalutationApproachDistanceUnits`,
   `visitReEngageApproachDistanceUnits`,
   `visitPollGateTickSeconds`, `visitPollTurnCountThreshold`,
   `visitPollSilenceGameMinutes`,
   `visitPollMaxIntervalGameMinutes`,
   `visitConclusionPollMaxConsecutiveFailures`,
   `visitMaxIgnoreNudges`, `visitOnHoldCombatMaxSeconds`,
   `visitValedictionDwellSeconds`,
   `visitReturnHomeExitDistanceUnits`,
   `visitReturnHomeTimeoutSeconds`, `visitHardTimeoutSeconds`.
2. Add default values matching the Settings table.
3. Add INI reads with `GetPrivateProfileInt` (or the existing
   Settings-reading helper).
4. Add each key + default + comment to
   `statics/SKSE/Plugins/NarrativeEngine.ini`, grouped under
   `[Director]` and `[Actions]` per the Settings section's
   grouping.

**Specifics:**

- `iVisitPollSilenceGameMinutes` and
  `iVisitPollMaxIntervalGameMinutes` are the only new keys in
  in-game minutes. Store them as ints; convert to game-seconds
  (multiply by 60) at the poll-gate evaluation site so the
  runtime comparison is against
  `RE::Calendar::GetSingleton()->GetHoursPassed() * 3600.0` (or
  the equivalent seconds accessor).
- The INI comment for each of the two in-game-minute keys must
  explicitly note that they're measured in game time, not real
  time, and reference the ~30 real-sec equivalent at Skyrim's
  default timescale 20.

**Verify:** `pwsh -File build.ps1 build` succeeds. Boot Skyrim;
the existing `Settings: loaded from …` debug log line shows the
new keys with their configured values (bump one via the INI and
reload to confirm the read path).

---

### Step 2 — `LocationKeywords::IsVisitHostile` predicate

- [x] Complete

**[CLAUDE]**

**Goal:** Add the predicate `IsAvailable` gates on.

**Files:**

- `include/LocationKeywords.h` — declare the predicate.
- `src/LocationKeywords.cpp` — implement.

**Sub-tasks:**

1. Add a `kFaceToFaceHostileExtra` (or `kVisitHostileExtra`)
   static array next to the existing `kDangerous` array,
   containing `LocTypeJail`, `LocTypeArena`, `LocTypeBarracks`.
2. Implement `IsVisitHostile(location)`:
   - Return true immediately if `IsDangerous(location)` returns
     true.
   - Otherwise scan the location's keyword array for any keyword
     in `kVisitHostileExtra`. Return true on hit.
   - Return false.

**Specifics:**

- Keep the predicate cheap: one loop over the location's keyword
  array, no allocations. `IsDangerous` already follows this
  shape; `IsVisitHostile` is a wrapper that delegates + extends.

**Verify:** Build clean. Add a temporary debug log line in
`ActionDispatcher` that prints
`LocationKeywords::IsVisitHostile(currentLocation)` on every
tick. Boot Skyrim; walk between a market (false), a dungeon
(true), a jail cell (true), and a mine (true). Remove the debug
log after verification.

---

### Step 3 — `VisitState` data module + co-save persistence

- [x] Complete

**[CLAUDE]**

**Goal:** Stand up the single-slot state module that owns the
data C++ needs during a running visit, plus its co-save round
trip. No state machine transitions yet — this is the data
substrate.

**Files:**

- `include/VisitState.h` — public API.
- `src/VisitState.cpp` — implementation.
- `src/Plugin.cpp` — wire `VisitState::OnSave/OnLoad/OnRevert`
  into the serialization dispatcher.

**Sub-tasks:**

1. Declare `VisitState::Mode` enum: `Idle`, `Composing`,
   `Salutation`, `Discuss`, `OnHold`, `ReEngage`, `Valediction`,
   `ReturnHome`. Used for logging/dashboard only.
2. Declare a `Snapshot` struct holding: `RE::FormID
   senderFormID`, `RE::FormID returnCellFormID`, `RE::NiPoint3
   returnPosition`, `float returnAngleZ`, `RE::FormID
   returnAnchorFormID`, `std::string briefingText`, `std::string
   topicTag`, `std::string mood`, `std::vector<std::string>
   tags`, `double dispatchedAtRealSeconds`, `std::uint8_t
   ignoreNudgeCount`, `std::uint8_t consecutivePollFailures`.
3. Declare a `HistoryEntry` struct for the dashboard's recent
   ring: `double dispatchedAt`, `std::string senderName`,
   `std::string topicTag`, enum outcome (`completed`,
   `unsatisfied`, `rolled_back`, `aborted`), `double
   durationSeconds`. Ring buffer size 10, per-process only (not
   persisted).
4. Public API: `GetSnapshot()`, `SetSnapshot(Snapshot)`,
   `Reset()`, `PushHistory(HistoryEntry)`, `GetHistory()`,
   `OnSave`, `OnLoad`, `OnRevert`.
5. Add `DerivePhase()` helper: takes the `g_composingSender`
   in-process flag (declared as `std::atomic<bool>` at file
   scope) and returns `Composing` when the flag is set,
   otherwise looks up `_ne_VisitQuest` and reads its current
   stage, mapping 0 → `Idle`, 10 → `Salutation`, 20 → `Discuss`,
   25 → `OnHold`, 27 → `ReEngage`, 30 → `Valediction`, 50 →
   `ReturnHome`, 60 → `Idle` (rollback in progress), 200 →
   `Idle` (shutting down). Returns `Idle` if the quest isn't
   resolved yet. For this step, since the quest doesn't exist,
   `DerivePhase()` returns `Idle` unconditionally when the flag
   is clear — the real mapping wires in at Step 8 once the CK
   content resolves.
6. Storage is one `Snapshot` field guarded by `std::mutex`.
7. `OnSave`: open a record of type `'NEVS'` version 1, write the
   Snapshot fields per the **Persistence** section's payload
   layout.
8. `OnLoad`: read the payload; `ResolveFormID` for
   `senderFormID`, `returnCellFormID`, `returnAnchorFormID`
   (log warning + clear on failure). Reconciliation branches
   are stubbed for now — the reload cases that require touching
   the quest wire in at Step 8 and beyond. For this step:
   log `VisitState::OnLoad: restored snapshot` and leave the
   Snapshot as-loaded.
9. `OnRevert`: `Reset()`.
10. In `Plugin.cpp`, register the three callbacks alongside the
    Phase 03 / 04 records.

**Specifics:**

- Co-save record type ID `'NEVS'`, version 1. Freeze on merge.
- The `Snapshot::tags` field serializes as JSON per the schema
  in **Persistence** (`tagsJson` length + bytes).
- Do NOT persist `Mode` — it's derived. Do NOT persist the
  recent-history ring — it's per-process.

**Verify:** Build clean. Boot Skyrim with an empty snapshot;
save; the `'NEVS'` record should appear in the co-save
(inspect via xEdit-on-cosave if needed). Reload; log shows
`VisitState::OnLoad: restored snapshot`.

---

### Step 4 — Extract `SenderCandidatePool` shared helper

- [x] Complete

**[CLAUDE]**

**Goal:** Factor the candidate-build code out of Phase 04's
`LetterComposer` into a shared module so `VisitComposer` (Step
5) can reuse it. This is a non-behavioral refactor; letters
must keep working.

**Files:**

- `include/SenderCandidatePool.h` / `src/SenderCandidatePool.cpp`
  — new module.
- `include/LetterComposer.h` / `src/LetterComposer.cpp` —
  refactor to route through the new helper.

**Sub-tasks:**

1. Design the helper's public API:

   ```cpp
   namespace NarrativeEngine::SenderCandidatePool
   {
       struct Candidate
       {
           RE::FormID  formID;
           std::string name;
           double      engagementScore;
           double      lastInteractedAt;
           std::vector<std::string> memories; // pre-formatted per-actor lines
       };

       struct BuildOptions
       {
           std::size_t maxCandidates              = 12;
           std::size_t maxMemoriesPerCandidate    = 6;
           std::function<bool(RE::Actor*)> extraViabilityFilter; // optional
       };

       std::vector<Candidate> Build(const BuildOptions& opts);
       std::size_t CountViable(std::size_t minRequired,
                                const std::function<bool(RE::Actor*)>& extraFilter);
   }
   ```

2. Move the top-N-by-engagement + per-candidate memory-fetch code
   from `LetterComposer::BuildContext` into
   `SenderCandidatePool::Build`. The letter-specific viability
   check (currently in `LetterComposer`) becomes the default;
   `extraViabilityFilter` is invoked on top for callers that
   need stricter gates.
3. Refactor `LetterComposer` to call
   `SenderCandidatePool::Build({maxCandidates=12,
   maxMemoriesPerCandidate=6})` and assemble the letter-specific
   prompt context from the returned `Candidate` vector.
4. `CountViable` is the cheap `IsAvailable`-time check — same
   candidate walk without the per-candidate memory fetch.

**Specifics:**

- The extra viability filter argument is what Step 5 will use to
  pass the Visit-specific stricter gates (unique / not in combat
  / etc.).
- Preserve the exact per-candidate context format
  `LetterComposer` currently emits so the letter prompt's LLM
  behavior doesn't drift.

**Verify:** Build clean. Trigger a letter dispatch in-game;
verify (via SKSE log) that the letter compose prompt still
receives the same candidate context structure as before, and
the letter still gets composed and delivered normally.

---

### Step 5 — Compose prompt + `VisitComposer` helper

- [x] Complete

**[CLAUDE]**

**Goal:** Author the visit compose prompt and stand up the
`VisitComposer` helper that fires the LLM call and parses the
response. Testable in isolation via a temporary console/timer
hook.

**Files:**

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_visit_compose.prompt`
  — new prompt.
- `include/VisitComposer.h` / `src/VisitComposer.cpp` — new
  helper.

**Sub-tasks:**

1. Author the prompt following `docs/CUSTOM_PROMPTS.md`. The
   prompt receives the candidate pool (via
   `SenderCandidatePool::Build` output), plus
   `desired_direction`, `tension_delta`, and `urgency_hint`.
   Domain-category guidance only (money / secrets / warning /
   apology / grief / request / rivalry / obligation). Output
   must be a JSON object with keys `sender_npc_form_id`,
   `briefing`, `topic_tag`, `mood`, `tags`. Constraints per the
   **Sender-selection LLM call** section (briefing 40–120 words,
   mood enum, tags array of 2–6).
2. Public API:

   ```cpp
   struct VisitBriefing
   {
       RE::FormID               senderNpcFormID;
       std::string              briefing;
       std::string              topicTag;
       std::string              mood;
       std::vector<std::string> tags;
   };

   void Compose(const ActionContext& ctx,
                int                  urgencyHint,  // 0=low, 1=med, 2=high
                std::function<void(std::optional<VisitBriefing>)> callback);
   ```

3. Implementation:
   - Build the candidate pool via `SenderCandidatePool::Build`
     with the Visit-specific viability filter (unique / alive /
     not in combat / not in dialogue / not follower / not
     disabled / has current location).
   - If `SenderCandidatePool::CountViable` reports fewer than
     `Settings::Get().visitMinSenderCandidates`, invoke callback
     with `std::nullopt` and log the shortfall.
   - Assemble the JSON context payload.
   - Fire `SkyrimNetAPI::SendCustomPromptToLLM(
     "narrative_engine_visit_compose",
     "narrative_engine_director", contextJson, callback)`.
   - In the response callback: strip markdown fences (reuse
     Phase 03's `StripMarkdownFences`), parse JSON, validate:
     required keys present, `sender_npc_form_id` matches a
     candidate FormID, briefing word count within bounds. Pass
     `LLMTextSanitizer::Sanitize` over every free-form string
     (`briefing`, `topicTag`, `mood`, each `tags` entry).
   - On validation failure: log the response, callback
     `std::nullopt`.
   - On success: build `VisitBriefing`, callback with populated
     result.
4. Add a temporary console/timer test hook in `Plugin.cpp` (a
   10-second-timer or a hotkey) that calls
   `VisitComposer::Compose` once and logs the parsed
   `VisitBriefing`. Remove this hook before merging (or before
   the next step's step is verified — leaving it in isn't harmful
   but is noise).

**Specifics:**

- The `desired_direction` field biases mood selection in the
  prompt (raise → menacing / urgent; lower → contrite /
  mournful / warm).
- The prompt should NOT include the player's current location.
- No location-context leaks either — the prompt is grounded in
  the candidates' own memories.

**Verify:** Build clean. Boot Skyrim in a save with several
recently-interacted NPCs. Trigger the test hook; SKSE log
shows a parsed `VisitBriefing` with plausible sender and a
40–120 word briefing. Or a specific validation-failure line
naming which key was wrong.

---

### Step 6 — `NPCVisitAction` skeleton + `IsAvailable` + `Start`-stub + registration

- [x] Complete

**[CLAUDE]**

**Goal:** Implement the `IAction` shape, register the action,
and make sure `IsAvailable` gates behave correctly. `Start` is
a stub for this step — the real dispatch chain wires in at Step
8. The dispatcher must see `npc_visit` as a selectable action
before we can end-to-end test anything downstream.

**Files:**

- `include/NPCVisitAction.h` — class declaration.
- `src/NPCVisitAction.cpp` — implementation.
- `src/Plugin.cpp` — register the action at `kDataLoaded` after
  Phase 04's letter action.

**Sub-tasks:**

1. Declare `class NPCVisitAction : public IAction`.
2. `Name()` returns `"npc_visit"`.
3. `Description()` returns a one-paragraph description
   contrasting visits against letters — see the design intent
   under **Why this phase exists**. The description must
   explicitly frame the choice: "in-person, urgent, cannot be a
   letter" so the action-select LLM can distinguish the two.
4. `Polarity()` returns `Either`.
5. `IsAvailable(ctx)`:
   - Return false if `ctx.player` is null.
   - Return false if
     `LocationKeywords::IsVisitHostile(ctx.player->GetCurrentLocation())`.
   - Return false if `!SkyrimNetAPI::IsMemorySystemReady()`.
   - Return false if
     `SenderCandidatePool::CountViable(Settings::Get().visitMinSenderCandidates,
     <visit viability filter>)` returns less than the threshold.
   - Otherwise return true.
6. `Start(ctx, parameters)`: log "not yet implemented (Step 8)"
   and return `StartResult{ started=false, detail="stub" }`.
7. `DetectAndRollbackFailedStart` and `DetectCompletion`: return
   false. Real implementations arrive in later steps.
8. Register the action in `Plugin.cpp` at `kDataLoaded` after
   the letter action registration, guarded by the same
   pattern.

**Specifics:**

- The candidate-count check in `IsAvailable` is not free — it
  walks SkyrimNet's engagement history and filters. It runs on
  every `IsAvailable` call the dispatcher makes, so avoid doing
  any per-candidate memory fetches here.
  `SenderCandidatePool::CountViable` must be structured to skip
  the memory fetch (unlike `Build`).

**Verify:** Build clean. Boot Skyrim; SKSE log shows
`NPCVisitAction registered`. Trigger a phase advance that makes
the dispatcher consider actions; log shows the action-select
prompt's candidate list now includes `npc_visit`. Walk into a
jail cell; force a fresh `IsAvailable` check (via the debug UI
or timer); confirm the log line
`NPCVisitAction::IsAvailable: blocked (IsVisitHostile)`. If the
action-select LLM picks `npc_visit`, the stub `Start` returns
false and the log shows the "not yet implemented" line.

---

### Step 7 — CK content: faction, quest, aliases, packages, Papyrus trampolines

- [ ] Complete

**[USER]** (Claude drafts the `.psc`; the user authors the CK
content and compiles the script there. Coupling analogous to
Phase 04 Step 14.)

**Goal:** Author all the CK-side content the visit action needs
so subsequent steps can call it. This is the biggest single
step of the phase, but the pieces are mostly mechanical.

**Sub-tasks:**

1. Draft `esp/Source/Scripts/_ne_VisitQuest.psc` (Claude) with:

   ```papyrus
   Scriptname _ne_VisitQuest extends Quest

   ; Wired to CK aliases by name.
   ReferenceAlias Property Sender       Auto
   ReferenceAlias Property SpawnMarker  Auto
   ReferenceAlias Property ReturnAnchor Auto

   ; Runs the given SkyrimNet action against the Sender. Called
   ; from C++ via VM dispatch — plugin-owned turns pass
   ; briefing argsJson; ContinueConversation nudges pass "".
   Function RunSenderAction(String actionName, String argsJson)
       Actor senderActor = Sender.GetActorReference()
       if senderActor == None
           Debug.Trace("[_ne_VisitQuest] RunSenderAction: Sender empty")
           return
       endIf
       SkyrimNetApi.ExecuteAction(actionName, senderActor, argsJson)
   EndFunction

   ; Terminal teardown. Disable+Delete the ReturnAnchor REFR
   ; (spawned during alias-fill), then Stop() the quest and
   ; Reset() so both packages release from the sender.
   Function Shutdown()
       Stop()
       ObjectReference returnAnchorRef = ReturnAnchor.GetReference()
       if returnAnchorRef != None
           returnAnchorRef.DisableNoWait()
           returnAnchorRef.Delete()
       endIf
       Reset()
   EndFunction
   ```

2. Open Creation Kit through MO2; load `NarrativeEngine.esp`.
   Compile `_ne_VisitQuest.psc` via the script editor.
3. Create the marker faction `_ne_VisitSenderFaction`:
   - EditorID: `_ne_VisitSenderFaction`.
   - No reactions, no crime, no relations, no vendors, no
     combat behavior. Pure marker.
4. Create the AI package `_ne_VisitFollow`:
   - Package type: `Follow`.
   - Target Actor: `PlayerRef`.
   - Follow Distance: set to `iVisitSalutationApproachDistanceUnits`'s
     default (300).
   - Flags: `Ignore Combat = false`, `Interrupt Override = None`,
     `Must Complete = off`, `Allow Idle Chatter = off`.
   - Package Condition (Conditions tab): Function `GetStage`,
     Parameters `Quest = _ne_VisitQuest`, Comparison `<`, Value
     `50`. Run On: Subject.
5. Create the AI package `_ne_VisitReturnTravel`:
   - Package type: `Travel`.
   - Destination: `Alias:ReturnAnchor` (the quest and its
     ReturnAnchor alias must exist first — do step 6 before
     completing this step, or come back here after).
   - Flags: same as the Follow package's non-idle flags.
   - Package Condition: Function `GetStage`, Parameters `Quest
     = _ne_VisitQuest`, Comparison `>=`, Value `50`. Run On:
     Subject.
6. Create the quest `_ne_VisitQuest`:
   - EditorID: `_ne_VisitQuest`.
   - Quest Data: **Start Game Enabled** OFF, **Run Once** OFF,
     **Allow Repeated Stages** OFF. Priority irrelevant. Event
     = None.
   - **Scripts tab**: attach the `_ne_VisitQuest` script.
7. On the Aliases tab, add three reference aliases:
   - **`Sender`**:
     - Fill Type: **Find Matching Reference**.
     - "In Loaded Area": **unchecked**.
     - "Closest": unchecked.
     - Conditions: one — Function `GetFactionRank`, Parameters
       `Faction = _ne_VisitSenderFaction`, Comparison `>=`,
       Value `4`. Run On: Subject.
     - Flags: `Optional` **OFF**.
     - Packages tab (on the alias): add both `_ne_VisitFollow`
       and `_ne_VisitReturnTravel`.
   - **`SpawnMarker`**:
     - Fill Type: **Find Matching Reference**.
     - "In Loaded Area": **checked**.
     - "Closest": checked, reference = `PlayerRef`.
     - Conditions:
       - `GetIsID XMarkerHeading == 1`. Run On: Subject.
       - `GetDistance PlayerRef >= 800`. Run On: Subject.
       - `GetDistance PlayerRef <= 2500`. Run On: Subject.
       - `GetLineOfSight PlayerRef == 0`. Run On: Subject.
     - Flags: `Optional` **OFF**.
   - **`ReturnAnchor`** (must be listed **after** `Sender` in
     the alias list so `At: Sender` resolves):
     - Fill Type: **Create Reference to Object**.
     - Object: `XMarkerHeading`.
     - Create at: **At**, Reference: `Alias:Sender`.
     - Level: irrelevant (marker has no level).
     - Flags: `Optional` **OFF**.
8. On the Stages tab, add stages 0, 10, 20, 25, 27, 30, 50, 60,
   200 with fragments per the **ESP content** section's stage
   table. Specifically:
   - Stage 0: Flags = **Startup Stage** ON. Fragment:
     `SetStage(10)`.
   - Stage 10: Fragment: `Sender.GetActorReference().MoveTo(SpawnMarker.GetReference()); Sender.GetActorReference().EvaluatePackage()`.
   - Stages 20, 25, 27, 30: empty fragments.
   - Stage 50: Fragment: `Sender.GetActorReference().EvaluatePackage()`.
   - Stage 60: Fragment: `SetStage(200)`.
   - Stage 200: Flags = **Complete Quest** ON. Fragment:
     `kmyQuest.Shutdown()` (where `kmyQuest` is CK's autocast
     for the attached quest script).
9. Save the ESP.

**Specifics:**

- The Sender alias needs both packages listed with Follow
  first (higher priority) and Travel second. The engine's
  package selector walks in priority order.
- Alias order in the quest matters: `Sender` must come **before**
  `ReturnAnchor` so `ReturnAnchor`'s `Create Reference to Object
  At: Sender` fill has a filled `Alias:Sender` to anchor
  against. `SpawnMarker` can sit anywhere in the list; it has
  no cross-alias dependency.
- The Stage 10 fragment's `EvaluatePackage()` is critical — it
  forces the package selector to re-evaluate, which is what
  binds the Follow package to the sender.

**Verify:** Open xEdit on `NarrativeEngine.esp`. Confirm:

- One FACT record `_ne_VisitSenderFaction`.
- Two PACK records `_ne_VisitFollow` and
  `_ne_VisitReturnTravel`, each with the correct type,
  destination, flags, and package condition.
- One QUST record `_ne_VisitQuest` with three ref aliases
  (Sender, SpawnMarker, ReturnAnchor) configured per sub-tasks
  7, the `_ne_VisitQuest` script attached, and stages 0/10/20/
  25/27/30/50/60/200 with the specified fragments.

Confirm CK compiled `_ne_VisitQuest.psc` without errors. Run
`pwsh -File build.ps1 build`; boot Skyrim; SKSE log shows no
ESP-load errors and no script-binding warnings on
`_ne_VisitQuest`.

---

### Step 8 — `Start`'s dispatch chain + Salutation-timeout rollback

- [x] Complete

**[CLAUDE]**

**Goal:** Wire `Start` to do the real work up through
`EnsureQuestStarted` — compose LLM call, snapshot, anchor
placement, faction promote, quest start. Also implement
`DetectAndRollbackFailedStart` for the Salutation timeout.
`Salutation → Discuss` and everything after is Step 9+.

**Files:**

- `src/NPCVisitAction.cpp` — replace the stub `Start`; add
  the verification poll.
- `include/NPCVisitAction.h` — declare private helper members.
- `src/VisitState.cpp` — flesh out the `DerivePhase()` mapping
  now that the quest exists.

**Sub-tasks:**

1. At `kDataLoaded`, resolve and cache pointers to:
   - `_ne_VisitQuest` (`RE::TESQuest*`).
   - `_ne_VisitSenderFaction` (`RE::TESFaction*`).
   - The three reference aliases on the quest (`BGSRefAlias*`
     for Sender, SpawnMarker, ReturnAnchor).
   Log an error if any resolution fails; permanently disable
   `IsAvailable` if the critical ones are missing. (No XMarker
   base form lookup is needed — the ReturnAnchor alias spawns
   the marker itself during alias-fill.)
2. Wire the `g_composingSender` in-process atomic flag (declared
   at file scope in `NPCVisitAction.cpp`).
3. Replace `Start`'s body with the sequence from **`Start`**:
   - Re-validate `IsAvailable`; return `{false, "preconditions
     failed at start time"}` if not.
   - Read `_ne_VisitQuest->GetCurrentStageID()`. If > 0 or
     `g_composingSender` is set, return `{false, "already in
     flight"}`.
   - Set `g_composingSender = true`.
   - Build the compose prompt context per Step 5 and fire
     `VisitComposer::Compose`.
   - Return `{true, "composing briefing"}` immediately.
4. In the compose callback (marshal to main thread first):
   - On `std::nullopt`: log; clear `g_composingSender`; call
     `ActionDispatcher::CompleteAction("npc_visit")` and clear
     any partial snapshot.
   - On success:
     - Snapshot sender's position / angle / cell into VisitState
       via `SetSnapshot`.
     - Sweep stale rank-4 members of
       `_ne_VisitSenderFaction`: walk
       `RE::ProcessLists`'s four actor lists, for any actor at
       rank ≥ 4 that isn't the intended sender, call
       `AddToFaction(fact, 0)` to demote.
     - Promote the chosen sender to rank 4 via
       `AddToFaction(fact, 4)`.
     - Cache briefing / topic / mood / tags in the Snapshot.
     - Call
       `quest->EnsureQuestStarted(engineResult, /*startNow=*/true)`.
       On `!callOk || !engineResult`, log the failure, demote
       sender, clear `g_composingSender`, call
       `ActionDispatcher::CompleteAction("npc_visit")`. No
       anchor cleanup needed on this path — either the
       ReturnAnchor alias never got to fire (earlier alias
       failed) or it fired and its REFR will be cleaned up
       when the quest `Reset()`s during rollback shutdown.
     - On success: read
       `returnAnchorAlias->GetReference()->GetFormID()` and
       stash it in the Snapshot for dashboard/logging
       visibility. Clear `g_composingSender`. Register the
       Salutation distance watchdog with `AsyncDispatch`
       polling on `iVisitPollGateTickSeconds` (or a fixed
       250ms — the Salutation watchdog is separate from the
       Discuss poll gate). The watchdog just logs
       distance-to-player each tick for now; the actual
       Salutation → Discuss transition wires in at Step 9.
5. Update `VisitState::DerivePhase()` to resolve the quest via
   the cached pointer and map its current stage to the enum.
6. Implement `DetectAndRollbackFailedStart` per the design:
   - If `g_composingSender`: return false.
   - Read the current stage.
   - Stage 10 within `visitApproachTimeoutSeconds`: return
     false.
   - Stage 10 past the window: read the ReturnAnchor alias's
     REFR and `sender->MoveTo(anchor)` (or fallback to
     `MoveTo(sender)`); demote sender; VM-dispatch
     `Quest.SetStage(60)` — the Stage 60 → 200 route runs
     Shutdown, which Disable+Deletes the anchor marker. Push a
     `rolled_back` history entry via `VisitState::PushHistory`.
     Return true.
   - Any other stage: return false.

**Specifics:**

- `EnsureQuestStarted` runs the engine's alias-fill pass
  synchronously in the happy path — Sender, SpawnMarker, and
  ReturnAnchor all fill (or spawn, in ReturnAnchor's case)
  during the call. All three aliases are `Optional` OFF, so if
  any fill misses (no XMarkerHeading in the SpawnMarker's
  distance band; sender got deleted between candidate
  selection and start; ReturnAnchor's `At: Sender` couldn't
  resolve because Sender itself failed), `engineResult`
  returns false and the chain aborts cleanly with no
  half-built state to unwind.
- ReturnAnchor's alias order matters: it must be positioned
  after Sender in the quest's alias list. If it's before,
  `At: Sender` resolves against an empty alias and the fill
  either fails outright or spawns the marker at world-origin.

**Verify:** Build clean. Boot Skyrim in a save with viable
sender candidates. Trigger a phase advance that makes the
dispatcher pick `npc_visit`. Watch the SKSE log:

- `NPCVisitAction: composing briefing (candidates=N)`.
- `VisitComposer: parsed briefing (sender=0x…, briefing=N chars, mood=X)`.
- `NPCVisitAction: snapshotted sender at (X,Y,Z) in cell 0x…`.
- `NPCVisitAction: promoted sender 0x… to rank 4`.
- `NPCVisitAction: EnsureQuestStarted ok — Sender=0x…, SpawnMarker=0x…, ReturnAnchor=0x…`.
- Stage advances to 10 automatically.

Visually confirm: the sender NPC warps in near the player at
the spawn marker and starts walking toward the player (Follow
package binding). Because Salutation → Discuss isn't wired yet,
after `iVisitApproachTimeoutSeconds` the rollback fires and
the sender teleports home. SKSE log shows
`DetectAndRollbackFailedStart: Salutation timeout — rolling
back`.

---

### Step 9 — `Salutation → Discuss` transition + opening line

- [x] Complete

**[CLAUDE]**

**Goal:** Wire the Salutation distance watchdog to fire the
opening line via `SkyrimNetApi.ExecuteAction` and advance the
quest to Stage 20 when the sender closes distance to the
player. The action stays in-flight through Discuss but nothing
tears it down yet — after the opening line the sender just
sits with the player until the outer hard-timeout eventually
hits. That's expected for this step.

**Files:**

- `src/NPCVisitAction.cpp` — flesh out the Salutation watchdog.

**Sub-tasks:**

1. During Start's callback (after `EnsureQuestStarted`), register
   the Salutation watchdog to poll every 250ms via
   `AsyncDispatch`. The watchdog:
   - Reads `Quest.GetCurrentStageID()`. Bails if not 10.
   - Reads the Sender alias's actor.
     `sender->GetPosition()` and player position; compute
     distance.
   - If distance ≤ `visitSalutationApproachDistanceUnits`:
     - VM-dispatch `RunSenderAction("<visit-conversation-action>",
       argsJson)` with the briefing payload and `turn_kind =
       "salutation"`.
     - VM-dispatch `Quest.SetStage(20)`.
     - Log the transition.
     - Unregister the watchdog.
2. Decide the plugin-owned SkyrimNet action name (see the
   **Sender speech via SkyrimNet's ExecuteAction** section).
   Store it as a compile-time constant in `NPCVisitAction.cpp`
   for now; if the action doesn't exist in SkyrimNet's default
   library, either fall back to `DirectNarration` OR register
   one via `SkyrimNetApi.RegisterAction` at `kDataLoaded`.
   Whichever path is chosen, document the decision in a comment.
3. Build the `argsJson` payload from the snapshot's briefing
   fields per the JSON shape in the design.

**Specifics:**

- 250ms is a compromise: fast enough to feel responsive when the
  sender's Follow package brings them into range, slow enough
  not to spam.
- If the Sender alias's actor is dead or null when the watchdog
  fires (e.g. the sender got attacked mid-approach), let the
  timeout branch of `DetectAndRollbackFailedStart` handle it —
  the watchdog just skips this tick.

**Verify:** Build clean. Trigger a visit dispatch. Watch:

- The sender approaches; when they get within ~300u, the log
  shows `NPCVisitAction: Salutation → Discuss (executeAction
  turn_kind=salutation)`.
- The sender speaks a line via SkyrimNet's normal channels
  (subtitles / voice / whatever SkyrimNet does). The line
  should be on-topic per the briefing.
- Quest stage advances to 20; confirm via console
  `sqv _ne_VisitQuest` (or the dashboard once Step 16 is
  done). Sender continues following the player until the outer
  timeout fires.

---

### Step 10 — Discuss-phase poll gate + poll fire + verdict logging

- [x] Complete

**[CLAUDE]**

**Goal:** Stand up the cheap-signal poll gate and the LLM poll
itself. Verdict logging only for this step; acting on the
verdict (advancing to Valediction / firing ContinueConversation)
wires in at Step 11.

**Files:**

- `include/VisitConclusionPoll.h` / `src/VisitConclusionPoll.cpp`
  — new module.
- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_visit_conclusion_poll.prompt`
  — new prompt.
- `src/NPCVisitAction.cpp` — arm/disarm the poll's gate tick
  around the Discuss stage.

**Sub-tasks:**

1. Author the poll prompt per the **Natural-conclusion LLM poll**
   section: `narrative_engine_director` variant, context payload
   (sender, player, sender_goal, topic_tag, mood,
   desired_direction, recent_lines, elapsed_seconds,
   nudge_count), output
   `{ should_conclude: bool, rationale: string }`.
2. Public API:

   ```cpp
   struct PollVerdict
   {
       bool        shouldConclude;
       std::string rationale;
   };

   void Arm(const VisitState::Snapshot& snapshot);
   void Disarm();
   bool GateTick(); // returns true if any gate tripped this tick
   void FirePoll(std::function<void(std::optional<PollVerdict>)> callback);
   ```

3. Internal state (all in the module's file scope, guarded by
   mutex):
   - `int turnsSinceLastPoll`.
   - `double lastPollGameSeconds`.
   - `double lastSpeechTurnGameSeconds`.
   - `int consecutiveFailures`.
4. `Arm`: reset trackers to 0 / current game-time.
5. `GateTick`: read `RE::Calendar` game-time seconds. Compute:
   - `turnsSinceLastPoll >= visitPollTurnCountThreshold`?
   - `now - lastSpeechTurnGameSeconds >= visitPollSilenceGameMinutes * 60`?
   - `now - lastPollGameSeconds >= visitPollMaxIntervalGameMinutes * 60`?
   Return true if any is true.
6. `FirePoll`:
   - Sample the last N speech turns from SkyrimNet's event
     history. Exact query surface: use SkyrimNet's Papyrus
     event-log API or, if not available from C++, VM-dispatch to
     a Papyrus helper that pulls the last N events with
     participant filter = sender + player. Log warning + pass
     empty `recent_lines` if the sample fails.
   - Build the context JSON.
   - Fire `SkyrimNetAPI::SendCustomPromptToLLM` with the poll
     prompt.
   - In the callback: parse the response, sanitize the rationale
     string, invoke the caller's callback with the parsed
     `PollVerdict` (or `std::nullopt` on parse failure).
   - Reset `turnsSinceLastPoll = 0` and `lastPollGameSeconds =
     now` regardless of verdict.
7. Add a public `RegisterSpeechTurn()` function the caller
   invokes when a new turn is observed — updates
   `lastSpeechTurnGameSeconds = now` and increments
   `turnsSinceLastPoll`. For this step, sample from SkyrimNet
   the same way `recent_lines` does — the actual wiring might be
   a periodic sample every ~2s that increments the counter each
   time a new-since-last-check turn appears in the event history.
8. In `NPCVisitAction.cpp`, on Salutation → Discuss transition,
   call `VisitConclusionPoll::Arm(snapshot)`. Register a
   `iVisitPollGateTickSeconds` (1s default) timer that calls
   `GateTick`; if true, `FirePoll` with a callback that just
   logs the verdict for this step:
   `VisitPoll: fired (verdict=<true/false>, rationale="…")`. On
   parse failure, log `VisitPoll: parse failed` and increment
   `consecutiveFailures`. When `consecutiveFailures >=
   visitConclusionPollMaxConsecutiveFailures`, log a warning but
   don't hard-abort yet (that wires in at Step 15).

**Specifics:**

- The `recent_lines` sample is the hardest single piece of
  wiring in this step. If SkyrimNet exposes a Papyrus event-log
  query, prefer that (via VM dispatch). If not, hook into
  whatever event log C++ already has visibility into.
- The gate tick's poll interval (1s) is intentionally cheap —
  three integer/timestamp comparisons per tick.

**Verify:** Build clean. Trigger a visit dispatch; wait for the
Salutation → Discuss transition. Log shows:

- `VisitPoll: armed`.
- Periodic gate ticks, most logging
  `VisitPoll: gate did not trip` (or nothing for cheap ticks).
- Eventually (turn count / silence / max-interval, whichever
  hits first) the gate trips and `VisitPoll: fired` appears with
  a verdict.
- Verdict rationale reads sensibly against the actual
  conversation.

Since Discuss doesn't advance yet, the visit still hits the
outer hard-timeout eventually. That's expected.

---

### Step 11 — Three-branch poll response handling + nudge escalation

- [x] Complete

**[CLAUDE]**

**Goal:** Act on the poll's verdict per the three-branch design:
`should_conclude=true` → SetStage(30); `should_conclude=false` +
silence-gate tripped → fire `ContinueConversation` + increment
counter; `should_conclude=false` + silence-gate not tripped →
no-op. Enforce the nudge cap forcing Valediction after N
consecutive `ContinueConversation` fires.

**Files:**

- `src/NPCVisitAction.cpp` — replace the "just log verdict" hook
  with the real branching.

**Sub-tasks:**

1. In the poll callback:
   - On `should_conclude == true`: log; VM-dispatch
     `Quest.SetStage(30)`. Disarm the poll timer (Valediction's
     side effects fire at Step 12).
   - On `should_conclude == false`:
     - Compute `silenceGameSeconds = now - lastSpeechTurnGameSeconds`.
     - If `silenceGameSeconds >= visitPollSilenceGameMinutes * 60`:
       - VM-dispatch
         `RunSenderAction("ContinueConversation", "")`.
       - Increment `Snapshot::ignoreNudgeCount`.
       - Persist the snapshot via `VisitState::SetSnapshot`.
       - If `ignoreNudgeCount >= visitMaxIgnoreNudges`:
         - Log the escalation.
         - VM-dispatch `Quest.SetStage(30)` to force Valediction
           with `nudge_count` in the closing args (Step 12 reads
           this).
         - Disarm the poll timer.
     - Else: no-op (log a low-verbosity line if desired).
2. When a new player speech turn is observed in SkyrimNet's
   event history (detected by `RegisterSpeechTurn` at Step 10),
   reset `ignoreNudgeCount = 0` and persist. Detection: the
   `RegisterSpeechTurn` callback receives the speaker identity;
   if it's the player, reset.

**Specifics:**

- If SkyrimNet's speech-turn query doesn't distinguish speaker,
  a coarser rule works: any new turn from either party resets
  the nudge counter. Slightly less strict but not broken.
- `SetSnapshot` after each mutation is important — a save
  during the ignore-nudge sequence should persist the counter.

**Verify:** Build clean. Trigger a visit dispatch and let it
run:

- Ignore the sender's opening line and wait. Every ~30
  game-seconds of silence, `VisitPoll: fired
  (verdict=false)`, `NPCVisitAction: silence-gate tripped, firing
  ContinueConversation (nudge #1)`. Sender speaks another line
  via SkyrimNet's ContinueConversation.
- After `visitMaxIgnoreNudges` (default 3) nudges without a
  reply, log shows
  `NPCVisitAction: nudge cap reached — forcing Valediction`
  and the quest advances to Stage 30. Since Valediction's side
  effects don't yet fire (Step 12), the visit stalls at Stage
  30 until the outer timeout hits. Expected for this step.
- Alternate test: reply to the sender via SkyrimNet chat before
  the nudge cap. Confirm the nudge counter resets. When the
  poll eventually returns `should_conclude=true`, the quest
  advances to Stage 30 normally.

---

### Step 12 — Valediction → ReturnHome (closing line + package swap)

- [x] Complete

**[CLAUDE]**

**Goal:** Wire the Stage 30 → 50 transition: fire the closing
line via `ExecuteAction` (with the current `nudge_count` in the
args), wait the dwell, VM-dispatch SetStage(50). Stage 50's
fragment (already authored in Step 7) calls
`EvaluatePackage()`, which swaps Follow → Travel and the sender
starts walking away.

**Files:**

- `src/NPCVisitAction.cpp` — add the Stage 30 entry handler.

**Sub-tasks:**

1. Add a small "stage-observed" callback: on each poll-gate
   tick (or a dedicated stage-watch tick), read
   `_ne_VisitQuest->GetCurrentStageID()`. If it changed since
   the last observation, dispatch to a handler.
2. On observed Stage 30 entry:
   - Build the closing-line argsJson: same shape as the
     Salutation payload but with `turn_kind = "valediction"`
     and `nudge_count = snapshot.ignoreNudgeCount`.
   - VM-dispatch `RunSenderAction("<visit-conversation-action>",
     closingArgsJson)`.
   - Schedule a one-shot timer for `visitValedictionDwellSeconds`
     (default 5s). When it fires, VM-dispatch
     `Quest.SetStage(50)`.
3. Stage 50 is entered — its fragment calls
   `EvaluatePackage()`. The sender's package selector re-runs;
   Follow's `GetStage < 50` condition now fails, Travel's
   `GetStage >= 50` passes; the sender starts walking toward
   the ReturnAnchor.

**Specifics:**

- Do NOT fire the closing-line ExecuteAction more than once —
  guard against multiple Stage 30 entries by clearing the
  scheduled Valediction dwell timer once it fires.
- The ReturnAnchor alias is always filled by the time Stage 50
  hits (alias-fill during `EnsureQuestStarted` is a
  precondition for reaching any post-Stage-0 state; if it
  didn't fill, Start rolled back before this code ran). Travel
  therefore always has a valid destination — no fallback
  needed for the "unfilled anchor" case.

**Verify:** Build clean. Trigger a visit dispatch. Reply to the
sender until the poll returns `should_conclude=true` (or wait
for the nudge cap to force Valediction). Log shows:

- `NPCVisitAction: Valediction entry (nudge_count=N)`.
- `RunSenderAction: valediction turn dispatched`.
- Sender speaks a closing line via SkyrimNet.
- After ~5s: `NPCVisitAction: Valediction dwell expired,
  advancing to ReturnHome (stage 50)`.
- Sender turns and starts walking away from the player toward
  wherever they came from. Visible walk-away behavior confirmed.

Since ReturnHome's watchdog + teleport aren't wired yet, the
sender walks until the outer hard-timeout eventually kills the
visit. Expected.

---

### Step 13 — ReturnHome watchdog + final teleport + shutdown

- [x] Complete

**[CLAUDE]**

**Goal:** Complete the ReturnHome state: watchdog polls the
three exit conditions; on trip, teleport sender home + demote +
disable anchor + SetStage(200) → Shutdown → Idle. This closes
the main-path lifecycle.

**Files:**

- `src/NPCVisitAction.cpp` — arm the ReturnHome watchdog on
  Stage 50 entry; implement `DetectCompletion`.

**Sub-tasks:**

1. On observed Stage 50 entry:
   - Record `returnHomeStartedAt = std::chrono::steady_clock::now()`.
   - Register the ReturnHome watchdog to poll every 500ms via
     `AsyncDispatch`.
2. Watchdog tick logic:
   - Read the Sender alias's actor.
   - Compute `distToPlayer =
     sender->GetPosition().GetDistance(player->GetPosition())`.
   - Read `sender->parentCell` and check its attachment state.
   - `elapsed = now - returnHomeStartedAt`.
   - Exit condition A:
     `distToPlayer >= visitReturnHomeExitDistanceUnits`.
   - Exit condition B: sender's parent cell isn't in the loaded
     set (use `RE::TESObjectCELL::IsAttached()` or the
     equivalent).
   - Exit condition C:
     `elapsed >= visitReturnHomeTimeoutSeconds`.
   - If any trips: invoke the shutdown chain (below); unregister
     the watchdog.
3. Shutdown chain (main thread):
   - Read the ReturnAnchor alias's REFR
     (`returnAnchorAlias->GetReference()`). If valid,
     `sender->MoveTo(anchor)`; else `sender->MoveTo(sender)`
     (fallback for the extremely rare case where the alias's
     ref got externally invalidated mid-run).
   - Set the sender's Z angle to `snapshot.returnAngleZ`.
   - Call `sender->EvaluatePackage()`.
   - Demote the sender from `_ne_VisitSenderFaction` (rank 4 →
     rank 0). Skip if sender is dead.
   - VM-dispatch `Quest.SetStage(200)`. Stage 200's fragment
     calls `Shutdown()` — Disable+Delete the anchor REFR, then
     `Stop() + Reset()`. (Anchor cleanup lives in the Papyrus
     Shutdown fragment now, not in C++, since the alias owns
     the marker.)
   - Push a `completed` history entry via
     `VisitState::PushHistory` (unless the entry is a
     hard-abort, which is Step 15 — for now everything is
     `completed`; if `ignoreNudgeCount >= visitMaxIgnoreNudges`,
     push `unsatisfied` instead).
   - Set `g_terminalCleanupDone = true`.
   - Reset `VisitState` after a brief delay (schedule a one-shot
     100ms timer so the Papyrus Reset() has time to run).
4. Implement `DetectCompletion` per the design:
   - Return true when
     `_ne_VisitQuest->GetCurrentStageID() == 0` AND
     `!_ne_VisitQuest->IsCompleted()` (per the CLAUDE.md memory
     on `IsRunning` unreliability) AND `g_terminalCleanupDone`.
   - Clear `g_terminalCleanupDone` after returning true so the
     next dispatch starts clean.

**Specifics:**

- The `g_terminalCleanupDone` flag is what gates the
  dispatcher from re-firing the visit until cleanup fully
  unwinds. Without it, the dispatcher could try to Start a new
  visit while Papyrus's Reset() is still running.
- The 100ms delay before VisitState reset is a defensive buffer
  against the Papyrus VM lag on Reset(). Adjust if it turns out
  to be flaky.

**Verify:** Build clean. Trigger a visit dispatch, let it run
through the full natural cycle:

- Discuss reaches natural conclusion; Valediction fires and
  dwells; ReturnHome entered.
- Sender walks away from the player. Log shows periodic
  `ReturnHomeWatchdog: dist=Nu, cell=attached, elapsed=Ns`.
- When sender crosses 1500u (or leaves the loaded cells), log
  shows `ReturnHomeWatchdog: exit condition tripped (distance)`
  and the sender teleports home.
- Log confirms: sender demoted, anchor disabled and deleted,
  Stage 200 set, Shutdown fired.
- `sqv _ne_VisitQuest` shows the quest at stage 0, stopped.
- `DetectCompletion` returns true; dispatcher applies cooldown.
- Dashboard's action-in-flight indicator clears (if the
  dashboard is up).

Test the timeout branch: dispatch a visit; when Valediction
fires, use console to disable the sender's Travel package
(`sender.disable ai`) so they don't walk. After
`iVisitReturnHomeTimeoutSeconds` (default 120s), the timeout
exit condition trips and the teleport fires anyway.

---

### Step 14 — OnHold / ReEngage detours

- [x] Complete

**[CLAUDE]**

**Goal:** Wire the situational detours: DialogueMenu / combat /
scripted-scene events transition Discuss → OnHold; return to
free state transitions OnHold → ReEngage → Discuss with a
resumption line.

**Files:**

- `src/NPCVisitAction.cpp` — event sink registrations + OnHold
  logic.
- `src/Plugin.cpp` — register the new sinks at `kDataLoaded`.

**Sub-tasks:**

1. Register a `MenuOpenCloseEvent` sink filtered to
   `DialogueMenu::MENU_NAME`. On open: if the current stage is
   20 (Discuss), VM-dispatch `Quest.SetStage(25)` and record
   the trigger (`player_dialogue`).
2. Register a `TESCombatEvent` sink. On the player or sender
   entering combat while stage 20: VM-dispatch
   `Quest.SetStage(25)` and record the trigger
   (`player_combat` or `sender_combat`). Record
   `combatStartedAt = now`.
3. Detect scripted-scene entry: check on a periodic tick (~1s)
   whether either the player or the sender is currently in a
   running scene via `RE::TESScene`'s participant list. If yes
   while stage 20: transition to OnHold with trigger
   `scripted_scene`. (If a ModEvent surface is available, prefer
   that.)
4. On OnHold entry:
   - Disarm the natural-conclusion poll gate tick.
   - Store the OnHold trigger and combat-start timestamp in
     VisitState.
5. Periodic tick during OnHold: check whether all the OnHold
   trigger conditions have cleared (no DialogueMenu, not in
   combat, not in a scene). If yes: VM-dispatch
   `Quest.SetStage(27)` (ReEngage).
6. Combat-triggered OnHold has a timeout: if
   `now - combatStartedAt >= visitOnHoldCombatMaxSeconds` (60s
   default) and still in combat: this is a hard-abort trigger
   (wires in at Step 15). For this step, log the condition and
   force-transition to Stage 50 via `SetStage(50)` so the
   sender at least walks off. A cleaner hard-abort comes in
   Step 15.
7. On ReEngage entry (Stage 27):
   - Register a ReEngage watchdog polling every 250ms.
   - When `distToPlayer <= visitReEngageApproachDistanceUnits`
     (default 400u) AND all OnHold triggers still clear:
     - Build a resumption-line argsJson with `turn_kind =
       "reengage"`.
     - VM-dispatch `RunSenderAction("<visit-conversation-action>",
       argsJson)`.
     - VM-dispatch `Quest.SetStage(20)` (back to Discuss).
     - Re-arm the poll gate tick.
     - Unregister the ReEngage watchdog.

**Specifics:**

- The MenuTopicManager's `speaker` field can be read to detect
  whether the DialogueMenu the player opened is with the sender
  or someone else. For visit purposes it doesn't matter (any
  dialogue-menu open triggers OnHold), but this could be used
  to elide the transition if it's the sender being talked to —
  deferred for now.
- The ReEngage watchdog also needs to check whether the OnHold
  triggers have re-tripped (player re-entered dialogue, etc.).
  On re-trip during ReEngage: transition back to Stage 25.

**Verify:** Build clean. Trigger a visit dispatch; wait for
Discuss. Then:

- Talk to any unrelated NPC (opens DialogueMenu). Log shows
  `NPCVisitAction: OnHold entry (trigger=player_dialogue)`;
  quest advances to Stage 25.
- Exit DialogueMenu. Log shows `NPCVisitAction: OnHold
  triggers cleared, transitioning to ReEngage`; quest advances
  to Stage 27.
- Sender closes distance again (Follow package handles it).
  Log shows
  `NPCVisitAction: ReEngage watchdog tripped, dispatching
  resumption line`; sender speaks; quest advances to Stage 20.
- Poll gate tick resumes. Confirm Discuss cadence returns to
  normal.

Alternate test — combat OnHold: draw weapon and attack a
nearby chicken (or use console to force combat). OnHold entry
with trigger `player_combat`. Exit combat, ReEngage fires.

---

### Step 15 — Hard-abort escape hatches

- [x] Complete

**[CLAUDE]**

**Goal:** Wire the hard-abort paths for sender/player death,
outer wall-clock timeout, ignore-nudge cap during a stalled
conversation, and consecutive poll failures. The machine
tears down cleanly (skipping the ReturnHome walk) from any
state.

**Files:**

- `src/NPCVisitAction.cpp` — death event sinks; centralize the
  hard-abort teardown routine.

**Sub-tasks:**

1. Register a `TESDeathEvent` sink. On the sender or player
   dying while the visit is running (any non-Idle stage):
   invoke the hard-abort routine with reason `sender_death` or
   `player_death`.
2. On each `iVisitPollGateTickSeconds` tick, also check the
   outer timeout:
   `now - snapshot.dispatchedAtRealSeconds >= visitHardTimeoutSeconds`
   → hard-abort with reason `outer_timeout`.
3. On each poll-gate tick during OnHold with combat trigger,
   check the combat timeout: if elapsed exceeds
   `visitOnHoldCombatMaxSeconds`, hard-abort with reason
   `combat_stuck`.
4. In the poll callback (Step 11), when `consecutiveFailures >=
   visitConclusionPollMaxConsecutiveFailures`: hard-abort with
   reason `poll_broken`.
5. Refactor the shutdown chain (Step 13) into a shared
   `TeardownVisit(reason)` routine. Both the normal ReturnHome
   exit and the hard-abort path invoke it. The hard-abort path
   differs by:
   - Skipping the visible ReturnHome walk (no
     Valediction/closing line, no walk-away). Sender teleports
     home immediately.
   - Skipping the teleport itself if the sender is dead.
   - Pushing a history entry with outcome `aborted` (vs.
     `completed` / `unsatisfied` / `rolled_back`).
   - Setting `g_terminalCleanupDone = true` normally.
6. Log every hard-abort with the reason and the current quest
   stage at abort time.

**Specifics:**

- Hard-abort from a pre-ReturnHome state (Salutation, Discuss,
  OnHold, ReEngage, Valediction) skips the ReturnHome walk
  entirely. Hard-abort during ReturnHome (in-progress walk)
  short-circuits the walk with an immediate teleport.
- Player death has an interesting edge case: after teleporting
  the sender home and demoting, the player's own respawn /
  reload flow takes over. The visit teardown is orthogonal.

**Verify:** Build clean. Trigger a visit dispatch and let it
reach Discuss.

- **Sender death test**: `sqv _ne_VisitQuest` to get the
  sender's REFR, then console `<refid>.kill`. Log shows
  `NPCVisitAction: hard-abort (reason=sender_death, stage=20)`;
  quest advances directly to 200; visit ends. Dashboard shows
  history entry with outcome `aborted`.
- **Outer timeout test**: temporarily lower
  `iVisitHardTimeoutSeconds` in the INI to 60s. Dispatch a
  visit; ignore it. After 60s of wall-clock, log shows
  `NPCVisitAction: hard-abort (reason=outer_timeout, stage=20)`.
- **Combat-stuck test**: enter combat with a strong hostile
  during Discuss so combat lasts >60s. OnHold with combat
  trigger; after `visitOnHoldCombatMaxSeconds`, hard-abort
  fires.

---

### Step 16 — Dashboard: Visit tab + panels + payload

- [x] Complete

**[CLAUDE]**

**Goal:** Add the third tab to the dashboard tab bar and wire
its three sections (Current conversation / Recent poll verdicts /
Recent history). Emit the `visit` payload from C++.

**Files:**

- `src/DashboardUIManager.cpp` — extend `ComposeFullStateJSON`
  to emit the `visit` object per the schema in the **Dashboard**
  section.
- `dashboard/src/types.ts` — extend `DirectorState` with the
  `visit` payload.
- `dashboard/src/components/TabBar.tsx` — third tab button.
- `dashboard/src/components/tabs/VisitTab.tsx` — new component
  hosting the three sections.
- `dashboard/src/components/CurrentConversationPanel.tsx` —
  new; phase-aware auxiliary lines per the tab spec.
- `dashboard/src/components/RecentPollsPanel.tsx` — new; last
  ~5 poll verdicts.
- `dashboard/src/components/VisitHistory.tsx` — new; outcome
  pills.
- `dashboard/src/App.tsx` — extend `activeTab` union; add
  conditional render.
- `dashboard/styles.css` — phase-badge palette and outcome
  pills.

**Sub-tasks:**

1. On the C++ side, populate the `visit` payload each tick:
   read `VisitState`'s snapshot, `DerivePhase()`, the recent
   poll ring (a new C++-side ring buffer added to
   `VisitConclusionPoll` in Step 10 — up to 5 recent verdicts),
   and the recent-history ring.
2. Extend `DirectorState` in `types.ts` with the `visit` shape
   from the design section.
3. Build `VisitTab.tsx` as three stacked components:
   `CurrentConversationPanel` (conditional on `visit.mode !==
   'idle'`), `RecentPollsPanel` (conditional on either running
   or having recent verdicts), `VisitHistory` (always).
4. `CurrentConversationPanel` renders sender/topic/mood/
   briefing-preview, phase badge, nudge counter, phase-specific
   aux line (Salutation elapsed/distance, Discuss next-gate
   summary, OnHold trigger, ReEngage distance, Valediction
   dwell, ReturnHome distance/cell/timeout), and elapsed-in-
   lifecycle vs. hard-timeout.
5. `RecentPollsPanel` renders the last ~5 verdicts with
   timestamp/verdict/rationale.
6. `VisitHistory` renders the last ~10 dispatches with
   color-coded outcome pills.
7. Wire the tab bar and App-level state per the file map.
8. Add the palette to `styles.css` per the design spec.

**Specifics:**

- The dashboard's polling loop is the trigger for updating the
  aux-line countdown/distance values — no additional polling
  needed on the C++ side beyond the fields in the payload.
- The `next-gate summary` for Discuss combines the three gate
  states — pick the closest-to-tripping one and display its
  current value / threshold.

**Verify:** Build clean (C++ + Rollup). Open dashboard with F7.
Confirm three tabs. Switch to Visit. With no visit in progress,
the panel shows an empty state and just the (possibly empty)
history list.

Trigger a visit dispatch:

- Panel appears in Salutation with the aux line showing
  distance-to-approach.
- After the sender approaches, panel flips to Discuss; the
  next-gate summary updates each poll-gate tick.
- Poll verdicts appear in the middle panel with their rationale
  strings.
- On natural conclusion → Valediction → ReturnHome → Idle: each
  phase reflects with the correct badge color; on completion,
  a new history entry appears with the correct outcome pill.

Trigger a hard-abort (sender death). Panel unmounts;
history entry with `aborted` outcome appears.

---

### Step 17 — End-to-end integration verification

- [x] Complete

**[USER]**

**Goal:** Play through the full lifecycle and verify all
documented behaviors work together end-to-end. This is the
gating step for phase completion.

**Sub-tasks:**

1. Boot Skyrim in a save with at least three unique NPCs the
   player has recently interacted with (any post-Phase-04
   session accumulates enough engagement data).
2. Open the dashboard (F7). Confirm three tabs and Visit tab
   shows `mode: idle`.
3. Force-advance the Director to a tick where an urgent-personal
   beat is a plausible pick — e.g. dwell in Rising Action past
   the ideal duration with the Director's desired direction set
   to Raise (or Falling Action with Lower for a contrite visit).
   The action-select LLM should be able to pick `npc_visit`.
4. Confirm dispatch:
   - Sender warps to a nearby out-of-sight marker.
   - Sender walks up to the player via the Follow package.
   - Panel shows Salutation → Discuss transition once the
     opening line is spoken.
5. **Sender's opening line is on-topic.** Confirm via
   subtitles or the log that the opening line references the
   composer's briefing content. This is the load-bearing test
   for the ExecuteAction + briefing argsJson pipeline.
6. Have a conversation with the sender via SkyrimNet's chat.
   Confirm poll verdicts appear in the dashboard's recent-polls
   panel with rationale strings that track the conversation.
7. Let the poll return `should_conclude: true`. Confirm
   Valediction fires with an appropriate closing line and the
   sender walks away during ReturnHome.
8. Confirm the sender teleports back to their pre-dispatch
   location once out of range (or after the timeout).
9. **OnHold / ReEngage test**: dispatch another visit; during
   Discuss, press E on any unrelated NPC to open DialogueMenu.
   Confirm OnHold with `player_dialogue` trigger; exit menu;
   confirm ReEngage → Discuss with a resumption line.
10. **Ignore-irritation test**: dispatch another visit; during
    Discuss, walk away from the sender and ignore them for
    ~3 minutes of real-time. Confirm ContinueConversation fires
    at each silence threshold; after the nudge cap, Valediction
    fires with an elevated nudge-flavored closing line.
11. **Hard-abort test**: dispatch another visit; kill the
    sender via console during Discuss. Confirm the visit ends
    cleanly, no orphaned quest state, and history entry shows
    `aborted`.
12. **Save/reload test**: dispatch a visit; save mid-Discuss;
    quit to main menu; reload. Confirm the visit resumes
    correctly — sender still following, poll continues on its
    gate, dashboard shows the correct state.
13. **Director budget check**: while a visit is running, watch
    the dashboard's Director tab. Confirm no other Director
    actions fire until the visit reaches Idle (dispatcher
    respects the in-flight window).
14. **Cross-action legibility**: over a longer play session,
    watch action-select decisions in the log. Confirm the LLM
    picks `npc_visit` for beats that would be awkward as letters
    (urgent, apology, threat-in-person) and prefers
    `npc_letter` for beats that read fine on paper.

**Verify:** Every sub-task's expected behavior happens. No
crashes, no soft-locks, no stuck quest state. The dashboard's
counts and timers always match the actual runtime state.

---

## Done condition

Phase 05 is complete when:

- All 17 implementation steps are checked off.
- Step 17's integration-verification run passes without
  intervention.
- The SKSE log over a 30-minute session shows visits
  dispatching, Discuss phases reaching natural conclusions via
  the poll (not just outer-timeout aborts), and senders
  returning to origin without drift.
- OnHold / ReEngage transitions fire correctly when the player
  enters vanilla dialogue mid-Discuss, and the visit picks up
  cleanly.
- The Director does not fire other actions while a visit is
  running.
- Save/load preserves in-flight state; mid-run reload either
  resumes cleanly or aborts to Idle with the sender demoted and
  returned home.
- The action-select LLM picks `npc_visit` vs. `npc_letter`
  sensibly per the "urgent-personal" description.
- The natural-conclusion poll's rationale strings track
  sensibly against the actual conversation.
