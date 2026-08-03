# IntelEngine — Message Delivery / Courier System

> **Reading discipline.** This is a preserved analysis of *one* abandoned mod's solution to "an NPC who can't come
> to the player sends someone who can." IntelEngine shipped, but it was abandoned mid-development and its author
> would not claim it was bug-free. Read the "Weaknesses and bugs" section as seriously as the rest. Nothing here is
> a recommendation for NarrativeEngine.

The README bullet this document covers:

> - Deliver messages from NPCs who can't come themselves

That bullet maps to the **`message` story type** of IntelEngine's LLM "Dungeon Master" — an autonomous courier
system where the DM picks a *sender*, the C++ layer picks a *messenger*, and the messenger physically walks to the
player and speaks the message. It is closely entangled with two player-driven siblings that share the same storage
keys and the same bio submodule, so both are covered:

- **`message` story type** — autonomous, chosen by the LLM Story DM. Sender NPC → **player**, via a courier.
  Entry point `IntelEngine_StoryEngine.HandleMessageDispatch()`.
- **`DeliverMessage` action** — the player asks an NPC in dialogue. Agent NPC → **another NPC**.
  Entry point `IntelEngine_NPCTasks.DeliverMessage()`.
- **`ScheduleDelivery` action** — same, deferred to a future game time.
  Entry point `IntelEngine_Schedule.ScheduleDelivery()`.

**There is no physical item anywhere in this feature.** No `BOOK`, no `MESG`, no note, no inventory transfer. Every
message is a *verbal* message: a string that lives in StorageUtil and gets spoken through SkyrimNet narration. I
grepped the whole `IntelEngine_StoryEngine.psc` for `AddItem`/`RemoveItem`/`EquipItem` and found no matches, and the
delivery functions in `IntelEngine_NPCTasks.psc` contain no inventory calls.

---

## 1. High-level overview, step by step

### 1A. The DM-driven courier (sender who can't come → player)

1. **Story tick fires.** `IntelEngine_StoryEngine` runs a game-time scheduler (default interval **3.0 game hours**,
   `IntelEngine_Core.psc:1696-1698`). It builds a candidate pool of loaded NPCs plus world context and sends one
   prompt (`intel_story_dm.prompt`) to the LLM asking *who* should act and *what type* of story fits.
2. **DM returns JSON.** For this feature the shape is
   (`intel_story_dm.prompt:263`):

   ```json
   {"should_act":true,"type":"message","npc":"SenderName","narration":"wanted to warn about bandit attacks",
    "destination":"The Bee and Barb","msgContent":"I need to speak with you privately","meetTime":"evening"}
   ```

   `npc` is the **sender**, not the carrier. The prompt tells the DM explicitly: *"The system finds a messenger
   automatically"* (`intel_story_dm.prompt:98`), and *`"sender"` = leave EMPTY* (line 99).
3. **Validation gauntlet** (`IntelEngine_StoryEngine.psc:1434-1568`). The named sender is resolved to an exact
   `Actor` from the candidate pool; then hold restriction, story cooldown, C++ field validation, and a
   type-specific check that `msgContent` is non-empty (lines 1560-1567). Notably, `message` is **exempt** from the
   high-status/Jarl travel ban (lines 1484 and 1544) — *because* a courier will do the walking. That exemption is
   the whole reason this story type exists.
4. **Messenger selection** (`HandleMessageDispatch`, `IntelEngine_StoryEngine.psc:2575-2617`). Papyrus calls the
   native `IntelEngine.FindMessengerForSender(senderNPC)`, which runs a five-phase cascade in C++
   (`NPCIndex.cpp:1212-1334`; detailed in §3). If it returns nothing:
   - if the sender is CIVILIAN class, **the sender delivers it personally** (line 2589);
   - otherwise the whole story is dropped (line 2592).
5. **Memory wiring.** Both parties get facts so they can talk about the arrangement later:

   ```papyrus
   Core.InjectFact(senderNPC, "asked " + messengerName + " to deliver a message to "
                              + playerName + ": " + msgContent)
   Core.InjectFact(messenger, "was sent by " + senderName + " to deliver a message to "
                              + playerName + ": " + msgContent)
   Core.SendPersistentMemory(messenger, Game.GetPlayer(),
                             messengerName + " set out to deliver a message from " + ...)
   ```

   (`IntelEngine_StoryEngine.psc:2601-2603`.)
6. **Message stored on the carrier** — four StorageUtil string keys on the messenger actor
   (`IntelEngine_StoryEngine.psc:2610-2613`): `Intel_MessageSender`, `Intel_MessageContent`, `Intel_MessageDest`,
   `Intel_MessageTime`.
7. **Dispatch and travel.** `DispatchToTarget(messenger, player, narration, "story")`
   (`IntelEngine_StoryEngine.psc:1673-1769`) allocates one of 5 task slots, sets a linked-ref travel target,
   applies a travel package, starts stuck/off-screen tracking, caps the off-screen travel estimate at
   **0.25 game hours** for player-targeted stories (lines 1747-1757), and arms a 150 ms C++ proximity monitor for
   fast arrival detection (line 1766). If the player is inside their own home, a *knocking at your door* menu
   fires first (lines 1676-1710) with three outcomes: let them in / send them away / ignore.
8. **Journey monitoring** (3 s poll + proximity callback). Aborts for danger-zone policy, player-home policy, hold
   restriction, blocked location. Stuck escalation can teleport the courier behind the player; a hard timeout at
   `MaxTravelDaysConfig` (**1.0 game day**, `IntelEngine_StoryEngine.psc:45`) teleports **and force-arrives** in one
   step (lines 2482-2496).
9. **Arrival** (`OnMessageArrived`, `IntelEngine_StoryEngine.psc:2619-2668`):
   - Narration is composed as `"delivered a message from " + senderName + ": " + msgContent` and sent to SkyrimNet
     via `DirectNarration` — i.e. the courier *says it out loud* to the player.
   - A persistent SkyrimNet memory is written on courier↔player so both recall it later (line 2636).
   - **Urgency safety net:** if the DM supplied both a meeting destination *and* an urgent-sounding `msgContent`,
     the meeting is dropped as self-contradictory (lines 2638-2647) using the native `IsUrgentMessage()`.
   - **Meeting invitation:** if a destination survives, `Schedule.ScheduleMeeting(sender, msgDest, meetTime)`
     schedules the *sender* to walk to the rendezvous (lines 2649-2662), defaulting to `"evening"` if no time.
   - The event is added to the anti-repetition ring, and the courier switches to a sandbox "linger" package near
     the player until the player walks away (`FinishArrivalWithLinger`, lines 1950-1977).
10. **Termination.** `CleanupStoryDispatch()` clears the slot, removes packages, and unsets all four
    `Intel_Message*` keys on the courier (`IntelEngine_StoryEngine.psc:4892-4940`).

### 1B. The player-driven delivery (NPC → NPC)

1. Player says something like *"go tell Alvor the shipment arrived"*. SkyrimNet matches the `DeliverMessage` action
   YAML and calls `IntelEngine_NPCTasks.DeliverMessage(akAgent, targetName, msgContent, meetLocation, meetTime)`.
2. Validation: agent alive/not in combat, non-empty `msgContent`, duplicate-task guard, an MCM confirmation
   messagebox, a 15-second real-time re-selection cooldown, fuzzy target lookup with a "did you mean?" suggestion,
   target-not-dead / not-disabled / not-self / not-player checks (`IntelEngine_NPCTasks.psc:533-601`).
3. Slot allocation, home-door unlock, follower dismissal, message stored on the **agent** as `Intel_Message`, plus
   `Intel_DeliveryMeetLocation` / `Intel_DeliveryMeetTime` (lines 603-630).
4. Travel: linked ref to the target *actor* (Skyrim pathfinds to cross-cell actor targets natively), walk package if
   in the player's cell, jog if off-screen (lines 638-648).
5. Arrival (`OnArrivedToDeliver`, lines 2133-2209): store the message on the recipient via
   `Core.StoreReceivedMessage()`, write persistent memory, schedule the follow-up meeting for the *recipient* if one
   was requested, then either narrate the delivery in front of the player or (player absent) register a persistent
   event describing it.
6. Return leg: if **Report Back After Delivery** is on (default on, `IntelEngine_Core.psc:221`), the agent jogs back
   to the player, sandboxes, and narrates *"returned after delivering a message to X and wants to report back."*
   (`IntelEngine_NPCTasks.psc:2215-2283`).

`ScheduleDelivery` is the same thing with a game-time delay: it parses a natural time expression, parks the message
in a 10-slot schedule array plus mirrored StorageUtil keys (`IntelEngine_Schedule.psc:223-259`), and
`ExecuteScheduledTask()` later reads them back out (line 541 onward).

---

## 2. Intended gameplay experience

Inferred from the prompt text, the tuning values, and the code comments — the author left no separate design doc for
this feature.

- **"Important people can reach you without breaking immersion."** The load-bearing purpose is stated in a comment:
  high-status NPCs (Jarls, stewards, court wizards, housecarls) are *forbidden* from physically walking to the
  player for every other story type, and `message` plus `quest` are the two exemptions
  (`IntelEngine_StoryEngine.psc:1479-1488`). The C++ selection helper says the same thing from the other side:

  ```cpp
  // Messengers must be common NPCs. Jarls, court wizards, stewards, and
  // housecarls outrank any sender and wouldn't physically run errands —
  // excluding them here prevents absurd substitutions like "Jarl Balgruuf
  // delivers a note on behalf of Farengar".
  ```

  (`NPCIndex.cpp:1220-1223`.) The fantasy being sold is social hierarchy: the Jarl summons you; a guard walks over
  and tells you so.
- **Vanilla Courier, but real.** The cascade deliberately avoids Bethesda's `Courier` actor — there's explicit code
  handling the fact that vanilla Courier/Messenger refs are kept `Disable()`d between deliveries, substituting a
  real walking NPC instead (`IntelEngine_StoryEngine.psc:1440-1469`). The action YAML bans the LLM from naming
  `"Courier"`, `"Messenger"`, `"Traveler"`, `"Stranger"` as targets (`intel_delivermessage.yaml:41-43`). The intent
  is that the person who brings you news is *somebody you could have met*, from the sender's own household or hold.
- **Local, plausible logistics.** `HoldPolicyMessage = 2` — "Same hold (except followers) — messages go via local
  messengers" (`IntelEngine_StoryEngine.psc:77`). The messenger cascade prefers, in order, someone from the
  sender's household, then a social associate with shared event history, then a hold guard, then a townsfolk. The
  designed feeling is *"Balgruuf sent his housecarl"*, not *"a random Nord materialised"*.
- **Messages create follow-up plot.** The optional `destination` + `meetTime` fields turn a message into an
  *invitation*: the sender is scheduled to travel to the named place at the named hour and wait there. That hooks
  the courier feature into the scheduling/meeting system, so a message can become an actual appointment the player
  keeps or blows off (and no-shows are remembered elsewhere in the mod).
- **Consistency policing.** The DM prompt insists *"msgContent and meetTime MUST be consistent. If meetTime is set,
  msgContent should reference the time"* (`intel_story_dm.prompt:104`) and forbids night meetings at palaces or
  shops as trespassing (line 103). The `IsUrgentMessage` back-stop exists because the LLM kept saying "come at
  once" *and* "meet me this afternoon" in the same breath — the code comment says exactly that
  (`IntelEngine_StoryEngine.psc:2638-2640`).
- **First contact matters.** The DM prompt has a whole "Player familiarity" rule set: a `stranger` courier must not
  use the player's name and should ask *"Are you the one they call …?"* (`intel_story_dm.prompt:189-191`), and the
  quest-courier path injects a fact telling the NPC they were "given a physical description and told to look for
  them" (`IntelEngine_StoryEngine.psc:3131`).
- **The player is never handed a UI.** No journal entry, no note in the inventory, no message box with the text.
  The message arrives as spoken dialogue only. The design bet is that SkyrimNet's narration → speech pipeline makes
  a talking courier more immersive than a quest log line. The cost is that a distracted player can miss the content
  entirely; the only durable record is inside SkyrimNet's memory DB.

---

## 3. Implementation breakdown

### 3.1 Files

Papyrus (`IntelEngine-NativePlugin/Source/Scripts/`):

- `IntelEngine_StoryEngine.psc` — `HandleMessageDispatch` (2575), `OnMessageArrived` (2619),
  dispatch/validation (1330-1627), travel monitor (2340-2497), cleanup (4881-4940).
- `IntelEngine_NPCTasks.psc` — `DeliverMessage` (516), `OnArrivedToDeliver` (2133), `BeginDeliveryReturn` (2215).
- `IntelEngine_Schedule.psc` — `ScheduleDelivery` (223), `PrepareScheduleSlot` (272), `ExecuteScheduledTask` (541).
- `IntelEngine_Core.psc` — `StoreReceivedMessage` (423), `SendTaskNarration` (1460), `SendPersistentMemory` (1466),
  `InjectFact` (1487), `NotifyPlayer` (1637), Director dispatch (2252).

C++ (`IntelEngine-NativePlugin/SKSE/src/`):

- `NPCIndex.cpp` — `FindMessengerForSender` (1212), `IsEligibleStoryCandidate` (1014), `PassesHoldRestriction` (1458).
- `Papyrus.cpp` — `StoryResponseGetField` (1861), `IsUrgentMessage` (5056), `BuildStuckNarration` (5047),
  `ArmProximityArrival` (1370).
- `SlotTracker.{h,cpp}` — 5-slot task state, SKSE co-save serialization; does **not** carry message text.
- `ProximityMonitor.{h,cpp}` — 150 ms arrival-detection worker; no persistence.

SkyrimNet assets (`IntelEngine-GamePlugin/SKSE/Plugins/SkyrimNet/`):

- `config/actions/intel_delivermessage.yaml` — player-driven NPC→NPC action definition.
- `config/actions/intel_scheduledelivery.yaml` — deferred variant.
- `prompts/intel_story_dm.prompt` — `message` story-type spec (96-106) and JSON exemplar (263).
- `prompts/submodules/character_bio/0197_intel_received_messages.prompt` — renders a received message into a bio.

Dashboard: `web/dashboard/src/components/DirectorTab.jsx` — manual `message` dispatch form (128-140).

No ESP form is specific to this feature. It reuses the shared `IntelEngine` quest, its 5 agent + 5 target reference
aliases (`Core.GetTargetAlias(slot)`), the shared travel packages (`TravelPackage_Walk`, `TravelPackage_Jog`,
`SandboxNearPlayerPackage` at priorities `PRIORITY_TRAVEL` / `PRIORITY_SANDBOX`), and the `IntelEngine_TravelTarget`
linked-ref keyword driven through `PO3_SKSEFunctions.SetLinkedRef`. `docs/ESP_STRUCTURE.md:29` lists
`IntelEngine_NPCTasks` as "NPC fetch/message/escort tasks" and defines no message-specific record.

### 3.2 Carrier selection — the five-phase cascade

`NPCIndex::FindMessengerForSender()` (`NPCIndex.cpp:1212-1334`). Common gate for every phase
(`isValidMessenger`, lines 1219-1227): not the sender, not the player, **not high-status**, and passes
`IsEligibleStoryCandidate` — which additionally requires not deleted, has a parent cell, **not in combat**, not a
player teammate, not hostile to the player, and **not in the player's current cell**
(`NPCIndex.cpp:1014-1027`).

1. **Household members of the sender** — bed-ownership index via `LocationResolver::GetHouseholdMembers`, matched
   against loaded actors by `TESNPC` base pointer.
2. **Social associates** — top 10 from SkyrimNet's memory DB by shared-event score, **filtered to the sender's
   hold**.
3. **Same-hold guards** — archetype `GUARD`; random pick via `std::mt19937`.
4. **Same-hold civilians** — archetype `CIVILIAN`; random pick.
5. **Any-hold civilian or guard** — explicit last resort, commented *"Breaks perfect hold fidelity but keeps
   dispatch from stalling on 'Courier'"* (lines 1319-1321).

Phases 3-5 share a single `ForEachLoadedActor` sweep (lines 1285-1297) — so the candidate set is **only actors in
loaded cells**. Returns `nullptr` if all five phases come up empty.

### 3.3 Gates, cooldowns, and magic numbers

- **Story tick `3.0` game hours** (`Core.psc:1696-1698`, `Intel_MCM_StoryInterval`) — how often the DM runs at all.
- **Per-NPC story cooldown `24.0` game hours** (`Core.psc:1700-1702`, `Intel_MCM_StoryCooldown`) —
  `ApplyCooldownCheck` (`StoryEngine.psc:1078-1099`) blocks re-picking the same NPC.
- **`HoldPolicyMessage = 2`** (`StoryEngine.psc:77`) — cross-hold senders blocked unless the sender is a potential
  follower (`NPCIndex.cpp:1490-1493`).
- **`MaxTravelDaysConfig = 1.0` game day** (`StoryEngine.psc:45`) — hard travel timeout → teleport + force-arrive.
- **`MAX_STORY_OFFSCREEN_HOURS = 0.25`** (`StoryEngine.psc:1748`) — cap on the off-screen travel estimate for
  player-targeted stories.
- **`MONITOR_INTERVAL = 3.0` s** (`StoryEngine.psc:36`) — Papyrus arrival poll; the C++ monitor runs at 150 ms.
- **5 task slots / 10 schedule slots** (`SlotTracker.h:24`, `IntelEngine_Schedule.psc`) — and only **one** story
  dispatch is active globally (`IsActive`).
- **15 s real-time cooldown** (`NPCTasks.psc:564-568`) — anti re-selection loop on player-driven delivery.
- **Facts capped at 10 per NPC, FIFO** (`Core.psc:1498-1504`) — how message facts age out of a bio.
- **Urgency word list, 8 phrases** (`Papyrus.cpp:5060-5063`) — `"immediate"`, `"right now"`, `"at once"`,
  `"right away"`, `"urgently"`, `"without delay"`, `"this instant"`, `"as soon as possible"`.
- **Meeting time default `"evening"`** (`StoryEngine.psc:2657-2659`) — when the DM gives a destination but no time.

### 3.4 State: StorageUtil keys and what persists

Format: **key** (type, owner actor) — written by → cleared by.

- **`Intel_MessageSender`** (String; **courier** on the story path, **recipient** on the player-driven path) —
  `StoryEngine.psc:2610` and `Core.psc:438` → `CleanupStoryDispatch` (4918), `CleanupQuest` (4636).
- **`Intel_MessageContent`** (String, courier) — `StoryEngine.psc:2611` → same as above.
- **`Intel_MessageDest`** (String, courier) — `StoryEngine.psc:2612` → `CleanupStoryDispatch` (4920).
- **`Intel_MessageTime`** (**String**, a word like `"evening"`, courier) — `StoryEngine.psc:2613` →
  `CleanupStoryDispatch` (4921).
- **`Intel_MessageTime`** (**Float**, game time, recipient) — `Core.psc:439` → **never cleared**.
- **`Intel_ReceivedMessage`** (String, recipient) — `Core.psc:437` → **never cleared**.
- **`Intel_Message`** (String, agent on the player-driven path) — `NPCTasks.psc:626` → `Core.psc:656` when task
  history is saved.
- **`Intel_DeliveryMeetLocation`** / **`Intel_DeliveryMeetTime`** (String, agent) — `NPCTasks.psc:629-630` →
  `NPCTasks.psc:2174-2175`.
- **`Intel_ScheduledMessage`** and siblings (String, agent) — `Schedule.psc:249-251` → `ClearScheduleSlot`.
- **`Intel_StoryNarration`**, **`Intel_IsStoryDispatch`** (String/Int, courier) — `StoryEngine.psc:1736-1737` →
  `CleanupStoryDispatch`.

**Persistence model.** Three separate stores, and the message text is only in one of them:

- **StorageUtil (PapyrusUtil co-save)** holds every `Intel_Message*` string. This is where the actual message text
  lives across save/load.
- **The `.ess` save** holds Papyrus quest-script properties: `ActiveStoryType`, `ActiveStoryNPC`, `ActiveNarration`,
  `IsActive`, and the `ScheduledMessages[]` array.
- **The SKSE co-save (`SlotTracker::Save/Load`)** holds slot state only: agent FormID, state, task type, target
  name, speed, deadline, off-screen arrival (`SlotTracker.h:26-38`). **No message text.** Task recovery on load
  rebuilds packages from this, then reads the text back out of StorageUtil.
- Nothing about a pending message is written to a `TESForm` field, and there is no `MESG`/`BOOK` record, so Skyrim's
  ASCII-only engine string fields are not directly in the path — but `Debug.Notification` is (see §4).

### 3.5 How the LLM text is extracted, stored, and displayed — and whether it is sanitized

Extraction is `ExtractJsonField(response, "msgContent")` → `IntelEngine.StoryResponseGetField` →
`Papyrus.cpp:1861-1949`. The relevant lines:

```cpp
for (auto& [key, value] : j.items()) {
    std::string keyLower = key;                       // keys lowercased
    if (value.is_string()) {
        s_cachedFields[keyLower] = value.get<std::string>();   // <-- verbatim, no sanitizer
    } else if (value.is_null()) { ... } else { s_cachedFields[keyLower] = value.dump(); }
}
...
return RE::BSFixedString(result.c_str());
```

**No sanitization is applied.** The raw UTF-8 bytes the LLM produced — smart quotes, em-dashes, ellipses, NBSPs,
accented letters — pass straight into a `BSFixedString` and from there into StorageUtil, into narration strings, and
into `Debug.Notification`. The codebase *does* contain two sanitizers, but neither is on this path:

- `MemoryDB::SanitizeForPrompt` (`MemoryDB.cpp:1191-1280`) — a JSON-escaping pass that emits non-ASCII as
  `\uXXXX`. Applied to memory-DB text being embedded in prompts, not to `msgContent`.
- `NPCIndex::SanitizeForHistory` + `TruncateUtf8Inplace` (`NPCIndex.cpp:2568-2606`) — applied to the *narration*
  and NPC name recorded in the dispatch-history ring buffer (`RecordStoryDispatch`, called at
  `StoryEngine.psc:1432`), but **not** to `msgContent`.

Display paths for the text:

- `Core.SendTaskNarration(...)` → `SkyrimNetApi.DirectNarration(msgText, actor, target)` (`Core.psc:1460-1463`) —
  the courier speaks it.
- `Core.SendPersistentMemory(...)` → `SkyrimNetApi.RegisterPersistentEvent` (`Core.psc:1466-1471`) — written to
  SkyrimNet's event DB.
- `Core.InjectFact(...)` → StorageUtil string list, pre-rendered by the native `RenderFactsSection` into
  `Intel_FactsRendered`, surfaced by `0800_intel_facts.prompt`.
- `0197_intel_received_messages.prompt` for NPC→NPC deliveries:

  ```jinja
  {% set received_msg = papyrus_util("GetStringValue", actorUUID, "Intel_ReceivedMessage", "") %}
  {% set msg_from    = papyrus_util("GetStringValue", actorUUID, "Intel_MessageSender", "") %}
  ...
  {{ msg_from }} came to me recently and said:
  > "{{ received_msg }}"
  ```

  plus a relative-time line computed from the float `Intel_MessageTime` ("just now" … "some time ago").
- `Debug.Notification` via `Core.NotifyPlayer` (`Core.psc:1637-1640`) and `Core.DebugMsg` when debug mode is on
  (`Core.psc:1788-1793`). `DebugMsg` is called with strings that embed the raw message text — e.g. `InjectFact`'s
  trailing `DebugMsg("Fact injected into … : " + factText)` (`Core.psc:1524`), where `factText` contains
  `msgContent`.

### 3.6 The bio-side story (what the courier "knows" while walking)

The courier's slot task type is `"story"`, not `"deliver_message"` (`DispatchToTarget(messenger, player, narration,
"story")`), so `0801_intel_task_awareness.prompt:34` renders *"I am currently on my way to find …"* — it does **not**
say "delivering a message". The message content reaches the courier's dialogue context solely through the injected
fact from step 5, which surfaces in `0800_intel_facts.prompt`. `0197_intel_received_messages.prompt` is *not* used
for the story courier at all — only for NPC→NPC deliveries, where `Core.StoreReceivedMessage()` writes the trio of
keys onto the recipient.

### 3.7 Manual override

The PrismaUI dashboard's Director tab exposes a hand-authored `message` dispatch: `msgContent` (required),
`destination`, `meetTime` — with the hint *"A messenger NPC is auto-selected."*
(`DirectorTab.jsx:128-140`). It routes through `Core.OnDashboardDispatchStory` (`Core.psc:2252+`) into the same
`HandleMessageDispatch`, deliberately skipping cooldown and MCM checks.

---

## 4. Weaknesses and bugs

### Confirmed by reading the code

1. **No sanitization of LLM text, anywhere on this path.** `StoryResponseGetField` returns
   `value.get<std::string>()` verbatim (`Papyrus.cpp:1910`). The message text then reaches `Debug.Notification`
   (through `Core.DebugMsg`, e.g. `Core.psc:1524`), StorageUtil co-save payloads, and re-entrant LLM prompts. The
   two sanitizers that exist in the codebase (`MemoryDB::SanitizeForPrompt`, `NPCIndex::SanitizeForHistory`) are
   applied to *other* fields — narration and memory-DB text — never to `msgContent`. Any smart quote or em-dash the
   model emits goes through untouched.

2. **`Intel_MessageSender` is one key with two incompatible meanings.** `Core.StoreReceivedMessage` writes it on the
   **recipient** to mean *"who sent me the message I'm holding"* (`Core.psc:438`). `HandleMessageDispatch` writes it
   on the **courier** to mean *"whose message am I carrying"* (`StoryEngine.psc:2610`). An NPC who received a
   message earlier and is later chosen as a courier gets `Intel_MessageSender` overwritten while
   `Intel_ReceivedMessage` still holds the *old* message — and `0197_intel_received_messages.prompt` renders the
   pair unconditionally (it only checks `received_msg != ""`). Result: *"«new sender» came to me recently and said:
   «old, unrelated message»"*. Symmetrically, `CleanupStoryDispatch` unsets `Intel_MessageSender`
   (`StoryEngine.psc:4918`), which silently strips the attribution off a genuine received message that NPC was
   holding, leaving the bio to render *" came to me recently and said: …"* with an empty name.

3. **`Intel_ReceivedMessage` is never cleared and holds exactly one message forever.** Grepping the whole repo, the
   only writer is `Core.psc:437` and there is no `UnsetStringValue` for it anywhere. Every NPC therefore carries
   their most recent received message in their bio permanently, and each new delivery silently destroys the previous
   one. There is no history and no expiry — unlike facts, which are FIFO-capped at 10.

4. **`Intel_MessageTime` is a Float on one path and a String on the other.** Float game-time on the recipient
   (`Core.psc:439`), a word like `"evening"` on the courier (`StoryEngine.psc:2613`). StorageUtil keeps separate
   namespaces per type so they don't clobber each other, but `CleanupStoryDispatch` only unsets the String variant
   (`4921`). The Float is never cleared, so once bug 3 has stranded a message, the "This was … ago" line in the bio
   drifts to *"some time ago"* and stays there.

5. **Orphaned message keys when dispatch bails after they're written.** `HandleMessageDispatch` writes the four
   `Intel_Message*` keys (2610-2613) and *then* calls `DispatchToTarget`, which can return early without cleanup in
   three places: player-home knock answered "Send them away" (1696-1701), "Ignore"/timeout (1702-1707), and no free
   task slot (1712-1722). All three just reset `ActiveStoryType = ""` and `return` — `ActiveStoryNPC` was never set
   (it's assigned at line 1732), so `CleanupStoryDispatch` can never find that actor to clean it. The keys leak onto
   the courier permanently, which then feeds bug 2.

6. **A failed courier leaves no trace for the sender.** When `AbortStoryTravel` fires (danger zone, hold
   restriction, blocked location, stuck-with-teleport-disabled), `CleanupStoryDispatch` unsets `Intel_MessageContent`
   and the message is simply gone. The sender still carries the fact *"asked X to deliver a message to the player"*
   with no counterpart saying it never arrived. The quest path *does* handle this — it injects *"never heard back
   about the … threat"* (`StoryEngine.psc:4898`) — so the omission on the message path looks like an oversight, not
   a decision.

7. **The timeout is a teleport, not a failure.** At `MaxTravelDaysConfig` (1.0 game day), if `AllowStuckTeleport` is
   on (default true), the courier is teleported behind the player and `OnStoryNPCArrived()` is called immediately
   (`StoryEngine.psc:2487-2495`). So "carrier never arrives" is largely designed away — at the price of a courier
   materialising from nowhere. The comment above it documents an *earlier* version of this bug: teleporting to
   3500 units without clearing task state caused an infinite re-teleport loop every 3 seconds. With teleport
   disabled, the same timeout aborts silently with no narration at all (line 2493).

8. **Meeting scheduling re-resolves the sender by display name.** `OnMessageArrived` calls
   `IntelEngine.FindNPCByName(senderName)` (`StoryEngine.psc:2653`) rather than keeping the sender `Actor` that was
   already resolved by exact FormID at dispatch time. Fuzzy display-name lookup on a generic name (a hold guard, a
   modded duplicate) can schedule the *wrong* actor to walk to the rendezvous. IntelEngine's own name-resolution
   failure modes are documented elsewhere in this prior-art set; this is an instance of the same pattern.

9. **The self-delivery fallback contradicts the feature's premise.** If no messenger is found and the sender is
   CIVILIAN class, the sender walks the message over personally (`StoryEngine.psc:2586-2595`). The feature is
   advertised as "NPCs who *can't come themselves*". Downstream, `OnMessageArrived`'s
   `senderName != ActiveStoryNPC.GetDisplayName()` test (line 2627) suppresses the "delivered a message from …"
   narration, but the meeting-scheduling block at 2650 still fires, scheduling the NPC standing right in front of
   the player to travel to the rendezvous.

10. **Hold restriction is checked on the sender, not the carrier.** `CheckHoldRestriction(npc, storyType)`
    (`StoryEngine.psc:1491`) is evaluated against `npc`, which for `message` is the **sender**. With
    `HoldPolicyMessage = 2`, a sender in a different hold is rejected outright unless they're a potential follower
    (`NPCIndex.cpp:1490-1493`) — i.e. the *far-away sender* case the courier system exists to serve is the one it
    blocks. Meanwhile the carrier, who does all the walking, is never hold-checked against the player's hold at all.

11. **Carrier selection is biased toward whoever happens to be loaded.** Phases 3-5 sweep
    `ProcessUtils::ForEachLoadedActor` (`NPCIndex.cpp:1285`), and phase 5 explicitly abandons hold fidelity to avoid
    stalling (lines 1319-1329). In practice the courier is often just a nearby NPC, which undercuts the "sent from
    afar" fiction. Conversely, in a sparsely-populated interior the cascade returns `nullptr` and the entire story
    is discarded for any non-civilian sender.

12. **`IsUrgentMessage` is a hardcoded eight-phrase English substring match** (`Papyrus.cpp:5056-5068`), lowercased
    and matched with `find()`. It misses *"hurry"*, *"come quickly"*, *"now"*, and false-positives on negations like
    *"not immediately"*. It is also only consulted when *both* a destination and a meet time are present
    (`StoryEngine.psc:2641`). This is a by-feel patch over LLM inconsistency, not a general solution.

13. **`"evening"` is a silent default meeting time.** If the DM supplies a destination but no `meetTime`,
    the sender is scheduled for the evening (`StoryEngine.psc:2657-2659`) and the player is told nothing about when.

14. **Player-driven `DeliverMessage` fires side effects before its own guards.** The order in
    `IntelEngine_NPCTasks.psc` is: register the SkyrimNet event *"left to deliver a message to X (task in
    progress)"* (line 558) → `Core.OverrideExistingTask(akAgent)` (561) → cooldown check that can `Return false`
    (564-568) → target lookup that can `Return false` (571-581) → **free-slot check** that can `Return false`
    (604-608). So on any of those failures the NPC's memory already says they left, and any task they were
    previously doing has already been cancelled.

15. **The message is attributed to the courier, not the author.** For player-driven deliveries,
    `Core.StoreReceivedMessage(target, agent, msgContent)` (`NPCTasks.psc:2148`) passes the *agent* as sender, so
    the recipient's bio renders *"«courier» came to me recently and said …"*. That the player composed the message
    is lost from the recipient's context entirely.

16. **Raw LLM text is interpolated into a prompt inside quotes, unescaped.**
    `Core.SendTaskNarration(target, agentName + " found " + targetName + " and delivered a message: \"" +
    msgContent + "\"", agent)` (`NPCTasks.psc:2189`). One model's free-form output is spliced into another model's
    input with quote characters around it and no escaping — a quote-breaking and prompt-injection surface.

17. **The C++ proximity fast-path does not survive a save/load.** `ArmProximityArrival` is only called at dispatch
    time (13 call sites, all in dispatch functions), and `ProximityMonitor.h` contains no `Save`/`Load`/serialization
    at all. After reloading mid-journey, arrival detection falls back to the 3-second Papyrus poll, which the
    comment at `StoryEngine.psc:1762-1765` says was causing "face-bumping" — the exact problem the monitor was added
    to fix.

18. **Only one story dispatch can be in flight at a time.** `IsActive` / `ActiveStoryNPC` / `ActiveStoryType` are
    single-valued quest properties, so a courier occupies the entire story engine until it arrives, aborts, or times
    out (up to a full game day). There is no queue; competing messages are simply never generated.

19. **Stale documentation in code.** The `InjectFact` header comment says facts appear in
    `0497_intel_facts.prompt` (`Core.psc:1483`); the shipped file is `0800_intel_facts.prompt`. Minor, but it is the
    kind of drift that makes the prompt layer hard to audit.

### Suspected — plausible from the code but not proven by reading alone

- **Divergence between the `.ess` and the PapyrusUtil co-save yields empty messages.** `ActiveStoryType` lives in
  the `.ess` while `Intel_MessageContent` lives in StorageUtil. If the two ever disagree (co-save lost, PapyrusUtil
  reinstalled, mid-flight save restored across a mod change), `OnMessageArrived` has no guard: it will narrate
  `"delivered a message from : "` with both fields empty. I did not find any validation of these fields at arrival
  time, but I also could not confirm the failure in-game.
- **`FindMessengerForSender` runs a full loaded-actor sweep plus a memory-DB query synchronously from Papyrus.**
  Phase 1 additionally does a nested loop over household bases inside `ForEachLoadedActor`. In a crowded city cell
  this is called on the Papyrus→native path during a story tick. Likely a frame-time spike; not measured.
- **Randomised guard/civilian picks use `static thread_local std::mt19937 rng(std::random_device{}())` re-declared
  in three separate blocks** (`NPCIndex.cpp:1301, 1311, 1323`). Functionally fine, but three independent generators
  in one function is a smell and suggests copy-paste rather than intent.
- **`Intel_FactNPCs` registry growth.** `InjectFact` appends every touched NPC's FormID to a player-held IntList
  (`Core.psc:1517-1522`) with a comment saying a Maintenance sweep cleans it. I read the add path but not the sweep,
  so I cannot confirm the list is actually bounded over a long playthrough.
- **No delivery-failure path when the recipient of an NPC→NPC message moves during travel.** The destination is the
  target `Actor` itself and Skyrim re-pathfinds, so this probably works; but with `MaxTravelDaysConfig` applying to
  story dispatch and a separate distance-based deadline applying to tasks
  (`SetDistanceBasedDeadline`, `NPCTasks.psc:660`), a target who keeps moving may exhaust the deadline. Not traced
  to a concrete failure.
