# IntelEngine Prior Art — Grudge Ambush (stealth approach, combat, yield)

> **Reading discipline.** This is a forensic write-up of **one** IntelEngine feature, produced by reading the
> abandoned source. IntelEngine shipped but was never polished; nothing here is a recommendation, and several
> behaviours documented by its own author do not match its code. Every substantive claim cites a file and line so a
> reader can verify. See [`../README.md`](../README.md) for the full prior-art discipline.

The README bullet this covers:

> - **Ambush you** for real grudges — with stealth approach, combat, and a yield system

Scope: the **single-NPC** `ambush` story type (and its `ambush_charge` / `ambush_combat` sub-states). The separate
`faction_ambush` type (3–7 spawned faction soldiers) is a different README bullet and a completely different code
path; it is referenced here only where the two touch.

Paths are given relative to the two IntelEngine repos:

- `NP/` = `C:\Projects\IntelEngine-NativePlugin\`
- `GP/` = `C:\Projects\IntelEngine-GamePlugin\`

---

## 1. High-level overview, step by step

### Step 0 — There is no grudge meter

The most important structural fact: **IntelEngine never accumulates a numeric grudge.** There is no counter, no
threshold, no hostility score, and no "grudge" identifier anywhere in the C++ or Papyrus source. A search for
`grudge` across the whole native repo returns only README prose and two prompt lines. "Real grudge" is enforced
**entirely by prompt instruction to the LLM**, evaluated against at most **two** SkyrimNet memory rows per
candidate.

### Step 1 — Story DM tick

`IntelEngine_StoryEngine.TickScheduler()` (`NP/Source/Scripts/IntelEngine_StoryEngine.psc:593`) fires on a
game-time timer. Default cadence is the `IntelEngine_StoryEngineInterval` global = **2.0 game hours** (verified by
parsing the shipped `GP/IntelEngine.esp`); a real-time backup poll runs every 30 s (`IDLE_POLL_INTERVAL`,
`:37`) so low-timescale games still tick.

### Step 2 — Candidate pool assembly (C++)

`NPCIndex::BuildDungeonMasterContext` (`NP/SKSE/src/NPCIndex.cpp`, markdown assembly around `:2451`–`:2500`) builds
a Markdown candidate pool. Per candidate it emits: name, archetype, gender, location, hold, a bucketed distance
string, a familiarity tier (`stranger` / `aware` / `acquainted`), "Last met player", bio, faction, relationships,
**Memories**, last conversation, recent events, and a `Knows:` world-knowledge block.

Candidate eligibility (`NPCIndex::IsEligibleStoryCandidate`, `:1014`, over `PassesCommonEligibility`, `:930`)
requires a display name, `ActorTypeNPC`, non-child race, alive, not disabled, no active task, off story cooldown,
not in a blocked faction/name list — and notably rejects actors who are `IsInCombat()`, `IsPlayerTeammate()`,
**`IsHostileToActor(player)`**, or in the player's own cell.

The memory budget is the whole basis for "real grudge":

```cpp
// NP/SKSE/src/NPCIndex.cpp:2016
snap.memoriesPerCandidate = std::min(Settings::GetSingleton()->maxMemoriesInContext, 2);
```

### Step 3 — The DM prompt decides

The pool is rendered into `intel_story_dm.prompt`
(`GP/SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt`) and sent as a single custom-prompt LLM call. The whole
ambush type definition is four lines:

```text
### ambush
Hostile NPC stalks the player. Requires hostile motive AND combat capability.
- CIVILIANS cannot ambush. NPC MUST have a real grudge or hostile memory from the candidate pool.
- "sender" = optional — who hired or sent the ambusher. Leave empty if acting alone.
```

(`intel_story_dm.prompt:68-73`.) The response shape is a raw JSON object, capped at **700 characters** total with
narration under 120 characters (`:239`):

```text
ambush: {"should_act":true,"type":"ambush","npc":"Name",
         "narration":"tracked the road nursing a grudge over the stolen bounty","sender":"HirerName"}
```

### Step 4 — Server-side validation (Papyrus + C++)

`HandleStoryDMResponse` (`IntelEngine_StoryEngine.psc:1375` onward) applies, in order: `should_act` parse →
type/npc presence → `IntelEngine.ResolveStoryCandidate` (FormID resolution from the pool, not fuzzy name match) →
**high-status rejection** (`:1484`) → **hold restriction** (`:1491`) → **per-NPC story cooldown** (`:1496`) →
**interior rejection for `ambush`/`stalker`** (`:1515-1522`) → `IntelEngine.ValidateStoryResponse` (C++, MCM
toggle + field-shape only) → Jarl rejection → routing.

Every rejection calls `IntelEngine.MarkLastDispatchFailed(reason)`, and the reason string is fed back into the
next tick's prompt as a `[REJECTED — reason]` history entry (prompt rule 14, `:210-226`) — a self-correcting
feedback loop for the LLM.

### Step 5 — Dispatch and the stealth/charge coin flip

`HandleAmbushStalkerDispatch` (`IntelEngine_StoryEngine.psc:2674`):

1. Persistent SkyrimNet memory on the ambusher: `"<Name> <narration>"`.
2. `Core.InjectFact(npc, narration)` — the motive enters the NPC's live bio context.
3. If `sender` was supplied, a fact is injected on the sender: `"sent <Name> to ambush <Player>"`.
4. **A 50/50 coin flip** picks the approach:

   ```papyrus
   ; :2696-2704
   If storyType == "ambush"
       If Utility.RandomInt(0, 1) == 1
           storyType = "ambush_charge"
   ```

5. `DispatchToTarget(npc, player, narration, "story")` allocates one of the **5 shared agent slots**, applies the
   travel package, arms stuck/off-screen tracking, arms the C++ proximity monitor, and switches the quest to the
   3-second real-time monitor loop.
6. For stealth `ambush` only: `Intel_SneakPhase = 0` and `Intel_SneakStartTime = <real time>`.

### Step 6 — Approach

Every 3 s (`MONITOR_INTERVAL`), `CheckStoryNPCArrival` (`:2214`) runs. For `ambush` with the NPC 3D-loaded
(`:2259`):

- **Phase 0 → 1:** when distance ≤ `SNEAK_APPROACH_DISTANCE` (**2000 u**) and > `AMBUSH_CONFRONT_DISTANCE`
  (**500 u**), call `StartSneaking()` and set phase 1. If the NPC was already inside 500 u when phase 0 was
  evaluated, it confronts immediately.
- **Phase 1 resolution:** confront when distance ≤ **500 u**, or when `ActiveStoryNPC.IsDetectedBy(player)`, or on
  a hard proximity failsafe of **400 u**.
- **Abandon:** if `Intel_SneakStartTime` is older than `SNEAK_TIMEOUT_SECONDS` (**300 s**) *and*
  `Core.ShouldReleaseLinger()` is true (player > **800 u** away or unloaded), the NPC exits sneak, the slot is
  cleared, all package overrides are stripped, and the player gets a "gave up" notification (`:2262-2276`).

`ambush_charge` does **not** enter that block at all. It relies on the ordinary arrival path — `ARRIVAL_DISTANCE`
= 300 u, or the C++ `ProximityMonitor` firing at its 150 u actor threshold on a 150 ms cadence — and
`OnStoryNPCArrived` routes both `ambush_charge` and `ambush` straight to `OnAmbushConfront` (`:2523-2527`).

### Step 7 — Confrontation and combat handoff

`OnAmbushConfront` (`:2887`):

```papyrus
If ActiveStoryNPC.IsSneaking()
    ActiveStoryNPC.StartSneaking()          ; toggle out of sneak
EndIf
Core.InjectFact(ActiveStoryNPC, "confronted <Player> with weapon drawn, ready to attack")
Core.SendTaskNarration(ActiveStoryNPC, ActiveNarration, Game.GetPlayer())   ; SkyrimNet DirectNarration
Utility.Wait(1.5)
ActiveStoryNPC.GetActorBase().SetEssential(true)   ; bleedout instead of death, so yield can fire
ActiveStoryNPC.SetActorValue("Confidence", 4)      ; Foolhardy — never flees
ActiveStoryNPC.StartCombat(Game.GetPlayer())
ActiveStoryType = "ambush_combat"
```

Note what is *not* here: no faction change, no `SetRelationshipRank`, no crime-faction handling, no AI-package
swap. Combat is started by the raw `StartCombat` call while the travel package override is still installed at
priority 100.

### Step 8 — Combat monitoring and yield

`CheckAmbushCombat` (`:2912`), polled every 3 s, has four exits:

| Condition | Action |
| --- | --- |
| `Intel_CombatStartTime` older than 300 s **and** player > 800 u away | force-end: clear essential, stop combat, Aggression 0 / Confidence 2, linger |
| `IsDead()` | clear essential, clear slot, cleanup (see §4 — unreachable in practice) |
| **`IsBleedingOut()`** | `OnAmbushYield()` |
| `!IsInCombat()` | combat ended some other way (player fled, guards intervened): clear essential, Aggression 0 / Confidence 2, linger |

`OnAmbushYield` (`:2960`) is the payoff:

```papyrus
ActiveStoryNPC.GetActorBase().SetEssential(false)      ; player may now finish them
ActiveStoryNPC.StopCombat()
ActiveStoryNPC.StopCombatAlarm()
ActiveStoryNPC.SetActorValue("Aggression", 0)
ActiveStoryNPC.SetActorValue("Confidence", 1)
ActiveStoryNPC.RestoreActorValue("Health", 50.0)       ; enough to stand up out of bleedout
Core.InjectFact(ActiveStoryNPC, "was beaten in combat by <Player> and yielded, begging for mercy")
Core.SendTaskNarration(ActiveStoryNPC,
    "dropped to one knee and yielded to <Player>, exhausted and beaten", Game.GetPlayer())
```

There is **no dialogue menu, no surrender UI, no player choice prompt**. The yield is a fact injection plus a
SkyrimNet narration; whatever conversation follows is produced by SkyrimNet from that fact.

### Step 9 — Linger and cleanup

`FinishArrivalWithLinger` (`:1950`) is the shared terminal path for every story type: clear the agent slot,
`CleanupStoryDispatch()`, link the NPC to *itself*, apply `SandboxNearPlayerPackage` at priority 90, ensure
building access, and register the NPC in the `Intel_StoryLingerActors` list. The lingering NPC is released when
the player walks > 800 u away (`Core.ShouldReleaseLinger`, `IntelEngine_Core.psc:925`) or after
`LINGER_TIMEOUT_SECONDS` = 300 s, with a 30-second grace period first (`:2037-2039`).

`CleanupStoryDispatch` (`:4892`) unsets `Intel_IsStoryDispatch`, `Intel_StoryNarration`, `Intel_SneakPhase`,
`Intel_SneakStartTime`, `Intel_CombatStartTime`, `Intel_OffscreenArrival`, clears `ActiveStoryNPC`/`Type`/
`Narration`, sets `IsActive = false`, and restarts the game-time scheduler.

### Step 10 — What survives a save

On game load, `RestartMonitoring` (`:265`) explicitly **abandons** the whole thing:

```papyrus
; :352-370
ElseIf ActiveStoryType == "ambush" || ActiveStoryType == "ambush_charge"
     || ActiveStoryType == "stalker" || ActiveStoryType == "ambush_combat"
    ; Sneak/combat/charge phase can't be recovered reliably -- abandon, let NPC go home
    If ActiveStoryType == "ambush_combat"
        ActiveStoryNPC.GetActorBase().SetEssential(false)
        ...
```

What *does* persist: the motive memory in SkyrimNet's DB, the injected facts (`Intel_Facts`, FIFO cap 10 per NPC),
the `Intel_StoryLastPicked` cooldown stamp, and the `Intel_RecentStoryEvents` ring buffer (8 entries) on the
player.

---

## 2. Intended gameplay experience

The author states the target beat verbatim in the README's narrative pitch (`NP/README.md:35`):

> And later that night, sitting in the inn, **a warrior you'd wronged three days ago tracked you down**. He snuck
> through the door, drew his blade, and attacked. You beat him — he yielded, dropped to his knees, and begged for
> mercy. He told you someone sent him. Someone you thought was a friend. That fact is now in his bio. And in
> yours.

And in the story-type table (`NP/README.md:372`):

> **Ambush** — Hostile NPCs with real grudges stalk and attack you. Stealth or charge variants. Beat them and
> they yield — talk, kill, or walk away.

Reading the tuning and the prompt together, the intended experience decomposes into five deliberate design goals:

**Consequence, not random encounter.** The gating is all about provenance. The prompt forbids fabrication
outright ("Candidate data is your ONLY source of truth", rule 5) and specifically demands "a real grudge or
hostile memory from the candidate pool." Meanwhile the candidate filter rejects anyone who is *already*
`IsHostileToActor(player)` (`NPCIndex.cpp:1023`) — i.e. an ambusher is by construction a named NPC the player has
interacted with, never a bandit. The intent is unmistakable: the attacker must be *someone*, and the player must
be able to reconstruct why.

**Rarity.** The Story DM ticks every 2 game hours but the prompt hammers restraint — "Rejecting is the NORMAL
outcome… Hours of silence followed by one perfect moment is the goal" (rule 3), plus a 24-game-hour per-NPC
cooldown and a rotation rule that penalises repeating a type. Ambush is one of ten types competing for that one
slot. It is designed as a rare punctuation mark.

**Dread before violence.** The stealth variant is built to make the player *notice something* before it happens:
a 2000 u sneak entry, a 500 u confront ring, and an explicit "detected while sneaking → confront early" branch
(`:2313`) so the player who spots the stalker triggers the reveal themselves. Interiors are rejected at dispatch
because "interiors are too small for sneak gameplay" (`:1514`). The 50/50 charge variant exists so the player
can't learn a single tell.

**Mercy as content.** The essential flag exists solely to reach the yield: the comment says so —
`"Make essential so they bleedout instead of dying -- gives yield a chance to fire"` (`:2899`). Health is
restored to let them physically stand up, aggression is zeroed, and then the whole thing is handed to SkyrimNet as
a fact ("begging for mercy") plus a narration. The mechanical fight is a *setup* for a conversation. The player is
then left with a live, disarmed, defeated person and no prompt — kill, interrogate, or walk away.

**The grudge propagates.** The optional `sender` field means an ambush can implicate a third party, and the
sender gets their own injected fact ("sent X to ambush Y"). That fact is visible to *that NPC's* SkyrimNet bio,
so the player can go confront the person who ordered it. The README's "Someone you thought was a friend" is
exactly this mechanism.

---

## 3. Implementation breakdown

### 3.1 Where the code lives

| Layer | File | Role |
| --- | --- | --- |
| Papyrus (all ambush logic) | `NP/Source/Scripts/IntelEngine_StoryEngine.psc` | 5119 lines; ambush occupies ~`:2674-3017` plus monitor hooks |
| Papyrus (shared infra) | `NP/Source/Scripts/IntelEngine_Core.psc` | slots, packages, linger, narration/fact/memory bridges |
| Papyrus (MCM) | `NP/Source/Scripts/IntelEngine_MCM.psc` | `Ambush` toggle, 7-way hold policy, story cooldown slider |
| C++ candidate pool | `NP/SKSE/src/NPCIndex.cpp` | eligibility, archetype, hold restriction, DM context markdown |
| C++ validation | `NP/SKSE/src/FactionPolitics.cpp:2049-2110+` | `BuildExcludeList`, `ValidateStoryResponse` |
| C++ arrival fast-path | `NP/SKSE/src/ProximityMonitor.cpp/.h` | 150 ms watcher, 150 u actor threshold, single-shot |
| Prompt | `GP/SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt` | the only place "grudge" is defined |
| Bio submodule | `GP/…/prompts/submodules/character_bio/0800_intel_facts.prompt` | renders `Intel_Facts` so the ambusher's motive/yield reach dialogue |
| Dashboard | `NP/web/dashboard/src/components/DirectorTab.jsx:118-121`, `StoryTab.jsx:7` | manual ambush dispatch, per-type toggle |

**There is no SkyrimNet action YAML for ambush.** `GP/SKSE/Plugins/SkyrimNet/config/actions/` contains 14 files,
all player-driven tasks (travel, fetch, deliver, schedule…). Ambush is not an LLM-callable tool; it exists only as
a branch of the DM's single custom prompt.

### 3.2 ESP forms actually used

Parsed directly from the shipped `GP/IntelEngine.esp` (binary record walk), because the checked-in design docs are
stale — see §4.

- **Quest** `IntelEngine`, FormID `0x02000D61`. One quest for the entire mod.
- **Aliases** (12): `PlayerAlias`, `AgentAlias00`–`04`, `TargetAlias00`–`04`, `QuestTarget`. The five
  `AgentAlias` slots are the shared concurrency pool; ambush takes one of them via `Core.AllocateSlot`. Agent/target
  alias `FNAM` flag word is **`0x0A`** — two bits only. No alias in the quest carries an `ALPC`/`PKID` subrecord,
  i.e. **no packages are attached to any alias**; all AI comes from runtime `ActorUtil.AddPackageOverride`.
- **Keyword** `IntelEngine_TravelTarget` (`0x02000D63`) — the linked-ref keyword every travel package points at.
- **Faction** `IntelEngine_TaskFaction` (`0x02000D62`) — NPCs on a task are added to it; removed in `ClearSlot`.
- **Globals**: `IntelEngine_StoryEngineEnabled` = 1, `IntelEngine_StoryEngineInterval` = **2.0** (game hours),
  `IntelEngine_StoryEngineCooldown` = **24.0** (game hours), `IntelEngine_MaxConcurrentTasks` = 5.
- **Packages** used by ambush: `IntelEngine_TravelPackage_Stalk`, `IntelEngine_TravelPackage_Jog`,
  `IntelEngine_SandboxAroundPlayer`.

Raw `PKDT`/`PLDT` bytes for the four travel packages (little-endian hex, straight from the ESP):

```text
Walk   PKDT 40200000 12 00 00 00 fffe 0000   PLDT 06000000 630d0002 00000000
Jog    PKDT 40200000 12 00 01 00 fffe 0000   PLDT 06000000 630d0002 00000000
Run    PKDT 40200000 12 00 02 00 fffe 0000   PLDT 06000000 630d0002 00000000
Stalk  PKDT 40200200 12 00 00 00 fffe 0000   PLDT 06000000 630d0002 20030000
```

Decoding those, **verified against vanilla records** rather than against IntelEngine's own docs. Bit names were
established by parsing `Skyrim.esm` PKDT bytes and matching them to the Spriggit YAML export at
`C:\Projects\spriggit-output\Skyrim\Packages\`:

| Bit | Name | Established from |
| --- | --- | --- |
| `0x40` | `UnlockDoorsAtPackageStart` | `MQ203OrgnarBehindBar` (`0x0ADE4D`), flags `0x40`, Spriggit lists exactly that one flag |
| `0x2000` | `PreferredSpeed` | `dunHillgrundsTombSandboxInside` (`0x0A9598`), flags `0x20C0` = the three named flags |
| `0x20000` | `AlwaysSneak` | `CWMission04FriendWait` (`0x0D682E`), flags `0x00822000`, Spriggit lists `PreferredSpeed, AlwaysSneak, WeaponDrawn` |

So, confirmed:

- **Walk / Jog / Run** = `UnlockDoorsAtPackageStart | PreferredSpeed`, speed byte `00` / `01` / `02`.
- **Stalk** = `UnlockDoorsAtPackageStart | PreferredSpeed | AlwaysSneak`, speed byte `00` (**walk**). The author's
  property comment (`IntelEngine_Core.psc:77-78`, *"walk speed with Always Sneak flag, 800-unit arrive radius"*)
  is accurate for once.
- `PLDT` location type `06` with FormID `0x02000D63` (= the `IntelEngine_TravelTarget` keyword) on all four —
  "near linked reference". The trailing radius differs: Walk/Jog/Run **0**, Stalk **`0x320` = 800 units**, again
  matching the comment. `IntelEngine_SandboxAroundPlayer` uses the same location type with radius `0xC8` =
  **200 units**.
- **Nothing combat-related is set.** `IgnoreCombat`, `WeaponDrawn`, `WeaponsUnequipped`, and `NoCombatAlert` all
  exist in this record format (they appear on vanilla packages in the Spriggit export) and none are present on
  any of the four IntelEngine travel packages.
- The **interrupt-override byte is `00` = None** on all four (byte index 5 of PKDT; vanilla
  `MQ206PaarthurnaxCombatHoldPosition` `0x01733D` carries `04`, which Spriggit renders as
  `InterruptOverride: Combat`). None + no `IgnoreCombat` is exactly the configuration that lets combat preempt
  the approach package — i.e. this part of the design is correct, and the combat-handoff problems documented in
  §4 are not caused by package flags.
- All four use the vanilla `Travel` package template (`PKCU` template FormID `0x00016FAA`, `EditorID: Travel`).

### 3.3 Package application mechanics

All packages go on via SKSE/PapyrusUtil `ActorUtil.AddPackageOverride`, not via the ESP:

```papyrus
; IntelEngine_StoryEngine.psc:4950-4973 — ReapplyTravelPackage
Package travelPkg = Core.TravelPackage_Jog
If ActiveStoryType == "stalker" || ActiveStoryType == "ambush"
    travelPkg = Core.TravelPackage_Stalk
EndIf
Core.RemoveAllPackages(npc, false)                        ; = ActorUtil.ClearPackageOverride
PO3_SKSEFunctions.SetLinkedRef(npc, target, Core.IntelEngine_TravelTarget)
ActorUtil.AddPackageOverride(npc, travelPkg, Core.PRIORITY_TRAVEL, 1)   ; priority 100
Utility.Wait(0.1)
npc.EvaluatePackage()
```

`PRIORITY_TRAVEL` = 100, `PRIORITY_SANDBOX` = 90 (`IntelEngine_Core.psc:104-108`). These are **package-override
stack priorities**, an ActorUtil concept, not a Creation Kit package field.

`Core.RemoveAllPackages` is `ActorUtil.ClearPackageOverride(actor)` (`IntelEngine_Core.psc:1267`) — it removes
*every* override on the actor, including other mods'. A separate `RemoveIntelPackages` exists (`:1273`) that
removes only IntelEngine's, but the ambush path never uses it.

### 3.4 Gating: what is actually enforced where

| Gate | Enforced by | Effective? |
| --- | --- | --- |
| "CIVILIANS cannot ambush" | prompt text only | **prompt only — see §4.1** |
| Combat capability | `NPCIndex::GetEligibleStoryTypes` `:1540-1543` (`!isCivilian`) | **dead code, never called** |
| Not already hostile / in combat / teammate / same cell | `IsEligibleStoryCandidate` `:1019-1024` | yes, at pool build |
| High-status NPC (Jarls, stewards…) | Papyrus `:1484`, C++ `IsHighStatus` faction list | yes |
| Hold restriction (`HoldPolicyAmbush`, default **1**) | `IntelEngine.CheckHoldRestriction` → `PassesHoldRestriction` `:1458` | yes |
| Per-NPC cooldown (24 game hours) | `ApplyCooldownCheck` `:1078` | yes |
| Exterior only | Papyrus `:1515-1522` **and** C++ `BuildExcludeList` env bit 0 (`FactionPolitics.cpp:2091-2096`) | yes, twice |
| MCM `Ambush` toggle | toggle bitmask bit 3 → `BuildExcludeList` → `ValidateStoryResponse` | yes |
| Dangerous location | only `informant` is excluded (`FactionPolitics.cpp:2097-2099`) | ambush is **not** blocked in danger zones |

`ClassifyNPCArchetype` (`NPCIndex.cpp:1336-1390`) is not an enum. It maps 14 hard-coded lowercase CK class names
(`citizen`, `farmer`, `beggar`, `child`, `bard's college`, `food vendor`, `peddler`, `apothecary`, `blacksmith`,
`fence`, `innkeeper`, `lumberjack`, `miner`, `vendor`) to `"CIVILIAN"` and otherwise **uppercases the raw class
name and passes it to the LLM verbatim**. Only when an actor has no class at all does it fall back to a
skill-sum classifier (one-handed+two-handed+block vs destruction+conjuration+restoration vs
sneak+pickpocket+lockpicking, `best >= 75.0f` → `WARRIOR` / `MAGE` / `ROGUE`, else `CIVILIAN`).

### 3.5 Complete tuning constants

All from `IntelEngine_StoryEngine.psc:36-43`, `:140-143`, `IntelEngine_Core.psc:104-136`, and the ESP globals.

| Constant | Value | Meaning |
| --- | --- | --- |
| `IntelEngine_StoryEngineInterval` | 2.0 game hours | DM tick cadence |
| `IntelEngine_StoryEngineCooldown` | 24.0 game hours | per-NPC re-pick cooldown |
| `MONITOR_INTERVAL` | 3.0 s | real-time monitor poll (yield detection cadence) |
| `IDLE_POLL_INTERVAL` | 30.0 s | real-time backup tick |
| `SNEAK_APPROACH_DISTANCE` | 2000 u | phase 0 → 1 (enter sneak) |
| `AMBUSH_CONFRONT_DISTANCE` | 500 u | confront ring |
| proximity failsafe | 400 u | hard-coded literal at `:2313` |
| `SNEAK_TIMEOUT_SECONDS` | 300 s | abandon sneak; also reused as the *combat* timeout |
| `ARRIVAL_DISTANCE` | 300 u | ordinary arrival (used by `ambush_charge`) |
| `ProximityMonitor` actor threshold | 150 u @ 150 ms | C++ fast-path arrival |
| `LINGER_RELEASE_DISTANCE` | 800 u | player-walked-away release, MCM-configurable |
| `LINGER_TIMEOUT_SECONDS` | 300 s | linger hard timeout |
| `TELEPORT_OFFSET_EXTERIOR` | 3500 u | leapfrog placement behind player |
| `TELEPORT_OFFSET_INTERIOR` | 500 u | interior leapfrog |
| `MAX_STORY_OFFSCREEN_HOURS` | 0.25 game hours | cap on off-screen travel estimate (`:1748`) |
| `STUCK_DISTANCE_THRESHOLD` | 50 u | stuck-detector movement threshold |
| Confidence during combat | 4 (Foolhardy) | `:2903` |
| Confidence after yield | 1 | `:2971` |
| Aggression after yield/end | 0 | `:2925`, `:2952`, `:2970` |
| Health restored on yield | 50.0 points (flat) | `:2974` |
| ambush stealth-vs-charge | `Utility.RandomInt(0,1)` | 50/50 |
| `Intel_Facts` cap | 10 per NPC, FIFO | `IntelEngine_Core.psc:1498-1504` |
| `Intel_RecentStoryEvents` cap | 8 on player | `:4979-4985` |
| DM response budget | 700 chars total, 120 char narration | prompt `:239` |

### 3.6 Persisted state

| Where | Key / property | Notes |
| --- | --- | --- |
| Quest script properties (`Auto Hidden`) | `IsActive`, `ActiveStoryNPC`, `ActiveStoryType`, `ActiveNarration` | survive save; deliberately discarded on load for ambush |
| StorageUtil on the ambusher | `Intel_IsStoryDispatch`, `Intel_StoryNarration`, `Intel_SneakPhase`, `Intel_SneakStartTime`, `Intel_CombatStartTime`, `Intel_TaskStartTime`, `Intel_OffscreenArrival`, `Intel_Slot`, `Intel_State`, `Intel_TaskType`, `Intel_Target` | all unset by `CleanupStoryDispatch` / `Core.ClearSlot` |
| StorageUtil on the ambusher | `Intel_StoryLastPicked` (float, game time) | the 24-hour cooldown stamp; written **before** the ambush plays out |
| StorageUtil on the ambusher | `Intel_Facts` / `Intel_FactTimes` / `Intel_FactsRendered` | motive + "confronted…" + "yielded, begging for mercy" |
| StorageUtil on the player | `Intel_RecentStoryEvents`, `Intel_StoryLingerActors`, `Intel_SocialLog_*` | anti-repetition + linger tracking |
| StorageUtil on the quest | `Intel_CooldownActors` (FormList) | so C++ cooldown mirror can be re-warmed on load |
| SkyrimNet memory DB | persistent event: `"<Name> <narration>"` | the durable "grudge" record; survives even the abandon-on-load |
| Actor base record (game save) | `SetEssential(true/false)` | **mutates the ActorBase, not the reference — see §4.5** |
| Actor values | `Aggression`, `Confidence` | **never saved/restored — see §4.6** |

Everything ambush-related lives in Papyrus properties and StorageUtil. The C++ `SlotTracker` co-save
serialization tracks the slot, but no ambush-specific state is in the co-save.

### 3.7 ModEvents

None. Despite the mod's general ModEvent-heavy reputation, the ambush path uses **zero** ModEvents. All
cross-boundary work is direct native calls: `SkyrimNetApi.DirectNarration`,
`SkyrimNetApi.RegisterPersistentEvent`, and IntelEngine's own `IntelEngine.*` natives.

### 3.8 Manual dispatch

`IntelEngine_Core.psc:2305-2306` routes the dashboard Director tab straight into
`StoryEngine.HandleAmbushStalkerDispatch`, explicitly **skipping cooldown and MCM checks** (`:2281`:
*"Director skips cooldown/MCM checks intentionally — it's a manual DM override"*). The Director form offers a
narration field and an optional `sender` (`DirectorTab.jsx:118-121`).

---

## 4. Weaknesses and bugs

Each item is marked **[confirmed]** (read directly in the code / parsed from the ESP) or **[suspected]** (a
mechanism I can trace but could not verify at runtime).

### 4.1 The "civilians cannot ambush" gate is dead code — **[confirmed]**

`NPCIndex::GetEligibleStoryTypes` (`NPCIndex.cpp:1497-1564`) contains the entire per-candidate type gate,
including:

```cpp
// ambush: combat-capable only + hold restriction
if (!isCivilian && PassesHoldRestriction(actor, playerHold, npcIndex->GetHoldRestrictionPolicy("ambush")))
    types.push_back("ambush");
```

A repo-wide search for `EligibleStoryTypes` returns exactly three hits: the definition, its own log line, and the
header declaration. **It is never called.** No `Eligible:` line is emitted into the candidate pool markdown, and
`FactionPolitics::ValidateStoryResponse` (`:2110`+) validates only the MCM toggle and field shapes — it never
looks at the NPC. The only surviving enforcement is the prompt sentence "CIVILIANS cannot ambush."

Consequence: **an LLM that ignores that line will get an innkeeper dispatched to ambush the player**, and the
default hold policy won't stop it. `HoldPolicyAmbush` defaults to 1 = "block civilians *crossing holds*", so a
same-hold civilian passes `PassesHoldRestriction` unconditionally (`:1487`).

### 4.2 The documented behaviour contradicts the code in three places — **[confirmed]**

- `NP/ARCHITECTURE.md:300`: *"only warriors/rogues can ambush"*. False twice over — the (dead) gate is
  `!isCivilian`, which admits `MAGE`, `PRIEST`, and every uppercased CK class string; and the gate never runs.
- `NP/ARCHITECTURE.md:300` also describes a fixed archetype enum ("WARRIOR, MAGE, ROGUE, PRIEST, NOBLE, BARD,
  CIVILIAN"). `ClassifyNPCArchetype` actually passes through arbitrary uppercased class names.
- `NP/docs/ESP_STRUCTURE.md` is stale for everything in this feature. It lists 6 AI packages and no Stalk package;
  it claims aliases have an **"Initially Cleared"** flag (the shipped aliases' `FNAM` is `0x0A`, two bits, and no
  such flag exists in the format); and it claims *"Packages to attach to each Agent Alias"* — the shipped quest
  record contains **no `ALPC`/`PKID` subrecords on any alias at all**. Its "Package Priority Guidelines" table
  presents 100/90/80/70 as if they were CK package fields; they are `ActorUtil.AddPackageOverride` stack
  priorities. Treat this document as fabricated for CK purposes.

### 4.3 "Charge" doesn't charge; "stealth" doesn't start stealthy — **[confirmed]**

The dispatch comment says *"Ambush variety: 50% stealth (stalk package) vs 50% charge (sprint + attack)"*
(`:2696`). There is **no sprint anywhere in the codebase**. `ReapplyTravelPackage` (`:4959`) only special-cases
`"stalker"` and `"ambush"`; because `HandleAmbushStalkerDispatch` sets `ActiveStoryType = "ambush_charge"`
*before* calling `DispatchToTarget` → `ReapplyTravelPackage`, the charge variant silently falls through to
`Core.TravelPackage_Jog`. The "charge" is a jog. `TravelPackage_Run` exists and is never used by ambush.

The mirror-image bug affects the stealth variant. The debug line says
*"dispatched (jog → sneak at 2000u)"* (`:2717`) and the phase-0 comment says *"jog normally until then"*
(`:2290`), but `DispatchToTarget` already called `ReapplyTravelPackage` at `:1739` while
`ActiveStoryType == "ambush"`, so **`TravelPackage_Stalk` (walk speed + always-sneak, 800 u arrive radius) is
installed for the entire journey**. The stealth ambusher walk-sneaks from wherever they were, possibly across a
hold. Phase 0 does nothing except decide when `StartSneaking()` gets called.

Downstream: because walking is slow, the ambush leans hard on the off-screen estimate (capped to 0.25 game hours,
`:1748`) and then on `ImmersiveTeleportToTarget`, which does `MoveTo` **3500 units behind the player in
exteriors**. That is far enough to usually be off-camera but it is a pure "behind the player's facing" offset with
no line-of-sight or occlusion check (`IntelEngine.GetOffsetBehind`), so a player who turns around at the wrong
moment can watch the ambusher materialise.

### 4.4 The sneak phase can deadlock the entire Story DM — **[confirmed]**

`CheckStoryNPCArrival` returns unconditionally at the end of the phase ≥ 1 block:

```papyrus
; :2339-2341
    ; Stay in sneak ? don't fall through to normal arrival check.
    ; Both resolve via detection (IsDetectedBy) or distance threshold.
    return
```

That early return skips **every** safety net below it: stuck detection + leapfrog recovery (`:2468`), the
`MaxTravelDaysConfig` travel timeout (`:2487`), off-screen progress (`:2457`), and all the abort checks
(danger zone, blocked location, hold restriction, player-entered-interior).

The only remaining escape for a phase-1 ambusher is the 300-second sneak timeout at `:2262` — which **also
requires `Core.ShouldReleaseLinger()`**, i.e. the player must be more than 800 units away or the NPC unloaded.

So: an ambusher who gets stuck on terrain between roughly 500 u and 2000 u from the player, while remaining
undetected, never confronts, never times out, and never gets unstuck. `IsActive` stays `true` forever, one of the
five agent slots stays allocated, and because `TickScheduler` gates on `IsActive`, **the entire Story DM stops
dispatching anything** until the player walks 800 units away. This is the single most serious defect in the
feature.

### 4.5 `SetEssential` is applied to the ActorBase and can leak permanently — **[confirmed]**

```papyrus
; :2900
ActiveStoryNPC.GetActorBase().SetEssential(true)
```

Two problems.

**Shared bases.** `GetActorBase()` on a generic actor returns a base record shared by every instance. Candidate
eligibility requires only a display name and `ActorTypeNPC` (`NPCIndex.cpp:930-954`) — a "Whiterun Guard" or any
mod-added generic townsfolk qualifies. Ambushing with one of them makes **every actor sharing that base
essential**, and the flag is written into the save.

**The clear path is unreachable.** `CheckAmbushCombat` has an `IsDead()` branch that clears essential
(`:2933-2941`) — but `CheckStoryNPCArrival` guards the whole function with its own dead check that runs first and
does **not** clear it:

```papyrus
; :2215-2225
If ActiveStoryNPC == None || ActiveStoryNPC.IsDead() || ActiveStoryNPC.IsDisabled()
    ... ClearSlot / RemoveAllPackages / CleanupStoryDispatch
    return
EndIf
```

So `CheckAmbushCombat`'s dead branch is dead code, and any route by which an ambusher dies while flagged essential
(console kill, another mod's damage, script kill, disable) leaves the base essential forever. The load-time
recovery at `:355-360` covers only the case where the save happened during `ambush_combat` *and* the script
properties survived intact.

### 4.6 Aggression and Confidence are never restored — **[confirmed]**

`OnAmbushConfront` sets `Confidence = 4`; the yield sets `Aggression = 0, Confidence = 1`; the timeout and
natural-end paths set `Aggression = 0, Confidence = 2` (`:2903`, `:2925-2926`, `:2952-2953`, `:2970-2971`). None
of them read or store the original values.

This is demonstrably an oversight rather than a design choice, because the *quest courier* path in the same file
does it correctly:

```papyrus
; :3118-3119
StorageUtil.SetFloatValue(npc, "Intel_OrigAggression", npc.GetActorValue("Aggression"))
npc.SetActorValue("Aggression", 0)
```

…and `CleanupStoryDispatch` (`:4906-4910`) restores `Intel_OrigAggression` — a key the ambush path never writes.
Result: a yielded ambusher is permanently left at Aggression 0 / Confidence 1. If that NPC was a guard or a
soldier, they will never initiate combat again for any reason, for the rest of the save.

### 4.7 `ambush_charge` is missing from the exterior-only abort lists — **[confirmed]**

`:2397` and `:2448` abort the dispatch when the player enters an interior for
`road_encounter || stalker || ambush`. `"ambush_charge"` is not in either list, so half of all ambushes will keep
pathing after a player who goes indoors, even though the type was rejected at dispatch for exactly that reason
(`:1515-1522`). Whether that is a bug or an accidental improvement is unclear, but it is undocumented and
asymmetric.

### 4.8 Yield detection is a 3-second poll of a transient state — **[suspected]**

`IsBleedingOut()` is sampled once per `MONITOR_INTERVAL` (3.0 s). An essential actor recovers from bleedout on
its own after a short interval and re-enters combat. If a bleedout begins and ends inside a 3-second window, the
yield is silently missed and the fight continues until the next bleedout happens to line up with a poll. I could
not measure vanilla bleedout duration from the source, so this is a mechanism, not a measured failure.

### 4.9 The yield can fire on a corpse — **[suspected]**

`OnAmbushYield` (`:2960-2986`) clears essential **first**, then stops combat, then restores 50 health, then
narrates. Between `SetEssential(false)` and `RestoreActorValue`, the actor is a non-essential actor at ~0 health
in bleedout with the player mid-swing. A landed hit kills them, and the function proceeds to inject
`"was beaten in combat by X and yielded, begging for mercy"` and fire a narration at a dead actor — there is no
`IsDead()` check anywhere in the function. The flat 50-point restore is also a magic number chosen by feel; for a
high-max-health NPC it is a sliver.

### 4.10 Re-entrancy window in `OnAmbushConfront` — **[suspected]**

`OnAmbushConfront` calls `Utility.Wait(1.5)` (`:2897`) **before** it flips `ActiveStoryType` to `"ambush_combat"`
(`:2907`). During that window the state still reads `"ambush"`. Meanwhile the C++ `ProximityMonitor` watch armed
in `DispatchToTarget` (`:1766`) is still live — it is only disarmed by `Core.ClearSlot`
(`IntelEngine_Core.psc:688`), which has not run — and it fires `OnProximityArrived` at a 150 u threshold on a
150 ms cadence, which calls `CheckStoryNPCArrival`, which for `ActiveStoryType == "ambush"` at ≤ 500 u calls
`OnAmbushConfront` **again**: a second narration and a second `StartCombat`. The watch is single-shot per arm
(`ProximityMonitor.cpp:174`) so this can happen at most once.

This is not speculative as a bug *class* — the author documented the identical failure mode elsewhere in the same
function:

```papyrus
; :2227-2231
; Corrupt state detection: IsActive true but no type means something went wrong
; during concurrent event processing (seen with Sylvi seek_player — FinishArrivalWithLinger
; Utility.Wait re-entry corrupted state). Clean up to prevent tick death.
```

### 4.11 Combat handoff ignores crime, factions, and guards — **[confirmed by absence]**

The ambush path performs no faction manipulation whatsoever. Compare `faction_ambush`, where the C++
`SpawnBattleSoldiers` strips crime factions so the player takes no bounty (`:2759-2766`). For a single-NPC ambush:

- The ambusher assaulting the player inside a hold is a crime; guards will engage the ambusher.
- The player killing the ambusher in a settlement produces a **murder bounty**.
- Guard intervention lands in the `!IsInCombat()` branch (`:2949`), which treats it as an ordinary end and hands
  the NPC to the linger sandbox — no acknowledgement that the story was resolved by a third party.

Given that `IsEligibleStoryCandidate` deliberately selects *non-hostile named NPCs*, and the DM prompt happily
picks NPCs in towns (ambush is not exterior-restricted to *wilderness*, only to exteriors), town ambushes are a
likely common case.

### 4.12 The stalker exits sneak more thoroughly than the ambusher — **[confirmed]**

`OnStalkerDetected` does both halves of the sneak exit:

```papyrus
; :2996-2999
If ActiveStoryNPC.IsSneaking()
    ActiveStoryNPC.StartSneaking()
EndIf
Debug.SendAnimationEvent(ActiveStoryNPC, "sneakStop")
```

`OnAmbushConfront` (`:2888-2890`) omits the `sneakStop` animation event. The sneak-timeout path (`:2265-2268`)
includes it. Only the confront path — the one the player actually watches — is missing it, so the ambusher may
enter combat still in the crouch animation.

### 4.13 `StartSneaking()` is a toggle applied to an already-sneaking actor — **[suspected]**

`Actor.StartSneaking()` toggles rather than sets — the author's own code relies on that at `:2265-2267` and
`:2888-2890` to exit sneak. But `:2293` calls `StartSneaking()` to *enter* sneak on an actor already running the
always-sneak Stalk package (see §4.3). If the package flag makes `IsSneaking()` true, that call toggles sneak
**off** at exactly the 2000 u mark. I could not confirm whether the always-sneak package flag sets the actor's
sneak state as observed by Papyrus, so this is a mechanism, not a confirmed failure — but it is a direct
consequence of §4.3 and would not exist if the package selection matched the comment.

### 4.14 Dead constant — **[confirmed]**

`Int Property AMBUSH_STANDING_PENALTY = -3 AutoReadOnly` (`:182`) has no readers anywhere in the repo. A faction
standing consequence for ambushes was evidently planned and never wired up.

### 4.15 Save/load abandonment burns the cooldown — **[confirmed]**

`ApplyCooldownCheck` (`:1094`) stamps `Intel_StoryLastPicked = now` **at dispatch time**, before the ambush plays
out. `RestartMonitoring` (`:352-370`) abandons any in-flight ambush on load. So a player who saves during the
stalk and reloads — or who crashes — loses the ambush entirely, *and* the same NPC is now locked out for 24 game
hours. The SkyrimNet motive memory persists, so the DM may re-pick them eventually, but the specific staged
encounter is gone with no retry.

### 4.16 `ClearPackageOverride` collateral — **[confirmed]**

`ReapplyTravelPackage` calls `Core.RemoveAllPackages` = `ActorUtil.ClearPackageOverride`
(`IntelEngine_Core.psc:1267`), which removes **all** package overrides on the actor, including those installed by
other mods (SkyrimNet, Nether's Follower Framework, etc.). The codebase has a narrower
`RemoveIntelPackages` (`:1273`) specifically to avoid this, and uses it for follower-owned actors — the ambush
path does not.

### 4.17 Narrower observations

- **Anti-repetition is recorded at the wrong moment.** `AddRecentStoryEvent("ambush: …")` fires inside
  `OnAmbushConfront` (`:2909`). An ambush that times out during the sneak leaves nothing in the 8-entry ring
  buffer, so the DM has no signal that it tried and failed.
- **The `sender` is unvalidated.** `:2685-2694` resolves the LLM-supplied sender name via `FindNPCByName` and
  injects "sent X to ambush Y" onto whoever matches — with no check that the sender plausibly knows the ambusher,
  is alive, or is anywhere nearby. A hallucinated or fuzzy-matched name writes a permanent fact onto an innocent
  bystander.
- **`ambush_charge` defers arrival during unrelated combat.** `:2359-2361` returns early from arrival whenever
  `player.IsInCombat()`. A charging ambusher will jog up to a player fighting a wolf and stand there.
- **`SNEAK_TIMEOUT_SECONDS` does double duty.** The same 300 s constant is the sneak-approach timeout *and* the
  combat timeout (`:2920`) — two unrelated situations sharing one magic number.
- **No player-facing yield affordance.** The yield produces narration + a fact and nothing else: no dialogue
  topic, no `SetRelationshipRank` change, no bounty/standing effect (see §4.14), no state that would let a later
  system know this specific NPC once yielded to the player. Everything downstream depends on SkyrimNet noticing
  the injected fact, which is itself FIFO-evicted after 10 facts.
- **Nothing prevents an ambush from being dispatched in a dangerous location.** `BuildExcludeList` excludes only
  `informant` for `isDangerous` (`FactionPolitics.cpp:2097-2099`), so the DM may send an ambusher into a dungeon
  approach or dragon-lair exterior — combined with §4.4, prime deadlock territory.

---

## 5. What I could not determine

- Runtime behaviour of `IsSneaking()` under an always-sneak package (§4.13), vanilla essential-bleedout duration
  versus the 3-second poll (§4.8), and whether the `OnAmbushConfront` re-entrancy window (§4.10) actually fires
  in practice. All three need in-game testing, not source reading.
- Whether IntelEngine's five-slot `ProximityMonitor` and the Story DM's single-dispatch model ever produced the
  deadlock in §4.4 for real players — no bug tracker or changelog entry mentions it
  (`GP/CHANGELOG.md` was not exhaustively searched for this).
