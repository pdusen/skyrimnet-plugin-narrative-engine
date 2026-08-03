# IntelEngine Prior Art — NPC-to-NPC Autonomous Interactions

> **Reading discipline.** This documents how *IntelEngine* built one NPC-to-NPC social system. IntelEngine was
> abandoned mid-development, not polished; its author would not claim it was bug-free. Nothing here is a
> requirement, a recommendation, or a verified-correct pattern for NarrativeEngine. Every substantive claim cites a
> file and line so you can check it yourself.

The README feature line this covers:

> Interact with each other independently — arguments, deals, whispered conspiracies

Internally IntelEngine calls this the **NPC Social Life** system, or the **NPC DM tick**. It is a second,
independent LLM "dungeon master" loop that runs alongside the player-centric Story DM and emits exactly two story
types: `npc_interaction` and `npc_gossip`.

---

## 1. High-level overview, step by step

1. **Cadence.** A shared game-time scheduler (`IntelEngine_StoryEngine.psc`, `OnUpdateGameTime` at :492 and an
   idle-poll inside `OnUpdate` at :513-587) calls `TickScheduler()`, which unconditionally calls
   `TickNPCInteractions()` (`IntelEngine_StoryEngine.psc:617`). That function self-gates on its own interval —
   `NPCTickIntervalHours`, default **1.5 game hours** (`IntelEngine_StoryEngine.psc:109`, MCM slider at
   `IntelEngine_MCM.psc:521`).

2. **Re-entrancy guard.** `NPCTickPending` blocks a second tick while an LLM call is in flight; a C++ watchdog
   (`IntelEngine.ShouldResetPending("npcInteraction", 1.0, now)`) clears it after 1 game hour or after a save
   reload (`IntelEngine_StoryEngine.psc:796-812`).

3. **Candidate snapshot (main thread).** `IntelEngine.BeginAsyncNPCDMTick(4, ...)` →
   `NPCIndex::BuildNPCTickSnapshot` (`NPCIndex.cpp:1676-1753`) walks **all four process-list tiers** via
   `ProcessUtils::ForEachLoadedActor` and captures every eligible loaded actor. This is the entire candidate
   universe — NPCs the engine is not currently simulating never appear.

4. **Grouping and scoring (worker thread).** `NPCIndex::BuildNPCInteractionContextFromSnapshot`
   (`NPCIndex.cpp:1759-1912`) buckets candidates by **location name**, discards any bucket with fewer than 2 NPCs,
   scores the survivors, keeps the top 4 groups, and renders them as markdown with bios, memories, and world
   knowledge.

5. **LLM decision.** Papyrus fires
   `SkyrimNetApi.SendCustomPromptToLLM("intel_story_npc_dm", "intel_story_dm", contextJson, ...)`
   (`IntelEngine_StoryEngine.psc:829`). The prompt (`prompts/intel_story_npc_dm.prompt`) asks the model to pick
   **one pair from one group** and return a single JSON object under 700 characters, or `{"should_act":false}`.

6. **Validation.** `OnNPCInteractionResponse` (`IntelEngine_StoryEngine.psc:838-948`) checks the per-type MCM
   toggles, resolves both names back to `Actor` refs via `NPCIndex::ResolveStoryCandidate`
   (`NPCIndex.cpp:805-826`), and requires **both** NPCs to be off a 24-game-hour social cooldown before stamping
   either (`:883-889`).

7. **State is written immediately — before anything happens in the world.** A persistent SkyrimNet event is
   registered, `fact1`/`fact2` are injected into each NPC's bio (or the gossip is injected into both sides), and a
   dashboard log entry is appended (`:891-924`).

8. **Physical staging, decided purely by cell membership** (`:927-947`):
   - **Both NPCs in the player's cell** → `PerformVisibleInteraction` fires immediately, no travel.
   - **Exactly one in the player's cell** (and neither is a follower, and no social dispatch is already running)
     → the off-screen one is dispatched to *walk to the other* via `DispatchNPCSocial`.
   - **Neither in the player's cell** → nothing physical. Facts/gossip only, plus a log line.

9. **Convergence.** `DispatchNPCSocial` (`:954-987`) claims one of 5 shared task slots, wipes the traveler's
   package overrides, links them to the target with the `IntelEngine_TravelTarget` keyword, and applies
   `TravelPackage_Jog` at priority 100. `CheckNPCSocialArrival` polls every 3 real-time seconds (`:989-1042`)
   for arrival within 300 units, with off-screen teleport, stuck recovery, and a 4-game-hour hard timeout.

10. **The exchange.** `PerformVisibleInteraction` (`:1790-1802`) makes the two NPCs face each other, waits 0.5s,
    and fires **one** `SkyrimNetApi.DirectNarration(summary, npc1, npc2)`. That is the whole rendering step. There
    is no turn loop, no scripted back-and-forth, no scene state.

11. **Teardown.** `OnNPCSocialArrived` (`:1055-1060`) immediately calls `CleanupNPCSocialDispatch`, which clears
    the slot, strips all package overrides, and nulls the dispatch state. The traveler reverts to base AI in the
    same frame the narration is issued.

12. **Gossip ripple (npc_gossip only).** If both NPCs were off-screen, `SpreadGossipOffScreen`
    (`:1629-1667`) chains the rumor through a random 1-10 further NPCs, each hop picked by
    `NPCIndex::GetRelatedCandidate` (shared-event-history ranking), writing `InjectGossip` at every hop.

---

## 2. Intended gameplay experience

Read from the prompt text, the tuning values, and the code comments, the design intent is:

**A world that gossips about you behind your back.** The prompt is explicit that overhearing is the payoff:

> **Player nearby — make it worth witnessing** — if "player nearby: yes", the player will see and hear this
> interaction. Make it dramatic, interesting, or revealing. Arguments, confessions, hushed gossip, heated
> debates — things the player would want to stop and listen to.
> — `prompts/intel_story_npc_dm.prompt:91`

The scoring reinforces this: a location group containing an NPC in the player's cell gets a flat **+2.0** score
bonus (`NPCIndex.cpp:1811`), and the group header literally tells the model `(player nearby: yes)`
(`NPCIndex.cpp:1859`). The system is tuned to stage its drama in earshot.

**Continuity, not vignettes.** Rule 2 of the prompt is "Ground narration in real memories — do not fabricate
history or events", and the interaction guidance says "continuing an existing story is always more compelling than
starting a new one" (`:34`). Each interaction writes `fact1`/`fact2` — per-NPC past-tense memory fragments — that
are re-rendered into both NPCs' bios on every subsequent dialogue turn
(`submodules/character_bio/0800_intel_facts.prompt`). The intended arc is: two NPCs argue → both remember it →
the memory shows up in the pair pool next tick → the DM continues the thread.

**Silence is a valid outcome.** Rule 1: "REJECT if nothing compelling. Return should_act:false. Quality over
quantity. NPCs don't need to interact every tick — silence is natural." (`:83`). Combined with a 24-hour
per-NPC social cooldown (`NPCSocialCooldownHours = 24.0`, `IntelEngine_StoryEngine.psc:112`), the author was
clearly targeting *rare and meaningful* rather than *constant chatter*.

**Rumours as a propagation mechanic.** `npc_gossip` is designed as an information-spread simulation, not just
flavour: "Good gossip creates ripple effects: if NPC-A tells NPC-B about something NPC-C did, it gives NPC-B an
opinion about NPC-C they didn't have before" (`:44`). The off-screen chain of up to 10 hops is the mechanical
expression of that.

**Personality-appropriate framing.** The prompt tells the model to let archetype shape the interaction: "a stern
warrior doesn't gossip frivolously. A shy alchemist doesn't pick fights" (`:19`), and the pair pool feeds it a
class archetype, gender, bio summary, and two recent memories per NPC (`NPCIndex.cpp:1861-1895`).

**"Whispered conspiracies" specifically** maps to prompt rule 11:

> **Followers scheming** — NPCs marked "(follower)" travel with the player. When two followers interact about the
> player while the player is nearby, narrate it as whispered and conspiratorial ("leaned in and whispered
> to...", "glanced at the player, then quietly told...").
> — `prompts/intel_story_npc_dm.prompt:93`

**This intent was never realised** — see finding W1 below. The follower path is unreachable in the shipped code.

---

## 3. Implementation breakdown

### 3.1 Named interaction types — what actually exists

The prompt offers exactly **two response types**, `npc_interaction` and `npc_gossip`. There are **no separate
"argument", "deal", or "conspiracy" code paths** — the ten flavours named in the prompt are pure prose guidance
inside a single JSON schema:

> Two NPCs interact: argument, deal, romance, shared meal, training, brawl, favor, warning, confession,
> celebration.
> — `prompts/intel_story_npc_dm.prompt:30` and `:53`

Nothing in the C++ or Papyrus branches on the flavour. `OnNPCInteractionResponse` only distinguishes
`npc_interaction` (inject two facts) from `npc_gossip` (inject gossip both sides + chain-spread)
(`IntelEngine_StoryEngine.psc:897-923`). So:

- **"Arguments"** — implemented only as a word in the prompt; produces the same narration + two facts as any
  other interaction. No hostility, no combat, no relationship-value change.
- **"Deals"** — same. No gold transfer, no inventory change, no faction or disposition effect.
- **"Whispered conspiracies"** — the dedicated follower/conspiratorial rule exists in the prompt and is
  **unreachable** (finding W1).

The README's three named flavours are therefore best understood as *prompt vocabulary*, not features.

### 3.2 Papyrus — `IntelEngine_StoryEngine.psc`

Scheduling and state properties:

| Property | Default | Line | Meaning |
| --- | --- | --- | --- |
| `NPCTickEnabled` | `true` | 110 | Master toggle for the whole social tick |
| `NPCTickIntervalHours` | `1.5` | 109 | Game-hours between NPC DM ticks |
| `NPCSocialCooldownHours` | `24.0` | 112 | Per-NPC re-pick cooldown |
| `TypeNPCInteractionEnabled` | `true` | 67 | Per-type MCM toggle |
| `TypeNPCGossipEnabled` | `true` | 68 | Per-type MCM toggle |
| `LastNPCTickTime` | `0.0` | 108 | Game-time (days) of last tick |
| `NPCTickPending` | `false` | 111 | LLM-in-flight guard |
| `IsNPCStoryActive` | `false` | 115 | A social travel dispatch is running |
| `NPCSocialTraveler` / `NPCSocialTarget` | `None` | 116-117 | The dispatched pair |
| `NPCSocialNarration` / `NPCSocialType` | `""` | 118-119 | Carried to the arrival handler |

Key functions:

- `TickNPCInteractions()` — `:787-818`. Gates, stamps `LastNPCTickTime`, sets `NPCTickPending`, calls
  `IntelEngine.MarkSystemPending("npcInteraction", now)`, then `BeginAsyncNPCDMTick(4, ...)`.
- `OnNPCDMContextReady(String contextJson)` — `:820-836`. Fires the LLM request; clears the pending flag inline on
  failure (the comment explains it is inlined rather than reusing `SendStoryLLMRequest` precisely so the
  `npcInteraction` watchdog doesn't leak for an hour).
- `OnNPCInteractionResponse(String response, Int success)` — `:838-948`. The whole decision-to-state pipeline.
- `DispatchNPCSocial(npc, target, narration, storyType)` — `:954-987`.
- `CheckNPCSocialArrival()` — `:989-1042`, driven from `OnUpdate` at `:539-541`.
- `TeleportNPCSocialAndResume(distance)` — `:1044-1053`.
- `OnNPCSocialArrived()` / `CleanupNPCSocialDispatch()` — `:1055-1076`.
- `CheckNPCSocialCooldown` / `SetNPCSocialCooldown` — `:1101-1119`. Deliberately split so a rejected pair doesn't
  half-stamp a cooldown ("Check cooldown for BOTH before applying either", `:882`).
- `SpreadGossipOffScreen` — `:1629-1667`.
- `PerformVisibleInteraction` — `:1790-1802`.
- `AddNPCSocialLog` / `AddRecentStoryEvent` / `BuildInteractionSummary` — `:4987-5024`, `:4979-4985`, `:5031-5033`.

### 3.3 C++ — pair selection

`NPCIndex::BuildNPCTickSnapshot` (`NPCIndex.cpp:1676-1753`) — **Phase A, main thread.** Filters, in order:

```text
actor != player, has parent cell
!IsDead(), !IsDisabled()
!IsInCombat()
!IsHostileToActor(player)
!SlotTracker::HasActiveTask() && !SlotTracker::IsOnCooldown()
HasKeyword(ActorTypeNPC 0x00013794)
!IsPlayerTeammate()                      <-- excludes every follower
has non-empty display name
!IsGenericCreatureName(name)             <-- "Bandit", "Draugr", "Dremora", ...
!race->IsChildRace()
!IsOnStoryCooldown(formId, storyCooldownHours)
!IsOnSocialCooldown(formId)              <-- the 24h social cooldown mirror
GetNPCLocationName(actor) != ""
location not in {"Marker Storage Unit", "TestTony"}
```

Per-actor it records: `formId`, `cellFormId`, display name + lowercase name, location string, archetype
(`ClassifyNPCArchetype`, `NPCIndex.cpp:1336-1390` — the CK class name uppercased, or `CIVILIAN`/`WARRIOR`/`MAGE`/
`ROGUE`), a cheap bio fallback line, sex, and `isFollower = false` **hardcoded** (`:1743`).

`NPCIndex::BuildNPCInteractionContextFromSnapshot` (`:1759-1912`) — **Phase B, worker thread:**

1. `MemoryDB::GetSociallyActiveFormIDs(20)` (`MemoryDB.cpp:937-972`) pulls SkyrimNet's `npcToNpcEventCount` per
   actor as a raw "social score".
2. Bucket by lowercased location string; drop buckets with `< 2` NPCs (`:1798`).
3. Score each group:

   ```text
   groupScore = npcs.size()
              + sum(socialScore) * 0.3
              + uniform_random[0.0, 1.0)          // deliberate jitter
              + 2.0  if any member shares the player's cell
   ```

   (`:1800-1811`)
4. Sort descending, truncate to `maxPairs` groups — **4** by default (`:1821-1825`; Papyrus passes 4 at
   `IntelEngine_StoryEngine.psc:817`).
5. Rebuild `m_npcCandidatePool` (lowercase name → FormID) used to resolve the LLM's chosen names (`:1828-1836`).
6. Emit markdown, **at most 3 NPCs per group** (`:1861`):

   ```text
   ## World State
   - Player: <name> at <location>
   - Time: <time of day>

   <political summary from FactionPolitics::BuildPoliticalSummary>

   ## NPC Groups by Location
   ### The Bannered Mare (player nearby: yes)
   - Mikael [BARD, Male] {bio summary}: <2 formatted memories>
     Knows: <up to 2 world-knowledge facts>.
   ...

   ## Story Type Picks This Session
   npc_interaction: 3, npc_gossip: 2
   ```

`Papyrus::BuildNPCInteractionRequestJsonCore` (`Papyrus.cpp:2031-2048`) wraps that into the request JSON:

```text
{ "npcPairPool": "<escaped markdown>",
  "preferredType": "npc_interaction" | "npc_gossip" | "",
  "player_at_inn": "0" | "1",
  "latest_witness_event": "<recent political event>" }
```

`preferredType` comes from `NPCIndex::GetPreferredNPCType()` (`:2732-2744`): if one type has been picked 2+ more
times than the other this session, the underrepresented one is forced. `player_at_inn` is a `LocTypeInn` keyword
check (`FactionPolitics.cpp:1144-1151`).

`BeginAsyncNPCDMTick` (`Papyrus.cpp:2063-2116`) implements the three-phase pattern: main-thread snapshot → worker
`AsyncDispatch::Submit` for the SQL and file-I/O heavy markdown build → `SKSE::GetTaskInterface()->AddTask` back to
main thread to call `OnNPCDMContextReady`. If the snapshot is empty it fires the callback inline with `""`.

### 3.4 Convergence — the AI package path

There is **no bespoke "converse" package**. The social dispatch reuses the generic travel machinery:

```papyrus
Core.RemoveAllPackages(npc, false)
PO3_SKSEFunctions.SetLinkedRef(npc, target as ObjectReference, Core.IntelEngine_TravelTarget)
ActorUtil.AddPackageOverride(npc, Core.TravelPackage_Jog, Core.PRIORITY_TRAVEL, 1)
Utility.Wait(0.1)
npc.EvaluatePackage()
```

(`IntelEngine_StoryEngine.psc:974-978`)

ESP forms involved (per `docs/ESP_STRUCTURE.md:73-155`):

- **Quest**: the single `IntelEngine` quest, with `AgentAlias00`-`AgentAlias04` (Optional / Allow Reuse) — 5 slots
  total, **shared** with all player-issued tasks (`MAX_SLOTS = 5`, `IntelEngine_Core.psc:101`).
- **Package**: `IntelEngine_TravelPackage_Jog` — a Travel package targeting "near linked reference" with the
  `IntelEngine_TravelTarget` keyword, radius 256.
- **Keyword**: `IntelEngine_TravelTarget` (linked-ref keyword).
- **Faction**: `IntelEngine_TaskFaction`, joined in `AllocateSlot` (`IntelEngine_Core.psc:588`).

Distances and timing constants:

| Constant | Value | Location |
| --- | --- | --- |
| `MONITOR_INTERVAL` | 3.0 s real time | `IntelEngine_StoryEngine.psc:36` |
| `ARRIVAL_DISTANCE` | 300 units | `IntelEngine_Core.psc:113` |
| `STUCK_DISTANCE_THRESHOLD` | 50 units | `IntelEngine_Core.psc:121` |
| `PRIORITY_TRAVEL` | 100 | `IntelEngine_Core.psc:104` |
| Social travel timeout | 4 game hours | `IntelEngine_StoryEngine.psc:1037` |
| Stuck-teleport offset | `TELEPORT_OFFSET_EXTERIOR` = 3500 units | `IntelEngine_StoryEngine.psc:143, 1022, 1032` |
| Timeout-teleport offset | 200 units | `IntelEngine_StoryEngine.psc:1039` |

`CheckNPCSocialArrival` logic (`:989-1042`), in order: abort if traveler or target dead/disabled/null → arrive if
distance ≤ 300 → abort if the slot vanished → if the traveler is not 3D-loaded, defer to
`Core.HandleOffScreenTravel` (time-estimate-based `MoveTo`) → else `IntelEngine.CheckStuckStatus`, where level 1
triggers `Core.SoftStuckRecovery` (random 100-unit nudge + re-apply package + `PathToReference`) and level ≥ 3
triggers a 3500-unit teleport behind the target → finally, past 4 game hours, teleport to 200 units and
force-arrive.

### 3.5 Rendering the exchange

`PerformVisibleInteraction` in full (`IntelEngine_StoryEngine.psc:1790-1802`):

```papyrus
npc1.SetLookAt(npc2)
npc2.SetLookAt(npc1)
Utility.Wait(0.5)
String summary = BuildInteractionSummary(npc1, eventText, npc2)
Core.SendTaskNarration(npc1, summary, npc2)
AddRecentStoryEvent(eventType + ": " + summary)
```

`BuildInteractionSummary` is `npc1 + " " + eventText + " with " + npc2` (`:5031-5033`), and `SendTaskNarration` is
a thin wrapper over `SkyrimNetApi.DirectNarration(msgText, akActor, akTarget)` (`IntelEngine_Core.psc:1460-1464`).

Per SkyrimNet's own API docs, `DirectNarration` registers "an event that forces the LLM to respond to a factual
event"; with an originator and a target set, "the speaker will address that specific target"
(`SkyrimNet-GamePlugin/Source/Scripts/SkyrimNetApi.psc:176-200`). So IntelEngine issues **exactly one** narration
event and hands off. **Turn count is one, from IntelEngine's side.** Whether SkyrimNet then generates a reply from
npc2 is SkyrimNet's own NPC-to-NPC continuation behaviour and is not controlled, configured, or awaited by
IntelEngine anywhere — I could not determine it from these repos.

### 3.6 Consequences and persisted state

**Written on every accepted decision** (`IntelEngine_StoryEngine.psc:891-924`), regardless of what happens
physically:

- `Core.SendPersistentMemory(npc1, npc2, narration)` → `SkyrimNetApi.RegisterPersistentEvent` — goes into
  SkyrimNet's own event database, queryable by future DM ticks.
- `npc_interaction`: `Core.InjectFact(npc1, fact1)` and `InjectFact(npc2, fact2)`
  (`IntelEngine_Core.psc:1487-1525`) — `StringList` `Intel_Facts` + `FloatList` `Intel_FactTimes` on each actor,
  **FIFO cap 10, no time expiry**, plus a pre-rendered `Intel_FactsRendered` string value (because
  `papyrus_util("GetStringList")` cannot see lists created during the current session).
- `npc_gossip`: `Core.InjectGossip(npc1, npc2, gossip)` (`IntelEngine_Core.psc:1533-1596`) — writes
  `Intel_GossipHeard` / `Intel_GossipHeardFrom` / `Intel_GossipHeardTimes` on the receiver and
  `Intel_GossipTold` / `Intel_GossipToldTo` / `Intel_GossipToldTimes` on the giver, **cap 5 each**, plus
  `Intel_GossipRendered` on both.
- Social cooldown: `Intel_NPCSocialLastPicked` (float, game days) on each actor, plus the actor appended to the
  quest-scoped `Intel_SocialCooldownActors` FormList (`:1111-1119`).
- Dashboard ring buffer on the player: `Intel_SocialLog_Type` / `_NPC1` / `_NPC2` / `_Text` / `_Location` /
  `_Detail`, capped at **5** (`:4987-5024`).
- Anti-repetition ring buffer on the player: `Intel_RecentStoryEvents`, capped at **8** (`:4979-4985`).

**Consumed by the LLM later** via SkyrimNet bio submodules:

- `submodules/character_bio/0800_intel_facts.prompt` renders `## Known Facts` with fuzzy time refs ("just now",
  "a few days ago").
- `submodules/character_bio/0200_intel_gossip.prompt` renders `## Rumors Heard` and `## Rumors Shared`.
- `submodules/character_bio/0801_intel_task_awareness.prompt` renders the in-flight task; `SlotTracker.cpp:259`
  maps the `npc_social` task type to the phrase **"going to talk with \<target\>"**.

**Volatile C++ mirrors** (rebuilt each game load, not co-saved):

- `NPCIndex::m_socialCooldowns` + `m_socialCooldownHours` (`NPCIndex.cpp:2961-2989`), warmed from StorageUtil by
  `WarmCooldownMirror()` on load (`IntelEngine_StoryEngine.psc:1148-1170`).
- `NPCIndex::m_npcCandidatePool` (name → FormID), rebuilt every tick.
- `NPCIndex::m_storyTypeCounts`, warmed from `Intel_RecentStoryEvents` by `WarmStoryTypeCounts()`.

**Save/load behaviour.** An in-flight social travel dispatch is *abandoned* on load — `IsNPCStoryActive` triggers
`CleanupNPCSocialDispatch()` with the comment "packages lost, travel state unrecoverable"
(`IntelEngine_StoryEngine.psc:322-326`). All the memory state survives, because it lives in StorageUtil and
SkyrimNet's database.

### 3.7 Is an interaction between two off-screen NPCs simulated?

Partially, and less than the README implies:

- The candidate pool only ever contains **loaded** actors (all four process tiers, `ProcessUtils.h:20-38`). NPCs in
  a different hold are not candidates. There is no world-wide background social simulation.
- Within that pool, a pair where **neither** NPC is in the player's cell produces **memory writes and a log line
  only** — no travel, no narration, no dialogue (`IntelEngine_StoryEngine.psc:942-947`). Both NPCs will
  nonetheless talk about it later, because the facts are in their bios.
- The one genuinely long-range mechanic is the gossip chain: `NPCIndex::GetRelatedCandidate`
  (`NPCIndex.cpp:1168-1210`) resolves candidates out of SkyrimNet's memory DB by FormID and only requires
  `IsEligibleStoryCandidate` (`:1014-1027`), which excludes the player's own cell but not distance — so a rumour
  can propagate to NPCs anywhere in the world.

### 3.8 Manual dispatch (Director tab)

The PrismaUI dashboard exposes a manual override: `DirectorTab.jsx:261-275, 359-369` posts
`dispatchNpcSocial` → `DashboardUIManager::OnDispatchNpcSocialStatic` (`DashboardUIManager.cpp:657-694`) → ModEvent
`IntelEngine_DashboardDispatchNpcSocial` → `IntelEngine_Core.psc:2329-2382`. This path is a *different*, less
careful reimplementation of the DM path — see findings W12.

### 3.9 MCM / settings surface

`IntelEngine_MCM.psc:502-527` under the header "NPC Social Life": Enable NPC Interactions, NPC Interaction toggle,
NPC Gossip toggle, NPC Interaction Interval (hours), NPC Social Cooldown (hours). The dashboard Settings tab mirrors
these (`SettingsTab.jsx:167-198`). There are **no** SkyrimNet action YAMLs for this feature — it is entirely DM-tick
driven, never LLM-tool-invoked (the 13 YAMLs in `config/actions/` are all player-facing task actions).

---

## 4. Weaknesses and bugs

Each item is marked **[confirmed]** (read directly in the code) or **[suspected]** (inferred; would need runtime
testing to prove).

### W1. "Whispered conspiracies" is unreachable dead prompt text — [confirmed]

Prompt rule 11 (`intel_story_npc_dm.prompt:93`) is the only place the README's "whispered conspiracies" idea is
expressed, and it triggers on NPCs "marked `(follower)`". But:

- `BuildNPCTickSnapshot` rejects every follower: `if (actor->IsPlayerTeammate()) return false;`
  (`NPCIndex.cpp:1713`), with the comment "Exclude active followers — they're with the player, not independently
  socializing."
- `a.isFollower = false; // already filtered followers above` (`NPCIndex.cpp:1743`).
- The only consumer, `if (a->isFollower) md += " (follower)";` (`NPCIndex.cpp:1868`), can therefore never emit the
  marker.
- Belt and braces: the travel branch also refuses followers
  (`IntelEngine_StoryEngine.psc:935`).

Two followers can never be picked, so the conspiratorial-whisper framing never fires. This is the single most
significant gap between the advertised feature and the code.

### W2. Player-configured blocklists and whitelists are silently ignored — [confirmed]

The Story DM funnels every candidate through `PassesCommonEligibility` (`NPCIndex.cpp:929-1011`), which applies the
faction blocklist, NPC-name blocklist, faction whitelist, NPC whitelist, and the danger-zone / player-home
policies. `BuildNPCTickSnapshot` (`NPCIndex.cpp:1703-1731`) **does not call it** — it reimplements a subset of the
checks inline and omits all six of those. A player who puts `Nazeem` in `story.npc_blocklist` (documented in
`manifest.yaml` as "Blocked NPCs will never be picked by the Dungeon Master") will still get Nazeem in
`npc_interaction`.

### W3. Memories are committed before the interaction happens — [confirmed]

`OnNPCInteractionResponse` writes the persistent event, both facts (or the gossip), the dashboard log, **and both
24-hour cooldowns** at `:891-924`, *before* the visibility branch at `:927`. If the traveler then fails to arrive —
no free slot (`:956-963`), stuck, killed, target moves, save reloaded (`:322-326`) — both NPCs still permanently
remember an argument that never occurred, and both are locked out of the system for a day. There is no
compensating rollback anywhere.

### W4. "Both visible" fires with no distance check at all — [confirmed]

The instant-interaction branch tests only cell membership:

```papyrus
Bool npc1Visible = (playerCell != None && npc1.GetParentCell() == playerCell)
Bool npc2Visible = (playerCell != None && npc2.GetParentCell() == playerCell)
If npc1Visible && npc2Visible
    PerformVisibleInteraction(npc1, npc2, narration, storyType)
```

(`IntelEngine_StoryEngine.psc:928-934`)

In a large interior (Dragonsreach, the Palace of the Kings) or a big exterior cell, two NPCs several thousand units
apart with walls between them will `SetLookAt` each other through geometry and fire a narration as if they were
face to face. No convergence is even attempted in this branch.

### W5. Nothing checks that the player can actually perceive the arrival narration — [confirmed gap]

`OnNPCSocialArrived` (`:1055-1060`) fires `DirectNarration` unconditionally. The dispatch was justified by *one*
NPC being in the player's cell at decision time, but the walk can take up to 4 game hours and the player is free to
leave. There is no re-check of player cell, distance, or 3D-loaded state before the narration. IntelEngine may well
be paying for an LLM dialogue generation for an exchange nobody hears. Whether SkyrimNet independently suppresses
out-of-earshot narrations is **[suspected, undetermined]** — nothing in these repos settles it.

### W6. No linger and no building access on the social arrival path — [confirmed]

The player-centric story path ends in `FinishArrivalWithLinger` (`:1959-1977`), which applies a sandbox package so
the NPC stays put during the conversation, calls `Core.EnsureBuildingAccess` to unlock the interior door so the
player can follow, and registers the NPC for linger cleanup. The social path does none of that:
`OnNPCSocialArrived` → `PerformVisibleInteraction` → `CleanupNPCSocialDispatch` → `Core.ClearSlot` →
`RemoveAllPackages` — all in the same call. The traveler is handed back to its vanilla schedule package in the same
frame the narration is issued, so it will typically walk away while the line is still being generated/spoken.
`EnsureBuildingAccess` is never called on this path (`grep` shows call sites only at `Travel.psc:609`,
`StoryEngine.psc:1972`, `StoryEngine.psc:3583`), despite its own docstring claiming "Called at all arrival points
(Travel, StoryEngine, NPC Social)" (`IntelEngine_Core.psc:1196-1199`).

### W7. `RemoveAllPackages` nukes other mods' package overrides — [confirmed]

`DispatchNPCSocial` (`:974`) and `TeleportNPCSocialAndResume` (`:1048`) call `Core.RemoveAllPackages`, which is
`ActorUtil.ClearPackageOverride(akActor)` — the function's own comment warns "This strips packages from ALL mods.
Only use on task agents that IntelEngine fully owns" (`IntelEngine_Core.psc:1261-1266`). A sibling function
`RemoveIntelPackages` exists for exactly this reason and is not used here. Any SkyrimNet or third-party package
override on the chosen NPC is destroyed and never restored.

### W8. `SetLookAt` is never cleared — [confirmed]

`ClearLookAt` does not appear anywhere in IntelEngine's Papyrus (grep across `Source/Scripts` returns 13
`SetLookAt` call sites and zero `ClearLookAt`). The two interaction participants are left head-tracking each other
indefinitely. Cosmetic, but it is a leak.

### W9. A gossip chain wipes the anti-repetition buffer — [confirmed]

`SpreadGossipOffScreen` calls `AddRecentStoryEvent` once per hop, up to 10 times (`:1649-1657`).
`AddRecentStoryEvent` caps `Intel_RecentStoryEvents` at 8 entries (`:4982-4984`). A single long chain therefore
evicts every other story type from the buffer. That buffer is what `WarmStoryTypeCounts()` reads on load to
reseed `m_storyTypeCounts`, and what feeds the Story DM's variety enforcement — so one gossip event can distort
type balancing for the rest of the session.

### W10. The gossip chain does up to ten synchronous cross-DLL SQL calls on the Papyrus thread — [suspected]

Each loop iteration calls `IntelEngine.GetRelatedCandidate` (`:1653`), which does
`MemoryDB::GetRelatedCandidateFormIDs` → `SkyrimNetAPI::GetRelatedActors` (JSON over the SkyrimNet DLL boundary)
plus `GetRecentPlayerInteractionNames` → `GetPlayerContext` (`NPCIndex.cpp:1176-1206`). Ten iterations means up to
twenty synchronous queries inside a Papyrus `While` loop, on the main-thread-latency path. The rest of the DM tick
was explicitly moved off-thread for exactly this reason (`BeginAsyncNPCDMTick`, "Eliminates ~20-50ms main-thread
stutter from synchronous SQL + actor scans", `IntelEngine_StoryEngine.psc:814-816`) — the gossip chain never got
that treatment.

### W11. The gossip chain ignores the social cooldown — [confirmed]

`GetRelatedCandidate` → `IsEligibleStoryCandidate` (`NPCIndex.cpp:1014-1027`) checks the *story* cooldown via
`PassesCommonEligibility` but never `IsOnSocialCooldown`. An NPC who just had an interaction can be handed three
rumours in the same chain.

### W12. The Director (dashboard) path is an inconsistent parallel implementation — [confirmed]

`IntelEngine_Core.psc:2329-2382`:

- For `npc_gossip` it calls `InjectFact` instead of `InjectGossip` (`:2366-2367`), so the gossip never reaches the
  `Intel_Gossip*` lists or the "Rumors Heard/Shared" bio section, and the injected wording is malformed:
  `"told <npc2> that someone " + gossip` produces e.g. *"told Sven that someone was overheard arguing with the
  steward"*.
- It never stamps `Intel_NPCSocialLastPicked`, so a manual dispatch does not consume a cooldown.
- It uses `IntelEngine.FindNPCByName` rather than `ResolveStoryCandidate`, skipping the candidate-pool exact match.
- Its distance gate is `dist < 5000.0 && dist > 200.0` (`:2374-2375`). If the two NPCs are **already within 200
  units** — the ideal case — it logs "too far apart or already close" and does **nothing**: no travel *and* no
  `DirectNarration`. The interaction exists only as injected facts.

### W13. Dead code left behind from an earlier architecture — [confirmed]

`ActiveStoryType` is never assigned `"npc_interaction"` or `"npc_gossip"` anywhere (all assignment sites:
`IntelEngine_StoryEngine.psc:1588, 1616, 2615, 2706, 2907, 3284, 3421`, plus clears), and
`intel_story_dm.prompt` contains no mention of either type. Therefore `IsNPCToNPCType()` (`:713-716`) always returns
false, and the following are unreachable:

- the NPC-to-NPC guard in `DispatchToTarget`'s player-home knock prompt (`:1677`)
- `DispatchToTarget`'s `target != player` handling and `ActiveSecondNPC` assignment (`:1716-1728`)
- the `arrivalTarget = ActiveSecondNPC` branch (`:2354-2355`)
- the NPC-to-NPC face-each-other-and-narrate branch in `CheckStoryNPCArrival` (`:2541-2546`)
- the NPC-to-NPC linger target selection (`:2560-2567`) and `:4953-4954`

Two further functions are defined and never called anywhere in either repo: `WarmCooldownsForPool()` (`:1253-1290`)
and `WarmSocialCooldownsForPool()` (`:1292-1323`).

### W14. Type-balance counters are incremented before validation — [confirmed]

`IntelEngine.NotifyStoryTypePicked(storyType)` runs at `:871`, *before* `ResolveStoryCandidate` (`:873`) and before
the cooldown check (`:883`). A pick that is then thrown away for an unresolvable name or a cooling-down NPC still
increments `m_storyTypeCounts`, which drives both `GetPreferredNPCType()` and the "## Story Type Picks This
Session" block shown back to the model. The model is told about interactions that never happened.

### W15. `maxPairs` is not a pair count, and the emitted NPCs are arbitrary — [confirmed]

`BuildNPCTickSnapshot(int maxPairs)` / `groups.resize(snap.maxPairs)` (`NPCIndex.cpp:1676, 1823-1825`) truncates
*location groups*, not pairs. With the default 4 and up to 3 NPCs emitted per group (`:1861`), the model can see up
to 12 NPCs. Separately, the `socialScore` computed at `:1778-1782` is used only in the group score — the three NPCs
actually written into the markdown are `group.npcs[0..2]` in unsorted `std::unordered_map` bucket order (`:1862`),
so the most socially active NPCs in a crowded inn are as likely to be dropped as included.

### W16. Location grouping is by cell/location *name* — [confirmed mechanism, suspected impact]

`GetNPCLocationName` (`NPCIndex.cpp:1037-1058`) returns the cell name, falling back to
`GetCurrentLocation()->GetFullName()`, falling back to the editor location. For exteriors this typically collapses
to a settlement name, so every NPC anywhere in Whiterun's exterior groups together and the prompt asserts they
"share a physical location". Combined with W4 (no distance check when both are in the player's cell) and the
`ARRIVAL_DISTANCE` of 300 units, the physical plausibility of a given pairing is unverified at every stage.

### W17. The social teleport always uses the *exterior* offset — [confirmed constant misuse]

`TeleportNPCSocialAndResume` is only ever called with `TELEPORT_OFFSET_EXTERIOR` (3500 units) or 200
(`:1022, 1032, 1039`). The sibling `ImmersiveTeleportToTarget` used by the story path correctly branches on
`targetCell.IsInterior()` and uses `TELEPORT_OFFSET_INTERIOR` (500) indoors (`:2503-2514`). A stuck social traveler
whose target is inside an inn gets `MoveTo`'d 3500 units from that target — very likely outside the interior's
geometry. **[suspected]** consequence: NPC dropped into void space or clamped somewhere nonsensical, then
re-pathing from there.

### W18. LLM-returned strings are never sanitized — [confirmed]

`ExtractJsonField` (`IntelEngine_StoryEngine.psc:5117-5119`) is `IntelEngine.StoryResponseGetField`, which returns
the raw `nlohmann::json` string value with no character normalisation (`Papyrus.cpp:1861-1949`; it only strips
markdown code fences and lowercases key names). The resulting `narration`, `fact1`, `fact2`, and `gossip` go
straight into `StorageUtil` string lists, into `DirectNarration`, into the pre-rendered bio strings, and back into
subsequent prompts. IntelEngine does have sanitizers (`MemoryDB::SanitizeForPrompt`,
`NPCIndex.cpp:2568 SanitizeForHistory`) but neither is applied on this path. Smart quotes, em-dashes and NBSPs from
the model land unmodified in engine-facing text fields.

### W19. Unbounded per-save state — [confirmed]

`Intel_FactNPCs` (an `IntList` on the player) gains a FormID for every NPC that ever receives a fact
(`IntelEngine_Core.psc:1518-1522`). The only removal is in `CleanExpiredFacts`, which removes an entry when that
NPC's fact list is empty (`:1607-1615`) — but facts have no expiry, they are pure FIFO capped at 10
(`:1488-1489, 1498-1504`). Once an NPC has one fact it stays in the registry forever, and
`CleanExpiredFactsGlobal()` sweeps the whole list on every game load.

### W20. Concurrency: one social dispatch, five shared slots — [confirmed design limit]

`IsNPCStoryActive` allows exactly one social traveler at a time (`:935`), and that traveler consumes one of the 5
`AgentAlias` slots shared with every player-issued fetch/deliver/escort/schedule task
(`Core.FindFreeAgentSlot`, `:956`). During busy play the social dispatch will routinely find no free slot and
silently degrade to the off-screen fact-only path (`:956-963`) — after the memories and cooldowns have already been
committed (W3).

### W21. Cosmetic: the narration summary double-names the second NPC — [confirmed]

The prompt's own examples put npc2's name inside the narration:

```text
"narration":"confronted Name2 about a stolen shipment"
```

(`intel_story_npc_dm.prompt:104`)

and `BuildInteractionSummary` appends `" with " + npc2` (`:5031-5033`), producing
*"Mikael confronted Sven about a stolen shipment with Sven"* as the text handed to `DirectNarration`. Meanwhile the
persistent event registered at `:891` uses the bare narration with **no** subject at all — *"confronted Sven about
a stolen shipment"* — so the two records of the same event disagree about who did what.

### W22. Magic numbers chosen by feel — [confirmed, no justification found in code or docs]

`socialScore * 0.3`, the `+2.0` player-nearby bonus, the `[0,1)` random jitter, `maxPairs = 4`, 3 NPCs per group,
2 memories and 2 world-knowledge entries per NPC, the diff-of-2 threshold in `GetPreferredNPCType`, the 700-char
response budget, the 24-hour social cooldown, the 1.5-hour tick, the 4-hour travel timeout, and the
`dist < 5000 && dist > 200` Director gate all appear without derivation or comment anywhere in the repos. The
700-character limit in particular is asserted to the model three times ("The system truncates longer responses,
breaking parsing") but I found **no truncation code** in `Papyrus.cpp` implementing it — **[suspected]** that
constraint is either enforced inside SkyrimNet or is stale prompt lore.
