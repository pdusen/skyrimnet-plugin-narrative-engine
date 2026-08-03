# IntelEngine — Offer Quests (bounty hunt, rescue, item retrieval, faction war battle)

> **Reading discipline.** This documents one feature of **IntelEngine**, an abandoned SkyrimNet plugin, as prior art.
> It is a record of what one author built and how it broke — not a design NarrativeEngine should mirror. Every claim
> below cites a file and line so it can be verified. See [`../README.md`](../README.md) for the full discipline.

The README bullet this covers:

> - Offer quests — bounty hunts, rescue missions, item retrieval, and faction war battles

Those four player-facing categories are implemented as **six internal sub-types** of a single `quest` story type:

| README category    | Internal `questSubType`    | Notes                                                       |
| ------------------ | -------------------------- | ----------------------------------------------------------- |
| Bounty hunt        | `combat`, `faction_combat` | Kill spawned enemies at a location                          |
| Rescue mission     | `rescue`, `faction_rescue` | Free a real named NPC held by spawned enemies               |
| Item retrieval     | `find_item`                | Take a specific item from a spawned chest guarded by a boss |
| Faction war battle | `faction_battle`           | Join a full faction battle (delegates to the battle system) |

`faction_combat` and `faction_rescue` are **normalized away** immediately after validation —
`IntelEngine_StoryEngine.psc:3244-3248` rewrites them to `combat` / `rescue` and keeps only `QuestAlliedFaction` set.
Everything downstream sees three real code paths (`combat`, `rescue`, `find_item`) plus the special-cased
`faction_battle`.

**All of it is DM-driven. There is no player-facing "ask for a quest" action.** The action YAML folder
(`IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/config/actions/`) contains 13 actions — travel, fetch, deliver,
escort, search, schedule, cancel, change speed, report conduct — and none of them is a quest action. The only ways a
quest starts are the autonomous Story DM tick and the dashboard's manual Director dispatch.

---

## 1. High-level overview, step by step

### Step 0 — the tick that can produce a quest

`IntelEngine_StoryEngine.TickScheduler()` (`IntelEngine_StoryEngine.psc:593`) runs off a shared game-time timer. It
self-gates on the Story DM interval — `Intel_MCM_StoryInterval`, **default 3 game hours**
(`IntelEngine_Core.psc:1696-1698`) — and only proceeds when `!IsActive` and the player is not in combat
(`:667-669`).

It then calls `IntelEngine.BeginAsyncStoryDMTick(7, LongAbsenceDaysConfig, excludeList, …)` (`:699`) — **7 candidate
NPCs**, context built on a worker thread, callback `OnStoryDMContextReady` → `SendCustomPromptToLLM("intel_story_dm",
…)`.

The exclude list is a bitmask of MCM toggles packed in Papyrus (`:718-781`) and expanded in C++
(`FactionPolitics::BuildExcludeList`). Bit 6 is `quest`; bits 8–13 are the six sub-types. Disabled types are stamped
into the prompt as `show_quest_rescue = "0"` etc. (`Papyrus.cpp:2498-2530`), so the LLM literally never sees the
section for a disabled sub-type.

Faction sub-types are additionally gated on political state: the prompt only renders `faction_combat` /
`faction_rescue` / `faction_battle` if `hostile_factions` / `friendly_factions` are non-empty
(`intel_story_dm.prompt:138-166`), and those strings are built from **player standing ≥ 40 (friendly) and ≤ −40
(hostile, or "enemy of a friendly faction")** in `Papyrus.cpp:2544-2602`. (Note: `FEATURE_OVERVIEW.md` says faction
quests unlock at standing 20+; the shipped threshold is 40.)

### Step 1 — the DM picks a quest

The LLM returns a single JSON object under a hard **700-character budget**, narration < 120 chars, `msgContent` < 150
(`intel_story_dm.prompt:239`). A quest response looks like:

```json
{"should_act":true,"type":"quest","npc":"Courier","narration":"pleaded for help finding a kidnapped friend",
 "sender":"QuestGiver","msgContent":"Bandits took Camilla!","questLocation":"Fort Greymoor","enemyType":"bandit",
 "questSubType":"rescue","victimName":"Camilla Valerius"}
```

The prompt tells the DM to choose one of **two delivery modes** for non-faction sub-types
(`intel_story_dm.prompt:111-114`):

- **COURIER** — `npc` = messenger, `sender` = quest giver. Mandatory for Jarls, stewards, court wizards, housecarls.
- **DIRECT** — `npc` = quest giver, `sender` = `""`. Only for "common people who would realistically travel."

Faction sub-types must leave **both** `npc` and `sender` empty; the system picks the courier itself.

### Step 2 — validation gauntlet

`OnDungeonMasterResponse` (`:1364`) then `HandleQuestDispatch` (`:3023`) run a long series of rejects. In order:

1. Generic story-response checks: `should_act`, `type`, NPC resolves via `ResolveStoryCandidate`, high-status/Jarl
   filter (quest is **exempt** from the high-status block because courier mode exists), hold restriction, per-NPC
   story cooldown (default 24 game hours), C++ `ValidateStoryResponse` (`FactionPolitics.cpp:2110`) which whitelists
   the six sub-types and enforces per-sub-type required fields.
2. `QuestActive` — **only one IntelEngine quest can exist at a time** (`:1553-1558`, again at `:3024`).
3. Jarl in DIRECT mode → reject (`:3045-3049`).
4. Missing `questLocation`, or missing `enemyType` for anything but `faction_battle` → reject (`:3051`).
5. **Rescue victim validation** (`:3056-3088`): `FindNPCByName` must resolve; not dead/disabled; not the courier;
   not the sender; **not a player teammate** (follower framework overrides the bleedout state); not on story
   cooldown. Missing `victimName` → reject.
6. **find_item validation** (`:3090-3101`): `ValidateQuestItem` (ItemIndex fuzzy) — on failure, silently substitutes
   `GetRandomQuestItemName(500)` (a random ≥500-gold item, excluding recent ones).
7. **Faction courier selection** (`:3103-3133`): `FindFactionMember(alliedFaction)` picks a loaded member, saves and
   zeroes their Aggression so nobody attacks the courier, skips the cooldown check ("faction couriers are
   generic/interchangeable soldiers"), and rewrites the narration via `BuildFactionQuestNarration` (one of four
   canned strings, `:5043`).
8. `ResolveAnyDestination(npc, questLocation)` must return a reference (`:3135`).
9. `IsLocationNonCombative(questDest)` → reject temples and a hardcoded sacred-site list
   (`CellAnalyzer.cpp:563-592`: High Hrothgar, Sky Haven Temple, Eldergleam Sanctuary, Ancestor Glade, Blue Palace,
   Palace of the Kings, Mistveil Keep, Understone Keep, Dragonsreach).
10. `Core.FindFreeAgentSlot() < 0` → reject **before** any quest state is written (an explicitly-commented fix for
    an orphaned-quest bug, `:3145-3150`).
11. `faction_battle` only: `ValidateFactionBattleDispatch` (`BattleManager.cpp:1045`) resolves the enemy faction
    (exact match → Levenshtein ≤ 2 fuzzy → configured war enemy), rejects if no enemy or a battle is already
    running; Papyrus separately checks `Core.Battle.BattleScheduled`.

### Step 3 — quest state is committed and a courier is dispatched

`:3221-3285` sets ~20 `Auto Hidden` properties on the quest script (all save-persisted), injects "asked X for help"
facts into the quest giver (and a separate "was sent to deliver a plea" fact into the courier when they differ),
injects "was captured by … and held against my will" into the rescue victim, records the location/item/victim in the
C++ anti-repetition ring buffers, pushes the whole thing to `QuestStateTracker` for the SkyrimNet decorator, and
finally calls `DispatchToTarget(npc, player, narration, "story")`.

Faction quests additionally call `IntelEngine.RemovePlayerCrimeFactions()` here (`:3238-3243`), intended to give the
player bounty immunity for the whole quest so stray hits during a battle don't create a bounty. See §4 — the FormID
table behind it is wrong.

`DispatchToTarget` (`:1673`) allocates one of the 5 agent slots, applies `TravelPackage_Jog` at priority 100 via a
linked ref, arms the 150 ms C++ proximity monitor, and starts stuck/off-screen tracking. The courier now physically
walks to the player.

### Step 4 — the offer reaches the player

On arrival, `OnQuestNPCArrived` (`:3288`) fires:

1. Narrates through SkyrimNet ("delivered word from X: <msgContent>").
2. Pins the NPC in place with a priority-100 sandbox so SkyrimNet's own `TalkToPlayer` can't walk them off, then
   **waits 8 real seconds** so the LLM conversation has time to happen, showing
   `Debug.Notification("They seem to have more to ask...")` (`:3316-3324`).
3. Shows a `SkyMessage` menu. Direct, non-follower, non-faction givers get three options
   — **"Lead the way" / "I'll go alone" / "Not interested"**. Everyone else gets two —
   **"I'll check it out" / "Not interested"** (`:3362-3374`).

Prompt text is per sub-type (`:3347-3360`), e.g. *"Ysolda pleads for help rescuing Camilla Valerius near Fort
Greymoor."*, *"…tells you about a legendary glass blade near Ansilvund."*, *"…rallies you to fight alongside the
Imperial Legion near Karthwasten."*

- **Not interested** → inject "learned that <player> refused to help…" into the quest giver, hide the objective,
  `CleanupQuest()`, NPC lingers and then wanders off.
- **I'll go alone** → notification `Quest: <msgContent> [<location>]`, pre-place targets, place the marker, NPC
  lingers.
- **Lead the way** → same, then `BeginQuestGuide` — the NPC jogs to the location *with* the player, stopping to wait
  when the player falls more than 2000 units behind and resuming at 800
  (`QUEST_GUIDE_WAIT_DIST` / `QUEST_GUIDE_RESUME_DIST`, `:137-138`), with a 5-game-hour guide timeout
  (`QUEST_GUIDE_TIMEOUT_HOURS`), stuck recovery, and off-screen teleport handling (`CheckQuestGuide`, `:3449`).

### Step 5 — target pre-placement

`PrePlaceQuestTargets` (`:3592`) runs at acceptance. It asks `GetDungeonBossAnchor(questLocationName)` for a
pre-indexed boss-room reference (`LocationResolver.cpp:1287`, built at startup by scanning every dungeon-keyworded
`BGSLocation`'s `specialRefs` for a Boss/BossContainer LocRefType inside an interior cell). If one exists:

- **rescue** — victim is `MoveTo`'d to the anchor, stripped of packages, pinned with a sandbox override, made
  Essential (original flag saved to `Intel_WasEssential`), given `SetNoBleedoutRecovery(true)` and then
  `DamageActorValue("Health", currentHealth + 100)` — the engine's native bleedout kneel state.
- **find_item** — `SpawnQuestChest(anchor, itemName)` places a `TreasBossBanditChest` and adds the item.
- **combat** — `SpawnQuestBoss(anchor, enemyType)` places the boss (which is also the compass target).

Regular enemies are **deliberately not** spawned yet: "they need a loaded cell for AI init" (`:3665`).
`faction_battle` skips pre-placement entirely (`:3601-3603`).

If there is no indexed anchor, the quest falls into a **deferred** path and enemies are placed later by scanning
whatever cell the player walks into.

### Step 6 — the map marker

`PlaceQuestMarker` (`:4751`) points the `QuestTarget` reference alias at the best-known target (victim → chest →
boss → the exterior location), calls `SetActive(true)`, `SetObjectiveDisplayed(0, true)` and `SetStage(100)` (whose
CK fragment redundantly re-displays objective 0). The alias is re-pointed and the objective flickered
off/on whenever the real target is later created deeper in the dungeon (`:3990-3995`, `:4233-4239`, `:4306-4312`).

### Step 7 — arrival, spawning, and the four-layer proximity test

`CheckQuestProximity` (`:3676`) runs on the 3-second real-time monitor. If enemies aren't spawned yet:

- **Pre-placed quests** wait for `QuestBossAnchor.Is3DLoaded()` (or a "target nearby" safety check: player within
  4000 units of the quest location *and* within 2000 of the victim/boss). Then it spawns the regular enemies at the
  anchor — `DisableNoWait()` → `MoveTo(anchor, ±300, ±300)` → `EnableNoWait()`, so the player never sees pop-in —
  re-applies the victim's bleedout, and re-adds the chest item (containers created in unloaded cells don't sync to
  the inventory UI, `Papyrus.cpp:3949-3954`).
- **Deferred quests** run a four-layer "is the player at the quest area" test (`:3811-3882`): (1) marker 3D-loaded
  *and* within 4000 units; (2) same parent cell; (3) same `BGSLocation` *and* within 6000 units; (4) "we previously
  saw a dungeon entrance and the player is now in an interior." A guard blocks spawning while the player is in a
  safe interior — otherwise "Is3DLoaded on exterior markers can return true from inside a nearby building, which
  would spawn bandits in The Bannered Mare when the quest is at Nilheim" (`:3798-3801`).

Inside a dungeon, the system tracks door-transition depth and calls `ScanAheadForAnchor(player)`
(`Papyrus.cpp:3859`) — it follows doors into adjacent interior cells and looks for prisoner furniture (shackle,
manacle, cage, gibbet, stocks, pillory, prison, captive, torture) and then dungeon landmarks (word wall > boss chest
> chest > coffin/sarcophagus > shrine/altar). The anchor is always at least one door ahead, so placement is
invisible. After **depth ≥ 3 or 5 failed scans**, it gives up and spawns near the player (`:4051-4055`).

### Step 8 — completion detection (per type)

| Sub-type         | Completion condition                                                                     | Code            |
| ---------------- | ---------------------------------------------------------------------------------------- | --------------- |
| `combat`         | `AreAllQuestEnemiesDead()` — every actor in the `Intel_QuestSpawnedNPCs` FormList is dead, deleted, disabled or `None` | `:4154`, `:4336` |
| `rescue`         | All quest enemies dead **and** `IsAreaClearOfHostiles()` (no hostile within 3000 units of the victim); *or* player walks within 200 units of the victim while not in combat | `:4138-4150` |
| `find_item`      | The named item is no longer in the spawned chest (`IsQuestItemInChest` false)             | `:4084-4089`    |
| `faction_battle` | Battle system reports not-active and not-scheduled, after `BATTLE_MIN_START_DELAY`        | `:4077-4081`    |

Rescue also has a **failure** state: if the victim dies, `OnQuestFailed()` fires (`:4135`, `:4551`).

### Step 9 — rewards

There is **no gold or item reward**. Grepping the Papyrus sources for `AddItem` / `Gold001` / `RewardPlayer` returns
nothing. What the player actually gets:

- **Narrative reward** — `Core.InjectFact(QuestGiver, "learned that <player> …")`, plus a fact to the guide NPC if
  different, plus, for rescue, a fact + `Intel_RescueNarration` on the victim keyed to how well they knew the player
  beforehand (`GetPlayerInteractionCount`: stranger / seen-before / acquainted, three distinct texts, `:4424-4438`).
- **A companion to talk to** — the freed victim gets `TravelPackage_Walk` toward the player and enters the linger
  system so they walk up and speak (`:4457-4468`).
- **Faction standing** — `+5` with the allied faction and `−5` with the enemy faction
  (`QUEST_STANDING_REWARD` / `QUEST_STANDING_PENALTY`, `:180-181`), plus one of three randomized flavour
  notifications ("Your name carries weight among the …"). `faction_battle` delegates standing to the battle system.
- **The item itself** (find_item) and whatever the dungeon holds.
- **Partial credit** — half standing (`5 / 2 = 2`) when spawning failed three times (`:4313-4330`), when the battle
  system was busy (`:3917-3921`), or when a scheduled battle was lost across a save reload (`:410-424`).

### Step 10 — expiry, failure, cleanup

`CheckQuestExpiry` (`:4567`) warns at 75 % of `QUEST_EXPIRY_DAYS` ("You should hurry — the situation won't hold much
longer.") and expires at 100 % (**default 1.0 game day**, MCM-tunable). Expiry injects a disappointment fact tuned
per sub-type, cancels a pending battle, and calls `CleanupQuest`.

`CleanupQuest` (`:4610`) hides the objective, clears the spawned-NPC FormList, clears the courier's slot/packages
and StorageUtil keys, un-restrains and heals the victim (injecting "managed to escape … on my own" if the player
never freed them) and `MoveToMyEditorLocation()`s them, deletes the chest, restores the player's crime factions, and
resets ~25 properties.

---

## 2. Intended gameplay experience

Read from the prompt text, the tuning values, and the code comments, the author was chasing five things:

**A world that hands you work instead of waiting on a quest board.** The prompt frames the LLM as *"the Dungeon
Master for {{playerName}}'s world"* whose job is to *"create meaningful story moments that make the world feel
alive"* (`intel_story_dm.prompt:11`). Quests aren't picked up; they arrive on foot. A named NPC or a courier
physically walks across Skyrim, has an LLM conversation with you, and only then does the menu appear.

**Scarcity over volume.** Rule 3 is blunt: *"You do NOT have to dispatch every tick. Rejecting is the NORMAL
outcome… Hours of silence followed by one perfect moment is the goal."* The one-active-quest cap, the 3-hour tick,
the 24-hour per-NPC cooldown, and the six-entry dispatch-history rotation rules all push the same way. Rule 14
explicitly asks the world to *"rotate through emotional registers — a tense quest, a quiet seek_player, a pulpy
road_encounter… not five quests in a row."*

**Personal stakes, rationed.** Rule 15 ("Strike where it hurts") tells the DM to periodically target *"a lover,
sworn companion, child, mentor, family member"* as a rescue victim — *"This is gold for emotional stakes, but the
pattern's power comes from scarcity"* — with a hard cooldown forbidding two consecutive strikes at the same beloved
NPC. The `rescue` sub-type is where the whole system's emotional bet sits.

**Quests that feel like they were always there.** An enormous amount of engineering goes into *hiding the seams*:
the boss-room `DungeonIndex` so the victim is already deep inside before you enter; `ScanAheadForAnchor` so
placement is always at least one door ahead; the disable → move → enable spawn sequence; the deliberate refusal to
spawn while the player is in an inn; the multi-layer distance gates so a quest at Nilheim doesn't fire from
Ivarstead. The comment *"vanilla-style: place targets at boss room before player enters"* (`:3589`) is the design
statement.

**Consequences that live in NPC heads, not in a ledger.** With no gold reward, the payoff is that people *know*.
The quest giver learns you helped, refused, were late, or never came. The rescued victim remembers being freed by a
stranger whose name they didn't know. If you murder the person you just rescued, `CheckRescuedNPCDeaths` (`:2056`)
decides whether there were witnesses — and the quest giver either learns the truth or *"heard that <victim> died
under suspicious circumstances shortly after being rescued."* That whole subsystem exists purely to make a betrayal
land socially.

Secondary intents visible in the tuning: the guide option ("Lead the way") exists so a quest can be a walk with
someone rather than a marker; the 8-second delay before the menu exists so the SkyrimNet conversation happens
*before* the mechanical prompt; the crime-faction removal during faction quests exists so a chaotic battle doesn't
saddle you with a bounty.

---

## 3. Implementation breakdown

### Where the code lives

| Layer                | File                                                                  | Role                                    |
| -------------------- | --------------------------------------------------------------------- | --------------------------------------- |
| Orchestration        | `Source/Scripts/IntelEngine_StoryEngine.psc:3019-4829`                | The entire quest lifecycle (~1800 lines) |
| Native bridge        | `SKSE/src/Papyrus.cpp:3011-3981`                                      | Spawning, chest, anchors, item checks   |
| Decorator state      | `SKSE/src/QuestStateTracker.h`                                        | Active-quest text for SkyrimNet         |
| Item resolution      | `SKSE/src/ItemIndex.cpp`                                              | find_item name → form                   |
| Location + anchors   | `SKSE/src/LocationResolver.cpp:1180-1331`, `:1908-2022`               | DungeonIndex, `ResolveAnyDestination`   |
| Anti-repetition      | `SKSE/src/NPCIndex.cpp:2515-2824`                                     | Recent locations/items/victims, history |
| Response validation  | `SKSE/src/FactionPolitics.cpp:2110-2220`                              | Sub-type whitelist + field presence     |
| Faction battle hook  | `SKSE/src/BattleManager.cpp:1045-1109`, `Source/Scripts/IntelEngine_Battle.psc` | Validate + run the battle     |
| Prompt               | `IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt` | Sections 107-168, 265-290       |
| Manual dispatch      | `Source/Scripts/IntelEngine_Core.psc:2252-2323`                       | Director tab ModEvent                   |
| UI                   | `web/dashboard/src/components/{StoryTab,DirectorTab,SettingsTab}.jsx` | Display, manual dispatch, toggles       |

### ESP forms used

- **Quest `IntelEngine`** (fragment script `QF_IntelEngine_02000D61`, i.e. FormID `0x02000D61`) — start-game-enabled,
  hosts every subsystem script including `IntelEngine_StoryEngine`.
- **`ReferenceAlias QuestTarget`** — the only quest-specific alias. `ForceRefTo`'d at the victim / chest / boss /
  location marker. Confirmed present in the ESP alongside `PlayerAlias`, `AgentAlias00-04`, `TargetAlias00-04`
  (string table of `IntelEngine.esp`; also a fragment property `Alias_QuestTarget`).
- **Objective 0** (`QUEST_OBJECTIVE_ID = 0`) and **stage 100** — the stage fragment calls
  `SetObjectiveDisplayed(0, true)` as a backup to the direct call.
- **Packages** (properties on `IntelEngine_Core`): `TravelPackage_Jog` (courier + guide, priority
  `PRIORITY_TRAVEL` = 100), `TravelPackage_Walk` (freed victim walking to player), `SandboxNearPlayerPackage`
  (pinning the courier during the 8 s wait at priority 100; lingering and victim-pinning at `PRIORITY_SANDBOX`).
- **Keyword `IntelEngine_TravelTarget`** — the linked-ref key for all of the above, set via
  `PO3_SKSEFunctions.SetLinkedRef`.
- **No quest-specific globals, keywords, or factions were added** — quest state lives entirely in `Auto Hidden`
  script properties.

### Persisted state

Everything on the left survives saves; everything on the right does not.

```text
SAVE-PERSISTED
  Quest script properties (~25): QuestActive, QuestGiver, QuestGuideNPC, QuestLocation, QuestEnemyType,
    QuestLocationName, QuestSubType, QuestBriefing, QuestVictimNPC, QuestVictimName, QuestItemName,
    QuestItemDesc, QuestItemChest, QuestBossNPC, QuestBossAnchor, QuestAlliedFaction,
    QuestBattleEnemyFaction, QuestBattleScheduled, QuestStartTime, QuestEnemiesSpawned, QuestPrePlaced,
    QuestVictimFreed, QuestVictimInFurniture, QuestFurnitureScanned, QuestDeferredToInterior,
    QuestDungeonLastCell, QuestDungeonDepth, QuestDungeonScanFails, QuestSpawnAttempts, QuestExpiryWarned
  StorageUtil on the player: Intel_QuestSpawnedNPCs (FormList), Intel_RecentlyRescuedNPCs (FormList),
    Intel_RecentStoryEvents (last 8 strings), Intel_StoryLingerActors
  StorageUtil on NPCs: Intel_MessageSender, Intel_MessageContent, Intel_QuestLocation, Intel_WasEssential,
    Intel_OrigAggression, Intel_RescueQuestGiver, Intel_RescuePlayerName, Intel_RescueTime,
    Intel_RescueNarration, Intel_StoryLastPicked, Intel_MeetingLingerApproaching
  The quest objective/stage and the forced QuestTarget alias (engine-side quest state)

NOT PERSISTED (rebuilt or lost on every game load)
  QuestStateTracker singleton  -> re-pushed by OnGameLoad (StoryEngine.psc:296-305)
  NPCIndex m_recentQuestItems (8), m_recentRescueVictims (6), m_recentQuestLocations (8)  -> lost, no warm-up
  NPCIndex m_recentDispatches (6-entry dispatch history)                                  -> lost
  BattleManager playerCrimeFactionsRemoved_                                               -> lost
  The SKSE co-save (Plugin.cpp:83-127) stores only the save ID and SlotTracker — no quest data.
```

### Enemy spawning (`Papyrus.cpp:3181-3307`, `:3442-3504`)

`enemyType` is one of `"bandit"`, `"draugr"`, `"dragon"`, or `"faction:<FactionId>"`.

| enemyType | Regular leveled actors                          | Count | Boss                   |
| --------- | ----------------------------------------------- | ----- | ---------------------- |
| bandit    | `LvlBanditMeleeAny` + `LvlBanditMissile` (alt.)  | 3–5   | `LvlBanditBoss`        |
| draugr    | `LvlDraugrMeleeAllMale` + `LvlDraugrMissileMale` | 2–4   | `LvlDraugrWarlockMale` |
| dragon    | `EncDragon01Fire`                                | 1     | `EncDragon01Fire`      |
| faction:X | faction's configured soldier template            | 4–6   | same template          |

`LookupLeveledActor` (`:3063`) is a four-stage cascade: EditorID map → full form-array enumeration → **prefix match**
(picks randomly among all forms whose EditorID starts with the requested string, to survive Requiem/Lorerim replacing
leveled lists) → a hardcoded FormID table for vanilla forms whose EditorIDs SSE strips at runtime
(`Papyrus.cpp:3036-3061`). It skips prefixes `REQ_NULL_`, `ERDP`, `manny_GF_`, `QVK`, `SocDLC`.

Faction spawns additionally resolve the leveled list to concrete NPCs by hand, walking up to 10 levels of nested
`TESLevCharacter` via `CalculateCurrentFormList` at the player's level (`:3216-3235`).

Spawned actors go into `Intel_QuestSpawnedNPCs` on the player and are placed with `PlaceObjectAtMe(base, true)`
(force-persist). The chest uses `PlaceObjectAtMe(chestBase, false)` — **not** persistent.

### find_item resolution

`ItemIndex::BuildIndex` (`ItemIndex.cpp:45`) indexes every named weapon, armor, misc item and spell tome at
`kDataLoaded`, keyed by lowercase name, keeping the higher-gold form on name collisions. Items ≥ **500 gold**
(`VALUABLE_THRESHOLD`) also go into a `m_valuableItems` list. `FindByName` is exact → `FuzzyFind` (Levenshtein,
maxDist **3**) → **unconditional substring match**. `ValidateName` is exact → fuzzy with maxDist 2.
`GetRandomQuestItemName(500)` picks uniformly from valuables, excluding the last 8 used, with two progressively
weaker fallbacks and a final "ignore exclusions entirely."

### Rescue victim state machine

Three mutually exclusive representations, chosen by what the cell contains:

1. **Furniture** — if `FindUsablePrisonerFurniture` (Furniture form type only, so an NPC can `Activate` it) finds
   shackles/stocks in the victim's cell, the victim is healed, moved, `Activate`d into the furniture, and pinned
   with `SetDontMove(true)`, re-applied every 3-second tick (`:4095-4123`).
2. **Bleedout** (default) — `SetEssential(true)` + `SetNoBleedoutRecovery(true)` +
   `DamageActorValue("Health", currentHealth + 100)` + `EvaluatePackage()`. The comment explains the choice: this is
   "the pattern that reliably triggers the bleedout state + kneel anim on essential actors," and `SetDontMove` is
   *deliberately not* used because "it blocks the fall-to-kneel animation" (`:3629-3633`, `:4120-4121`). Health is
   re-pinned every tick against natural regen.
3. **Decorative prop** — if only a Static/Activator cage mesh exists, the victim is nudged onto it and left in
   bleedout.

`FreeQuestVictim` (`:4374`) reverses whichever one applied and heals to full.

### faction_battle

The thinnest sub-type: it owns almost no logic of its own. On player arrival at the location
(`:3884-3935`) it sets `QuestBattleScheduled = true` and `QuestEnemiesSpawned = true` (both **before**
`StartBattleImmediate`, because that function contains latent `Utility.Wait` calls that would otherwise let
`CheckQuestProximity` re-enter and start a second battle), hands the battle system
`QuestAutoJoinFaction` + `BattleSpawnAnchor`, and then just polls `IntelEngine.IsBattleActive()`.

Three staged notifications create the buildup: *"You hear the clink of armor ahead."* → *"Sounds of fighting carry
from up ahead."* → completion. Standing is applied entirely by the battle system's `ApplyPostBattleStanding`;
`RecordFactionBattleCompletion` (`FactionPolitics.cpp:2234`) builds the notification, the quest-giver fact, a
distinct "received word that…" fact for every faction leader, and records a `player_combat` political event.

### Anti-repetition

Three C++ ring buffers, emitted into the DM prompt as a "Recent Quest History (avoid repeats)" block
(`NPCIndex.cpp:2525-2540`): quest locations (8), find_item targets (8), rescue victims (6). Plus a six-entry
dispatch history with per-entry `[DISPATCHED]` / `[REJECTED — reason]` markers that the prompt's Rule 14 reads back
as feedback, and per-type pick counts warmed from `Intel_RecentStoryEvents` on load.

### SkyrimNet integration

- **Prompt:** `intel_story_dm.prompt` — quest section at lines 107-168, response templates at 265-290. Every
  sub-type is wrapped in `{% if show_quest_<subtype> == "1" %}`, so the LLM's menu shrinks with the MCM toggles.
- **Decorator:** `get_intelengine_quests`, registered at `kDataLoaded` (`Plugin.cpp:346-360`), returns
  `QuestStateTracker::GetFormattedQuestInfo()` — e.g. `**The Rescue at Fort Greymoor** (Rescue): Ysolda asked for
  help rescuing Camilla Valerius from bandit at Fort Greymoor on behalf of the ImperialFaction and said: "Bandits
  took Camilla!"`. This is how any NPC you talk to can discuss the active quest.
- **No action YAML.** The player cannot request or hand in a quest through dialogue.

### Manual dispatch

`OnDashboardDispatchStory` (ModEvent, `IntelEngine_Core.psc:2252`) routes a JSON payload from the dashboard's
Director tab straight into `StoryEngine.HandleQuestDispatch`. It **deliberately skips the cooldown and MCM checks**
("it's a manual DM override", `:2281`) — but not `HandleQuestDispatch`'s own validation.

### The tuning numbers, collected

```text
Story DM tick interval        3 game hours (Intel_MCM_StoryInterval)
Per-NPC story cooldown        24 game hours (Intel_MCM_StoryCooldown)
Candidate pool size           7
LLM response budget           700 chars total / 120 narration / 150 msgContent
Concurrent quests             1
Quest expiry                  1.0 game day (MCM slider), warning at 75%
Guide timeout                 5 game hours; wait at 2000u, resume at 800u
Courier pin before menu       8 real seconds
Proximity gates               4000u (marker), 6000u (BGSLocation), 2000u (victim/boss), 200u (manual free)
Hostile-scan radius           3000u (IsAreaClearOfHostiles, CheckRescuedNPCDeaths witnesses)
Dungeon scan give-up          depth >= 3 OR 5 failed scans
Spawn retry give-up           3 attempts -> partial completion
Enemy spread                  +/-300u regular, +/-150u boss
Faction standing              +5 ally / -5 enemy; partial = 2 (integer 5/2)
Faction visibility            friendly >= 40, hostile <= -40
Item value floor              500 gold
Anti-repeat buffers           8 locations / 8 items / 6 victims / 6 dispatches / 8 recent story events
Post-rescue death watch       1 game day
```

---

## 4. Weaknesses and bugs

Split into **confirmed** (read directly in the code) and **suspected** (inferred, would need in-game or xEdit
verification).

### Confirmed — LLM-named targets resolving wrong

- **`victimName` goes through the worst resolver in the codebase.** `IntelEngine.FindNPCByName` →
  `NPCIndex::FindByName`, whose fourth stage is an unconditional first-hit substring match with no quality
  threshold — the documented source of the `Sten → Stenvarr` and `<PLAYER_NAME>'s Shadow` bugs
  (see [`../NAME_RESOLUTION_FAILURE_MODES.md`](../NAME_RESOLUTION_FAILURE_MODES.md)). A rescue quest can therefore
  kidnap a completely different NPC than the one the DM named, and the player is told to rescue *the name the LLM
  said*, which is what the prompt text and the SkyrimNet decorator both echo. The victim is then teleported into a
  dungeon, made Essential and put into permanent bleedout.
- **`questLocation` NPC-name collision was a real shipped bug.** `LocationResolver::ResolveAnyDestination` carries
  the comment: *"Location names take priority over NPC names because fuzzy NPC matching can produce false positives
  (e.g., 'Nilheim' → 'Wilhelm' at distance 4, resolving a dungeon quest to the innkeeper's home instead)"*
  (`LocationResolver.cpp:1971-1974`). The fix was reordering, not tightening — Phase 3.5 still resolves an
  unrecognised location string to *an NPC's home*, so a hallucinated location name can silently become someone's
  bedroom, which then gets bandits spawned in it.
- **`itemName` failures are silently replaced.** If `ValidateQuestItem` rejects the DM's item, the code substitutes
  a random ≥500-gold item (`:3092-3100`) — but `QuestItemDesc` (the LLM's flavour text, e.g. *"a legendary glass
  blade"*) is **not** updated. The player is told about a glass blade and finds a Daedric Warhammer. The completion
  notification (`"You claim the " + QuestItemDesc`) repeats the wrong description.
- **`ItemIndex::FindByName` substring stage.** Same trap as NPCs: after exact and fuzzy, it returns the first name
  where either string contains the other (`ItemIndex.cpp:237-251`). `"Sword"` matches whatever comes first.

### Confirmed — quests that cannot complete or never clear

- **Pre-placed quests never fall back.** In the pre-placed branch, if `SpawnQuestEnemies` returns zero actors, the
  code logs *"WARNING - no enemies spawned at boss room, falling back to deferred"* and then `return`s
  (`:3791-3795`). `QuestPrePlaced` is still true and `QuestSpawnAttempts` is never incremented (it is only bumped
  inside the separate `SpawnQuestEnemies()` Papyrus function), so the next tick re-enters the identical branch. The
  advertised fallback does not exist and the quest spins until expiry. The 3-attempt partial-completion safety net
  is unreachable from this path.
- **`AreAllQuestEnemiesDead()` returns false on an empty list** (`:4339-4341`). Combined with the above, a
  zero-spawn pre-placed quest can never complete.
- **find_item completion depends on a non-persistent chest.** The chest is created with
  `PlaceObjectAtMe(chestBase, false)` (`Papyrus.cpp:3360`). The code elsewhere explicitly acknowledges that
  "non-persistent `PlaceObjectAtMe` refs in unloaded cells" revert position after save/reload (`:3699-3701`). If
  the chest ref is lost, `IsQuestItemInChest` can never flip and the quest can only expire.
- **`IsAreaClearOfHostiles` scans *all* nearby actors**, not just quest-spawned ones (`:4355-4372`). Any unrelated
  hostile within 3000 units of the victim — wandering draugr, a hostile animal, a Requiem-added spawn — blocks the
  auto-complete path. Only the 200-unit manual-free fallback saves it, and that requires the player to be out of
  combat.
- **Saving and reloading while the courier is walking silently kills the quest.** `OnGameLoad`'s final `Else` branch
  abandons any in-flight story dispatch (`:371-382`) → `CleanupStoryDispatch` → which, because `ActiveStoryType ==
  "quest"`, calls `CleanupQuest` (`:4893-4902`). The player never learns a quest was offered; the quest giver gets a
  "the request seems to have fallen through" fact.
- **Player-home knock prompt orphans the quest.** `HandleQuestDispatch` commits `QuestActive = true` and *then*
  calls `DispatchToTarget`, which — if the player is at home — shows a knock prompt and, on "Send them away" or
  "Ignore"/timeout, sets `ActiveStoryType = ""` and returns without touching quest state
  (`:1696-1708`). Result: `QuestActive` stays true with no courier, no marker, and no path to completion, blocking
  all new quests until `CheckQuestExpiry` fires a game day later. (The same shape as the bug the `FindFreeAgentSlot`
  pre-check at `:3145-3150` was added to fix — that fix covers only the slot case.)

### Confirmed — reward and crime exploits

- **The hold-crime-faction FormID table is almost entirely wrong.** `BattleManager.h:388-398` hardcodes nine
  FormIDs commented as Whiterun/Rift/Reach/Eastmarch/Haafingar/Hjaalmarch/Pale/Falkreath/Winterhold —
  `0x00029DB0` through `0x00029DB8`. Checked against the Spriggit export of retail Skyrim SE
  (`C:\Projects\spriggit-output\Skyrim\Factions\`):

  ```text
  0x00029DB0 -> CrimeFactionHaafingar        (a crime faction, but labelled "Whiterun")
  0x00029DB1 -> TownIrontreeMillFaction      (not a crime faction at all)
  0x00029DB2 -> TownHalfMoonMillFaction      (not a crime faction at all)
  0x00029DB3..0x00029DB8 -> no FACT record exists in Skyrim.esm
  ```

  The real hold crime factions are `CrimeFactionWhiterun 0267EA`, `CrimeFactionRift 02816B`,
  `CrimeFactionReach 02816C`, `CrimeFactionEastmarch 0267E3`, `CrimeFactionHjaalmarch 02816D`,
  `CrimeFactionPale 02816E`, `CrimeFactionFalkreath 028170`, `CrimeFactionWinterhold 02816F` — none of which the
  code references. Since `TESForm::LookupByID<RE::TESFaction>` type-checks and returns null on mismatch, the loop
  effectively touches **exactly one** faction: Haafingar. So the whole "quest-level bounty immunity" feature works
  only in Solitude's hold, and the same table is used by `SnapshotBounties` to strip crime factions from spawned
  battle soldiers — meaning killing faction-quest or battle soldiers anywhere outside Haafingar *does* generate a
  bounty, which is the exact thing the code was written to prevent. This is a textbook example of the
  "IntelEngine's magic constants are guesses" hazard.
- **What the table does do is a bounty-wipe exploit in Haafingar.** `RemovePlayerCrimeFactions` removes the player
  from `CrimeFactionHaafingar` for the whole quest duration (up to a game day), and
  `RestorePlayerCrimeFactions` doesn't just re-add it — it calls `SetCrimeGold(0)` / `SetCrimeGoldViolent(0)` on
  every faction in the table (`BattleManager.cpp:1268-1274`). So accepting any faction quest and letting it expire
  clears the player's Solitude bounty, gated only on standing ≥ 40 with one faction. Had the FormIDs been correct
  this would have been a whole-of-Skyrim bounty wipe.
- **`playerCrimeFactionsRemoved_` is C++-only state, not co-saved.** If the quest properties are ever lost while
  the faction removal (a save-persisted actor state) is not, the player stays permanently outside
  `CrimeFactionHaafingar`. The `CleanupQuest` belt-and-braces restore only fires while `QuestAlliedFaction` is
  still set.
- **Partial-credit standing is granted in three places** (spawn failure, battle-busy, battle-lost-on-load). The
  battle-lost-on-load path (`:410-424`) triggers on game load, granting +2 standing; it self-clears via
  `CleanupQuest`, but it is a "reload to be rewarded for not fighting" path.

### Confirmed — state leaks and save bloat

- **Spawned enemies are never cleaned up.** `CleanupQuest` calls
  `StorageUtil.FormListClear(player, "Intel_QuestSpawnedNPCs")` (`:4615`) and nothing else. The actors themselves
  were created with `PlaceObjectAtMe(base, true)` — **force-persist** — so on every expired, failed, refused or
  abandoned quest, 3–7 persistent leveled actors are left in the world forever, and the only handle to them has
  just been discarded. Contrast the chest and the battle marker, which *are* disabled and deleted.
- **`Intel_RecentlyRescuedNPCs` can grow unbounded in one direction.** Entries are removed on death, on disable, or
  after one game day alive — but the day-based expiry only runs while `CheckRescuedNPCDeaths` is called from
  `TickScheduler`, which is gated behind the shared game-time timer. Entries for NPCs that are alive, loaded and
  never re-visited persist until that tick runs.
- **`QuestBattleMarker` is dead code.** Declared (`:156`) and cleaned up (`:4695-4699`), never assigned anywhere in
  the repository.
- **`QuestAllowVictimDeath` is a dead setting.** Exposed in MCM (`IntelEngine_MCM.psc:1016`), the dashboard
  (`SettingsTab.jsx:338`, with the warning *"WARNING: Rescued NPCs can die during combat. Can break main quests!"*)
  and the settings JSON — but **never read by any gameplay code**. The victim is unconditionally made Essential.
- **`QuestSpawnCount` is assigned, not accumulated, in the pre-placed path** (`:3784`), overwriting any
  dispatch-time boss count. It is only used for logging, so the impact is cosmetic.

### Confirmed — DM feedback loop corruption

- **Most quest rejections never reach the dispatch history.** `RecordStoryDispatch` optimistically marks every
  response `Dispatched` and relies on `MarkLastDispatchFailed` to flip it (`NPCIndex.cpp:2592-2635`). But
  `HandleQuestDispatch` returns silently on at least a dozen rejection paths — missing fields, unresolvable victim,
  victim is a teammate, unresolvable location, non-combative location, no free slot, no faction member, battle
  validation failure — **none of which call `MarkLastDispatchFailed`**. The prompt's Rule 14 then tells the DM
  *"DISPATCHED entries = the world actually saw this happen. Use them for variety/cooldown checks"* about events
  that never occurred. The DM's variety logic is being fed phantom successes.
- **The location/item/victim rotation buffers are marked used at dispatch, not at acceptance**
  (`:3265-3274`). A quest the player refuses still burns its location and victim out of rotation.
- **All anti-repetition state is lost on game load.** `m_recentQuestLocations`, `m_recentQuestItems`,
  `m_recentRescueVictims` and `m_recentDispatches` are in-memory C++ deques with no co-save serialization and no
  warm-up equivalent to `WarmStoryTypeCounts` (which only recovers per-type counts from the 8-entry
  `Intel_RecentStoryEvents` list). After any reload the DM can immediately repeat the last location and victim.

### Confirmed — alias recovery hazard

`PlaceQuestMarker` has a runtime-recovery path for saves made before `QuestTargetAlias` was authored (`:4751-4770`).
It walks aliases from the top down and takes the first one that is not one of the ten agent/target aliases:

```papyrus
Int i = GetNumAliases() - 1
While i >= 1 && QuestTargetAlias == None
    ReferenceAlias ra = GetAlias(i) as ReferenceAlias
    If ra != None && ra != Core.AgentAlias00 && ... && ra != Core.TargetAlias04
        QuestTargetAlias = ra
```

**`PlayerAlias` is not in the exclusion list.** If `PlayerAlias` sits at a higher alias index than `QuestTarget`,
this loop assigns the player's alias as the quest-marker alias and then `ForceRefTo`s it at the quest location —
clobbering the alias that hosts `IntelEngine_PlayerAlias`. The loop also stops at `i >= 1`, so an alias at index 0
can never be recovered. From the ESP string table, `QuestTarget` appears to be the *first* alias and `PlayerAlias`
the second, which would make both failure modes fire — but that ordering is **suspected**, inferred from byte
offsets in `IntelEngine.esp`, not verified in xEdit.

### Suspected

- **Permanent Essential flag on rescue victims.** `Intel_WasEssential` is captured in three separate places
  (`:3626`, `:3981`, `:4223`) as `IsEssential()` *before* `SetEssential(true)`. If two of those run for the same
  quest — plausible when the ahead-anchor path runs on consecutive ticks after a zero-enemy spawn — the second
  capture records `1`, and the restore leaves the NPC permanently Essential. `GetActorBase().SetEssential` also
  mutates the *base* form, so a victim whose base is shared (a generic named NPC, or a mod-added template) would
  make every actor of that base Essential.
- **Re-entrancy around the 8-second menu wait.** `OnQuestNPCArrived` yields the Papyrus thread across
  `Utility.Wait(8.0)` and again inside `SkyMessage.Show` (which blocks until the player answers, potentially
  minutes). `IsActive = false` is set to stop `CheckStoryNPCArrival` re-entering, and there are post-wait validity
  checks for dead/in-combat/slot-cleared — but `CheckQuestProximity` is *not* gated by `IsActive`, only by
  `ActiveStoryType == "quest"` (`:3685-3687`), which is still true here. A player who is already standing at the
  quest location when the courier arrives should be protected by that check; a player who walks there during the
  menu wait may not be.
- **`OnQuestComplete` sets `QuestActive = false` first as a re-entrancy guard** (`:4398-4401`) but then performs
  long latent work (`Utility.Wait(0.1)`, package application, faction calls). `CheckQuestExpiry` and
  `CheckQuestProximity` both early-out on `!QuestActive`, so this looks correct, but any concurrent path that
  reads other quest properties during that window sees a half-torn-down quest.
- **`RemoveQuestMarker` is called with `completed = false` in `CleanupQuest`** *and* the caller frequently calls
  `RemoveQuestMarker(true)` first (`:4547-4548`). The second call runs `SetObjectiveDisplayed(0, false)` after
  `SetObjectiveCompleted(0)`. Whether the journal ends up showing a completed or a vanished objective depends on
  engine ordering; not verified in game.
- **Prefix-match spawning can pick absurd actors.** `LookupLeveledActor`'s prefix stage returns a *random* form
  among everything starting with e.g. `LvlBanditMelee`. On a heavy modlist this pool is unaudited beyond five
  hardcoded prefix skips, and the result is spawned in a group of 3–5.
- **`faction_battle` marker handoff.** `PlaceQuestMarker` points the alias at `QuestLocation` and comments that
  "SpawnFullBattle will move it to the battle leader's head once soldiers spawn" (`:4795-4799`). That handoff lives
  in the battle system; if the battle never spawns, the marker stays on the raw location.

### Design-level weaknesses worth naming

- **One quest at a time, globally.** Any orphaned quest state (see the knock-prompt bug) blocks the entire feature
  for a game day. There is no player-facing "abandon quest" control — only the MCM timeout slider.
- **The player's only decision surface is a 2–3 button `SkyMessage`.** Everything expressive happens in the
  SkyrimNet conversation, which has no mechanical connection to the menu; the LLM cannot know what the player
  chose, and the player cannot negotiate terms, ask questions, or hand the quest in through dialogue.
- **No reward economy at all.** Standing and injected memories are the entire payoff. Whether that is a feature
  (it is clearly deliberate) or a gap depends on the design, but it means a `combat` quest is mechanically
  indistinguishable from "a marker appeared."
- **The quest giver is never revisited.** Completion injects a fact into them wherever they are; there is no
  return-to-giver step, so the social payoff only materialises if the player happens to talk to them again.
- **Magic numbers chosen by feel.** The 4000/6000/2000/200-unit proximity gates, `depth >= 3 || scanFails >= 5`,
  3 spawn attempts, 8-second menu delay, ±300-unit spread, 500-gold item floor, and the 40/−40 standing thresholds
  all appear without derivation or comment justifying the specific value.
