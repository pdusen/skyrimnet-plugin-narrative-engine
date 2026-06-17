# IntelEngine — `.esp` / Creation Kit Setup

> **Reading discipline.** This describes the forms **IntelEngine** chose to author. It is not a list of forms
> NarrativeEngine should create. Open this file when you've decided NarrativeEngine needs a particular CK form
> shape (a quest backbone, a slot pattern with reference aliases, linked-ref keyword routing for AI packages, a
> busy-marker faction, …) and want to see one working setup. Do not pre-import any of it. See
> [`README.md`](README.md) for the full discipline.
>
> **Note on accuracy:** this doc is AI-generated and contains at least one fabricated CK field name ("Initially
> Cleared" alias flag) and a non-existent concept (AI Package "Priority"). Verify against the CK Wiki and against
> the actual `IntelEngine.esp` (inspectable in xEdit) before trusting any specific UI claim here.
>
> **Preserved copy** of `C:\Projects\IntelEngine-NativePlugin\docs\ESP_STRUCTURE.md`, plus extra notes from inspecting
> the GamePlugin tree. Documents what the `.esp` actually contains as Creation Kit forms.

---

## Overview

IntelEngine's `.esp` contains:

- 1 Quest (`IntelEngine`)
- 10 Reference Aliases (5 agent + 5 target, for 5 concurrent task slots)
- 6 AI Packages (plus speed variants)
- 4–6 Keywords (linked-ref keying)
- 3 Globals
- 1 Faction (`IntelEngine_TaskFaction`, used as a "busy" marker)

There is also a Creation Kit-generated **Quest Fragment** script: `QF_IntelEngine_02000D61.psc/.pex`. This is
auto-generated when fragments are attached to quest stages in CK; it lives alongside the hand-written
`IntelEngine_*.psc` scripts.

---

## Quest: IntelEngine

**Editor ID:** `IntelEngine`
**Type:** Miscellaneous
**Start Game Enabled:** Yes
**Run Once:** No

### Quest Scripts attached

1. `IntelEngine_Core` — Main initialization and decorator registration
2. `IntelEngine_Travel` — Travel and navigation system
3. `IntelEngine_NPCTasks` — NPC fetch/message/escort tasks
4. `IntelEngine_Schedule` — Time-based scheduling
5. `IntelEngine_SimpleTasks` — Simple errand tasks (note: this is from the original ESP_STRUCTURE.md, may have been
   renamed/folded in the shipped version)

Note: the shipped `Scripts/` folder also contains `IntelEngine_StoryEngine.pex`, `IntelEngine_Politics.pex`,
`IntelEngine_Battle.pex`, and `IntelEngine_MCM.pex`. These are likely attached to either this quest or separate
quests/aliases added in later versions (the ESP_STRUCTURE.md is from an earlier draft).

---

## Reference Aliases — the "slot" pattern

10 ReferenceAliases divided into two groups, providing 5 concurrent task slots.

### Agent Aliases (5)

The NPC performing a task lives in one of these.

| Alias          | Flags                                    | Purpose           |
| -------------- | ---------------------------------------- | ----------------- |
| `AgentAlias00` | Optional, Allow Reuse, Initially Cleared | Task agent slot 0 |
| `AgentAlias01` | Optional, Allow Reuse, Initially Cleared | Task agent slot 1 |
| `AgentAlias02` | Optional, Allow Reuse, Initially Cleared | Task agent slot 2 |
| `AgentAlias03` | Optional, Allow Reuse, Initially Cleared | Task agent slot 3 |
| `AgentAlias04` | Optional, Allow Reuse, Initially Cleared | Task agent slot 4 |

**Packages attached to each Agent Alias** (gated by linked-ref keyword conditions):

- `IntelEngine_TravelPackage` — runs when alias has a linked ref via `IntelEngine_TravelTarget`
- `IntelEngine_EscortPackage` — runs when alias has a linked ref via `IntelEngine_EscortTarget`
- `IntelEngine_SandboxPackage` — runs when alias has a linked ref via `IntelEngine_SandboxLocation`

### Target Aliases (5)

The NPC being fetched/escorted lives in one of these.

| Alias           | Flags                                    | Purpose           |
| --------------- | ---------------------------------------- | ----------------- |
| `TargetAlias00` | Optional, Allow Reuse, Initially Cleared | Target NPC slot 0 |
| `TargetAlias01` | Optional, Allow Reuse, Initially Cleared | Target NPC slot 1 |
| `TargetAlias02` | Optional, Allow Reuse, Initially Cleared | Target NPC slot 2 |
| `TargetAlias03` | Optional, Allow Reuse, Initially Cleared | Target NPC slot 3 |
| `TargetAlias04` | Optional, Allow Reuse, Initially Cleared | Target NPC slot 4 |

**Packages attached:** `IntelEngine_FollowAgentPackage` — follow the agent via `IntelEngine_AgentLink` linked ref.

### The slot allocation model

Five slots = up to five NPCs on tasks simultaneously. The C++ `SlotTracker` owns the canonical state for each slot
(agent FormID, state, task type, target, deadline, etc.). Papyrus `IntelEngine_Core.AllocateSlot()` finds the first
empty agent alias, force-fills it with the NPC, applies `IntelEngine_TaskFaction`, sets the appropriate linked ref
(which gates which package runs), and the AI takes over.

---

## AI Packages — the speed-variant + linked-ref pattern

- **`IntelEngine_TravelPackage_Walk`** — Travel; walk speed; linked-ref keyword `IntelEngine_TravelTarget`.
- **`IntelEngine_TravelPackage_Jog`** — Travel; jog speed; linked-ref keyword `IntelEngine_TravelTarget`.
- **`IntelEngine_TravelPackage_Run`** — Travel; run speed; linked-ref keyword `IntelEngine_TravelTarget`.
- **`IntelEngine_EscortPackage`** — Escort; leads the target NPC back to the player; linked-ref keyword
  `IntelEngine_EscortTarget`.
- **`IntelEngine_FollowAgentPackage`** — Follow; target NPC follows the agent; linked-ref keyword
  `IntelEngine_AgentLink`.
- **`IntelEngine_SandboxPackage`** — Sandbox; idles at destination while waiting; uses current location or a
  linked-ref keyword.
- **`IntelEngine_WaitPackage`** — Sandbox/Guard; waits at a specific spot for the player; linked-ref keyword
  `IntelEngine_WaitLocation`.
- **`IntelEngine_ApproachPackage`** — Travel; walks to a nearby object/NPC; linked-ref keyword.

### Package settings (selected highlights)

- `TravelPackage`: location = near linked ref, radius 256u, schedule "Always". Speed differs per variant.
- `EscortPackage`: escort target = linked ref, follow distance 200u, no wait at location.
- `FollowAgentPackage`: follow target = linked ref, follow distance 150u, stay close.
- `SandboxPackage`: location radius 512u, allow eating/sitting, no sleeping.
- `WaitPackage`: radius 64u (stay put), face linked marker if set.
- `ApproachPackage`: radius 128u, walk only, single-use.

### Why three Travel variants instead of a speed parameter?

Papyrus has no clean way to swap an AI package's speed mid-stride. By baking each speed into its own package, you swap
packages instead — atomic and engine-friendly. The `ChangeSpeed` action is implemented as "remove current travel
package, apply the other speed variant."

---

## Keywords (linked-ref keying)

| Editor ID                     | Purpose                                  |
| ----------------------------- | ---------------------------------------- |
| `IntelEngine_TravelTarget`    | Linked ref → travel destination          |
| `IntelEngine_EscortTarget`    | Linked ref → NPC being escorted          |
| `IntelEngine_AgentLink`       | Linked ref → agent for target to follow  |
| `IntelEngine_WaitLocation`    | Linked ref → wait/meeting spot           |
| `IntelEngine_SandboxLocation` | Linked ref → sandbox area                |
| `IntelEngine_TaskAssigned`    | Marker keyword on NPCs with active tasks |

### Why this pattern works

In Creation Kit, an AI package's target can be specified as "linked ref by keyword." Adding the keyword to an actor
with `SetLinkedRef(targetRef, keyword)` from Papyrus (or po3's Papyrus Extender) tells the package where to go
*without recompiling the package*. This is how you get a single travel package that can target anywhere in the game.

---

## Globals

| Editor ID                        | Type  | Default | Purpose                          |
| -------------------------------- | ----- | ------- | -------------------------------- |
| `IntelEngine_DebugMode`          | Short | 0       | Enable debug messages (1=on)     |
| `IntelEngine_MaxConcurrentTasks` | Short | 5       | Max simultaneous tasks           |
| `IntelEngine_DefaultWaitHours`   | Float | 24.0    | Default wait time at destination |

GlobalVariables are the canonical place to put MCM-tunable values that Papyrus needs to read frequently — they persist
with the save automatically and are addressable from condition functions.

---

## Faction: `IntelEngine_TaskFaction`

Purpose: marker faction. NPCs are added on task assignment, removed on completion. This enables:

- Quick "is this NPC busy" check from anywhere
- Use as a precondition in package conditions
- Prevention of double-assignment

The C++ side reads faction membership to populate the `is_busy` decorator and the `busy_reason(uuid)` template helper.
**This is the canonical "this NPC is owned by my plugin right now" marker.**

---

## Package Priority Guidelines

| Priority | Package Type                | When Used                   |
| -------- | --------------------------- | --------------------------- |
| 100      | Travel/Escort               | Active task execution       |
| 90       | Wait/Sandbox at destination | Arrived, waiting for player |
| 80       | Follow agent                | Target following messenger  |
| 70       | Approach                    | Walking to nearby object    |

Priority 100 ensures IntelEngine tasks override:

- Default sandbox packages
- Schedule packages (eat, sleep, work)
- Other-mod packages (typically 50–70)

If another mod fights at priority 100, raise to 110.

---

## Papyrus property setup (selected)

`IntelEngine_Core.psc`'s properties — for reference if NarrativeEngine builds a similar slot system:

```text
Quest Property IntelEngine Auto

ReferenceAlias Property AgentAlias00 Auto
...
ReferenceAlias Property AgentAlias04 Auto
ReferenceAlias Property TargetAlias00 Auto
...
ReferenceAlias Property TargetAlias04 Auto

Keyword Property IntelEngine_TravelTarget Auto
Keyword Property IntelEngine_EscortTarget Auto
Keyword Property IntelEngine_AgentLink Auto
Keyword Property IntelEngine_WaitLocation Auto

Package Property IntelEngine_TravelPackage_Walk Auto
Package Property IntelEngine_TravelPackage_Jog Auto
Package Property IntelEngine_TravelPackage_Run Auto
Package Property IntelEngine_EscortPackage Auto
Package Property IntelEngine_FollowAgentPackage Auto
Package Property IntelEngine_SandboxPackage Auto
Package Property IntelEngine_WaitPackage Auto

Faction Property IntelEngine_TaskFaction Auto

GlobalVariable Property IntelEngine_DebugMode Auto
GlobalVariable Property IntelEngine_MaxConcurrentTasks Auto
GlobalVariable Property IntelEngine_DefaultWaitHours Auto
```

---

## XMarker placement

For semantic location resolution, IntelEngine **does not require any hand-placed XMarkers**. The DLL scans existing
game data:

1. Doors — teleport markers reveal where doors lead
2. Stairs — furniture markers at top/bottom of stairs
3. Notable furniture — bars, fireplaces, beds with ownership

This is the right default — placing markers by hand doesn't scale to mod-added content.

---

## Compatibility notes

### Load Order

- Load after `SkyrimNet.esp`
- Load after any mods adding locations/NPCs the plugin should reference

### Conflicts

- Mods heavily modifying AI packages may interfere
- Priority 100 overrides most conflicts; raise to 110 if needed
- Action-name overlap with other SkyrimNet plugins is the bigger compatibility hazard (see
  [`FEATURE_OVERVIEW.md`](FEATURE_OVERVIEW.md))

### Safe to merge

The ESP merges cleanly with zMerge / similar tools.

---

## Testing checklist (from ESP_STRUCTURE.md)

After setting up in Creation Kit:

- [ ] Quest starts on game load
- [ ] Scripts compile without errors
- [ ] All properties are filled
- [ ] Packages have correct conditions
- [ ] Keywords are properly named
- [ ] Faction is created

In-game testing:

```text
help IntelEngine
sqv IntelEngine
; Should show all aliases as "None" initially
```
