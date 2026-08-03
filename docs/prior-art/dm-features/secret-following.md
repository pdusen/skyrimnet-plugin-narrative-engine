# IntelEngine Prior Art — Secret Following (the `stalker` story type)

> **Reading discipline.** This is a reconstruction of how *IntelEngine* built one feature, written from its source.
> IntelEngine was abandoned mid-development; nothing here is a verified-correct pattern. Every substantive claim
> below cites a file and line so it can be checked. See [`../README.md`](../README.md) for the full discipline.

The README feature bullet this document covers:

> - Secretly follow you out of obsession until caught

In the code this is the Story-DM story type `stalker`. It shares almost all of its plumbing with the `ambush`
story type — both are handled by one dispatch function and one monitoring branch — so the two are described
together wherever they are literally the same code.

**Scope note.** There is no separate stalker subsystem, no C++ module, no SkyrimNet action, and no dedicated ESP
quest. The whole feature is ~120 lines of Papyrus inside `IntelEngine_StoryEngine.psc`, one AI package record,
two `StorageUtil` keys, and about six lines of an LLM prompt.

**Paths.** File names below are given bare; they resolve against the two IntelEngine repos as follows.

- `IntelEngine_*.psc` → `C:\Projects\IntelEngine-NativePlugin\Source\Scripts\`
- `*.cpp` / `*.h` → `C:\Projects\IntelEngine-NativePlugin\SKSE\src\`
- `docs/*.md`, `ARCHITECTURE.md`, `README.md`, `web/` → `C:\Projects\IntelEngine-NativePlugin\`
- `IntelEngine.esp`, `SKSE/Plugins/SkyrimNet/**` → `C:\Projects\IntelEngine-GamePlugin\`

---

## 1. High-level overview, step by step

1. **The Story DM tick fires.** `IntelEngine_StoryEngine.TickScheduler()` runs off a shared game-time timer,
   self-gated to the story interval (default **3 game hours**, `IntelEngine_Core.psc:1696-1698`). It refuses to
   run while another story dispatch is in flight (`IsActive`, `IntelEngine_StoryEngine.psc:655-667`), with a
   6-game-hour C++ watchdog to clear a stuck flag.

2. **C++ builds a candidate pool.** `NPCIndex` walks loaded actors, filters them
   (`NPCIndex::IsEligibleStoryCandidate`, `NPCIndex.cpp:1014-1027` — not dead, not in combat, not a teammate,
   not hostile, **not in the player's cell**), and renders a markdown pool with each NPC's bio, archetype,
   distance, "Knows player" familiarity, memories and recent events.

3. **Per-candidate type eligibility is computed in C++.** `NPCIndex::GetEligibleStoryTypes`
   (`NPCIndex.cpp:1497-1564`) decides which story types each candidate may be used for. For `stalker` the gate is
   **exterior only, archetype != `CIVILIAN`, and the stalker hold-restriction policy**
   (`NPCIndex.cpp:1545-1548`).

4. **One LLM call chooses who and what.** `intel_story_dm.prompt` is rendered with the pool and the enabled type
   list, and returns a single JSON object. The stalker shape is exactly:

   ```json
   {"should_act":true,"type":"stalker","npc":"Name","narration":"was caught following <Player> out of jealousy"}
   ```

   (`SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt:259-261`.) Note the narration is written **for the
   moment of being caught**, not for the following.

5. **Papyrus re-validates.** `OnDungeonMasterResponse` rejects the pick if the NPC is missing/dead/disabled,
   high-status or a Jarl, fails the hold restriction, is on the per-NPC story cooldown (default **24 game
   hours**), or if the **player is currently in an interior** (`IntelEngine_StoryEngine.psc:1470-1522`). Each
   rejection is written into a dispatch-history ring buffer that is fed back to the DM next tick as
   `[REJECTED — reason]`.

6. **Motive is written into the world as memory, not as state.** `HandleAmbushStalkerDispatch`
   (`IntelEngine_StoryEngine.psc:2674-2724`) registers a SkyrimNet persistent event on the stalker, injects the
   narration as a bio "fact", and — if the DM named a `sender` — injects a fact on that third NPC that they
   *"encouraged &lt;stalker&gt; to follow &lt;player&gt;"*. There is no obsession score, flag, keyword or faction.

7. **Dispatch.** `DispatchToTarget` (`IntelEngine_StoryEngine.psc:1673-1769`) takes one of five task slots, puts
   the NPC in the quest's `AgentAliasNN` alias and in `IntelEngine_TaskFaction`, sets the player as the actor's
   linked ref under the `IntelEngine_TravelTarget` keyword, and applies the stalk AI package as a package
   override at priority 100. It also arms the 150 ms C++ `ProximityMonitor` for the slot and starts a 3-second
   Papyrus poll.

8. **Sneak phase is set immediately.** Unlike ambushers (who jog until 2000 units out), stalkers start at
   `Intel_SneakPhase = 1` with the sneak package already applied (`IntelEngine_StoryEngine.psc:2709-2718`), and
   `Intel_SneakStartTime` is stamped with real time.

9. **Getting there.** If the stalker is not 3D-loaded, the normal off-screen travel estimate runs, capped at
   **0.25 game hours** for player-targeted stories (`IntelEngine_StoryEngine.psc:1747-1757`). When the estimate
   elapses, `ImmersiveTeleportToTarget` **moves the NPC to 3500 units behind the player**
   (`:2503-2514`, `TELEPORT_OFFSET_EXTERIOR = 3500`) and re-applies the stalk package.

10. **The follow loop.** Every 3 seconds, while the stalker is 3D-loaded, `CheckStoryNPCArrival` runs a
    stalker-specific branch (`IntelEngine_StoryEngine.psc:2258-2344`):

    - **caught check** — `IsDetectedBy(player)` **or** raw distance ≤ 400 units → `OnStalkerDetected()`;
    - **hold** — phase 1 and distance ≤ `STALKER_KEEP_DISTANCE` (800) → strip all package overrides, phase 2;
    - **resume** — phase 2 and distance > `STALKER_RESUME_DISTANCE` (1000) → re-apply the stalk package, phase 1;
    - **give up** — `SNEAK_TIMEOUT_SECONDS` (300 real seconds) elapsed *and* the player is more than 800 units
      away → stop sneaking, clear the slot, strip packages, and print an on-screen notification.

    The branch `return`s unconditionally once phase ≥ 1 (`:2341`), so none of the generic travel logic below it
    runs for a loaded stalker.

11. **Caught.** `OnStalkerDetected` (`:2989-3017`) exits sneak, injects the fact *"was caught secretly following
    &lt;player&gt;"*, fires the DM's narration through SkyrimNet's `DirectNarration` (which is what makes the NPC
    speak), registers a persistent memory, logs an anti-repetition entry, and hands off to the shared
    `FinishArrivalWithLinger` path.

12. **Confrontation = linger.** There is no bespoke confrontation scene. The caught NPC is given the tight
    200-unit sandbox package linked to their own position and added to the linger list
    (`:1950-1977`). SkyrimNet's normal dialogue drives the scene, informed by the injected fact. The linger ends
    after **300 real seconds** or as soon as the player walks more than **800 units** away (after a 30-second
    grace), at which point `Core.ReleaseLinger` sends them home (`:2033-2042`, `IntelEngine_Core.psc:946-990`).

13. **Teardown.** `CleanupStoryDispatch` (`:4892-4940`) clears `Intel_SneakPhase`, `Intel_SneakStartTime`,
    `Intel_IsStoryDispatch` and `Intel_StoryNarration`, releases the story lock and restarts the game-time
    scheduler. `Core.ClearSlot` empties the alias, removes the task faction, clears linked refs, strips package
    overrides and disarms the proximity monitor (`IntelEngine_Core.psc:611-690`).

There is **no flee phase** — see §4.

---

## 2. Intended gameplay experience

Read from the prompt text, the README, and the tuning constants, the intent is fairly legible:

- **A quiet, non-combat dread beat.** The README pitch is explicit: *"Stalker — Romantically obsessed NPCs
  secretly follow you until caught. No combat — the emotional confrontation is the payoff."*
  (`IntelEngine-NativePlugin/README.md:373`). It is deliberately positioned as the counterpart to `ambush`,
  which uses the same sneak machinery but ends in a fight and a yield.

- **The payoff is the awkward moment, not the surveillance.** The prompt instructs the DM: *"Narration fires at
  the moment {{ playerName }} CATCHES the stalker. Write the confrontation, not the hiding."*
  (`intel_story_dm.prompt:93`). The entire authored content of the feature is one sentence that plays at the
  end. The following itself has no beats, no escalation, no player-facing feedback.

- **Motive is supposed to come from real relationship history.** *"NPC MUST have romantic feelings, jealousy, or
  obsessive curiosity in their memories"* (`intel_story_dm.prompt:91`), and Rule 5 of the DM prompt makes the
  candidate's memories the only permitted source of truth (`:193`). The design point is that the stalker should
  be someone the player actually built a relationship with, so the confrontation lands.

- **Outdoors, at a distance, on foot.** 800 units hold / 1000 units resume / 400 units auto-catch, exterior-only,
  walk pace. The mental model is clearly "someone trailing you down a road at the edge of visibility", not
  "someone tailing you through Riften's alleys" — interiors are rejected outright at dispatch
  (`IntelEngine_StoryEngine.psc:1514-1522`) with the stated reason *"interiors are too small for sneak
  gameplay"*.

- **Third-party complicity as flavour.** The optional `sender` field lets the DM say someone *encouraged* the
  stalking, and that NPC gets a fact about it (`:2691`), so the player can later confront the instigator too.
  The dashboard exposes this as an "Encouraged by" field (`web/dashboard/src/components/DirectorTab.jsx:123-126`).

- **Rarity.** A 24-game-hour per-NPC cooldown, a 3-game-hour DM interval, a single global story slot, and a DM
  prompt that repeatedly says rejecting is the normal outcome (`intel_story_dm.prompt:187`) all point at this
  being a once-in-a-while moment rather than a recurring mechanic.

What the tuning does **not** show any sign of: an escalation arc, repeat stalking by the same NPC, a "you are
being watched" ambient signal, any way for the player to catch the stalker deliberately, or any consequence
system after the confrontation beyond an injected memory.

---

## 3. Implementation breakdown

### 3.1 Where the code lives

| Layer | File | Role |
| --- | --- | --- |
| Papyrus (all logic) | `Source/Scripts/IntelEngine_StoryEngine.psc` | Dispatch, follow loop, catch, teardown |
| Papyrus (shared) | `Source/Scripts/IntelEngine_Core.psc` | Slots, packages, linger, narration/memory helpers |
| C++ candidate gating | `SKSE/src/NPCIndex.cpp` | Eligibility, archetype, hold restriction, DM context |
| C++ validation | `SKSE/src/FactionPolitics.cpp` | `BuildExcludeList`, `ValidateStoryResponse` |
| C++ arrival fast path | `SKSE/src/ProximityMonitor.h/.cpp` | 150 ms proximity watch on the slot |
| Prompt | `SKSE/Plugins/SkyrimNet/prompts/intel_story_dm.prompt` | The only authored text in the feature |
| ESP | `IntelEngine.esp`, `IntelEngine_TravelPackage_Stalk` | The one bespoke record |
| UI | `web/dashboard/src/components/{StoryTab,DirectorTab}.jsx`, `IntelEngine_MCM.psc` | Toggles + manual dispatch |

There is **no** `Stalker.cpp`, no stalker Papyrus script, and **no SkyrimNet action YAML** — the feature cannot be
triggered by an NPC through dialogue, only by the DM tick or the dashboard Director tab
(`IntelEngine_Core.psc:2305-2306`).

### 3.2 The ESP surface (values read directly from the binary record)

The `.esp` is binary, and the author's own `docs/ESP_STRUCTURE.md` does not mention the stalk package at all, so
the following was decoded from `IntelEngine-GamePlugin/IntelEngine.esp` rather than taken from the docs.

```text
PACK 02003DE6  IntelEngine_TravelPackage_Stalk
  PKDT flags     = 0x00022040   (Walk/Jog/Run are 0x00002040 — the delta is bit 0x00020000)
  PKDT type      = 18 (package), interrupt override = 0 (None), preferred speed = 0 (walk)
  PKCU template  = 00016FAA  (vanilla Skyrim.esm "Travel" package template), 3 data inputs
  PLDT           = type 6, data = 02000D63 (KYWD IntelEngine_TravelTarget), radius = 0x320 = 800
  PSDT           = month -1, day -1, hour -1, minute -1, duration 0  (unscheduled, runs indefinitely)
  CTDA           = none (zero conditions)
```

Corroboration and caveats:

- The **0x00020000** delta versus the three plain travel packages is the flag the author's property comment calls
  *"Always Sneak"* (`IntelEngine_Core.psc:77-78`). The bit position matches the commonly documented Skyrim
  `PKDT` "Always Sneak" flag, and it is the only difference between the stalk package and the walk package, so
  the claim is well-supported — but I could not check it against an authoritative CK/xEdit definition, so treat
  the *name* as probable rather than confirmed. The *behavioural* claim (it differs from Walk only in this one
  flag and stops at 800 units) is confirmed.
- `00016FAA` resolves to `Skyrim.esm` package `Travel` in the local Spriggit export
  (`spriggit-output/Skyrim/Packages/Travel - 016FAA_Skyrim.esm.yaml`) — confirmed.
- The **800-unit arrive radius** the property comment claims is confirmed by the `PLDT` radius field.
- The package has **no conditions** and is applied purely via `ActorUtil.AddPackageOverride`. This contradicts
  `docs/ESP_STRUCTURE.md:51-54`, which claims IntelEngine's travel packages carry `GetLinkedRef` conditions and
  are attached to alias package lists. Zero `CTDA` subrecords exist on any of the twelve packages in the plugin.

Everything else the stalker uses is shared infrastructure, not stalker-specific:

- **Quest** `02000D61 IntelEngine`, aliases `PlayerAlias`, `AgentAlias00..04`, `TargetAlias00..04`, `QuestTarget`.
- **Keyword** `02000D63 IntelEngine_TravelTarget` (linked-ref keyword the package points at).
- **Faction** `02000D62 IntelEngine_TaskFaction` (agents are added on slot allocation, removed on clear).
- **Globals** `IntelEngine_StoryEngineEnabled / Interval / Cooldown`.
- There is **no** stalker keyword, faction, global or quest stage. The whole plugin contains 12 packages,
  6 keywords, 6 globals, 3 factions and 2 quests.

### 3.3 Selection and motive

Motive is never modelled. It is asserted by the LLM and then written into SkyrimNet as text:

```papyrus
; IntelEngine_StoryEngine.psc:2678-2694
Core.SendPersistentMemory(npc, npc, npc.GetDisplayName() + " " + narration)
Core.InjectFact(npc, narration)
If senderName != ""
    ...
    Core.InjectFact(senderNPC, "encouraged " + npc.GetDisplayName() + " to follow " + playerName)
```

`SendPersistentMemory` calls SkyrimNet's `RegisterPersistentEvent`; `InjectFact` pushes onto a FIFO of at most 10
per-NPC facts in `StorageUtil` that a bio submodule renders into the NPC's dialogue context
(`IntelEngine_Core.psc:1466-1516`).

Candidate gating, in order:

| Gate | Where | Effect |
| --- | --- | --- |
| Not in player's cell, not hostile, not a teammate, not in combat | `NPCIndex.cpp:1014-1027` | Pool membership |
| Exterior + `archetype != CIVILIAN` + hold policy | `NPCIndex.cpp:1545-1548` | `stalker` in eligible-type list |
| MCM toggle bit 4 | `IntelEngine_StoryEngine.psc:736-738`, `FactionPolitics.cpp:2065` | Type shown to DM at all |
| Player in interior | `FactionPolitics.cpp:2091-2096`, `StoryEngine.psc:1514-1522` | Excluded / rejected |
| High-status or Jarl | `IntelEngine_StoryEngine.psc:1484-1488`, `1544-1550` | Rejected |
| Per-NPC story cooldown, default 24 game hours | `IntelEngine_StoryEngine.psc:1078-1099` | Rejected |

`ClassifyNPCArchetype` (`NPCIndex.cpp:1336-1390`) maps only a fixed list of CK class names — citizen, farmer,
beggar, child, bard's college, food vendor, peddler, apothecary, blacksmith, fence, innkeeper, lumberjack, miner,
vendor — to `CIVILIAN`; every other class name is passed through uppercased. `ValidateStoryResponse`
(`FactionPolitics.cpp:2110+`) has no stalker-specific field validation at all.

### 3.4 The follow loop, in numbers

All constants are `AutoReadOnly` properties at the top of `IntelEngine_StoryEngine.psc`:

| Constant | Value | Line | Used for |
| --- | --- | --- | --- |
| `MONITOR_INTERVAL` | 3.0 s | 36 | Papyrus poll cadence for the whole story engine |
| `STALKER_KEEP_DISTANCE` | 800 units | 41 | Phase 1 → 2 (stop closing) |
| `STALKER_RESUME_DISTANCE` | 1000 units | 42 | Phase 2 → 1 (resume following) |
| `SNEAK_TIMEOUT_SECONDS` | 300 real s | 43 | Give-up timer |
| (hard-coded) | 400 units | 2320 | "Caught" proximity failsafe |
| `TELEPORT_OFFSET_EXTERIOR` | 3500 units | 143 | Off-screen catch-up teleport |
| `LINGER_TIMEOUT_SECONDS` | 300 real s | 140 | Post-catch confrontation window |
| `Core.LINGER_RELEASE_DISTANCE` | 800 units | `Core.psc:134` | Walk-away ends the confrontation |
| `Core.ARRIVAL_DISTANCE` | 300 units | `Core.psc:113` | Generic arrival (bypassed for stalkers) |
| `ProximityMonitor` | 150 ms / 150 units | `ProximityMonitor.h:43-46` | C++ fast-path arrival callback |

The core of the loop:

```papyrus
; IntelEngine_StoryEngine.psc:2318-2337
Else
    ; Stalker: detected by engine stealth system OR proximity failsafe (<400u)
    If ActiveStoryNPC.IsDetectedBy(player) || sneakDist <= 400.0
        OnStalkerDetected()
        return
    EndIf

    ; Phase 1 (following): hold position when too close
    If sneakPhase == 1 && sneakDist <= STALKER_KEEP_DISTANCE
        Core.RemoveAllPackages(ActiveStoryNPC, false)
        StorageUtil.SetIntValue(ActiveStoryNPC, "Intel_SneakPhase", 2)
    EndIf

    ; Phase 2 (holding): resume following when player moves away
    If sneakPhase == 2 && sneakDist > STALKER_RESUME_DISTANCE
        ReapplyTravelPackage(ActiveStoryNPC)
        StorageUtil.SetIntValue(ActiveStoryNPC, "Intel_SneakPhase", 1)
    EndIf
EndIf
```

"Caught" therefore has exactly two triggers: the engine's own actor-detection result, and a raw 3D distance
threshold. There is **no line-of-sight test, no facing/FOV test, no light-level test, and no minimum stalk
duration** anywhere in the codebase (a repo-wide search for `HasLOS` / `LineOfSight` returns nothing).

### 3.5 The catch and confrontation

```papyrus
; IntelEngine_StoryEngine.psc:2989-3016 (abridged)
Function OnStalkerDetected()
    {Stalker detected by player: stop sneaking, narrate caught, linger for dialogue.
    No flee phase — TranslateTo ignores navmesh and breaks immersion.}
    If ActiveStoryNPC.IsSneaking()
        ActiveStoryNPC.StartSneaking()          ; relied on as a toggle-off
    EndIf
    Debug.SendAnimationEvent(ActiveStoryNPC, "sneakStop")
    Core.InjectFact(ActiveStoryNPC, "was caught secretly following " + playerName)
    Core.SendTaskNarration(ActiveStoryNPC, ActiveNarration, Game.GetPlayer())
    Core.SendPersistentMemory(ActiveStoryNPC, Game.GetPlayer(), \
        npcName + " was caught secretly following " + playerName)
    AddRecentStoryEvent("stalker: " + npcName + " -- caught")
    FinishArrivalWithLinger(caughtNPC, Game.GetPlayer() as ObjectReference)
EndFunction
```

`SendTaskNarration` is `SkyrimNetApi.DirectNarration(msgText, actor, target)` (`IntelEngine_Core.psc:1460-1464`) —
that is the mechanism that turns the DM's one authored sentence into spoken dialogue. Everything the player
experiences as "the confrontation" is SkyrimNet improvising from the injected fact plus that narration, inside a
200-unit sandbox that lasts at most five real minutes.

### 3.6 Persisted state

| Where | Key / field | Survives save? | Notes |
| --- | --- | --- | --- |
| `StorageUtil` on the NPC | `Intel_SneakPhase` (int) | Yes | 0/1/2; cleared by `CleanupStoryDispatch` |
| `StorageUtil` on the NPC | `Intel_SneakStartTime` (float) | Yes | **Real** time, clamped on load at `:1218-1227` |
| `StorageUtil` on the NPC | `Intel_IsStoryDispatch`, `Intel_StoryNarration` | Yes | Feeds the task-aware submodule |
| `StorageUtil` on the NPC | `Intel_StoryLastPicked` (game time) | Yes | 24-hour story cooldown |
| `StorageUtil` on the player | `Intel_StoryLingerActors` (FormList) | Yes | Post-catch linger, released on load |
| `StorageUtil` on the player | `Intel_RecentStoryEvents` (last 8) | Yes | Anti-repetition, fed back to the DM |
| SkyrimNet | persistent event + up to 10 `Intel_Facts` | Yes | The only record of the obsession |
| Quest properties | `IsActive`, `ActiveStoryNPC`, `ActiveStoryType` | Yes | Abandoned on load (`:344-382`) |
| C++ `SlotTracker` | slot state | Yes (SKSE co-save) | Mirror for SkyrimNet decorators |

The in-flight stalk itself is explicitly **not** save-safe: on `OnPlayerLoadGame`, any dispatch whose
`ActiveStoryType` is `ambush`/`ambush_charge`/`ambush_combat`/`stalker` is torn down with the comment *"Sneak /
combat / charge phase can't be recovered reliably — abandon, let NPC go home"*
(`IntelEngine_StoryEngine.psc:352-370`).

### 3.7 Configuration surface

- **MCM**: an on/off toggle *"Stalker"* and a seven-way hold-restriction radio group
  (`IntelEngine_MCM.psc:497`, `:656-660`), synced to `HoldPolicyStalker` (default **1** = "same hold, civilians
  blocked", `IntelEngine_StoryEngine.psc:76`) and pushed to C++ via `SetHoldRestrictionPolicy("stalker", ...)`.
- **Dashboard**: the Story tab toggle (`StoryTab.jsx:9`) and a Director tab manual dispatch with an optional
  "Encouraged by" field (`DirectorTab.jsx:123-126`). Director dispatch deliberately skips cooldown and MCM checks
  (`IntelEngine_Core.psc:2280-2306`).
- No INI setting, no per-type distance/timeout tuning. Every distance and timer above is a compiled constant.

---

## 4. Weaknesses and bugs

### Confirmed by reading the code

1. **The advertised flee behaviour does not exist.** The DM prompt tells the LLM *"The NPC sneaks and hides. When
   detected, they flee — NO combat"* (`intel_story_dm.prompt:92`) and `ARCHITECTURE.md:649` describes the type as
   *"sneaks, flees when caught"*. The implementation's own docstring says the opposite: *"No flee phase —
   TranslateTo ignores navmesh and breaks immersion"* (`IntelEngine_StoryEngine.psc:2990-2991`). The LLM is
   therefore being briefed on a behaviour the engine will not produce, and may narrate a flight that never
   happens.

2. **Civilians can never be stalkers, which is the opposite of the stated design.** The eligibility gate is
   `!interior && !isCivilian` (`NPCIndex.cpp:1546`), so exactly the archetype the prompt asks for — the innkeeper,
   the farmer, the vendor with romantic feelings — is excluded, while guards, soldiers, mages and bandits are
   eligible. The author's own MCM comment for the type reads *"Same hold (civilians) — stalkers need proximity"*
   (`IntelEngine_StoryEngine.psc:76`), showing the intent was the reverse.

3. **The default hold policy for stalkers is a no-op.** Policy 1 means "block civilians from a different hold"
   (`NPCIndex.cpp:1493`), but civilians are already excluded from `stalker` entirely. The default setting
   therefore does nothing; the MCM presents a knob with no effect at its default value.

4. **The stalker walks while the player runs.** The stalk package's preferred speed is 0 (walk) — verified in the
   `PKDT` — with no speed variant and no catch-up path for a loaded actor. A player who is running, sprinting or
   mounted outpaces the follower permanently, so the distance grows monotonically until the 300-second timer
   ends the story. In practice the modal outcome of a stalker dispatch is "gave up", not "caught".

5. **Giving up broadcasts the secret the feature exists to keep.** The timeout path calls
   `Core.NotifyPlayer("Story: " + name + " gave up")` (`IntelEngine_StoryEngine.psc:2264`), and `NotifyPlayer` is
   an unconditional `Debug.Notification` — it is *not* gated on debug mode (`IntelEngine_Core.psc:1637-1640`).
   The player gets a corner message naming an NPC who was secretly following them, having never seen them. It
   also leaks the internal word "Story".

6. **"Caught" fires without the player being able to see anything.** The 400-unit failsafe
   (`IntelEngine_StoryEngine.psc:2320`) triggers regardless of facing, line of sight, cover, darkness or whether
   the player is in a menu. A stalker that closes to 400 units behind the player's back will stand up and
   announce *"was caught following you"* to the back of the player's head. There is also no minimum stalk
   duration, so a stalker can be "caught" seconds after being teleported in.

7. **A loaded stalker skips every abort and recovery check.** The stalker branch `return`s at `:2341` before the
   code that handles: player entering a blocked or non-whitelisted location (`:2363-2377`), danger-zone /
   player-home / hold-restriction aborts (`:2381-2391`), the exterior-only abort when the player goes indoors
   (`:2393-2404`), and stuck detection with soft recovery (`:2467-2479`). Consequences:
   - The *"lost track of them after they went inside and gave up"* narration only ever fires on the **off-screen**
     branch (`:2444-2455`). A loaded stalker follows the player into an inn or a dungeon — exactly the situation
     the interior gate at dispatch was written to prevent — or wedges itself against the door.
   - A stalker stuck on terrain simply stands there until the 300-second timeout; no leapfrog, no teleport.

8. **Fast travel teleports the stalker to your destination.** There is no fast-travel handling anywhere in either
   repo (a repo-wide search for `FastTravel` returns zero hits). After a fast travel the stalker is unloaded, so
   the sneak branch (gated on `Is3DLoaded()` at `:2259`) is skipped entirely and control falls through to the
   off-screen path, which teleports the NPC 3500 units behind the player when the estimate elapses
   (`:2456-2464`, `:2503-2514`). The NPC that was following you across Whiterun Hold reappears behind you at
   Solitude.

9. **The phase-2 "hold" is a hard stop into base AI, not a hide.** `Core.RemoveAllPackages(ActiveStoryNPC, false)`
   (`:2327`) clears **all** package overrides — including any from other mods, which the function's own comment
   warns about (`IntelEngine_Core.psc:1261-1266`) — passes `evaluate = false` so no `EvaluatePackage()` is
   issued, and adds nothing in their place. Because the Always Sneak flag lived on the package, the "hidden"
   follower stands up out of sneak at 800 units and reverts to their normal schedule until the player moves
   1000+ units away. The observable result is an NPC who stops, stands, wanders off, then starts trailing again.

10. **Every in-flight stalk dies on save/load with no fiction attached.** `:352-370` abandons the dispatch, strips
    packages and clears the slot. Nothing is narrated, no fact is injected, and the player is not told. The NPC is
    left standing wherever they were — potentially 800 units behind the player in open country — with their
    normal AI resuming. Only the original motive memory survives.

11. **Dead branch.** `If ActiveStoryType == "stalker" && sneakPhase == 0` (`:2283-2288`) is documented as the
    save/load recovery path, but a stalker's phase is set to 1 at dispatch (`:2712`) and the load path abandons
    the dispatch before this code can run. It can never execute for a stalker.

12. **`OnStoryNPCArrived` has no stalker case.** `:2520-2551` branches for `ambush`, `ambush_charge`, `message`,
    `quest` and NPC-to-NPC, then falls through to the generic seek_player narration. If that path is ever reached
    for a stalker, the confrontation line plays but the *"was caught secretly following"* fact and persistent
    memory are never injected, so the NPC's dialogue has no idea why it is talking. (Reaching it requires the
    stalker to be within 300 units while not 3D-loaded, so this is an edge case rather than a routine failure —
    but there is no guard preventing it.)

13. **Exiting sneak relies on an undocumented toggle.** `If npc.IsSneaking(): npc.StartSneaking()` (`:2996`)
    assumes `StartSneaking()` toggles rather than sets, backed up by a raw `Debug.SendAnimationEvent(npc,
    "sneakStop")`. Neither is an authoritative API contract; if the assumption is wrong the NPC holds the
    confrontation conversation crouched.

14. **The author's CK documentation does not describe this feature at all.** `docs/ESP_STRUCTURE.md` claims the
    plugin has "6 AI Packages" (`:9`) and lists none of the sneak, listen, paused-sandbox or stalk packages; the
    binary has 12. It also claims packages carry `GetLinkedRef` conditions and are attached to alias package
    lists (`:51-54`); the binary shows zero conditions on any package, and application is entirely via
    `ActorUtil.AddPackageOverride`. Any CK claim in that document should be re-derived from the `.esp`.

15. **The whole story engine is blocked for the duration of a stalk.** `IsActive` is a single global lock
    (`:655-667`), so a stalker occupying up to five real minutes prevents every other DM story from dispatching,
    with only a 6-game-hour watchdog as a backstop.

16. **Constants chosen by feel.** 400 / 800 / 1000 / 3500 / 300 s appear only as compiled `AutoReadOnly`
    properties with no derivation, no comment justifying the values, and no MCM exposure. 400 is a bare literal
    inline at `:2320` rather than a named constant, unlike its ambush counterpart `AMBUSH_CONFRONT_DISTANCE`.

### Suspected (not verifiable from source alone)

- **`IsDetectedBy` probably fires almost immediately for most stalkers.** `Actor.IsDetectedBy` reflects the
  engine's detection calculation, which is dominated by the sneaking actor's Sneak skill, distance, light and
  LOS. The DM's eligible candidates are explicitly non-civilians (guards, soldiers, mages) with no Sneak-skill
  requirement anywhere in the gating, so a low-Sneak NPC crouching in daylight 800 units in front of the player
  is likely "detected" on the first or second 3-second poll. If so, the intended slow-burn follow collapses into
  an immediate confrontation. This needs in-game measurement to confirm.

- **Package priority contention during the hold.** With all overrides stripped and no `EvaluatePackage()` call,
  the exact moment the NPC abandons the travel behaviour is left to the engine's own package re-evaluation
  cadence. The visible symptom (a delay before the NPC visibly stops) is plausible but unmeasured.

- **Task-awareness leakage.** While the stalk is in flight, `Intel_TaskType = "story"` and `Intel_StoryNarration`
  are live, and the bio submodule renders *"I am currently on my way to find &lt;Player&gt; — &lt;narration&gt;. To
  stop this task, use the CancelCurrentTask action."*
  (`prompts/submodules/character_bio/0801_intel_task_awareness.prompt:34`). Since the narration is written as the
  *caught* line, any dialogue involving the stalker before the catch — including SkyrimNet NPC-to-NPC chatter —
  could surface the secret and a mod-mechanical instruction. Both keys are cleared during the catch, so this only
  affects the pre-catch window; whether that window is reachable in practice depends on the detection timing
  above.

- **Cost.** The 3-second Papyrus poll is shared with the whole story engine, but the C++ `ProximityMonitor`
  (150 ms, main-thread marshalled) stays armed for the entire stalk even though the stalker branch never uses its
  callback for anything the 3-second poll does not already catch. Negligible in isolation; wasteful in principle.
