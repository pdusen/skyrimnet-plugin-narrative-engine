# IntelEngine Prior Art — "Seek You Out" Visits (`seek_player`)

> **Reading discipline.** This documents *one* IntelEngine feature as it was actually built. IntelEngine was abandoned
> mid-development, not polished. Everything below is "how someone got far enough to ship," not a verified-correct
> reference. Every substantive claim cites a file and line so it can be checked; where I could not determine something
> I say so explicitly.

This file covers the feature the IntelEngine README describes as:

> Seek you out over unfinished business, old friendships, or something they overheard

Internally this is the `seek_player` story type of IntelEngine's **Story Engine** (the "Dungeon Master"). The
`informant` type shares almost the entire pipeline with it, so it is mentioned where the code paths are literally the
same.

Source repos (read-only):

- `C:\Projects\IntelEngine-NativePlugin\` — C++ under `SKSE/src/`, Papyrus under `Source/Scripts/`, prompts under
  `SKSE/Plugins/SkyrimNet/prompts/`
- `C:\Projects\IntelEngine-GamePlugin\` — deployed copies of the same prompts/scripts plus the compiled artifacts

---

## 1. High-level overview, step by step

### 1.1 The tick fires

1. `IntelEngine_StoryEngine` is a quest script that owns one shared game-time timer for three subsystems (Story DM,
   NPC social DM, Politics). `StartScheduler()` registers `RegisterForSingleUpdateGameTime` at the **shortest** of the
   three intervals, plus a 30-second real-time backup poll for low-timescale games
   (`Source/Scripts/IntelEngine_StoryEngine.psc:209-248`, `IDLE_POLL_INTERVAL = 30.0` at line 37).
2. Every fire calls `TickScheduler()` (`:593`). It self-gates: the Story DM only runs when
   `(now - LastStoryTickTime) * 24 >= storyInterval`, default **3.0 game hours**
   (`Core.GetStoryEngineInterval()`, `IntelEngine_Core.psc:1696`; MCM slider default 3.0, range 0.5–168,
   `IntelEngine_MCM.psc:1102-1106`).
3. Hard gates before anything else: the Story Engine must be enabled; `IsActive` must be false (with a 6-game-hour
   C++ watchdog `ShouldResetPending("storyDM", 6.0, ...)` to unstick stale flags across save reloads,
   `:655-665`); and **the player must not be in combat** (`:669`).

### 1.2 Building the candidate pool (mostly off the main thread)

1. Papyrus copies the last 5 NPC-gossip log lines into C++ via `SetRecentGossipContext` (`:671-692`), packs the MCM
   per-type toggles into a bitmask, and calls
   `IntelEngine.BeginAsyncStoryDMTick(7, LongAbsenceDaysConfig, excludeList, ...)` (`:697-700`).
2. C++ runs a four-phase pipeline (`SKSE/src/Papyrus.cpp:2635-2713`):
   - **Phase 0 (worker thread)** — two SkyrimNet SQL calls: the set of NPCs the player has interacted with inside the
     absence window, and a ranked engagement list (`NPCIndex.cpp:1957-1965`).
   - **Phase A (main thread)** — engine snapshot: three passes over actors, producing value-only structs
     (`NPCIndex.cpp:1967-2136`).
   - **Phase B (worker thread)** — scoring, sorting, per-candidate SQL, markdown assembly
     (`NPCIndex.cpp:2142-2545`).
   - **Phase C (main thread)** — hand the finished JSON back to Papyrus `OnStoryDMContextReady`.
3. The three Phase-A passes are:
   - **Pass 1** — MemoryDB-ranked candidates (may be in unloaded cells). Skips creatures, skips anyone in the
     "recently interacted with player" set, skips ineligible actors, skips anyone in the player's own cell.
   - **Pass 2** — exactly **3 "random encounter" slots** taken from loaded actors that pass the strict filter.
   - **Pass 3** — up to **4 "location mates"** (loaded actors standing in the same named location as an existing
     candidate), which exist for the NPC-to-NPC story types rather than for `seek_player`.

### 1.3 Scoring and the prompt

1. Phase B scores every candidate (`NPCIndex.cpp:2195-2225`):

   ```text
   score = log2(dbScore + 1)
         + absenceBonus     // min(daysSinceLast/30, 2) * min(sqrt(interactionCount), 15) * 1.5
         + noveltyBonus     // 4.0 for a total stranger, else 4.0 / (1 + interactionCount)
         + geoBonus         // +3.0 if the NPC's settlement/hold == the player's
         + fofBonus         // +2.0 if a "friend of a friend" of the player's top-5 contacts
         + noise            // uniform 0.0 .. 3.0
   ```

2. The top `maxCandidates` (7) MemoryDB candidates plus up to 3 random ones become the pool, and the pool is written
   into `m_dmCandidatePool` (lowercased name → FormID) so the LLM's answer can be resolved back to an exact actor
   without fuzzy name matching (`:2249-2255`).
3. The markdown context contains: World State (player location, danger, interior/exterior, hold, time of day, at-inn,
   time since last dispatch), a 6-entry **Recent Dispatch History**, Recent Gossip, a political summary, then one
   block per candidate with Hold / Distance / **Knows player** / **Last met player** / Bio / Faction / Relationships /
   Memories / Last conversation / Recent events / `Knows:` world facts / In-game connections, then per-type pick
   counts and recent quest history (`:2257-2543`).

### 1.4 The LLM decides

`IntelEngine_StoryEngine.SendStoryLLMRequest` calls `SkyrimNetApi.SendCustomPromptToLLM("intel_story_dm", ...)`
(`:1351-1358`). The prompt
(`SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt`) asks the model to pick **one** story from the pool, or
reject. `seek_player` is section-gated on `show_seek_player == "1"` and instructs the model to prefer long-absent
intimate contacts. The model must answer with a single raw JSON object under 700 characters.

### 1.5 Validation gauntlet (Papyrus + C++)

`OnDungeonMasterResponse` (`:1364-1627`) runs, in order:

1. `should_act` parse; require `type`; require `npc`.
2. `NotifyStoryTypePicked` + `RecordStoryDispatch` — the dispatch is recorded **optimistically as DISPATCHED before
   any gate has run**.
3. `ResolveStoryCandidate(npcName)` — exact pool lookup, falling back to fuzzy `FindByName`. Dead/disabled rejects
   (with a special substitution path for vanilla `Courier`/`Messenger` refs, which sit `.Disable()`d between vanilla
   deliveries).
4. High-status reject (`IsHighStatusNPC`) — Jarls/stewards/court wizards/housecarls never physically travel.
5. Hold-restriction reject (`CheckHoldRestriction(npc, "seek_player")`).
6. Story cooldown reject (`ApplyCooldownCheck`) — default **24 game hours** per NPC.
7. `seek_player`/`informant`-specific re-validation: reject if the NPC ended up in the player's cell during the LLM
   round trip.
8. C++ `ValidateStoryResponse` (type not disabled by the toggle bitmask; quest sub-type field checks).
9. `IsJarl` reject.

Every reject calls `MarkLastDispatchFailed("<reason>")`, which flips the head of the dispatch ring buffer to
`REJECTED — <reason>`; the next tick's prompt shows it so the model can learn from its own failures
(`NPCIndex.cpp:2620-2635`, prompt rule 14).

### 1.6 Memory injection, then dispatch

On success (`:1574-1589`):

- A **persistent SkyrimNet event** is registered on the NPC: `"<NPC> set out to find <Player>"`. If
  `NPCKnowsPlayer(npc)` is false the wording becomes `"...set out to find someone known as '<Player>' — has never met
  them before, only heard the name"`, plus an injected fact telling the NPC to introduce themselves.
- A **fact** is injected into the NPC's bio: `"set out to find <Player> -- <narration>"`.
- `ActiveStoryType = "seek_player"`, then `DispatchToTarget(npc, player, narration, "story")`.

### 1.7 Optional "knock at the door" interception

If the player is inside their own home (`IsPlayerInOwnHome`) and an exterior door can be resolved,
`DispatchToTarget` shows a blocking `SkyMessage.Show` prompt — "<NPC> is knocking at your door." with **Let them in /
Send them away / Ignore**, 30-second timeout (`:1676-1710`). "Let them in" unlocks the cell door and teleports the NPC
to the exterior door before normal dispatch continues; the other two branches teleport the NPC to the door anyway,
inject a "was turned away" / "nobody answered" fact and memory, and abort.

### 1.8 Travel

`DispatchToTarget` (`:1712-1769`):

1. Allocate one of **5 agent slots** (`Core.FindFreeAgentSlot`, `IntelEngine_Core.psc:514`) — the same slot pool used
   by player-commanded travel/fetch/deliver tasks.
2. `Core.AllocateSlot(slot, npc, "story", playerName, 1)` — forces the NPC into a quest ReferenceAlias, adds them to
   `IntelEngine_TaskFaction`, and writes `Intel_TaskType="story"`, `Intel_Target`, `Intel_State=1`.
3. Set `IsActive`, `ActiveStoryNPC`, `ActiveNarration`; write `Intel_IsStoryDispatch=1` and `Intel_StoryNarration`.
4. `ReapplyTravelPackage(npc)` (`:4950-4973`) — strip all package overrides, set the player as the NPC's linked ref
   under keyword `IntelEngine_TravelTarget`, add `TravelPackage_Jog` at priority 100, `EvaluatePackage()`.
5. Initialize stuck tracking and off-screen travel estimation. For player-targeted stories the off-screen ETA is
   **capped at 0.25 game hours** to stop NPCs stranding when the player moves (`:1744-1758`).
6. `StopScheduler()`, arm the 150 ms C++ `ProximityMonitor` for fast arrival, and switch to a 3-second real-time
   `OnUpdate` poll.

While traveling, the NPC's SkyrimNet bio renders "I am currently **on my way to find \<Player\>** — \<narration\>"
via the `0801_intel_task_awareness` submodule, which reads `Intel_TaskType`/`Intel_State`/`Intel_StoryNarration`
straight out of StorageUtil (`prompts/submodules/character_bio/0801_intel_task_awareness.prompt:31-36`).

### 1.9 En-route abort conditions

`CheckStoryNPCArrival()` (`:2214-2497`) runs every 3 s and every proximity callback. For a player-targeted story it
aborts (with a narrated "turned back" line) when:

- the player enters a **blocked location** or leaves the **whitelisted** location set;
- the player enters a **dangerous** location and the danger-zone policy blocks this NPC;
- the player is at home and the player-home policy blocks this NPC;
- the hold restriction stops passing.

It pauses (does nothing) while the player is in combat (`:2359-2361`).

Recovery ladder, in order: soft stuck recovery (nudge + re-path), then "immersive teleport" — 500 units behind the
player in interiors, 3500 units behind in exteriors (`TELEPORT_OFFSET_INTERIOR/EXTERIOR`, `:142-143`,
`ImmersiveTeleportToTarget` `:2503-2514`) — then a hard timeout at `MaxTravelDaysConfig` (default **1 game day**)
which teleports and force-arrives in one step.

Off-screen NPCs never leapfrog; they arrive by elapsed-time estimate (`CheckOffScreenProgress`) and are then
teleported behind the player.

### 1.10 Arrival and the "conversation"

`OnStoryNPCArrived()` (`:2520-2569`) for `seek_player`:

1. `ActiveStoryNPC.SetLookAt(player)`.
2. `Core.SendTaskNarration(npc, ActiveNarration, player)` → `SkyrimNetApi.DirectNarration(...)`
   (`IntelEngine_Core.psc:1460-1464`). **This is the whole payoff.** There is no forced greeting, no forced dialogue
   scene, no dialogue topic. SkyrimNet receives a past-tense narration and the NPC reacts through its own dialogue
   system. Whether a conversation happens is up to SkyrimNet and the player.
3. `AddRecentStoryEvent("seek_player: <name> -- <narration>")` (rolling 8, `:4979-4985`).
4. `FinishArrivalWithLinger(npc, player)` (`:1950-1977`) — clear the slot (which writes a task-history entry visible
   in the NPC's bio), clear dispatch state, then **link the NPC to itself** and apply `SandboxNearPlayerPackage` at
   priority 90 so the NPC idles where it stands. (Linking to the player is explicitly avoided in a code comment
   because the NPC would follow forever.) `EnsureBuildingAccess` unlocks the door if the NPC ended up inside.

### 1.11 Termination

`CheckStoryLingerCleanup()` (`:1979-2054`) releases the NPC when either:

- **300 real-time seconds** elapse (`LINGER_TIMEOUT_SECONDS`, `:140`), or
- after a 30-second grace period, `Core.ShouldReleaseLinger(npc)` returns true — the player is more than
  `LINGER_RELEASE_DISTANCE` (default **800 units**, MCM-tunable 400–2000) away, or the NPC is unloaded, or they are in
  different cells (`IntelEngine_Core.psc:925-944`).

`ReleaseLinger` removes the sandbox override and either trusts the NPC's schedule AI, or (for sandbox-only NPCs)
walks/teleports them home (`IntelEngine_Core.psc:946-994`).

---

## 2. Intended gameplay experience

Reading the prompt text, the tuning constants, and the author's own README language, the design intent is fairly
legible:

**"Forgotten friends don't stay forgotten."** The README's own gloss is: *"NPCs travel to find you because of
unfinished business, old friendships, or something they overheard. Forgotten friends don't stay forgotten — NPCs you
haven't seen in a while are more likely to come looking."* (`IntelEngine-NativePlugin/README.md:369`.) The mechanical
expression of this is the `absenceBonus` scoring term and the `LongAbsenceDaysConfig` default of **3 days**: NPCs the
player spoke to recently are actively filtered *out* of the pool, and the ones with deep history plus long silence are
scored *up*.

**Absence itself is a valid motive — the author had to argue the LLM into this.** The prompt spends more words
defending `seek_player` than any other type:

```text
NPC travels to the player with purpose. They must have a genuine reason — and
**"missed them after a long absence"** is a genuine reason when the memories support it.
...
They do NOT need a fresh crisis to justify walking out the door.
```

and again in the global rules:

```text
**Long-absence "acquainted" NPCs are PRIME seek_player material.** ... A long-absent lover or sworn companion
walking in the door IS the compelling moment — don't skip it to dispatch another faction quest.
```

That repetition is itself evidence: the author kept losing the quiet emotional beat to the flashier quest/faction
types and wrote increasingly emphatic prompt language to claw it back.

**Rarity is the point.** Rule 3: *"You do NOT have to dispatch every tick. Rejecting is the NORMAL outcome. ... Hours
of silence followed by one perfect moment is the goal."* Reinforced mechanically by a 3-game-hour tick, a 24-game-hour
per-NPC cooldown, and a World State line that explicitly tells the model to raise its threshold when a story fired
recently and lower it after days of quiet (`NPCIndex.cpp:2270-2291`).

**No spoilers about where the player is.** The narration rules forbid the NPC "knowing" the player's location:

```text
- Bad: "went to Whiterun to find {{ playerName }}" (asserts location)
```

The fiction is that the NPC set out with a reason and found the player; the engine handles the "how" (including
teleporting them 3500 units behind the player when pathing fails). The player is meant to experience *someone showed
up because they cared*, not *the game spawned an NPC at me*.

**First contact should feel like first contact.** The `Knows player` tier (stranger / aware / acquainted, computed in
`NPCIndex.cpp:2429-2449`) drives prompt rules 4 and 11, and Papyrus mirrors it: a stranger gets a persistent memory
saying they've *never met* the player and a fact telling them to introduce themselves and confirm identity
(`IntelEngine_StoryEngine.psc:1580-1581`).

**Immersive interruption limits.** Danger-zone policy, player-home policy, per-type hold restriction, the location
blocklist/whitelist, and the knock-at-the-door prompt all exist so the visit can't ruin a dungeon crawl or teleport a
farmer into a draugr barrow. The default danger policy is "block civilians" and the default `seek_player` hold policy
is "same hold, civilians only" (`IntelEngine_StoryEngine.psc:49, 72`) — civilians stay local; combat-capable NPCs may
cross holds for a strong reason.

**The arrival is deliberately soft.** No dialogue is forced. The NPC narrates why they came and then *idles nearby*
for up to five real minutes; walk away and the moment simply ends and the NPC goes home. That is the same "natural
arrival" philosophy the scheduling feature uses.

---

## 3. Implementation breakdown

### 3.1 Files that matter

| Layer | File | Role for `seek_player` |
| --- | --- | --- |
| Papyrus | `Source/Scripts/IntelEngine_StoryEngine.psc` | Tick, DM response routing, dispatch, arrival, linger |
| Papyrus | `Source/Scripts/IntelEngine_Core.psc` | Slots, packages, linger release, narration/fact/memory APIs |
| Papyrus | `Source/Scripts/IntelEngine_MCM.psc` | Toggles, interval/cooldown/absence sliders, hold policy |
| C++ | `SKSE/src/NPCIndex.cpp/.h` | Candidate pool, scoring, markdown, cooldowns, dispatch ring buffer |
| C++ | `SKSE/src/Papyrus.cpp` | `BeginAsyncStoryDMTick`, request JSON, proximity arming, travel-time math |
| C++ | `SKSE/src/ProximityMonitor.cpp/.h` | 150 ms arrival detection |
| C++ | `SKSE/src/OffScreenTracker.cpp` | Time-based arrival for unloaded NPCs |
| C++ | `SKSE/src/CellAnalyzer.cpp` | `IsPlayerInDangerousLocation`, `IsPlayerInOwnHome` |
| C++ | `SKSE/src/FactionPolitics.cpp` | `ValidateStoryResponse`, political context block |
| Prompt | `SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt` | The DM decision prompt |
| Prompt | `.../submodules/character_bio/0800_intel_facts.prompt` | Renders injected facts into the NPC's bio |
| Prompt | `.../submodules/character_bio/0801_intel_task_awareness.prompt` | Renders "on my way to find X — …" |
| Config | `.../config/plugins/IntelEngine/manifest.yaml` | Blocklists/whitelists for story candidates |

There is **no SkyrimNet action YAML** for `seek_player`. It is not an LLM-selectable action for individual NPCs; it is
a top-level DM decision. The 14 action YAMLs under `config/actions/` are all player-driven task actions
(`intel_travel`, `intel_fetchnpc`, …).

### 3.2 ESP forms used

From `docs/ESP_STRUCTURE.md` and the property references in the scripts:

- **Quest** `IntelEngine_Quest` — the single quest hosting all subsystem scripts; `IntelEngine_StoryEngine` is one of
  its attached scripts. `BeginAsyncStoryDMTick` dispatches its callback by the quest's EditorID
  (`Papyrus.cpp:2646-2651`).
- **ReferenceAliases** — 5 agent slots + 5 target slots (`Core.GetAgentAlias(slot)`, `ForceRefTo`).
- **Packages** — `TravelPackage_Walk` / `_Jog` / `_Run` / `_Stalk`, `SandboxPackage`, `SandboxNearPlayerPackage`.
  `seek_player` uses `TravelPackage_Jog` at priority `PRIORITY_TRAVEL = 100` while traveling and
  `SandboxNearPlayerPackage` at `PRIORITY_SANDBOX = 90` on arrival (`IntelEngine_Core.psc:68-107`).
- **Keyword** `IntelEngine_TravelTarget` — the linked-ref keyword the travel package's target condition reads
  (`PO3_SKSEFunctions.SetLinkedRef`).
- **Faction** `IntelEngine_TaskFaction` — membership marks an NPC as busy.
- **Globals** — `IntelEngine_StoryEngineEnabled`, `IntelEngine_StoryEngineInterval`, `IntelEngine_StoryEngineCooldown`
  (the last is read directly from C++ by EditorID in `NPCIndex::GetStoryCooldownHours`, `NPCIndex.cpp:2876-2882`).

> Caveat: `docs/ESP_STRUCTURE.md` is an early design doc, not a dump of the shipped `IntelEngine.esp`. Some packages
> it lists (`IntelEngine_WaitPackage`, `IntelEngine_ApproachPackage`) are never referenced by the story path. Treat it
> as intent, not as ground truth about the binary plugin.

### 3.3 Concrete numbers

| Constant | Value | Where |
| --- | --- | --- |
| Story DM interval | 3.0 game hours (MCM 0.5–168) | `IntelEngine_Core.psc:1696`, `IntelEngine_MCM.psc:1104` |
| Per-NPC story cooldown | 24 game hours (MCM 6–72) | `IntelEngine_Core.psc:1700`, `IntelEngine_MCM.psc:1109` |
| Long-absence threshold | 3 days (MCM 1–14) | `IntelEngine_StoryEngine.psc:46`, `IntelEngine_MCM.psc:1118` |
| INI `storyMinAbsenceDays` | 3.0 (separate knob) | `SKSE/src/Settings.h:61` |
| Max candidates requested | 7 ranked + 3 random + 4 locmates | `IntelEngine_StoryEngine.psc:699`, `NPCIndex.cpp:2074` |
| Engagement query fan-out | `maxCandidates * 4 * 4` = 112 | `NPCIndex.cpp:1963`, `MemoryDB.cpp:770` |
| Memories per candidate | `min(Settings.maxMemoriesInContext, 2)` | `NPCIndex.cpp:2016` |
| Dispatch history ring | 6 entries, 100-byte narration cap | `NPCIndex.h:644-647` |
| Recent story events | 8 entries | `IntelEngine_StoryEngine.psc:4982` |
| Facts per NPC | 10, pure FIFO, no expiry | `IntelEngine_Core.psc:1498-1504` |
| Real-time monitor poll | 3.0 s | `IntelEngine_StoryEngine.psc:36` |
| Proximity monitor | 150 ms, 150 u XY, 120 u Z | `ProximityMonitor.h:43-55` |
| Arrival distance (Papyrus) | 300 u | `IntelEngine_Core.psc:113` |
| Linger release distance | 800 u (MCM 400–2000) | `IntelEngine_Core.psc:134` |
| Linger timeout | 300 real seconds | `IntelEngine_StoryEngine.psc:140` |
| Travel timeout | 1 game day (MCM 0.25–3) | `IntelEngine_StoryEngine.psc:45` |
| Off-screen ETA cap (player-target) | 0.25 game hours | `IntelEngine_StoryEngine.psc:1748` |
| Teleport offsets | 500 u interior / 3500 u exterior | `IntelEngine_StoryEngine.psc:142-143` |
| Travel-time model | `straightLine * 1.5 / 18000 u-per-hour * 3` | `Papyrus.cpp:1079-1093` |
| LLM response budget | < 700 chars total, narration < 120 | `intel_story_dm.prompt:239` |
| Concurrent agent slots | 5 (MCM 1–5) | `IntelEngine_Core.psc:514-539` |

### 3.4 The prompt shape

`intel_story_dm.prompt` is a single Jinja2 template with one `[ system ]` / `[ user ]` pair. Every story type's
section is wrapped in `{% if show_<type> == "1" %}`; the `show_*` values are computed in C++ from the MCM toggle
bitmask and are always emitted as `"1"`/`"0"` — never omitted — because SkyrimNet registers decorators that would
otherwise shadow a missing template variable (`Papyrus.cpp:2519-2530`, an unusually well-documented workaround).

The `seek_player` section (prompt lines 23-45) is prescriptive rather than descriptive: it enumerates "PRIME
candidates", gates, and four worked narration examples with two negative examples. The response schema is a flat JSON
object:

```text
{"should_act":true,"type":"seek_player","npc":"Name",
 "narration":"gave up waiting at the inn and set out to find <Player> after days of silence"}
```

Notably `seek_player` uses **only** `npc` and `narration` — no destination, no message content, no sub-type. All the
richness lives in that one sub-120-character past-tense verb phrase, which becomes both the injected bio fact and the
arrival narration.

### 3.5 State, persistence, and cross-boundary plumbing

**Papyrus quest properties (persist in the save):** `IsActive`, `ActiveStoryNPC`, `ActiveNarration`,
`ActiveStoryType`, `LastStoryTickTime`, `LastIdlePollTickTime`, all MCM-configurable values.

**StorageUtil keys written per dispatched NPC:**

- `Intel_TaskType = "story"`, `Intel_Target`, `Intel_Slot`, `Intel_State = 1`, `Intel_Speed` (`AllocateSlot`)
- `Intel_IsStoryDispatch = 1` (never read — see §4)
- `Intel_StoryNarration` (read by the task-awareness bio submodule)
- `Intel_StoryLastPicked` (cooldown), plus membership in `Intel_CooldownActors` on the quest
- `Intel_TaskStartTime`, `Intel_OffscreenArrival`
- `Intel_StoryLingerStart` and membership in `Intel_StoryLingerActors` (a FormList on the *player*)
- `Intel_Facts` / `Intel_FactTimes` / `Intel_FactsRendered` (fact injection)
- `Intel_TaskHistory` / `Intel_TaskHistoryTime` / `Intel_TaskHistoryRendered` (written on slot clear)

**C++ state is entirely volatile** — cooldown mirror, social cooldown mirror, story-type counts, dispatch ring buffer,
`m_dmCandidatePool`. It is warmed back from StorageUtil on load by `WarmCooldownMirror()` and
`WarmStoryTypeCounts()` (`:1121-1171`, `:1325-1345`). The SlotTracker is the only piece with real SKSE co-save
serialization, and that is task state, not story state.

**The visit itself does not survive a save/load.** `RestartMonitoring()` explicitly abandons any in-flight story
dispatch on game load (`:371-382`):

```text
; Story dispatches are transient ? abandon on load.
; Reapplying stale packages causes NPCs to resume old travels even after
; manual intervention or completion. The DM will dispatch new stories.
```

Lingering NPCs are likewise released on load (`:449-463`).

**ModEvents:** none in this path. The DM tick uses direct native calls plus
`AsyncDispatch::ExecuteQuestFunctionString` to call back into Papyrus by quest EditorID + script + function name. The
only ModEvents in the codebase are the PrismaUI dashboard callbacks (`IntelEngine_Core.psc:1918-2500`), one of which,
`OnDashboardDispatchStory` (`:2252-2323`), can manually fire a `seek_player` with a chosen NPC and narration —
deliberately skipping cooldown and MCM checks ("it's a manual DM override").

---

## 4. Weaknesses and bugs

### Confirmed by reading the code

**4.1 The candidate pool is printed in reverse, so the prompt's "#1–#3" advice points at the *worst* candidates.**
Phase B sorts descending by score, splits into ranked + random, concatenates (`ranked…, random…`), then does
`std::reverse(finalPool.begin(), finalPool.end())` before numbering the markdown headings `1..N`
(`NPCIndex.cpp:2227-2241, 2367-2371`). The stated reason is LLM recency bias — put the best last. But the prompt then
says:

```text
A candidate sitting at #1–#3 of the ranked pool with deep memories and a multi-day gap is almost always a better
pick than a stranger-courier faction quest.
```

After the reverse, positions #1–#3 are the **random-encounter/stranger slots** (they were appended last, so they land
first), and the highest-scored long-absence contact is printed *last* with the highest number. The prompt's most
emphatic `seek_player` steering instruction therefore points the model at exactly the candidates it is telling it to
avoid.

**4.2 The dispatch is recorded as DISPATCHED, and the "last story dispatched" clock is reset, before any validation
runs.** `NotifyStoryTypePicked` (which sets `m_lastStoryDispatchGameTime` and `m_lastStoryDispatchType`,
`NPCIndex.cpp:2547-2558`) and `RecordStoryDispatch` are both called at `IntelEngine_StoryEngine.psc:1428-1432`, before
NPC resolution, cooldown, hold restriction, high-status, interior, and C++ validation. `MarkLastDispatchFailed` later
flips the ring-buffer entry to REJECTED, but **nothing rolls back `m_lastStoryDispatchGameTime`**. So a run of failed
dispatches makes the World State line say "Last story dispatched: 0 game hours ago — recent, avoid dispatching the
EXACT same type" and suppresses the *next* real attempt, even though the world saw nothing. It also inflates the
per-session type counts (`m_storyTypeCounts[storyType]++` fires unconditionally) and the `world_quiet` flag
(`Papyrus.cpp:2605-2611`) reads the same poisoned timestamp.

**4.3 The story cooldown is burned on NPCs whose dispatch is then rejected.** `ApplyCooldownCheck`
(`IntelEngine_StoryEngine.psc:1078-1099`) *writes* `Intel_StoryLastPicked = now` as a side effect of passing the
check, and it runs at `:1496` — before C++ `ValidateStoryResponse` (`:1535`), before the Jarl check (`:1544`), and
before the quest-active check. An NPC picked for a `seek_player` that then fails validation is locked out of the pool
for 24 game hours anyway.

**4.4 "Send them away" / "Ignore" at the door teleports the NPC to the player's doorstep and abandons them there.**
In `DispatchToTarget` (`:1696-1708`), both rejection branches call `npc.MoveTo(exteriorDoor, ...)` and then `return`
without allocating a slot, applying a package, or registering linger cleanup. The NPC has been physically relocated
from wherever they were (possibly another hold) to the player's front door with no travel state and no way home. NPCs
with schedule AI will eventually wander back; sandbox-only NPCs will not. The comment two lines above claims "NPC
stays at their origin (unloaded/far away) — player never sees them," which is true only for the *prompt*, not for the
outcome branches.

**4.5 `GetEligibleStoryTypes` is dead code, and Papyrus still references the line it was meant to emit.**
`NPCIndex::GetEligibleStoryTypes` (`NPCIndex.cpp:1497-1564`) computes a per-candidate list of legal story types. It is
never called from anywhere — only declared (`NPCIndex.h:210`) and defined. The async markdown builder never emits an
`Eligible:` line. Yet `IntelEngine_StoryEngine.psc:1489-1490` still says *"Hold restriction enforcement — the LLM may
ignore the Eligible line."* The per-candidate constraint the prompt architecture assumes exists does not reach the
model at all; only the global `show_*` flags and the server-side rejects do.

**4.6 The archetype classifier's CIVILIAN list misses most non-combat vanilla classes, and 5 of its 14 entries match
nothing.** `ClassifyNPCArchetype` (`NPCIndex.cpp:1336-1390`) maps only these class *display names* to `CIVILIAN`:
citizen, farmer, beggar, child, bard's college, food vendor, peddler, apothecary, blacksmith, fence, innkeeper,
lumberjack, miner, vendor. Everything else is returned as its uppercased class name. Cross-checked against the vanilla
`CLAS` records in `C:\Projects\spriggit-output\Skyrim\Classes\`, the full set of English class names includes
`Bard`, `Priest`, `Fletcher`, `Pawnbroker`, `Tailor`, `Spell Vendor`, `Jailor`, `Prisoner` — none of which are in the
list, so all of them classify as non-civilian. Conversely `bard's college`, `peddler`, `fence`, `innkeeper`, and
`vendor` are not vanilla class names at all (the vanilla vendor classes are named `Apothecary`, `Blacksmith`,
`Fletcher`, `Food Vendor`, `Pawnbroker`, `Spell Vendor`, `Tailor`). Consequences: the "CIVILIAN NPCs NEVER enter
danger zones" gate and the default "same hold, civilians only" `seek_player` hold policy both fail open for priests,
bards, fletchers, pawnbrokers, and tailors — they will happily cross holds and walk into dungeons.

**4.7 The author's own comment documents a re-entrancy corruption bug in this exact path.** At
`IntelEngine_StoryEngine.psc:2227-2239`:

```text
; Corrupt state detection: IsActive true but no type means something went wrong
; during concurrent event processing (seen with Sylvi seek_player — FinishArrivalWithLinger
; Utility.Wait re-entry corrupted state). Clean up to prevent tick death.
```

`FinishArrivalWithLinger` calls `Utility.Wait(0.1)` (`:1969`) while `OnUpdate` is mid-flight; Papyrus can re-enter the
event during the wait. The fix shipped is detection-and-cleanup, not prevention, and the same `Utility.Wait(0.1)`
pattern remains in `ReapplyTravelPackage` (`:4971`) and `CheckStoryLingerCleanup` (`:2014`).

**4.8 `Intel_IsStoryDispatch` is written and cleared but never read.** Set at `:1736`, unset at `:4634`, `:4911`, and
`IntelEngine_Core.psc:740`. Nothing anywhere queries it. Dead state carried in every save.

**4.9 `WarmCooldownsForPool()` and `WarmSocialCooldownsForPool()` are dead code.** Both are fully implemented
(`IntelEngine_StoryEngine.psc:1253-1323`) with doc comments describing a "caller should rebuild context" contract, and
neither is called from anywhere in either repo. The pre-warm step they implement — checking StorageUtil cooldowns for
the freshly built pool before the LLM call — therefore never happens; the pool can (and on the first tick after a load
will) contain NPCs the C++ mirror does not yet know are on cooldown, wasting a DM turn on a candidate that
`ApplyCooldownCheck` will then reject.

**4.10 `PendingStoryType` is a guard that guards nothing.** Set to `"dm_analysis"` before the async tick (`:697`),
cleared in `ClearPending()` (`:4942`), never read as a condition anywhere. If the async pipeline drops its callback
(both `SKSE::GetTaskInterface()` failure paths in `Papyrus.cpp:2668-2672, 2699-2703` just log and return), nothing
detects the lost tick — it simply produces no story and the next tick proceeds.

**4.11 Unbounded, write-only player-attached lists.** `Intel_InteractedNPCs` (`IntelEngine_Core.psc:409-414`) is
appended to on every task completion via an O(n) `IntListFind` and is **never read anywhere**. `Intel_FactNPCs`
(`IntelEngine_Core.psc:1518-1522`) is a registry of every NPC that ever received a fact; since facts are pure FIFO
with no expiry (`CleanExpiredFacts` only removes the registry entry when an NPC has *zero* facts, which never happens
once one is injected), the registry grows monotonically for the life of the save and is swept entry-by-entry on every
game load (`CleanExpiredFactsGlobal`, `:1618-1635`) with a `Game.GetForm` + StorageUtil call each. On a long
playthrough that is a growing main-thread load-time cost with no upper bound.

**4.12 An in-flight visit is silently cancelled by any save/load, but its memories are not.** `RestartMonitoring`
abandons the dispatch (`:371-382`). The persistent SkyrimNet event `"<NPC> set out to find <Player>"` and the injected
fact `"set out to find <Player> -- <narration>"` were both written at dispatch time (`:1574-1587`) and are never
retracted. The NPC permanently remembers setting out to find the player and never arriving — and the 24-hour cooldown
also remains, so they will not be re-picked soon. Same applies to every abort path (danger zone, hold restriction,
blocked location): `AbortStoryTravel` (`:4881-4890`) narrates a "turned back" line but leaves the original
"set out to find" fact in place.

**4.13 Two different, independently-tuned "absence" settings with the same name.** `LongAbsenceDaysConfig` (MCM,
default 3) is what gets passed to the DM tick and drives the "recently interacted" exclusion. `Settings.
storyMinAbsenceDays` (INI, default 3) is used only inside `NPCIndex::GetStoryCooldownHours()` as
`max(mcmCooldown, absenceDays * 24)` (`NPCIndex.cpp:2876-2882`). Changing the MCM "Long Absence (days)" slider does
not change the cooldown floor, and changing the INI does not change the pool filter. Nothing surfaces the distinction
to the user.

**4.14 Prompt/validator mismatch: `narration` is never validated.** `ValidateStoryResponse`
(`FactionPolitics.cpp:2110-2219`) checks `should_act`, `type`, toggle exclusion, and quest sub-type fields — nothing
else. For `seek_player`, an empty or missing `narration` passes every gate and ends up as an empty
`SkyrimNetApi.DirectNarration` on arrival (`OnStoryNPCArrived`, `:2550`). The 700-character response cap in the prompt
exists precisely because "the system truncates longer responses, breaking parsing," so truncated/malformed narration
is a live scenario the validator does not cover.

### Suspected — not confirmed

**4.15 The "recently interacted with the player" filter may never match, because of a casing mismatch.**
`MemoryDB::GetRecentPlayerInteractionNames` inserts SkyrimNet's `recentInteractionNames` **as returned**, with no
case normalization (`MemoryDB.cpp:868-896`). All three Phase-A passes look them up lowercased:
`recentPlayerNPCs.count(StringUtils::ToLowerStd(name))` (`NPCIndex.cpp:2039, 2083, 2113`). If SkyrimNet returns
display names in normal capitalization ("Lydia"), the set lookup for `"lydia"` misses and the filter is a no-op —
which would mean the "don't dispatch someone you just talked to" rule silently does nothing, and the whole
absence-driven premise is carried only by the soft `absenceBonus` score. I could not confirm SkyrimNet's casing from
the IntelEngine repos alone; verifying requires SkyrimNet's `GetPlayerContext` implementation or a live log.

**4.16 Two concurrent DM ticks can orphan the first NPC.** `TickScheduler` gates on `If !IsActive` (`:667`), but
`IsActive` is only set inside `DispatchToTarget` — i.e. *after* the LLM round trip. `LastStoryTickTime` is stamped
before the async call (`:646`), so a second tick becomes legal 3 game hours later, which during `Wait`/sleep or at a
high timescale can elapse while the first LLM call is still outstanding. If both responses dispatch, the second
`DispatchToTarget` overwrites `ActiveStoryNPC`/`ActiveStoryType` while the first NPC still holds an agent slot, a
travel package, and a linked ref. Nothing in `CheckStoryNPCArrival` tracks more than one active story NPC, so the
first would keep walking with a permanently allocated slot until an unrelated cleanup path happens to touch it. I did
not find any guard against this, but I also could not confirm it fires in practice — the 6-hour `ShouldResetPending`
watchdog and the 3-hour interval make the window narrow outside of Wait/sleep.

**4.17 `MarkLastDispatchFailed` flips the head of the ring buffer, which may not be the entry that failed.** The C++
implementation defends against an empty buffer with a warning (`NPCIndex.cpp:2620-2635`), but not against a *different*
dispatch having been recorded in between. Because Papyrus's `RecordStoryDispatch` → gates → `MarkLastDispatchFailed`
sequence is not atomic and the ring buffer is shared with the manual Director dispatch path, an interleaving
(dashboard-driven dispatch, or the concurrent-tick scenario in 4.16) would mark the wrong entry REJECTED and feed the
LLM a false record. Unconfirmed — I could not construct a definite call ordering that triggers it.

**4.18 Cross-worldspace distance checks are unreliable in the arrival path.** `CheckStoryNPCArrival` uses
`ActiveStoryNPC.GetDistance(arrivalTarget)` and requires `dist <= 300 && dist > 0.0` (`:2406-2412`). Papyrus
`GetDistance` is known to return 0 across worldspaces; the code handles that case in `ShouldReleaseLinger`
(`IntelEngine_Core.psc:935-940`) with an explicit comment, but the arrival check does not — it just fails the
`> 0.0` guard and keeps polling until the travel timeout force-arrives. Probably benign (the timeout catches it) but
it means genuinely-arrived cross-worldspace NPCs wait out the full deadline instead of narrating.

**4.19 The travel-time model is a stack of round numbers.** `CalculateDeadlineFromDistance`
(`Papyrus.cpp:1050-1103`) is `straightLine * 1.5 (pathfinding fudge) / 18000 units-per-game-hour * 3.0 (safety
margin)`, clamped to `[0.5h, 18h]`, and returns a flat `minHours` whenever source and target are on opposite sides of
the interior/exterior boundary. The 18000 figure is annotated "walking speed at 20:1 timescale" — it is therefore
wrong for any other timescale, and `seek_player` NPCs jog rather than walk. For player-targeted stories the whole
estimate is then overridden by a hard 0.25-game-hour cap anyway (`IntelEngine_StoryEngine.psc:1744-1758`), which makes
most of the computation moot for this feature.

---

## 5. One-paragraph summary for a reader in a hurry

Every three game hours, IntelEngine builds a ~10-NPC candidate pool weighted toward people the player has *not* seen
recently, hands it plus rich per-NPC memory context to an LLM "Dungeon Master," and asks it to pick at most one story.
If it picks `seek_player`, Papyrus writes a "set out to find you" memory and a motivation fact into the NPC's SkyrimNet
bio, allocates one of five agent slots, points a jog travel package at the player via a linked ref, and monitors
arrival with a 150 ms native proximity watcher plus stuck/teleport/timeout recovery. On arrival the NPC looks at the
player, emits a single sub-120-character past-tense narration through SkyrimNet's `DirectNarration`, and idles nearby
for up to five real minutes or until the player walks 800 units away. Nothing about the visit survives a save/load.
The design intent — rare, absence-driven, emotionally-motivated visits that never announce how the NPC found you — is
clearly legible in the prompt; the implementation carries real defects around ordering (rejections poison the
"recent dispatch" clock and burn cooldowns), a reversed candidate pool that inverts the prompt's own ranking advice,
a civilian classifier that fails open for most non-combat vanilla classes, and several fully-written but never-called
code paths.
