# IntelEngine — Feature Overview (User-Facing)

> **Reading discipline.** This is the feature set **IntelEngine** chose. It is not a feature set NarrativeEngine
> should aim at. Open this file when you want to see what kinds of player-facing capabilities one author thought
> made sense in this space; treat each feature as evidence that *someone built this and it worked*, not as a
> requirement to mirror. See [`README.md`](README.md) for the full discipline.

This is what IntelEngine *does* from a gameplay perspective. Source: the IntelEngine README.

---

## The pitch

IntelEngine is an **NPC autonomy framework for SkyrimNet**. Where SkyrimNet gives NPCs the ability to *think and
speak* through an LLM, IntelEngine gives them the ability to **act** — physically, across the game world, on their
own two feet. Both on the player's command and on their own initiative.

It splits into three parts:

### Part 1 — Player-Driven Actions

Through natural dialogue, any NPC can:

- **Schedule** meetings, fetches, and deliveries for any future game time — *"at sunset"*, *"tomorrow morning"*,
  *"in three hours"*
- **Travel** to named locations, relative directions, or anywhere with a door
- **Fetch** people and escort them back to the player on foot
- **Deliver messages** to anyone — with optional meeting requests that schedule the recipient to travel
- **Search** for someone alongside the player (NPC leads, player follows)
- **Change speed** mid-task (walk/jog/run)
- **Cancel** the current task

### Part 2 — Dungeon Master (AI-Driven Stories)

Without player input, an LLM "DM" decides when NPCs should act on their own. Story types (each individually toggleable):

- **Seek Player** — NPC travels to find the player (absence-driven, intimacy-driven, unresolved memory)
- **Informant** — NPC approaches with gossip traceable to real game events
- **Road Encounter** — paths cross with an NPC on their own business (exterior only)
- **Ambush** — hostile NPC with a real grudge stalks/attacks; yield system
- **Faction Ambush** — 3–7 faction soldiers waylay the player; scales with hostility
- **Stalker** — obsessive NPC secretly follows, no combat; emotional confrontation
- **Message** — courier delivers a verbal message from an NPC who can't come themselves
- **Quest** — combat/rescue/find-item/faction-combat/faction-rescue/faction-battle subtypes
- **NPC Interaction** — two NPCs interact independently (argument, deal, romance, training, favor, warning)
- **NPC Gossip** — rumors spread through chains of up to 10 people

### Part 3 — Faction Politics (v3.0+)

A living simulation of 9 configurable factions:

- **Political DM** generates events every ~6 game hours: trade deals, espionage, border skirmishes, assassinations,
  war declarations, surrenders
- **Player standing** per faction rises and falls based on conduct, battle participation, NPC reports
- **Wars** with morale, army strength, off-screen battles, **and player-witnessed battles** (5 waves, 22 soldiers
  per side)
- **Faction quests** at standing 20+: combat missions, rescue ops, full-scale battle participation
- **Political awareness** injected into NPC bios

---

## Scheduling — the "core" feature

Natural time expressions parse to absolute game-hour values:

| Expression                      | Resolves to      |
| ------------------------------- | ---------------- |
| dawn, sunrise                   | 5–6 AM           |
| morning                         | 8 AM             |
| noon, midday                    | 12 PM            |
| afternoon                       | 2 PM             |
| evening                         | 6 PM             |
| sunset, dusk                    | 7–8 PM           |
| night                           | 10 PM            |
| midnight                        | 12 AM            |
| "in 2 hours", "in half an hour" | Relative to now  |
| "tonight"                       | This evening     |
| "tomorrow"                      | Tomorrow morning |

Key properties:

- **Early departure** — NPCs compute travel time from distance and depart in time to arrive on schedule (buffer
  default 2h, capped to 75% of time-until-meeting)
- **Natural arrival** — NPC idles at the meeting point (sit, lean, look around). Meeting is "live" by proximity.
  Walk away → meeting ends. No timer, no dialogue prompt.
- **No-shows remembered** — wait expires (default 3 game hours), NPC leaves, and the no-show is in their memory
- **Lateness tracked** — outcome flagged success / late-arrival / no-show; persists in memory

Up to **10** simultaneous scheduled tasks.

---

## Navigation properties

- **Dynamic world indexing** — at game-load, scan all four process list tiers and all cells/locations; mod-added
  content is automatically discoverable
- **Cross-cell travel** — no range limit; NPCs path through doors and cells using vanilla AI
- **Semantic resolution** — *upstairs* (Z-axis door scan), *outside* (door type analysis), *the back room*, *home*
  (per-NPC bed/cell index), *the kitchen* (cooking-station scan), *water* (bath furniture / river proximity)
- **Stuck recovery** — escalates: soft (re-evaluate packages) → progressive teleport (2000 → 1000 → 500 → 250 units)
  → force-complete timeout
- **Departure verification** — confirms NPC actually moved before treating the task as "in flight"

---

## NPC awareness (the SkyrimNet bio submodules)

Six (later seven) numbered Jinja2 fragments inject live state into every NPC's dialogue context every turn:

- **`0197_intel_received_messages`** — Who sent the NPC what, carried by whom, when.
- **`0198_intel_schedule_awareness`** — Active meetings, time-until-departure.
- **`0199_intel_meeting_outcome`** — Success / late / no-show outcomes from past meetings.
- **`0200_intel_gossip`** — Rumors heard (from whom) and shared (to whom).
- **`0800_intel_facts`** — Facts learned via story events, with natural time refs ("just now", "a few days
  ago").
- **`0801_intel_task_awareness`** — Current task ("traveling to Dragonsreach"), past task history with
  time-ago.
- **`0810_intel_political_awareness`** — Faction affiliation, recent political events, player standing.

This is **the** mechanism by which NPCs "know" things across dialogue cycles — Papyrus stores per-NPC state; the
submodule templates pull it via `papyrus_util(...)` helpers at render time.

---

## Concurrency limits and persistence

- **5 active tasks** simultaneous
- **10 scheduled tasks** queued
- **Save/load safe** — active and scheduled tasks survive saving/reloading; AI packages reconstruct on load
- **Independent recovery** per slot
- **Follower-aware** — followers temporarily released for task execution, restored after

---

## In-game dashboard (optional, via PrismaUI)

React 18 + Tailwind overlay accessible via hotkey (default Shift+7). Seven tabs:

- **Tasks** — 5 slots + 10 scheduled, with cancel buttons
- **Story** — toggles, active dispatch, NPC social log
- **Politics** — relations matrix, wars, player standings, battle history
- **Director** — manual story/political dispatch + direct action execution
- **Actions** — toggle action YAMLs on/off (writes to disk)
- **Packages** — active AI-package list with remove buttons
- **Settings** — full parity with MCM

JS↔C++ via PrismaUI: state pushed via `window.updateFullState(json)`, callbacks via `window.onDashboard_<action>(json)`.

---

## MCM (SkyUI) settings

The author exposed extensive tuning surface — relevant if NarrativeEngine wants to mirror the same configuration
approach:

- Debug Mode, Max Concurrent Tasks (1–5), Default Wait Hours, Task Confirmation, Report Back After Delivery,
  Meeting Timeout
- Danger Zone Policy / Player Home Visit Policy (4 levels each: Allow All / Block Civilians / Followers Only /
  Block All)
- Story Tick Interval, Travel Timeout, Quest Sub-Type toggles

---

## Compatibility caveat (worth remembering)

> SkyrimNet presents **all** registered actions to its AI when choosing what an NPC does. If two mods both register
> `travel`/`fetch`/`cancel` actions, the LLM sees duplicate options and may pick one mod's travel + another's cancel
> — which won't work because each cancel only knows about its own slots.

Implication for NarrativeEngine: when designing actions, deliberately decide whether to share/depend on
IntelEngine's actions (if installed alongside) or define non-overlapping ones — and document the conflict surface up
front.

---

## Planned (never built) features

The author had listed but had not built:

- **Eliminate Target** — dispatch an NPC to kill someone
- **Lockpick Door** — send an NPC to pick a lock
- **Steal Item** — task an NPC with theft from inventory/home
