# IntelEngine Prior Art — Gossip / Rumor Propagation

> **Reading discipline.** This documents how *IntelEngine* built one gossip-propagation system. IntelEngine was
> abandoned mid-development; its choices are "how someone got far enough to ship," not verified-correct design.
> Nothing here is a requirement for NarrativeEngine. See [`../README.md`](../README.md) for the full discipline.

**Feature under analysis** (IntelEngine README, "Part 2 — Dungeon Master"):

> Share gossip traced to real events, spreading through chains of up to 10 people

Every claim below cites a file and line so it can be checked. Where I could not determine something from the source,
I say so explicitly.

---

## 1. High-level overview, step by step

The system is a **background NPC-to-NPC social tick** that is entirely separate from the player-facing "Story DM"
tick. It never involves the player as a participant — the player only ever *overhears* it or reads it out of an NPC's
dialogue context later.

### Step 1 — The NPC social tick fires

`IntelEngine_StoryEngine.psc::TickNPCInteractions()`
(`C:\Projects\IntelEngine-NativePlugin\Source\Scripts\IntelEngine_StoryEngine.psc:787`) runs off the story
scheduler. Gates, in order:

- `NPCTickEnabled` must be true (MCM toggle, default `true`, line 110).
- At least one of `TypeNPCInteractionEnabled` / `TypeNPCGossipEnabled` must be true (line 791).
- A `NPCTickPending` in-flight flag, with a 1.0-game-hour watchdog reset via
  `IntelEngine.ShouldResetPending("npcInteraction", 1.0, currentTime)` (line 797).
- Game-time interval `NPCTickIntervalHours`, **default 1.5 game hours** (line 109, migration default re-applied at
  line 283).

### Step 2 — Build the candidate pool (two-phase, async)

`IntelEngine.BeginAsyncNPCDMTick(4, ...)` (line 817). The `4` is `maxPairs` — the maximum number of location groups.

**Phase A (main thread, snapshot only)** — `NPCIndex.cpp:1703` walks `ProcessUtils::ForEachLoadedActor` and keeps
actors that are: loaded with a parent cell, alive, not disabled, not in combat, not hostile to the player, without an
active task or slot cooldown, carrying the `ActorTypeNPC` keyword, not a player teammate, not a child race, not a
generic creature name, off the story cooldown *and* off the social cooldown, and with a resolvable location name
(hard-coded rejects: `"Marker Storage Unit"`, `"TestTony"` — `NPCIndex.cpp:1730`).

**Phase B (worker thread, markdown build)** — `NPCIndex::BuildNPCInteractionContextFromSnapshot`
(`NPCIndex.cpp:1759`):

- Group snapshot actors by lowercased location string; discard any group with fewer than 2 NPCs (line 1798).
- Score each group: `groupScore = npcCount + Σ(socialScore × 0.3) + uniform_random[0,1) + (2.0 if any member shares
  the player's cell)` (lines 1800–1811).
- Sort descending, keep the top `maxPairs` (4) groups (lines 1821–1825).
- Emit at most **3 NPCs per group** (line 1861), each as: display name, archetype, gender, `(follower)` marker, a
  `{bio summary}`, up to 2 formatted memories from SkyrimNet's memory DB, and up to 2 `Knows:` world-knowledge
  entries (lines 1866–1895).
- Prepend `## World State` (player name/location, time of day) and the faction political summary; append
  `## Story Type Picks This Session`.

### Step 3 — Choose a preferred interaction type and call the LLM

`BuildNPCInteractionRequestJsonCore` (`Papyrus.cpp:2031`) wraps the markdown as `npcPairPool` and adds
`preferredType` from `NPCIndex::GetPreferredNPCType()` (`NPCIndex.cpp:2732`):

```cpp
// Force underrepresented type when diff >= 2; no preference otherwise
if (gossipN - interactionN >= 2) return "npc_interaction";
if (interactionN - gossipN >= 2) return "npc_gossip";
return "";
```

It also passes `player_at_inn` and `latest_witness_event` (the most recent witnessable faction-politics event).

The call itself is `SkyrimNetApi.SendCustomPromptToLLM("intel_story_npc_dm", "intel_story_dm", contextJson, ...)`
(`IntelEngine_StoryEngine.psc:829`) — prompt `intel_story_npc_dm.prompt`, LLM profile `intel_story_dm`.

### Step 4 — The LLM picks a pair and writes one rumor

`intel_story_npc_dm.prompt` asks for a single JSON object, under 700 characters total, of the shape:

```json
{"should_act":true,"type":"npc_gossip","npc":"Gossiper","npc2":"Listener",
 "narration":"whispered a rumor about the steward",
 "gossip":"was overheard arguing with the steward about missing tribute payments"}
```

The `gossip` field is a past-tense verb phrase without a subject prefix, capped by the prompt at 120 characters.

### Step 5 — Validate and seed the rumor

`OnNPCInteractionResponse` (`IntelEngine_StoryEngine.psc:838`):

1. Clear the pending flag; bail if `success != 1` or `should_act` is false.
2. Extract `type` / `npc` / `npc2` / `narration`; bail if any is empty.
3. Re-check the per-type MCM toggle (line 866).
4. `IntelEngine.NotifyStoryTypePicked(storyType)` — bumps the session type counters.
5. Resolve both names to `Actor` via `ResolveStoryCandidate`; bail if either is `None`, dead, or disabled.
6. **Social cooldown**: both must pass `CheckNPCSocialCooldown` before either is stamped
   (`NPCSocialCooldownHours`, default **24.0 game hours**, line 112; check/stamp at lines 883–889).
7. `Core.SendPersistentMemory(npc1, npc2, narration)` → `SkyrimNetApi.RegisterPersistentEvent` — note this registers
   the **narration**, not the rumor text.
8. Gossip branch (lines 915–922):

```papyrus
ElseIf storyType == "npc_gossip"
    String gossipContent = ExtractJsonField(response, "gossip")
    If gossipContent != ""
        Core.InjectGossip(npc1, npc2, gossipContent)
        logDetail = "Gossip: " + gossipContent
        dashboardText = gossipContent
    EndIf
    SpreadGossipOffScreen(npc1, npc2, gossipContent)
EndIf
```

9. `AddNPCSocialLog(...)` records the event into the player-attached dashboard log (5-entry cap).

### Step 6 — Store the rumor on both NPCs

`IntelEngine_Core.psc::InjectGossip` (`IntelEngine_Core.psc:1533`) is the only writer of gossip state:

- Receiver gains `Intel_GossipHeard` (rumor text), `Intel_GossipHeardFrom` (giver's display name),
  `Intel_GossipHeardTimes` (game days), FIFO-capped at **5 entries** (lines 1547–1556).
- Giver gains `Intel_GossipTold`, `Intel_GossipToldTo`, `Intel_GossipToldTimes`, same 5-entry cap (1559–1568).
- Both actors get a **pre-rendered** `Intel_GossipRendered` string, built by two C++ natives and concatenated
  (heard-section + told-section), because `papyrus_util("GetStringList")` cannot see lists created during the current
  session but `GetStringValue` can (lines 1570–1593; the rationale is spelled out at `IntelEngine_Core.psc:1510`).

### Step 7 — Chain the rumor off-screen

`IntelEngine_StoryEngine.psc::SpreadGossipOffScreen` (line 1629) is the "up to 10 people" mechanism:

```papyrus
; Only chain-spread if both original NPCs are off-screen
If playerCell != None
    If originalGossiper.GetParentCell() == playerCell || firstRecipient.GetParentCell() == playerCell
        return
    EndIf
EndIf

String trackKey = "Intel_GossipChainTemp"
StorageUtil.IntListClear(player, trackKey)
StorageUtil.IntListAdd(player, trackKey, originalGossiper.GetFormID())
StorageUtil.IntListAdd(player, trackKey, firstRecipient.GetFormID())

Int spreads = Utility.RandomInt(1, 10)
Actor currentGiver = firstRecipient
Int s = 0
While s < spreads
    Actor nextRecipient = IntelEngine.GetRelatedCandidate(currentGiver)
    If nextRecipient != None && StorageUtil.IntListFind(player, trackKey, nextRecipient.GetFormID()) < 0
        Core.InjectGossip(currentGiver, nextRecipient, gossip)
        ...
        currentGiver = nextRecipient
    Else
        StorageUtil.IntListClear(player, trackKey)
        return
    EndIf
    s += 1
EndWhile
```

Each hop picks the next listener with `NPCIndex::GetRelatedCandidate` (`NPCIndex.cpp:1168`): rank NPCs by shared
event history with the current teller (top 15), fall back to "any ranked NPC" (top 30), skipping anyone the player
interacted with recently and anyone failing `IsEligibleStoryCandidate` (`NPCIndex.cpp:1014` — not deleted, has a
parent cell, not in combat, not a teammate, not hostile, **not in the player's cell**).

**The rumor text is byte-identical at every hop.** Only the attributed source changes: each recipient records the
*previous teller*, not the origin. There is no mutation, distortion, embellishment, confidence decay, or truncation
anywhere in the chain.

### Step 8 — Stage the visible half (optional)

Back in `OnNPCInteractionResponse` (lines 926–947), based on cell co-location:

- **Both in the player's cell** → `PerformVisibleInteraction` (line 1790): `SetLookAt` both ways, 0.5s wait, then
  `SkyrimNetApi.DirectNarration(summary, npc1, npc2)`. This is the moment the player can overhear a rumor being told.
- **Exactly one visible** (and no NPC-social dispatch already running, and neither is a follower) →
  `DispatchNPCSocial` (line 954): allocate one of the 5 task slots, `SetLinkedRef` to the target, apply
  `TravelPackage_Jog` at `PRIORITY_TRAVEL`, register stuck/off-screen tracking. On arrival,
  `OnNPCSocialArrived` → `PerformVisibleInteraction` → cleanup (lines 1055–1076).
- **Neither visible** → log only.

Note the chain in step 7 and the visible interaction in step 8 are mutually exclusive by construction: the chain
requires *neither* of the seed pair to be in the player's cell.

### Step 9 — Surface the rumor in dialogue

The bio submodule `SKSE/Plugins/SkyrimNet/prompts/submodules/character_bio/0200_intel_gossip.prompt` (identical in
both repos) is injected into every NPC's dialogue context. It has two paths:

- **First-person path** (`render_mode == "full"`): dumps the pre-rendered `Intel_GossipRendered` string verbatim,
  which the C++ natives format as `## Rumors I've Heard` / `- <Source> told me: <rumor> (<time-ago>)` and
  `## Rumors I've Shared` / `- I told <Recipient>: <rumor> (<time-ago>)` (`Papyrus.cpp:2764` and `2792`).
- **Third-person path** (`render_mode == "static"`): reads the six `Intel_Gossip*` lists live via `papyrus_util` and
  renders `## Rumors Heard` / `- <Source> told <Name>: <rumor> (<time-ago>)`, computing the time-ago in Jinja.

Time-ago buckets (`Papyrus.cpp::FormatRelativeTimeFromDays`, line 2724): `<0.1h` just now, `<1h` a few minutes ago,
`<3h` a short while ago, `<12h` earlier today, `<24h` yesterday, `<48h` a day ago, `<72h` a couple of days ago,
`<120h` a few days ago, `<168h` several days ago, else "some time ago".

### Step 10 — Feed gossip back to the *player-facing* Story DM

Separately, before each **Story DM** tick (not the NPC tick), `IntelEngine_StoryEngine.psc:670–692` reads the last 5
entries of the dashboard social log and pushes them to C++:

```papyrus
gossipLines += "- [" + gLoc + "] " + gNpc1 + " told " + gNpc2 + ": " + gText + "\n"
...
IntelEngine.SetRecentGossipContext(gossipLines)
```

`NPCIndex::SetRecentGossipContext` (`NPCIndex.cpp:2637`) tags untagged lines with the speaking NPC's hold name via a
single `ForEachLoadedActor` name→hold scan, and stores the result. `BuildDungeonMasterContext` injects it as a
`## Recent Gossip` block (`NPCIndex.cpp:2348`).

That block is what the **`informant`** story type consumes — a *different* feature where an NPC walks to the player
to relay a rumor. Its prompt gating (`intel_story_dm.prompt:47–56`) is strict:

> **Location matters for gossip**: Each gossip entry shows its hold in brackets (e.g., [Whiterun Hold]). An NPC can
> only relay gossip from their own hold or about widely-known public figures (Jarls, faction leaders).

Informant resolution (`IntelEngine_StoryEngine.psc:1600–1617`) writes **facts**, not gossip entries — the subject NPC
gets the raw gossip as a fact about themselves, and the informant gets `"heard from <sender> that <subject> <gossip>"`
or `"witnessed that <subject> <gossip>"`.

### Chain-depth claim: the README undercounts

The README and `ARCHITECTURE.md:653` both say "chains of up to 10 people." The code does not enforce that number.
`Utility.RandomInt(1, 10)` is inclusive on both ends, and it counts **additional** hops beyond the seed pair (the
function's own docstring says "chains through up to 10 **additional** NPCs"). Maximum reach is therefore:

- **12 distinct NPCs** (2 seed + 10 chained),
- **11 tellings** of the same rumor,
- and the same rumor stored on 12 actors × 2 storage forms each.

There is no configurable knob for this — `RandomInt(1, 10)` is a literal, not exposed in MCM, the dashboard, or any
INI/YAML.

---

## 2. Intended gameplay experience

Inferred from the prompt text, the tuning constants, the MCM hint strings, and the README.

**"The world talks about you when you're not there."** The MCM hint reads *"NPCs spread rumors and gossip among
themselves"* (`SettingsTab.jsx:198`) and the DM prompt frames the whole tick as *"background events — NPCs living
their lives, forming bonds, spreading rumors, and creating drama regardless of what the player is doing"*
(`intel_story_npc_dm.prompt:11`). The 1.5-hour tick with no player gating means the log fills while the player is
elsewhere; the payoff is opening the dashboard, or talking to a stranger, and finding that news moved without you.

**Gossip must be *earned*, never invented.** This is the single most-repeated instruction in the prompt:

> **Make gossip traceable** — if you pick npc_gossip, the rumor MUST come from something real in the memories shown.
> Never fabricate events. Gossip is how news travels in Skyrim — it should reference things that actually happened.

and the significance filter:

> **NOT gossip-worthy**: trivial actions (opened a door, ate food, walked somewhere, bought an item, stood around),
> routine observations, or mundane daily activities. If it wouldn't make someone stop and listen at a tavern, it's
> not gossip. … **If no gossip is important enough, pick npc_interaction instead.**

The author clearly fought the failure mode where an LLM narrates "Ysolda opened a door" as breaking news. The design
target is tavern-grade news: *"Combat, betrayal, political shifts, romantic affairs, crimes, heroic deeds,
disappearances, faction conflicts."*

**A visible web of who-told-whom.** The bio submodule deliberately shows both directions — rumors heard *and* rumors
shared, each with a name and a time-ago phrase. The README's own example of the intended payoff:

> *"Ysolda told me that Nazeem was seen lurking near the warehouse."*

The point is that an NPC can cite a source, so a player can trace a rumor backwards by asking the named person.

**Overhearing yourself being discussed.** Rule 9 and 10 of the prompt make this explicit — *"NPC gossip about
{{ playerName }} is especially compelling when the player is nearby — two NPCs whispering about the player's recent
deeds while the player is within earshot creates immersive moments"* — and the group scorer gives a flat `+2.0` bonus
to any location group containing the player's cell (`NPCIndex.cpp:1811`). Followers get a bespoke instruction to
whisper conspiratorially about the player when the player can see them (rule 11).

**Rumors are local, and locality is a constraint the player can reason about.** The whole hold-tagging apparatus
(`SetRecentGossipContext`, `HoldPolicyInformant = 6` meaning "same town, everyone" — `IntelEngine_StoryEngine.psc:73`,
commented *"gossip is local, nobody travels far for rumors"*) exists so that a Riften merchant cannot credibly know
what was said in Windhelm. The CHANGELOG lists "Fixed gossip without location context" as a shipped bugfix, so this
mattered enough to patch. **Caveat:** as §4 shows, that constraint is enforced only on the *informant* path and never
on the chain itself.

**Restraint over volume.** Rule 1 — *"REJECT if nothing compelling. Return should_act:false. Quality over quantity.
NPCs don't need to interact every tick — silence is natural"* — plus the 24-hour per-NPC social cooldown and the
type-balancing preference. The author wanted a slow drip, not a rumour mill.

---

## 3. Implementation breakdown

### Papyrus

| Script | Symbol | Role |
| --- | --- | --- |
| `IntelEngine_StoryEngine.psc` | `TickNPCInteractions()` (787) | The tick gate + async context kickoff |
| `IntelEngine_StoryEngine.psc` | `OnNPCDMContextReady()` (820) | Fires the LLM call; clears watchdog on failure |
| `IntelEngine_StoryEngine.psc` | `OnNPCInteractionResponse()` (838) | Parse, validate, cooldown, seed, chain, dispatch |
| `IntelEngine_StoryEngine.psc` | `SpreadGossipOffScreen()` (1629) | The chain loop |
| `IntelEngine_StoryEngine.psc` | `CheckNPCSocialCooldown` / `SetNPCSocialCooldown` (1101/1111) | 24h per-NPC gate |
| `IntelEngine_StoryEngine.psc` | `AddNPCSocialLog()` (4987) / `AddRecentStoryEvent()` (4979) | Dashboard + DM history |
| `IntelEngine_StoryEngine.psc` | `PerformVisibleInteraction()` (1790) / `DispatchNPCSocial()` (954) | On-screen staging |
| `IntelEngine_Core.psc` | `InjectGossip()` (1533) | The only writer of gossip state |
| `IntelEngine_Core.psc` | `OnDashboardDispatchNpcSocial()` (2329) | Director-mode manual trigger |
| `IntelEngine.psc` | `SetRecentGossipContext`, `RenderGossipHeardSection`, `RenderGossipToldSection` (549, 676, 679) | Native declarations |

### C++

| File | Symbol | Role |
| --- | --- | --- |
| `NPCIndex.cpp:1676` | `BuildNPCTickSnapshot` (Phase A) | Main-thread loaded-actor snapshot |
| `NPCIndex.cpp:1759` | `BuildNPCInteractionContextFromSnapshot` (Phase B) | Worker-thread location grouping + markdown |
| `NPCIndex.cpp:1168` | `GetRelatedCandidate` | Next-listener selection for each chain hop |
| `NPCIndex.cpp:1014` | `IsEligibleStoryCandidate` | Strict eligibility (incl. "not in player's cell") |
| `NPCIndex.cpp:2637` | `SetRecentGossipContext` | Hold-tags the social log for the Story DM prompt |
| `NPCIndex.cpp:2732` | `GetPreferredNPCType` | interaction/gossip balancing (diff ≥ 2) |
| `MemoryDB.cpp:826` | `GetRelatedCandidateFormIDs` | Social-graph ranking over SkyrimNet's event DB |
| `Papyrus.cpp:2764 / 2792` | `RenderGossipHeardSection` / `RenderGossipToldSection` | First-person bio pre-render |
| `Papyrus.cpp:2724` | `FormatRelativeTimeFromDays` | Time-ago bucketing |
| `Papyrus.cpp:1861` | `StoryResponseGetField` | Cached JSON field extraction (no sanitization) |

`MemoryDB::GetRelatedCandidateFormIDs` is a thin client over SkyrimNet's `GetRelatedActors(formId, maxCount*2,
86400.0, 604800.0)` — a 24-hour "short" window and a 7-day "medium" window — with a hand-tuned recency weighting
(`MemoryDB.cpp:842`):

```cpp
float score =
    actor.value("recentSharedEventsShort", 0) * 3.0f +
    (actor.value("recentSharedEventsMedium", 0) - actor.value("recentSharedEventsShort", 0)) * 1.0f +
    (actor.value("sharedEventCount", 0) - actor.value("recentSharedEventsMedium", 0)) * 0.3f;
```

### SkyrimNet assets

- **Prompt (generator):** `prompts/intel_story_npc_dm.prompt` — the NPC Social Life DM. Branches on `preferredType`
  into three variants (gossip-preferred, interaction-only, neutral).
- **Prompt (consumer, per-NPC bio):** `prompts/submodules/character_bio/0200_intel_gossip.prompt`.
- **Prompt (player-facing relay):** `prompts/intel_story_dm.prompt`, `### informant` section — reads the
  `## Recent Gossip` block that C++ injects into the candidate pool.
- **Action YAMLs:** *none*. There is no LLM-callable "share gossip" action. Gossip is exclusively DM-driven. (The
  only mention across the 14 action YAMLs is a trigger hint in `intel_delivermessage.yaml:17`, "You want to gossip or
  share news," which routes to the message-delivery feature, not this one.)

### ESP forms

Gossip has **no dedicated ESP forms**. It reuses the shared story infrastructure: the 5 agent alias slots,
`TravelPackage_Jog`, `IntelEngine_TravelTarget` (linked-ref keyword), and `PRIORITY_TRAVEL`. All gossip state lives
in StorageUtil and Papyrus quest properties.

### ModEvents

Only one, and only for manual triggering: `OnDashboardDispatchNpcSocial` (`IntelEngine_Core.psc:2329`), fired from
the PrismaUI Director tab (`DirectorTab.jsx:270`). The autonomous path uses no ModEvents — it is a direct
Papyrus↔native call chain plus a SkyrimNet LLM callback.

### Persisted state (StorageUtil keys)

Per-actor (survives save/load in the PapyrusUtil co-save; keyed on the actor form):

| Key | Type | Cap | Written by |
| --- | --- | --- | --- |
| `Intel_GossipHeard` | StringList | 5 | `InjectGossip` (receiver) |
| `Intel_GossipHeardFrom` | StringList | 5 | `InjectGossip` (giver's display name) |
| `Intel_GossipHeardTimes` | FloatList | 5 | `InjectGossip` (game days) |
| `Intel_GossipTold` | StringList | 5 | `InjectGossip` (giver) |
| `Intel_GossipToldTo` | StringList | 5 | `InjectGossip` (receiver's display name) |
| `Intel_GossipToldTimes` | FloatList | 5 | `InjectGossip` |
| `Intel_GossipRendered` | String | — | `InjectGossip`, both parties, full rebuild each time |
| `Intel_NPCSocialLastPicked` | Float | — | `SetNPCSocialCooldown` |

Per-player:

| Key | Type | Cap |
| --- | --- | --- |
| `Intel_GossipChainTemp` | IntList (FormIDs) | ~12, scratch |
| `Intel_SocialLog_Type` / `_NPC1` / `_NPC2` / `_Text` / `_Location` / `_Detail` | 6 parallel StringLists | 5 |
| `Intel_RecentStoryEvents` | StringList | 8 |

Per-quest formlist: `Intel_SocialCooldownActors` (used by `WarmCooldownMirror` at load to repopulate the volatile C++
cooldown mirror — `IntelEngine_StoryEngine.psc:1148–1170`).

### Magic numbers, collected

| Value | Meaning | Location |
| --- | --- | --- |
| `1.5` | NPC social tick interval, game hours | `IntelEngine_StoryEngine.psc:109` |
| `24.0` | Per-NPC social cooldown, game hours | `IntelEngine_StoryEngine.psc:112` |
| `1.0` | Pending-tick watchdog, game hours | `IntelEngine_StoryEngine.psc:797` |
| `4` | Max location groups in the pool | `IntelEngine_StoryEngine.psc:817` |
| `3` | Max NPCs printed per group | `NPCIndex.cpp:1861` |
| `2` | Min NPCs for a group to qualify | `NPCIndex.cpp:1798` |
| `0.3` / `2.0` | Social-score weight / player-nearby group bonus | `NPCIndex.cpp:1801`, `1811` |
| `RandomInt(1, 10)` | Chain hop count | `IntelEngine_StoryEngine.psc:1649` |
| `5` | Gossip entries retained per NPC per direction | `IntelEngine_Core.psc:1548`, `1560` |
| `8` | `Intel_RecentStoryEvents` cap | `IntelEngine_StoryEngine.psc:4982` |
| `5` | Dashboard social-log cap and Story-DM gossip-context lines | `:5016`, `:676` |
| `15` / `30` | Related-candidate / fallback query breadth | `NPCIndex.cpp:1181`, `1195` |
| `2` | Type-balance forcing threshold | `NPCIndex.cpp:2741` |
| `700` / `120` | Prompt-declared JSON and gossip-field char limits | `intel_story_npc_dm.prompt:90`, `:89` |
| `5000.0` / `200.0` | Director-mode travel distance window | `IntelEngine_Core.psc:2375` |

None of these are exposed as tunables except the tick interval and the two type toggles (MCM
`IntelEngine_MCM.psc:510–521`; dashboard `SettingsTab.jsx:166–199`).

---

## 4. Weaknesses and bugs

Split into **confirmed** (read directly in the source) and **suspected** (inference I could not close from the code
alone).

### Confirmed

**4.1 — The chain runs even when the rumor is empty.** `SpreadGossipOffScreen(npc1, npc2, gossipContent)` sits
*outside* the `If gossipContent != ""` guard (`IntelEngine_StoryEngine.psc:915–922`). `InjectGossip` no-ops on an
empty string, but the loop still executes: up to 10 `GetRelatedCandidate` calls (each a SQL query into SkyrimNet's
event DB, possibly plus a fuzzy-name actor scan), 10 `Debug.Trace` writes, and 10 `AddRecentStoryEvent` entries of
the form `"npc_gossip: X told Y: "` with a trailing empty payload. A malformed or truncated LLM response is exactly
the case that produces an empty `gossip` field, so this triggers on the failure path.

**4.2 — One chain can wipe the entire story-history buffer.** Each hop calls `AddRecentStoryEvent`
(`IntelEngine_StoryEngine.psc:1657`), which is FIFO-capped at 8 (`:4982`). A 10-hop chain therefore evicts *every*
other story record — ambush, quest, seek_player — replacing it with ten near-identical gossip lines. This buffer is
the DM's anti-repetition memory.

**4.3 — …and that corruption survives the save.** On load, `WarmStoryTypeCounts` (`:1325`) pipes those 8 entries into
`WarmStoryTypeCountsFromCSV` (`Papyrus.cpp:2190`), which calls `NotifyStoryTypePicked` once per entry. A chain-poisoned
buffer therefore warms `m_storyTypeCounts["npc_gossip"] = 8`, which pins `GetPreferredNPCType()` to
`"npc_interaction"` until interaction catches up. It also stamps `m_lastNPCDispatchGameTime = now`
(`NPCIndex.cpp:2547–2558`) at load time regardless of when the events actually happened.

**4.4 — Chain hops are invisible to every downstream consumer.** `AddNPCSocialLog` is called once, for the seed pair
only (`:924`). The dashboard log, and therefore the Story DM's `## Recent Gossip` block, and therefore the
`informant` story type, never see hops 3–12. A rumor can propagate through ten NPCs and remain un-relayable to the
player, because the only mechanism that walks a rumor to the player reads a log the chain never writes to.

**4.5 — Chain recipients bypass the cooldown system.** `CheckNPCSocialCooldown` / `SetNPCSocialCooldown` are applied
only to `npc1` and `npc2` (`:883–889`). Chain recipients are gated by `PassesCommonEligibility` (story cooldown, slot
cooldown) but nothing stamps their *social* cooldown, so the same well-connected NPC can be a chain recipient on
consecutive ticks, indefinitely. The 24-hour restraint the author designed applies to 2 of up to 12 participants.

**4.6 — The loop-guard aborts the chain instead of retrying.** `GetRelatedCandidate` is deterministic: Phase 1 sorts
by score and returns the *first* eligible actor (`NPCIndex.cpp:1182–1192`). It has no knowledge of
`Intel_GossipChainTemp`. When it hands back someone already in the chain, `SpreadGossipOffScreen` takes the `Else`
branch and **returns immediately** (`:1660–1663`) rather than asking for the next-best candidate. Since nothing about
the relation graph changes during a chain, densely-connected clusters (the common case — the graph is built from
shared events) will loop back almost at once.

**4.7 — Nothing bounds the number of NPCs carrying gossip state, and nothing ever cleans it.** `InjectFact` registers
every touched NPC in a global `Intel_FactNPCs` registry that `Maintenance()` sweeps on every load
(`IntelEngine_Core.psc:1517–1522`, `1618–1635`). `InjectGossip` has **no equivalent**: I grepped both repos for every
`Intel_Gossip*` key and the only writers are `InjectGossip` itself and the template reads
(see the full key list in §3). There is no expiry, no sweep, no migration, no removal on NPC death, and no cleanup on
uninstall. Per-NPC payload is bounded (10 list entries + a pre-rendered string of roughly 5 × (rumor + name + label),
call it a few hundred bytes), but the *number of NPCs* grows monotonically: at a 1.5-hour tick, with up to 12 actors
touched per gossip event, a long playthrough accumulates gossip state on a large fraction of Skyrim's named NPCs and
never releases any of it. This is the classic gossip-store failure mode and IntelEngine has it.

**4.8 — Every rumor is stored twice, and both copies are rebuilt on every hop.** `InjectGossip` does 12
`StorageUtil.*ListToArray` marshals and 4 native render calls **per injection** (`IntelEngine_Core.psc:1570–1593`) —
it rebuilds *both* NPCs' full heard+told sections even though only one section changed on each side. In a 10-hop
chain that is ~120 array marshals and ~40 native calls in one uninterrupted Papyrus loop, alongside up to 10
`GetRelatedCandidate` SQL queries. The author explicitly moved the *context build* off the main thread to kill a
"~20-50ms main-thread stutter" (`IntelEngine_StoryEngine.psc:814–816`) but left this loop fully synchronous.

**4.9 — Pre-rendered time-ago labels freeze at write time.** `RenderGossipHeardSection` is passed `currentTime` at
the moment of injection (`IntelEngine_Core.psc:1575`), and `Intel_GossipRendered` is only rewritten when that NPC
next gains or shares a rumor. An NPC who heard something a week ago and nothing since will render `(just now)`
forever in the first-person path. The list-based template path recomputes correctly — the two paths disagree by
design, not by accident.

**4.10 — The two render paths disagree on headings and grammar.** C++ emits `## Rumors I've Heard` /
`- <Source> told me: …` and `## Rumors I've Shared` / `- I told <Recipient>: …` (`Papyrus.cpp:2771`, `2799`, `2808`).
The template's list path emits `## Rumors Heard` / `- <Source> told <Name>: …` and `## Rumors Shared`
(`0200_intel_gossip.prompt:18`, `21`, `25`, `28`). Same data, two formats, chosen by `render_mode`.

**4.11 — The template's list path is the one the author documented as broken.** `IntelEngine_Core.psc:1510` states:
*"papyrus_util("GetStringList") can't see newly created lists during current session, but papyrus_util
("GetStringValue") can see SetStringValue immediately."* The entire pre-render mechanism exists to work around this.
Yet the `static` (third-person) branch of the submodule uses `GetStringList` exclusively — so third-person renders
show nothing for any NPC whose gossip lists were created during the current session. (The underlying PapyrusUtil
behavior is the author's claim; I did not verify it against PapyrusUtil source.)

**4.12 — Director mode writes the wrong thing.** The dashboard `npc_gossip` path (`IntelEngine_Core.psc:2363–2371`)
calls `InjectFact`, not `InjectGossip`:

```papyrus
InjectFact(npc1, "told " + npc2Name + " that someone " + gossip)
InjectFact(npc2, "heard from " + npc1Name + " that someone " + gossip)
```

Three problems: it lands in "Things I Know" instead of "Rumors I've Heard"; the literal `"someone"` placeholder
destroys the subject of the rumor; and it never chains. Player-triggered gossip is a materially different feature
from DM-triggered gossip, which is almost certainly not intended.

**4.13 — Provenance is exactly one hop deep, and it is stored as a display name.** `Intel_GossipHeardFrom` holds
`akGiver.GetDisplayName()` and nothing else — no origin actor, no hop counter, no seed-event ID, no chain ID. Three
hops out, nothing in the data connects the rumor to the real memory that seeded it. The mod's headline promise
("traced to real events") is enforced *solely* by the LLM prompt at seed time and is unverifiable and unrecoverable
thereafter. Display names also collide catastrophically in Skyrim (`Guard`, `Bandit`, `Whiterun Guard`) and change
when an NPC is renamed.

**4.14 — Chain spread has no geography check at all.** The hold constraint the author built and patched
(`SetRecentGossipContext` hold-tagging, `HoldPolicyInformant = 6`, the `intel_story_dm.prompt` hold rule) applies
**only** to the informant relay. `SpreadGossipOffScreen` and `GetRelatedCandidate` contain no hold, worldspace,
distance, or travel-time check whatsoever. A rumor seeded in Riften can land on a Solitude NPC on the very next hop
if the social graph ranks them highly. The design intent ("gossip is local, nobody travels far for rumors") is
contradicted by the propagation mechanism.

**4.15 — The NPC social DM cannot see existing gossip.** `SetRecentGossipContext` is called only on the *Story DM*
tick path (`IntelEngine_StoryEngine.psc:670–692`), and `m_recentGossipContext` is read only in
`BuildDungeonMasterContext` (`NPCIndex.cpp:2348–2356`). `BuildNPCInteractionContextFromSnapshot` never touches it.
The subsystem that *creates* rumors has no visibility into which rumors already exist, so it can and will re-seed the
same rumor repeatedly, and cannot deliberately advance an existing thread despite the prompt asking it to
("Continue an existing thread…").

**4.16 — The rumor never reaches SkyrimNet's memory DB.** `SendPersistentMemory(npc1, npc2, narration)` (`:891`) runs
*before* the type branch and registers the **narration** ("whispered a rumor about the steward"), not the `gossip`
payload. The rumor content lives only in IntelEngine's StorageUtil lists. Consequence (inference from the two code
paths): the next NPC-DM tick's per-NPC `Memories` lines will show the vague narration, so an LLM asked to "continue
the thread" has nothing concrete to continue.

**4.17 — No deduplication.** Nothing prevents the same rumor text landing on the same NPC twice from two different
tellers, and with a 5-entry cap a duplicate evicts a genuinely distinct rumor.

**4.18 — No LLM text sanitization anywhere on the path.** `StoryResponseGetField` (`Papyrus.cpp:1861–1944`) parses
the JSON and returns the raw string value into a `BSFixedString`. That string goes straight into StorageUtil, into
the pre-rendered bio section, into `DirectNarration`, and back into the next LLM prompt, with no normalization of
smart quotes, em-dashes, ellipses, NBSPs, or zero-width characters. (Flagged because NarrativeEngine's CLAUDE.md
mandates the opposite policy — this is a place IntelEngine is *not* a model to copy.)

**4.19 — `Intel_GossipChainTemp` leaks on one exit path.** The list is cleared at the top of the function
(`:1645`) and on both in-loop exits, but the early "one of the pair is in the player's cell" return at `:1638–1640`
happens *before* the clear. A stale ≤12-entry FormID list can therefore sit in the co-save indefinitely. Trivial in
size; noted because it shows the cleanup is ad-hoc rather than scoped.

**4.20 — Stale and false comments.** `IntelEngine_Core.psc:1530` and `:1537` both point at
`0195_intel_gossip.prompt`; the shipped file is `0200_intel_gossip.prompt` and no `0195` exists. `:1483–1484` claims
facts "expire after a configurable number of game days" immediately above code that is pure FIFO with no expiry, and
`CleanExpiredFacts` (`:1598`) is a no-op legacy migration. The README repeats the false claim ("Facts expire over
time, so recent events feel vivid while old news fades"). None of this breaks gossip, but it means the comments are
not trustworthy as documentation.

**4.21 — Response-length hazard is acknowledged but unguarded.** The prompt warns three times that responses over
700 characters are truncated and break parsing (`intel_story_npc_dm.prompt:90`, `:98`), and asks for `gossip` under
120 characters. Nothing in Papyrus or C++ validates either bound before storing. A 400-character `gossip` string
would be written verbatim to up to 12 NPCs, in duplicate (list + pre-rendered).

### Suspected

**4.22 — "Off-screen" chain spread may only reach loaded cells.** `IsEligibleStoryCandidate` requires
`actor->GetParentCell()` to be non-null (`NPCIndex.cpp:1020`) and to differ from the player's cell (`:1024`), and
`GetRelatedCandidate` bails outright if the player has no parent cell (`:1171–1172`). If Skyrim leaves `parentCell`
null for actors in unloaded cells — which is my understanding but which I did not confirm from this codebase or from
CommonLibSSE — then the "off-screen chain" is confined to actors in *loaded-but-not-the-player's* cells, i.e. the
surrounding cell grid, not the province. That would make "rumors spreading across Skyrim" substantially narrower than
advertised. **Verify against CommonLibSSE-NG before relying on either reading.**

**4.23 — Chains probably terminate at 1–2 hops in practice.** Follows from 4.6: deterministic selection plus
abort-on-repeat plus a static relation graph. I did not instrument this, so the *frequency* is inference; the
*mechanism* (4.6) is confirmed.

**4.24 — On a fresh save the "social graph" is really "the 30 most active NPCs."** `GetRelatedCandidateFormIDs` ranks
on SkyrimNet's shared-event history, which is dominated by events the player was present for. Early in a playthrough
Phase 1 will return nothing and Phase 2's `GetRankedCandidateFormIDs(30)` fallback (`NPCIndex.cpp:1195`) takes over —
"any ranked NPC," in fixed score order, with no locality. The chain then walks the same handful of NPCs every time.
Plausible from the code; not measured.

**4.25 — `GetRelatedCandidate` may be a main-thread cost inside a Papyrus loop.** It is a Papyrus native, so it runs
on the calling VM thread; `ResolveFromMemoryDB`'s stale-FormID fallback calls `FindByName`, whose last resort is a
live scan of all loaded actors (`NPCIndex.cpp:592–596`). Ten of those in one loop, plus 10 SQL round-trips, is a
plausible hitch source. I did not profile it, and I did not establish whether Papyrus native execution here blocks
the render thread.

---

## Summary judgment for NarrativeEngine

The *shape* of the idea is sound and cheap: an LLM seeds one rumor from real memories, two NPCs record it with
source attribution and a timestamp, and a bio submodule surfaces it in dialogue. That core — steps 5, 6 and 9 — is
about 120 lines of Papyrus and two C++ render helpers, and it works.

The *propagation* half is where it comes apart. The chain has no distortion, no provenance depth, no geography, no
cooldown, no cleanup, no dedup, and no feedback into either DM's context; it can silently corrupt the shared
story-history buffer; and its own loop guard makes long chains improbable. It also accumulates unbounded per-NPC
co-save state with no sweep, which is the single most expensive mistake to fix retroactively.

If NarrativeEngine wants rumor propagation, the parts worth stealing are the **prompt discipline** (traceability +
the explicit "not gossip-worthy" list + reject-if-boring) and the **dual heard/shared bio section with named source
and relative time**. The chain mechanism should be designed from scratch, and whatever replaces it needs a registry
and a sweep from day one.
