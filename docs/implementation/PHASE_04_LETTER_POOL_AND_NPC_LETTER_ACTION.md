# Phase 04 — Letter Pool and NPC Letter Action

The Director gains its first **social** lever — and the plugin gains a new general-purpose
subsystem along the way. This phase stands up a **LetterPool** (a reusable manager for a small
pool of overrideable Book records with dynamic content) and ships **one** action that uses it:
`NPCLetterAction`, which has a known NPC deliver a generated letter to the player by courier,
shaped to either raise or lower current tension.

---

## Why this phase exists

After Phase 03 the Director has exactly one lever and it's purely tension-*raising* (Ambush).
The narrative pyramid needs the other direction too: Falling Action and Resolution call for
beats that *de-escalate* — closure, acknowledgment, reflection, a small kindness — and Ambush
can't supply those. Letters are the cleanest first-class lowering move because:

- They're the **outdoor / safe-territory catch-all** in the way Ambush is for the
  raising side: a vanilla courier reaches the player anywhere in inhabited Skyrim that
  isn't actively hostile territory — exteriors of cities and towns, roads, settlements,
  player homes, inns, shops. Interior cells are fine specifically because vanilla's
  courier system fires on cell-change triggers, so a letter dispatched while the player
  is in an inn or a house lands the moment they step outside. The action gates only on
  "currently in dangerous location" (dungeons, bandit camps, lairs) — those are
  blacklisted because (a) couriers won't go there and (b) it's not a narratively right
  moment for a letter anyway.
- They're **polarity-flexible**: the same dispatch mechanism delivers a warm thank-you letter
  (lowering) or a menacing creditor demand (raising). Tone is set by the generated content,
  not by a different action type. So one action covers a wide slice of both directions.
- They have **emergent follow-through**: the letter's body enters SkyrimNet's event log and
  player-memory store, where it can shape future tick reads — both by other NPCs and by the
  Director itself. A letter delivered today is bait the Director may choose to follow up on
  tomorrow.
- They generalize to a **reusable subsystem**. The LetterPool is the first instance of a
  pattern this plugin will use for other dynamic in-game text: notes the player finds, signs
  rewritten by background sims, message-board postings, the dialogue snippet on a quest
  journal entry. Anything where we want a small pool of overrideable Book records driven by
  runtime content. Solving it once for letters means later content-mutating actions are an
  afternoon of integration rather than a re-derivation of all the engine plumbing.

The smoke test in `LetterSmokeTest.cpp` already proved every individual piece of engine
plumbing — runtime title mutation via `SetFullName`, body substitution via MinHook on
`BookMenu::OpenBookMenu`, plugin-side body access via MinHook on
`TESDescription::GetDescription`, read detection via `MenuOpenCloseEvent` filtered to
`BookMenu::MENU_NAME`, `kHasBeenRead` clear on recycle. Phase 04 is the productionization:
a real pool with eviction, per-slot persistence, a real action that uses it, and SkyrimNet
memory integration on both sides of the exchange.

---

## Scope

### In scope

- A **LetterPool** subsystem managing a pool of 20 pre-authored Book forms. Allocation,
  state tracking, eviction, location tracking, hook-backed body / title substitution,
  co-save persistence, and SkyrimNet memory writes on key lifecycle events.
- A second LLM prompt — `narrative_engine_letter_compose` — that generates per-letter
  content: sender NPC, letter body, mood tag, importance score.
- An `NPCLetterAction` (`IAction`) implementation:
  - `Polarity` = `Either` — the same action handles both raise and lower; content drives
    direction.
  - Requires the player to have at least N recent NPC interactions in SkyrimNet's memory
    (the sender candidate pool); otherwise `IsAvailable` returns false.
  - Requires the player to NOT be in a dangerous-location-keyword cell (dungeon,
    bandit camp, hostile lair, etc.). Interior cells in safe contexts (homes, inns,
    shops) are explicitly fine — the vanilla courier system queues the delivery and
    spawns on the player's next cell-change.
  - On `Start`: allocates a LetterPool slot, runs the content-generation LLM call,
    populates the slot, **VM-dispatches to vanilla `WICourierScript.AddItemToContainer`**
    so the existing vanilla courier system delivers the letter. No custom courier quest,
    no custom AI package, no custom Papyrus.
  - After dispatch, the action's `DetectAndRollbackFailedStart` and `DetectCompletion`
    polls verify the dispatch by checking the vanilla courier's inventory for the
    queued letter. If the letter appears within a short verification window
    (`iLetterDispatchVerifyDelaySeconds`, default 5s), the action completes
    normally and the global cooldown starts. If the letter is absent past the
    window, the dispatch is treated as failed: the slot is released back to the
    pool, no cooldown is applied, and the action can re-fire on the next tick.
  - Physical delivery, player read, and player discard happen on the vanilla
    courier's own timeline (potentially minutes later) and are tracked separately
    by the LetterPool for memory and eviction purposes — independent of Director
    timing.
- SkyrimNet memory writes (asymmetric, mirroring actual cognitive engagement):
  - On **delivery**: sender NPC gets an `EXPERIENCE` memory of having sent the
    letter. They engaged with it at the act of writing/sending; they remember
    regardless of whether the player ever reads.
  - On **read**: player gets an `EXPERIENCE` memory of having received the
    letter. The player only "knows what was in the letter" if they actually
    open it; an unread letter leaves no memory.
- Extension of `ActionContext` to include `DesiredDirection` so actions whose Polarity is
  `Either` know which way the Director wants tension to move.
- ESP content: 20 letter Book forms. No custom quest, no custom aliases, no custom
  AI package — the vanilla courier system handles dispatch.
- Co-save persistence for LetterPool slot state (body text, sender label, sender FormID,
  timestamps, state, current container handle).
- Dashboard surface: small "letter pool" panel showing pool occupancy, plus the existing
  fired-action line picks up "npc_letter" entries.

### Deferred (explicitly out)

- **Player reply letters.** No write-back surface for the player to author replies — that's
  its own design question and not required to make incoming letters work.
- **Multi-recipient or chain letters.** One sender, one player, one letter per dispatch.
- **Letter contents reacting to player choices.** The letter body is generated at dispatch
  time and is frozen from then on — no mid-flight rewrite if the world changes between
  dispatch and read.
- **Background-sim integration.** The letter's sender is picked from the player's
  SkyrimNet interaction history, not from any background simulation. When sims arrive in a
  later phase they can become an additional sender source.
- **CNAM tooltip mutation.** The smoke test confirmed CNAM is the inventory tooltip; this
  phase leaves the tooltip as whatever the ESP-baked default says ("A sealed letter"). The
  body is the player-facing payload; tooltip variation is polish for later.
- **Letter physical delivery animation polish.** The courier walks up and hands it over;
  no custom dialogue scene, no envelope-handoff animation. Vanilla courier feel.
- **A second LetterPool consumer.** The pool is built as a general subsystem but only
  `NPCLetterAction` uses it this phase.
- **Per-action cooldown** for `NPCLetterAction` beyond the global dispatcher cooldown.
  Letters can fire as often as the global cooldown allows.
- **Letter delivery during dungeon runs or other dangerous-location time.** Vanilla
  couriers don't enter dungeons, and even if we queued the delivery during a dungeon
  it might sit for many minutes of real-time before the player surfaces. We gate
  `IsAvailable` on "not in a dangerous-location-keyword cell" instead. Future work
  could add an indoor-delivery alternate path ("a folded note tucked into a side
  pocket of your pack") for cases where the Director really wants to interrupt a
  dungeon with a letter beat — but Resolution / Falling Action moments rarely happen
  mid-dungeon anyway, so this is more polish than core need.

---

## Core concepts

### LetterPool overview

A new module — `include/LetterPool.h` / `src/LetterPool.cpp` — owning a fixed pool of N
pre-authored Book forms (N = 20, configurable). Each form has a stable EditorID
(`_ne_PooledLetter01` through `_ne_PooledLetter20`) so the pool can resolve them at
`kDataLoaded` via `RE::TESForm::LookupByEditorID`.

The pool exposes a thin API for the action layer:

```cpp
// include/LetterPool.h
namespace NarrativeEngine::LetterPool
{
    struct AllocatedSlot
    {
        std::size_t slotIndex;      // for downstream tracking
        RE::FormID  bookFormID;     // the actual Book form to use
    };

    enum class AllocationFailure : std::uint8_t
    {
        PoolNotResolved,    // no Book forms could be looked up
        EvictionFailed,     // no slot could be freed (e.g. all in-flight pre-delivery)
    };

    // Allocate the next available slot. Order:
    //   1. A slot in state Free.
    //   2. If none free: oldest slot in state Read (by readAt), with full reference
    //      cleanup before returning.
    //   3. If none read: oldest slot in state InInventory (by deliveredAt), with full
    //      reference cleanup before returning.
    //   4. If none of those (everything in flight pre-delivery, very rare): return
    //      failure.
    std::expected<AllocatedSlot, AllocationFailure> Allocate();

    // Populate slot content (called after LLM round-trip). Mutates the underlying Book
    // form's FULL field via SetFullName and caches the body in the in-memory map the
    // hooks read from. Does NOT add to player inventory — that's the vanilla
    // courier's job once we VM-dispatch to WICourierScript.AddItemToContainer.
    void PopulateSlot(std::size_t        slotIndex,
                      std::string         senderLabel,    // title (FULL)
                      std::string         body,           // hook-substituted body
                      RE::FormID          senderNpcFormID,
                      const std::string&  topicTag);

    // Lifecycle transitions called by the action / quest / event sinks.
    // No container handle is stored on the slot — handlers receive transient
    // container refs as parameters where needed for synchronous cleanup, then
    // discard them. See "Slot container tracking" below.
    void MarkDelivered(std::size_t slotIndex);
    void MarkRead(std::size_t slotIndex);
    void MarkDiscardedToContainer(std::size_t slotIndex, RE::ObjectRefHandle newContainer);
    void MarkDroppedToCell(std::size_t slotIndex, RE::ObjectRefHandle worldRef);

    // Failure path called by NPCLetterAction when its post-dispatch verification
    // poll (DetectAndRollbackFailedStart) shows the letter never landed in the
    // vanilla courier's inventory. No reference cleanup is needed (the dispatch
    // never actually placed anything in the world); the slot just resets to Free.
    void AbortPending(std::size_t slotIndex);

    // Hook-side body lookup. Returns true and fills `outBody` if the FormID is one of
    // our pool forms and currently has cached content. The MinHooked GetDescription and
    // OpenBookMenu both call this.
    bool TryGetBody(RE::FormID bookFormID, std::string& outBody);

    void OnSave(SKSE::SerializationInterface*);
    void OnLoad(SKSE::SerializationInterface*, std::uint32_t version, std::uint32_t length);
    void OnRevert();

    // For the dashboard.
    struct PoolStats { std::size_t free; std::size_t inInventory; std::size_t read; std::size_t discarded; };
    PoolStats GetStats();
}
```

### Per-slot state

Each pool slot carries:

```cpp
struct Slot
{
    RE::FormID            bookFormID;       // resolved at kDataLoaded
    State                 state;
    std::string           body;             // hook-substituted text
    std::string           senderLabel;      // FULL field ("Letter from Ysolda")
    RE::FormID            senderNpcFormID;  // for SkyrimNet memory attribution
    std::string           topicTag;         // short label for memory text
    double                deliveredAt;      // initially set to "queued-with-courier"
                                            // time at PopulateSlot; overwritten with
                                            // actual delivery time at MarkDelivered.
                                            // Used both for "oldest InInventory"
                                            // eviction and for the PendingDelivery
                                            // timeout reconciliation on load.
    double                readAt;           // Unix-epoch real seconds; 0 if unread
};

enum class State : std::uint8_t
{
    Free,                  // slot unused
    PendingDelivery,       // populated; courier has not yet handed off
    InInventory,           // courier delivered; player has not yet read
    Read,                  // player closed BookMenu after opening
    Discarded,             // player removed via sell/drop/transfer
};
```

`state == Free` is the only state for which `currentContainer` and the timestamps are
meaningless. Every other state has a meaningful `currentContainer` — the player's
inventory ref for `InInventory` / `Read`, the destination container or cell ref for
`Discarded`, the courier's inventory during `PendingDelivery`.

### Allocation policy

1. **Free** — return a slot in state Free. Cheapest, no eviction needed.
2. **Oldest Read** — if no Free slots, evict the slot with the smallest non-zero `readAt`.
   The player has already engaged with this letter; recycling it is the least disruptive.
3. **Oldest InInventory** — if no Read slots, evict the slot with the smallest
   `deliveredAt` (the player has had it longest without opening it). The player will
   notice their unread letter has changed; that's the trade-off for hitting the pool
   ceiling.
4. **Discarded** is treated as Free for allocation purposes (the slot has no reference
   to clean up — eviction already happened when the player discarded it). In practice
   `MarkDiscardedToContainer` and `MarkDroppedToCell` transition the slot's state
   directly to Free after the auto-recycle step (see below); Discarded is a transient
   state in the same way PendingDelivery is.

Eviction runs a defensive scan-and-cleanup rather than relying on a tracked container
handle — see the discussion under **Slot container tracking** below for the rationale.
The sequence:

```
1. player->RemoveItem(book, INT_MAX, kRemove, nullptr, nullptr)
       // sweeps all copies from the player in one call (no-op if zero).
2. For each currently-loaded actor (via SKSE process lists):
       actor->RemoveItem(book, INT_MAX, kRemove, nullptr, nullptr)
       // bounded scan, ~50-100 actors; each call is cheap when the
       // actor doesn't hold the item.
3. For each loaded cell (player's current + adjacent loaded):
       cell->ForEachReference([&](TESObjectREFR& r) {
           if (r.GetBaseObject() == book) {
               r.Disable();
               r.SetDelete(true);
           }
       });
4. Clear kHasBeenRead on the Book form.
5. Clear in-memory slot fields. Reset state to Free.
```

Step 4's `kHasBeenRead` clear is the same trick the smoke test used — the engine
sets the bit on read and it persists per-form, so without clearing it every fresh
delivery would look pre-read in the inventory list.

### Slot container tracking

The Slot struct deliberately does **not** track the letter's current container
(no handle, no FormID, no per-slot location field, nothing persisted to the
co-save). The container is always derivable from state for the steady-state
lifecycle:

- `PendingDelivery` → vanilla courier (resolved via cached `WICourier` /
  `CourierRef` alias).
- `InInventory` / `Read` → player (by definition; `MarkDelivered` was triggered
  by `TESContainerChangedEvent` with `newContainer == player`).
- `Discarded` → transient, cleaned up synchronously inside the event sink
  before the state transition completes. Never persisted.

The discard sink (`TESContainerChangedEvent`) DOES receive the destination
container as a parameter on the event, but it consumes that information
immediately during cleanup and doesn't store it anywhere — there's no lifecycle
window where the slot is "in container X" with X being something we'd need to
remember later.

Brittleness — letters lingering in unexpected places after a slot is recycled
— is addressed by the defensive scan-and-cleanup above, which runs on every
recycle regardless of what slot state thinks. The scan catches missed
`TESContainerChangedEvent`s, third-party mod interference, and any other case
where our state diverges from engine reality.

**Residual edge case**: letters in *unloaded* containers (player gave one to a
follower, then dismissed them; follower walked to an unloaded cell). The
defensive scan only covers loaded space. The orphan letter sits in the
unloaded follower's inventory until they next load in, at which point the
player would see a letter with stale metadata and (because our hook reads
from the slot's current cache) whatever content is now in that slot.
Cosmetically odd, harmless functionally, extremely rare. Not worth designing
around — addressing it would require persisting container FormIDs and dealing
with handle / FormID stability for runtime-created refs, a meaningful tax for
an unlikely scenario.

### Hook integration

Two MinHook detours on the same target functions the smoke test proved, but the per-form
match table is replaced with a pool-wide lookup:

- `TESDescription::GetDescription` — filter on `fieldType == 'CSED'`, then call
  `LetterPool::TryGetBody(parent->GetFormID(), out)`. If `true`, substitute. If `false`,
  fall through. This is the path SkyrimNet's `book.GetDescription()` Papyrus grab uses to
  capture body text for its event log, so substituting here lets the LLM-authored body
  flow into SkyrimNet events, player-thoughts, and decorators.
- `BookMenu::OpenBookMenu` — filter on `book->GetFormID()` being one of our pool forms,
  then `LetterPool::TryGetBody(...)`. If `true`, construct a substitute `BSString` and
  pass it to the original instead of `a_description`. This is the path that feeds the
  Scaleform-side BookMenu renderer; substitution here is what the player visually reads.

Both hooks gate on the FormID being a known pool form (cheap unordered_set lookup), so
the cost on every non-pool book read is one hash hit.

### Discard detection

A new `TESContainerChangedEvent` sink, registered against
`RE::ScriptEventSourceHolder::GetSingleton()` at `kDataLoaded`. The event carries
`oldContainer`, `newContainer`, `baseObj`, `itemCount`.

Filter: `baseObj` is one of our pool form FormIDs. Then:

- `oldContainer == player FormID` and `newContainer != player`: letter left the player.
  - If `newContainer != 0`: the letter went into some other container (vendor, follower,
    chest). Call `LetterPool::MarkDiscardedToContainer(slotIndex, newContainerRef)`.
  - If `newContainer == 0`: the letter was dropped to the world. The actual world REFR
    comes from a separate engine call — `RE::TESObjectREFR::Find` against the base form
    in the player's current cell. Call `LetterPool::MarkDroppedToCell(slotIndex, worldRef)`.
- `newContainer == player FormID`: the letter entered the player. This is also how
  delivery is detected — the courier hands off, the engine fires
  `TESContainerChangedEvent` with `newContainer = player`, and the pool transitions the
  slot to `InInventory`. (See "Read detection" for why this is *not* how we detect read.)

The auto-recycle behavior from the design discussion is implemented inside
`MarkDiscardedToContainer` / `MarkDroppedToCell`: log the discard, run the same eviction
sequence as the allocator's eviction path, set state to Free, return the slot to the
allocation pool. Player's intent (sold the letter) and our intent (return the slot to the
pool) align — they wanted it gone, the recycle does the cleanup.

### Read detection

Same pattern the smoke test proved: a `MenuOpenCloseEvent` sink filtered to
`BookMenu::MENU_NAME` watching the *closing* edge, plus a per-pool-form flag set inside
the `BookMenu::OpenBookMenu` hook so we know which letter the close corresponds to.

`TESBookReadEvent` is *not* a viable signal — the smoke test established empirically that
the engine only fires it for world-reference book reads, not for inventory reads, and
our letters are always read from the player's inventory.

On the matching close:

1. Look up the slot whose `inMenu` flag is set. Clear the flag.
2. Call `LetterPool::MarkRead(slotIndex)`.
3. `MarkRead` sets `state = Read` and `readAt = now`, and fires the **player-side**
   SkyrimNet memory write (per the **SkyrimNet memory integration** section above)
   — the player only "knows what was in the letter" once they've actually engaged
   with it, so memory follows reading rather than receiving. The sender-side memory
   already fired back at delivery.

### Content-generation LLM call

The action runs its own LLM call inside `Start` — separate from and after the
action-select LLM call that picked it. Two reasons for the second call:

1. **Different output shape.** Action-select returns
   `{ action, parameters, narrative_note }`. Letter composition needs
   `{ sender_npc_form_id, sender_label, body, mood, topic_tag }`. Cramming the latter into
   the action-select prompt's `parameters` field would force every other action's
   selection prompt to also reason about letter shape.
2. **Different prompt cost profile.** Letter composition needs the recent SkyrimNet
   event log, the player's recent NPC interactions, and the per-NPC engagement scores
   the sender pick is keyed against. That's a much bigger context than action-select can
   afford on every tick — and action-select doesn't need it for any other action.

The prompt — `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_letter_compose.prompt`
— receives:

- `desired_direction`: `"raise"` or `"lower"`, passed via the dispatcher's
  `ActionContext`.
- `tension_delta`: positive integer; how much to nudge.
- `sender_candidates`: array, capped at top ~12 by engagement score so the prompt
  stays bounded. Each entry is:
  ```
  {
    form_id,
    name,
    engagement_score,
    last_interacted_at,
    memories: [   // this candidate's memory tail involving the player —
                  // pulled via PublicGetMemoriesForActor(form_id, ~6, "Dragonborn")
                  // so the semantic-search rank biases toward player-involving
                  // entries. Each memory:
        { text, importance, timestamp, type },
        ...
    ]
  }
  ```
  No separate top-level player-events array — every memory is already attributed to
  the candidate who holds it, in that candidate's own voice ("I saw the Dragonborn
  defeat a dragon at Whiterun" rather than "the player defeated a dragon at
  Whiterun"). That's exactly the shape the letter LLM needs to author a letter from
  any given candidate's perspective without having to cross-reference a global feed.

The prompt deliberately does NOT include the player's current location. The sender is
writing this letter from their own past context, has no way to know where the player
will be standing when the courier finds them, and shouldn't be reasoning about player
geography. The candidate's own location and situation are implicit in their memories
and identity, which is the only locational frame the letter should be grounded in.

Output: a JSON object with five keys.

- `sender_npc_form_id` — string (hex FormID). MUST match one of the `sender_candidates`
  FormIDs. Validated; mismatch → fail the action with `bad_sender` detail.
- `sender_label` — the FULL field for the book ("Letter from Ysolda", "A note from a
  stablehand"). 1–40 chars. Substituted into the Book form via `SetFullName`.
- `body` — the letter body. Plain text, no font tags (the action wraps it in the
  handwritten-font tag on receipt, as the smoke test established). 60–180 words.
- `mood` — one of `{ warm, neutral, urgent, menacing, mournful, businesslike }`. Used
  later for memory-importance scoring and (potentially) dashboard styling.
- `topic_tag` — 2–5 word topic label ("debt repayment", "market gossip about Belethor",
  "courier shorted me"). Used in the player-side SkyrimNet memory text and the dashboard
  pool panel.

This prompt follows `docs/CUSTOM_PROMPTS.md`'s discipline. Specifically: the prompt
provides *abstract domain categories* for topics (money / work / news / request /
grievance / gossip / social obligation) and explicitly instructs the LLM to invent the
specific situation itself, not pull from a list — the smoke test established that any
concrete topic list becomes an attractor and collapses the output distribution.

### SkyrimNet memory integration

SkyrimNet exposes `PublicAddMemory(formId, contentText, importance, memoryType, emotion,
location)` and `PublicIsMemorySystemReady()` via its `PublicAPI.h`. Two memory writes
per letter, fired at **two different lifecycle events** mirroring when each side
cognitively engaged with the letter:

1. **Sender NPC memory of having sent** — fired at delivery (in
   `LetterPool::MarkDelivered`):
   ```
   formId       = sender_npc_form_id
   contentText  = "Sent a letter to the Dragonborn about <topic_tag>."
   importance   = mood → importance map (urgent/menacing/mournful → ~0.7; warm → ~0.5;
                  neutral/businesslike → ~0.3)
   memoryType   = "EXPERIENCE"
   emotion      = mood
   location     = sender NPC's known location (via SkyrimNet's GetNPCLocation if
                  exposed; fall back to player's location)
   ```

2. **Player memory of receiving** — fired at read (in `LetterPool::MarkRead`):
   ```
   formId       = player FormID (0x14)
   contentText  = "Received a letter from <sender_label> — <topic_tag>."
   importance   = same mood → importance map
   memoryType   = "EXPERIENCE"
   emotion      = mood
   location     = player's current location at read time
   ```

The asymmetry is deliberate and models actual cognitive engagement:

- The **sender** engaged with the letter at the act of writing and sending it. They
  remember having sent it regardless of whether the player ever reads it — the
  sender has no telepathic line to the player's behavior, and their part of the
  transaction concluded the moment they handed it to the courier. Delivery is the
  closest concrete event our system has to that act of sending.
- The **player** engages with the letter only by reading it. If the player never
  opens the letter, they'd realistically have no memory of it — at best a vague
  awareness of having received some piece of correspondence they didn't bother
  with. The IRL player's own memory of "what was in that letter" only exists if
  they actually read it, so the in-fiction memory should follow the same logic:
  no read, no memory.

Both calls are guarded by `PublicIsMemorySystemReady()` (the API doc explicitly warns
that calls before the DB is ready will crash). Failures are logged but non-fatal —
the letter still works; the memory just isn't written.

### Direction flow through ActionContext

Phase 03's `ActionContext` doesn't carry the Director's desired tension direction —
ambush always raises tension, so it didn't need to. Letters need to know which way the
Director wants to push. Extend the struct:

```cpp
struct ActionContext
{
    // ... existing fields ...
    PhaseTracker::Direction desiredDirection = PhaseTracker::Direction::Raise;
    int                     tensionDelta     = 0;
};
```

`ActionDispatcher::ConsiderAction` populates both fields from the same values it already
computed for the action-select prompt and passes them through to every `IsAvailable`
call and the eventual `Start`. `AmbushAction` ignores them (still polarity=Raise);
`NPCLetterAction` feeds them into its content-generation LLM call.

This keeps the "Either"-polarity story straightforward: actions get the direction in the
context, no LLM round-trip is needed to communicate it, and any future Either action
gets the same plumbing for free.

---

## ESP content

The `.esp` gains its second action's content. All forms are ESL-safe FormIDs (FE-prefix).

### Pool: 20 letter Book forms

Editor IDs: `_ne_PooledLetter01` through `_ne_PooledLetter20`.

Each form:

- **Type**: BOOK, with the **Note** flag set in `OBJ_BOOK::Flag` (so it appears under
  the "Notes" inventory tab, not "Books").
- **Model**: vanilla rolled-paper letter mesh (the standard courier-letter model used by
  Bethesda's quest letters).
- **Inventory Icon**: vanilla letter icon.
- **FULL (name)**: placeholder string, e.g. `"Pooled Letter (unused)"`. Always overwritten
  at runtime via `SetFullName` before delivery — the player should never see the
  placeholder.
- **DESC (book text)**: placeholder string, e.g. `"(placeholder — should never display)"`.
  The OpenBookMenu hook always substitutes before rendering, but the field has to exist
  in the ESP so the Book record is valid.
- **CNAM (tooltip)**: a generic "A sealed letter." string. Left alone by this phase.
- **Value/Weight**: 0 / 0 (matching vanilla quest letters).
- **Keywords**: vanilla `VendorItemBook` so unmodified merchants will take them in trade.

The 20 forms are mechanically identical — only the EditorID differs. They're a pool of
interchangeable carriers for content; the differentiation happens at runtime.

### Vanilla courier dispatch via `WICourierScript.AddItemToContainer`

Skyrim's vanilla world-interaction layer already implements the entire courier
dispatch pipeline: queueing items for delivery, picking spawn locations, walking the
courier to the player, performing the handoff dialogue, and despawning the courier
afterward. The relevant Papyrus surface is the `WICourierScript` script attached to
Bethesda's `WICourier` quest (vanilla form). It exposes `AddItemToContainer(Form
akItem, int aiCount)` (and related variants) for queuing an item for the next
delivery cycle.

We **do not** author a custom courier quest, courier NPC, AI package, or
delivery-side Papyrus script. The phase needs **zero** new quest/alias/package CK
content beyond the 20 pooled Book forms in the previous subsection.

What we do need is a way to call into the vanilla script from C++. SKSE's
`BSScript::Internal::VirtualMachine` exposes `DispatchMethodCall` for invoking
Papyrus member functions on script-attached forms from native code. The pattern:

```cpp
auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
auto* policy = vm->GetObjectHandlePolicy();

// Resolve the vanilla WICourier quest by FormID (looked up at startup; cached).
RE::TESQuest* courierQuest = /* vanilla WICourier from Skyrim.esm */;
auto handle = policy->GetHandleForObject(RE::TESQuest::FORMTYPE, courierQuest);

// Build arguments. RE::MakeFunctionArguments handles the Form*+int marshal.
auto args = RE::MakeFunctionArguments(
    static_cast<RE::TESForm*>(poolBook),
    static_cast<std::int32_t>(1));

// Fire-and-forget — we don't need a return value. Empty callback functor.
RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
vm->DispatchMethodCall(handle, "WICourierScript"sv,
                       "AddItemToContainer"sv, args, callback);
```

This is the only "obstacle" the user named — the VM dispatch path requires a little
more boilerplate than a flat C++ call, but it's a known-good SKSE pattern. SkyrimNet
itself, IntelEngine, and most non-trivial SKSE plugins use it.

The dispatch is **asynchronous from the engine's perspective**: queueing an item
returns immediately; the courier shows up later when world conditions permit
(player out of combat, not in dialogue, in a place a courier can reach).

### Dispatch verification

A VM dispatch to `AddItemToContainer` is fire-and-forget — there's no return
value and no synchronous failure signal. The vanilla courier system could be in
a degraded state (`WICourier` quest stalled, mod conflict, save corruption that
broke the queue) and our dispatch would silently land nowhere. The same
verification pattern AmbushAction uses to confirm its quest actually started —
the `DetectAndRollbackFailedStart` / `DetectCompletion` polls on `IAction` —
applies here.

NPCLetterAction implements both:

- **`DetectAndRollbackFailedStart`**: until `secondsSinceStart` reaches
  `iLetterDispatchVerifyDelaySeconds` (default 5s), returns false (too early
  to give up). After the window, queries the vanilla courier's inventory for
  the pool book FormID we dispatched. If absent → calls
  `LetterPool::AbortPending(slotIndex)`, returns true so the dispatcher rolls
  back its in-flight bookkeeping (no cooldown applied; action can re-fire on
  the next tick).
- **`DetectCompletion`**: queries the same courier inventory. If the letter is
  present → returns true. The dispatcher clears in-flight and starts the
  global cooldown. Polled every tick; succeeds as soon as the dispatch lands
  (usually within one or two ticks).

The two polls check the same condition with inverted semantics: presence =
completion, sustained-absence-past-window = rollback. The dispatcher invokes
`DetectAndRollbackFailedStart` first per tick (per its IAction documentation),
so the ordering is naturally "is this a failed dispatch yet? if not, has it
landed yet?" — no race between the two.

**Locating the courier's inventory.** Vanilla `WICourier` has a persistent
`CourierRef` reference alias filled with the actual Courier NPC. The action
caches a pointer to `WICourier` at `kDataLoaded` (resolved by FormID), and on
each verification poll resolves the alias's reference and calls `GetItemCount`
on it. The Courier exists persistently across the game world's lifetime, so
the reference lookup is reliable as long as `WICourier` itself is in a sane
state — and if `WICourier` is broken, every letter dispatch will (correctly)
roll back on the verification step.

### Director-side completion semantics

Combined with the verification above, completion semantics work out cleanly:

- LLM-content failure (caught in the `Start` callback before dispatch even
  fires) → `LetterPool::Free` on the slot, call
  `ActionDispatcher::CompleteAction("npc_letter")` to unwind the dispatcher's
  in-flight state. Cooldown applies (we did consume an action-select slot).
- Dispatch succeeded, courier received the letter → `DetectCompletion` fires,
  dispatcher completes the action, cooldown applies normally.
- Dispatch silently failed, courier never got the letter →
  `DetectAndRollbackFailedStart` fires after the verification window,
  dispatcher rolls back, the slot returns to Free, **no cooldown** (the
  Director should be free to try again).

The cooldown timing for the success path is now (action-select LLM) +
(letter-compose LLM) + (verification delay, ~5s) + `iActionCooldownSeconds`.
Physical courier travel time and player read time remain outside the
Director-side budget.

### Other forms

- **No new factions** — courier already belongs to vanilla `CourierFaction`.
- **No new leveled lists** — `WICourier` owns its own courier spawn logic.
- **No new keywords** — uses vanilla `VendorItemBook` on the pool books.
- **No new ModEvents** — `_ne_ActionCompleted` from Phase 03 is reused, but the
  letter action fires it *itself from C++* (via `RE::SkyrimVM::SendModEvent` or a
  direct `ActionDispatcher::CompleteAction` C++ API — see Pipeline integration)
  rather than receiving it from Papyrus.

---

## The action: `NPCLetterAction`

The C++ side stays thin; the heavy lifting is the LLM content call and the VM
dispatch into vanilla `WICourierScript`.

### Preconditions (`IsAvailable`)

Action-specific checks only — the global preconditions
(combat / dialogue / scripted-scene / DND-cell) are handled by
`ActionDispatcher::CheckGlobalActionPreconditions` before any action's
`IsAvailable` runs, so they don't need to be repeated here.

Returns `true` only when **all** of:

- Player's current location is **not** flagged as dangerous, per the existing
  `LocationKeywords::IsDangerous()` predicate (the same one AmbushAction uses
  to keep itself OUT of safe areas — for letters the logic inverts: we use
  `IsDangerous` directly rather than `!IsSafe` because most ordinary roads
  and wilderness are neither safe nor dangerous, and letters should fire
  there too).
- `PublicIsMemorySystemReady()` returns true.
- A quick proxy query to SkyrimNet returns at least `iLetterMinSenderCandidates`
  (default 3) recently-engaged NPCs. Without enough candidates the letter would
  either fail or have to fall back to a generated persona, neither of which we
  want here.

The check explicitly does NOT exclude interior cells in safe contexts (player
homes, inns, shops) — those are fine because the vanilla courier system queues
the delivery and fires on the next cell-change, which is the moment the player
steps outside. Interior dungeons are excluded by the dangerous-location check
above — dungeons carry one of the keywords already in the existing `kDangerous`
list (`LocTypeDungeon`, `LocSetCave`, `LocSetNordicRuin`, etc.) regardless of
whether they're flagged as interior cells.

### Parameters

LLM-supplied parameters for this action are deliberately minimal — the LLM that picked
the action doesn't decide content; the action's own LLM call does.

- `urgency_hint` — optional string from action-select, one of
  `{ low, medium, high }`. Used as a soft input to the content-generation LLM call to
  bias `mood` and `importance`. Defaults to `medium` if omitted.

That's it. Everything else (sender, body, topic, tone) is decided in the content call
using the player's actual SkyrimNet history.

### `Start`

1. Re-validate `IsAvailable` (defends against state changes between selection and
   dispatch).
2. Allocate a pool slot via `LetterPool::Allocate()`. On failure → return
   `StartResult{ started=false, detail="pool exhausted" }`.
3. Build the content prompt context: pull sender candidates from SkyrimNet
   (`PublicGetActorEngagement` + per-NPC recent events for the top N), pull recent player
   events, attach `desired_direction` and `tension_delta` from the `ActionContext`.
4. Fire the async content LLM call via `SkyrimNetAPI::SendCustomPromptToLLM(
   "narrative_engine_letter_compose", "narrative_engine_director", contextJson, callback)`.
5. **`Start` returns immediately** with `StartResult{ started=true, detail="composing
   letter" }`. The dispatcher records the action as in-flight; completion is reported
   from the callback below.
6. In the callback (on the main thread after marshal):
   - Validate the LLM response shape: required keys present, `sender_npc_form_id`
     matches one of the candidates, body within length bounds.
   - On validation failure: release the slot, call
     `ActionDispatcher::CompleteAction("npc_letter")` so the dispatcher unwinds
     cleanly. Log the failure with the LLM response for diagnosis. (The Director sees
     a failed letter and a cooldown begins; the player sees nothing.)
   - On success: call `LetterPool::PopulateSlot(...)` with the parsed response. The slot
     transitions to `PendingDelivery`. The Book form's FULL is now the sender label, and
     the hook's body cache holds the LLM's body.
   - VM-dispatch to `WICourierScript.AddItemToContainer(poolBook, 1)` per the
     **Vanilla courier dispatch** section. The vanilla courier system queues the
     delivery and will resolve it on its own timeline.
   - **Do not** call `CompleteAction` here. The action stays in-flight; the
     dispatcher will see completion via `DetectCompletion` once the dispatch is
     verified (or rollback via `DetectAndRollbackFailedStart` if not). See
     **Dispatch verification** above.

After `Start`'s callback runs, the LetterPool slot is in `PendingDelivery`. The
dispatcher polls `DetectAndRollbackFailedStart` and `DetectCompletion` on each
subsequent tick: one of them will fire within `iLetterDispatchVerifyDelaySeconds`,
either completing the action and starting cooldown (success) or rolling back
and freeing the slot (silent dispatch failure). On the success path, the slot
remains in `PendingDelivery` until `TESContainerChangedEvent` fires with
`newContainer == player`, which transitions to `InInventory` and triggers the
sender-side SkyrimNet memory write. Player reading the letter later triggers
`MarkRead` and the player-side memory write. None of those events affect the
dispatcher; they're purely LetterPool lifecycle.

### What's NOT in this action (yet)

- No re-letter logic if the courier fails to reach the player (e.g. player teleports
  to a new worldspace mid-delivery). The stale-lock timeout from Phase 03 catches this;
  the slot is recycled on the next `Start` that needs it.
- No "follow-up letter from the same NPC" beyond what naturally falls out of the LLM
  seeing the sender NPC in its recent-events context on later ticks.
- No styled per-mood letter mesh — every letter uses the same rolled-paper Note mesh.
- No multi-page letters. The body is a single BookMenu page; the content prompt enforces
  word bounds that fit comfortably.

---

## Settings

New keys in `[Director]`:

| Key                                   | Default | Meaning                                                              |
| ------------------------------------- | ------: | -------------------------------------------------------------------- |
| `iLetterMinSenderCandidates`          | 3       | min recent NPC interactions required for `IsAvailable` to return true |

New `[Actions]` section keys:

| Key                                | Default | Meaning                                                            |
| ---------------------------------- | ------: | ------------------------------------------------------------------ |
| `iLetterContentMaxWords`           | 180     | upper bound on LLM-generated body length (enforced in content prompt) |
| `iLetterContentMinWords`           | 60      | lower bound                                                          |
| `iLetterPoolSize`                  | 20      | informational only; the pool size is fixed by the 20 ESP forms      |
| `iLetterDispatchVerifyDelaySeconds`| 5       | wall-clock seconds after `Start` before `DetectAndRollbackFailedStart` will give up on the VM dispatch if the letter still isn't in the courier's inventory |
| `iLetterPendingDeliveryTimeoutSeconds` | 600 | how long a slot may sit in PendingDelivery (i.e. queued with vanilla courier but not yet delivered) before being demoted to Free on load reconciliation |

New `[LetterPool]` section:

| Key                              | Default | Meaning                                                            |
| -------------------------------- | ------: | ------------------------------------------------------------------ |
| `iLetterPoolEvictionLogVerbosity`| 1       | 0 = silent, 1 = log evictions, 2 = log every state transition       |

---

## Persistence

New co-save record `'NELP'` (NarrativeEngine Letter Pool), versioned at 1. Payload:

```
uint32  slot_count          ; = pool size (20). Per-record validation guard.
for each slot:
    uint8                            state
    uint32                           bookFormID        ; via WriteRecordData; restore via ResolveFormID
    uint32                           senderNpcFormID   ; via WriteRecordData; restore via ResolveFormID
    double                           deliveredAt
    double                           readAt
    uint16                           bodyLen
    char[bodyLen]                    body
    uint16                           senderLabelLen
    char[senderLabelLen]             senderLabel
    uint16                           topicTagLen
    char[topicTagLen]                topicTag
```

No container handle is persisted — see the **Slot container tracking** section
in Core concepts for the rationale. The container is always derivable from
state, and the recycle-time defensive scan handles any drift.

FormIDs go through `SKSE::SerializationInterface::ResolveFormID` on read so the
pool survives load-order changes (a player adding or removing mods between save
and load won't corrupt the pool).

On load, after each slot is deserialized, run a reconciliation pass:

- `Free`: nothing to reconcile.
- `PendingDelivery`: the vanilla courier system owns delivery state and there's no
  C++-visible "is delivery still queued" check. Use a time-based fallback: if
  `now - deliveredAt > iLetterPendingDeliveryTimeoutSeconds` (default 600s
  real-time; while the slot is in PendingDelivery, `deliveredAt` is the
  queued-with-courier timestamp set in `PopulateSlot`), demote the slot to Free
  via the defensive recycle scan. Letters that have been pending longer than
  this almost certainly won't arrive (the player has moved through a worldspace
  change or the vanilla courier queue was already advanced past this entry).
- `InInventory` / `Read`: verify `player->GetItemCount(book) > 0`. If zero,
  demote to Free via the defensive recycle scan — the player removed the
  letter externally (console, save editor, etc.) and we missed the event;
  the scan covers any place the letter might have ended up in loaded space.
- `Discarded`: should never appear in saved state (`MarkDiscardedToContainer`
  and `MarkDroppedToCell` transition to Free synchronously), but if it does,
  demote to Free via the defensive recycle scan.

The reconciliation log line names every slot that got demoted, so save-editing
or external mod interference is diagnosable from the SKSE log.

---

## Pipeline integration

The dispatcher's per-tick check changes minimally:

- `ActionDispatcher::ConsiderAction` now populates the new `ActionContext` fields
  (`desiredDirection`, `tensionDelta`) before passing to `IsAvailable` and `Start`.
- `IsAvailable` filtering on the candidate pool still happens plugin-side; the
  action-select LLM still picks from the filtered set.
- `NPCLetterAction::Start` is the first action that itself makes an LLM call. The
  dispatcher's `g_inFlight` flag stays held across both LLM round-trips
  (action-select → letter-compose).
- The dispatcher gains a new C++-side completion entry point —
  `ActionDispatcher::CompleteAction(std::string_view actionName)` — that does the
  same work the existing `_ne_ActionCompleted` ModEvent sink does (push the action
  into the recency ring buffer, set `g_lastActionCompletedAt = now`, clear
  `g_actionInFlight`, push a dashboard refresh). The ModEvent sink internally calls
  this same function; Phase 03's quest-driven completion path still works unchanged.
  Letters use the C++ path only on the **LLM-failure branch** in `Start`'s async
  callback (so we don't have to do a Papyrus round-trip for an event we're
  generating ourselves). The **success path** uses `IAction::DetectCompletion`
  instead — see **Dispatch verification** above.
- The Director-side cooldown for letters starts when the dispatch is **verified
  to have landed in the courier's inventory** (via `DetectCompletion`), not at
  VM-dispatch return, not at physical delivery to the player, and not at read.
  The total Director-side latency for an action-firing letter tick is roughly:
  (action-select LLM round-trip) + (letter-compose LLM round-trip) +
  (verification delay, typically 5s) + `iActionCooldownSeconds`. Courier travel
  time, player reading time, and any post-delivery lifecycle do not appear in
  this budget.
- Failed dispatches (caught by `DetectAndRollbackFailedStart`) do NOT apply a
  cooldown — the dispatcher rolls back its in-flight state and the action is
  immediately re-selectable on the next tick. The Director can keep trying
  while the underlying issue (broken `WICourier` state, etc.) persists, with
  the action-select prompt's recency filter eventually demoting it from
  consideration after a few failures in a row.

The `_ne_ActionCompleted` ModEvent sink Phase 03 set up still serves `AmbushAction`
(which sends it from Papyrus on stage 100); the new C++ path is additive.

---

## Dashboard

The dashboard splits into **two tabs** in this phase, navigable via a tab bar at
the top of the panel. This gives Phase 04 its own dedicated screen for the
letter-pool surface (which has substantially more state to show than a
single-panel addition would fit) without crowding the existing Director tab.

### Dashboard root layout

Top-to-bottom rendering at the dashboard root, in order:

1. **StatusBanner** — global status pills (SkyrimNet availability +
   version, PrismaUI availability, Director enabled). Always visible
   regardless of active tab; status info isn't tab-specific.
2. **TabBar** — compact horizontal tab bar with two buttons (Director,
   Letters). Active tab highlighted.
3. **Active tab content** — either DirectorTab or LettersTab, swapped on
   tab change.

Future phases can add tabs to (2) without affecting (1) or the existing
tabs. Active-tab state lives in `App.tsx` (`useState`); switching tabs is
a pure client-side render swap with no backend impact.

### Tab 1 — Director

The existing Phases 01-03 content (minus StatusBanner which lives at the
root now), unchanged in layout: current phase + dwell time, Last
Evaluation panel, Recent Decisions list, Recent Events list. One small
subtraction: the `LastEvaluation` panel's `→ fired: <action>` row no
longer inlines letter-specific details (the previously-proposed
"`→ fired: npc_letter (Ysolda — debt repayment)`" addition is gone).
Letter details live on the Letters tab now where they have room to
breathe. The fired-action row stays as just `→ fired: npc_letter` for
letter dispatches, same shape as for ambush.

### Tab 2 — Letters

Two stacked sections:

**Recent dispatch detail** (top of tab, only rendered when at least one slot is
in a non-Free state). Shows the most recently-dispatched letter — the one with
the largest `deliveredAt` timestamp among non-Free slots — with full metadata:

- Sender label (the FULL field, e.g. "Letter from Ysolda")
- Topic tag (e.g. "debt repayment")
- Mood (e.g. "businesslike")
- State badge (PendingDelivery / InInventory / Read)
- Delivered-at timestamp (relative: "delivered 2m ago")
- Read-at timestamp if applicable (relative: "read 30s ago")
- Body preview (first ~200 chars of the cached body, font-wrap stripped for
  display)

When the player dispatches a new letter, this section updates to show the new
letter. When the most-recent slot recycles back to Free (and no other slot is
non-Free), the section unmounts entirely.

**Pool overview** (bottom of tab, always rendered):

- A small stats header: `Free: N  Pending: N  In Inventory: N  Read: N`
- A 20-row table listing every slot. Each row:
  - Slot index (1-20)
  - State badge with color coding (Free = muted, Pending = amber, InInventory =
    blue, Read = green)
  - Sender label (truncated to ~20 chars, empty for Free)
  - Topic tag (truncated to ~30 chars, empty for Free)
  - Relative age (`5m ago` / `12s ago` / `—` for Free)

Compact dense rendering — no body previews in the per-slot rows; that's only in
the recent-dispatch detail above.

### Schema additions on `DirectorState`

```ts
letter_pool: {
    slots: Array<{
        index: number;                      // 0-19
        state: 'free' | 'pending_delivery' | 'in_inventory' | 'read';
        sender_label: string;               // empty for free
        topic_tag: string;                  // empty for free
        mood: string;                       // empty for free
        body_preview: string;               // empty for free; first ~200 chars
        delivered_at: number;               // 0 for free; Unix epoch seconds
        read_at: number;                    // 0 for free / pending / inventory
    }>;
    most_recent_dispatch_slot: number | null;  // index of the slot to feature
                                                // in the recent-dispatch detail,
                                                // or null if all slots are Free
};
```

Aggregate stats (Free/Pending/InInventory/Read counts) are derived client-side
from `letter_pool.slots`. No separate `stats` field needed.

C++ side populates this in `ComposeFullStateJSON` by iterating the pool and
emitting one entry per slot. `most_recent_dispatch_slot` is computed by
scanning for the slot with the largest `deliveredAt` among non-Free slots.

---

## File map

New C++:

- `include/LetterPool.h` / `src/LetterPool.cpp`
- `include/NPCLetterAction.h` / `src/NPCLetterAction.cpp`

New prompt:

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_letter_compose.prompt`

New ESP content (authored in CK against `NarrativeEngine.esp`):

- `_ne_PooledLetter01` through `_ne_PooledLetter20` Book records (20 forms).

No new Papyrus, no new quest, no new aliases, no new AI package — the vanilla
`WICourierScript.AddItemToContainer` handles all delivery logic.

No new ModEvent names. Letters complete via a direct C++ call into
`ActionDispatcher::CompleteAction`; Phase 03's existing `_ne_ActionCompleted`
ModEvent sink continues to handle ambush completions.

Modified:

- `include/Settings.h`, `src/Settings.cpp` — new keys per the **Settings** section.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — document each new key inline.
- `include/IAction.h` — extend `ActionContext` with `desiredDirection` and `tensionDelta`.
- `include/ActionDispatcher.h`, `src/ActionDispatcher.cpp` — populate the new context
  fields before calling `IsAvailable` and `Start`. Add the public C++
  `CompleteAction(std::string_view)` entry point; refactor the existing ModEvent sink
  to delegate to it.
- `src/Plugin.cpp` — register `NPCLetterAction` at `kDataLoaded`; wire LetterPool's
  co-save callbacks; wire LetterPool's event sinks (`TESContainerChangedEvent`,
  `MenuOpenCloseEvent`) and the two MinHook installs.
- `src/DashboardUIManager.cpp` — emit the expanded `letter_pool` payload
  (full per-slot data + `most_recent_dispatch_slot` index).
- `dashboard/src/types.ts` — extend `DirectorState` with the new `letter_pool`
  shape per the **Schema additions** subsection.
- `dashboard/src/components/TabBar.tsx` — new component, tab bar at the
  dashboard root.
- `dashboard/src/components/tabs/DirectorTab.tsx` — wraps the existing
  Phase 01-03 panels (PhasePanel, LastEvaluation, DecisionList, EventList)
  for the first tab. StatusBanner is NOT inside this wrapper — it stays at
  the dashboard root above the tab bar (see App.tsx changes below).
- `dashboard/src/components/tabs/LettersTab.tsx` — new component, the
  Letters tab content (RecentDispatchDetail + PoolOverview).
- `dashboard/src/components/RecentDispatchDetail.tsx` — top section of the
  Letters tab.
- `dashboard/src/components/LetterPoolOverview.tsx` — bottom section of the
  Letters tab (stats header + 20-row slot table).
- `dashboard/src/App.tsx` — tab state (`useState<'director'|'letters'>`),
  conditional rendering, wires TabBar to the active-tab setter.
- `dashboard/src/components/LastEvaluation.tsx` — minor revision: drop the
  inline letter-action detail rendering proposed in earlier drafts; revert
  the row back to `→ fired: <action>` for letter dispatches.
- `dashboard/styles.css` — tab bar styling, color-coded state badges for
  the per-slot rows, recent-dispatch-detail layout.

---

## Implementation plan

Sequential. Each step is **entirely Claude's work (C++ / TypeScript / prompts)** or
**entirely the user's work (Creation Kit + Papyrus)**. Every step has a clear
self-contained verification.

---

### Step 1 — Remove the throwaway smoke test

**[CLAUDE]**

**Goal:** Delete the smoke-test scaffolding so Phase 04 starts from a clean baseline.
The smoke test served its purpose (proving the engine-hook plumbing); the production
LetterPool will not extend it but replace it.

**Files:**

- Delete `include/LetterSmokeTest.h`, `src/LetterSmokeTest.cpp`.
- Delete `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_letter_smoke.prompt`.
- `src/Plugin.cpp` — remove the `#include <LetterSmokeTest.h>` line, the
  `LetterSmokeTest::Initialize()` call in `kDataLoaded`, and the two
  `LetterSmokeTest::OnPostLoadGame()` calls in `kNewGame` and `kPostLoadGame`.

**Specifics:**

- The CK-side `_ne_CourierLetter01` form authored for the smoke test is left in place.
  It's harmless without the C++ wiring, and removing forms from an ESL after content
  has shipped is more disruptive than ignoring an unused one.
- MinHook stays as a vcpkg dep and as a CMake link; LetterPool will use it.
- The CUSTOM_PROMPTS.md and engine-findings docs that came out of the smoke test stay
  — they're cross-phase reference material.

**Verify:** `pwsh -File build.ps1 build` succeeds with no references to the removed
files. Boot Skyrim; SKSE log shows no `LetterSmokeTest::Initialize` lines, no
`OpenBookMenu hook fired` lines, no orphaned references. The previously-deployed
prompt file is gone from the mod folder after the build (statics deploy mirrors
deletions).

---

### Step 2 — Settings expansion and ActionContext extension

**[CLAUDE]**

**Goal:** Add every new INI key Phase 04 needs and extend the `IAction::ActionContext`
struct so downstream actions can receive the Director's desired direction.

**Files:**

- `include/Settings.h` — new fields per the **Settings** section.
- `src/Settings.cpp` — INI reads for each new key in `[Director]`, `[Actions]`,
  `[LetterPool]`.
- `statics/SKSE/Plugins/NarrativeEngine.ini` — document each new key inline.
- `include/IAction.h` — add `desiredDirection` and `tensionDelta` to `ActionContext`.
- `src/ActionDispatcher.cpp` — populate the new fields when building the context for
  `IsAvailable` filtering and `Start`. `AmbushAction` ignores them but compiles
  unchanged.

**Verify:** Build clean. Boot Skyrim; the existing `Settings: loaded from …` log
shows the new keys' values via debug mode. Optionally bump a value and reload to
confirm the read path.

---

### Step 3 — Author the 20 pooled letter Book forms in Creation Kit

**[USER]**

**Goal:** Create the pool of Book records the LetterPool will manage at runtime. All
20 forms are mechanically identical — only their EditorIDs differ.

**Sub-tasks:**

1. Open Creation Kit through MO2; load `NarrativeEngine.esp`.
2. Create the first Book record:
   - EditorID: `_ne_PooledLetter01`.
   - FULL (name): `Pooled Letter (unused)`.
   - DESC (book text): `(placeholder — should never display)`.
   - CNAM (description / tooltip): `A sealed letter.`
   - Model: vanilla rolled-paper letter mesh (the `Note` family — exact filename
     depends on which note mesh Bethesda's quest letters use; pick the standard one
     used by `MS09Letter` or similar reference vanilla letter forms).
   - Inventory Icon: vanilla letter icon (matches the mesh).
   - Value: 0, Weight: 0.
   - Flags: **Can't Be Taken** OFF, **Note** (in OBJ_BOOK Type dropdown) — sets
     the inventory category to Notes.
   - Keywords: add `VendorItemBook`.
   - Save.
3. Duplicate that form 19 more times (CK right-click → Duplicate), renaming the new
   EditorIDs `_ne_PooledLetter02` through `_ne_PooledLetter20`. All other fields
   stay identical.
4. Save the ESP.

**Verify:** Open xEdit on `NarrativeEngine.esp`. Confirm 20 BOOK records named
`_ne_PooledLetter01..20` exist. All carry the same FULL/DESC/CNAM/Model. After
running `pwsh -File build.ps1 build`, `<repo>/esp/NarrativeEngine.esp` reflects the
new forms via the standard sync. Boot Skyrim; SKSE log shows no errors loading the
ESP.

---

### Step 4 — LetterPool C++ scaffold

**[CLAUDE]**

**Goal:** Stand up the LetterPool module's structure: per-slot `Slot` struct, the pool
state guarded by a mutex, `Allocate` returning the next Free slot (no eviction yet),
form-FormID resolution from EditorIDs at `kDataLoaded`. No hooks, no LLM, no quest —
just the data structure and the lifecycle of in-memory state.

**Files:**

- `include/LetterPool.h` — public API per the **LetterPool overview** section, minus
  hook-side functions (added in Step 6) and event-driven transitions (added in
  Steps 7–8). For now: `Initialize`, `Allocate`, `PopulateSlot`, `Free` (manual
  release for testing), `GetStats`.
- `src/LetterPool.cpp` — implementation. Storage is `std::array<Slot, kPoolSize>`
  guarded by `std::mutex`. `Initialize` walks the 20 EditorIDs (`_ne_PooledLetter01`
  through `_ne_PooledLetter20`) and resolves each via `RE::TESForm::LookupByEditorID`,
  storing the resulting FormIDs in the slots. Any form that fails to resolve is
  logged as an error and marked unusable in the slot (state stays Free but allocation
  skips it).
- `src/Plugin.cpp` — call `LetterPool::Initialize()` in `kDataLoaded` after
  `ActionRegistry`.

**Specifics:**

- Allocation order in this step is "next slot with state Free." Eviction comes in
  Step 9.
- `PopulateSlot` mutates the Book form's `SetFullName` to the sender label and stores
  the body in the in-memory map. The hook integration in Step 6 will read from that
  map.
- A small dump-state helper logs `LetterPool: pool resolved (N forms; M failed)` so
  startup diagnostics surface any ESP wiring issues.

**Verify:** Build clean. Boot Skyrim; SKSE log shows
`LetterPool: pool resolved (20 forms; 0 failed)`. No runtime test possible yet —
nothing exercises `Allocate` until Step 13.

---

### Step 5 — Co-save persistence for LetterPool

**[CLAUDE]**

**Goal:** Add the `'NELP'` co-save record so pool state survives save/reload.
Reconciliation logic for restored handles. Nothing yet uses the pool, but the
serialization round-trip is testable by manually mutating slot state in a debug
helper and confirming it persists.

**Files:**

- `include/LetterPool.h` — add `OnSave`, `OnLoad`, `OnRevert` to the namespace.
- `src/LetterPool.cpp` — implementation of the three per the **Persistence** section.
  ResolveFormID round-trip for `bookFormID` and `senderNpcFormID`. Manual handle
  lookup for `currentContainer` after deserialization.
- `src/Plugin.cpp` — wire `LetterPool::OnSave/OnLoad/OnRevert` into the existing
  serialization dispatcher alongside the Phase 03 records.

**Specifics:**

- Co-save record type ID: `'NELP'`, version 1. The first written field is
  `slot_count` (=20). On read, mismatch logs a warning and demotes everything to
  Free — same defensive posture as Phase 03's records.
- On `OnLoad`, the reconciliation pass per the **Persistence** section runs after
  the raw deserialization. Every demoted slot logs a line naming the slot index and
  the reason.

**Verify:** Build clean. Boot Skyrim; save with all slots Free. The `'NELP'` record
should appear in the save (visible via xEdit on the co-save) with `slot_count=20`
and every slot in `Free` state. Reload; SKSE log shows
`LetterPool::OnLoad: restored 20 slot(s) (0 demoted)`.

---

### Step 6 — Install MinHook detours against the pool

**[CLAUDE]**

**Goal:** Port the proven smoke-test hooks (`TESDescription::GetDescription`,
`BookMenu::OpenBookMenu`) to read from the LetterPool's body cache instead of the
single-form smoke-test cache. Both hooks now match on pool membership instead of a
single FormID.

**Files:**

- `include/LetterPool.h` — add `TryGetBody(bookFormID, outBody)` and a private
  membership helper. Expose a public `IsManagedForm(bookFormID)` for the hooks.
- `src/LetterPool.cpp` — maintain an `std::unordered_set<RE::FormID> g_managedForms`
  populated in `Initialize` from the 20 resolved FormIDs. The hooks consult this for
  membership in O(1).
- `src/LetterPoolHooks.cpp` (new file, or inline in `LetterPool.cpp`) — the MinHook
  install code, ported from the smoke test. `HookedGetDescription` filters on
  `fieldType == 'CSED'` and `g_managedForms.contains(parent->GetFormID())`;
  `HookedOpenBookMenu` filters on `g_managedForms.contains(book->GetFormID())`. Both
  call `LetterPool::TryGetBody` and substitute on hit, fall through on miss.
- `src/Plugin.cpp` — call `LetterPool::InstallHooks()` after `LetterPool::Initialize`
  in `kDataLoaded`.

**Specifics:**

- The hooks reuse the exact MinHook init pattern from the smoke test
  (`MH_Initialize` idempotent under `MH_ERROR_ALREADY_INITIALIZED`,
  `MH_CreateHook` + `MH_EnableHook`, each logged with target/hook/trampoline
  addresses).
- The `engine-findings/hooking-engine-functions-with-minhook.md` doc covers
  this — point at it in source comments where the install happens.
- No DESC/CNAM fieldType filter on `OpenBookMenu` — the call always passes the
  description through whatever the engine's BookMenu pipeline expects, and we don't
  need to disambiguate by field type.

**Verify:** Build clean. Boot Skyrim; SKSE log shows both hook-install lines with
trampoline addresses. Without dispatching a letter yet (the action isn't built),
reading any other book in the game shouldn't trigger any LetterPool log lines —
the hooks are installed but every call falls through.

---

### Step 7 — MenuOpenCloseEvent sink for read detection

**[CLAUDE]**

**Goal:** Detect when the player closes BookMenu and, if it was a pool letter,
transition the slot to `Read`. Same pattern the smoke test proved.

**Files:**

- `include/LetterPool.h` — add `MarkRead(slotIndex)` to the public API; add an
  internal `OnOpenBookMenuForSlot(slotIndex)` flag-setter that the hook calls when
  the matched FormID resolves to a pool slot.
- `src/LetterPool.cpp` — implement `MarkRead` (state → Read, `readAt = now`, log).
  In `HookedOpenBookMenu`, after the substitution, call `OnOpenBookMenuForSlot` with
  the slot index. Maintain a per-process `std::atomic<int> g_currentOpenSlot = -1`
  set on open and consumed on close.
- New `LetterPool::BookMenuCloseSink` similar to the smoke test's
  `BookMenuCloseSink` — filters on `RE::BookMenu::MENU_NAME`, watches the closing
  edge, consumes `g_currentOpenSlot` (exchange to -1).
- `src/Plugin.cpp` — register the sink against `RE::UI::GetSingleton()` in
  `kDataLoaded` after `LetterPool::InstallHooks`.

**Specifics:**

- `MarkRead` fires the **player-side** SkyrimNet memory write (the player only
  "knows what was in the letter" once they actually open it). The sender-side
  memory fires at delivery instead (Step 14). The memory call itself is added
  in Step 14; for this step, `MarkRead` just sets the slot state, `readAt`
  timestamp, and logs.
- Stale-event guard same as the smoke test: if the close fires when no slot is in
  state `InInventory` (the player closed BookMenu for a book that wasn't ours), the
  exchange yields -1 and the handler returns.

**Verify:** Build clean. With no letter delivered yet, opening any non-pool book
should produce no `LetterPool` log lines. Manually delivering a letter (via a temp
debug console wired in a one-off branch, or just trust the integration test in
Step 13) and reading it should fire `LetterPool: slot N marked Read`.

---

### Step 8 — TESContainerChangedEvent sink for location tracking

**[CLAUDE]**

**Goal:** Track when pool letters move between containers. Drives `MarkDelivered`
(courier → player), `MarkDiscardedToContainer` (player → vendor/follower), and
`MarkDroppedToCell` (player → world).

**Files:**

- `include/LetterPool.h` — add `MarkDelivered`, `MarkDiscardedToContainer`,
  `MarkDroppedToCell` to the API.
- `src/LetterPool.cpp` — implement the three. `MarkDelivered` transitions
  `PendingDelivery → InInventory`, sets `deliveredAt` to the actual delivery
  time, and leaves the **sender-side** SkyrimNet memory write as a stub to be
  filled in by Step 14. (The player-side memory write does NOT fire here;
  that lives on `MarkRead`. No container handle is stored on the slot — the
  player's identity is implicit in the `InInventory` state, per the **Slot
  container tracking** subsection in Core concepts.)
  `MarkDiscardedToContainer` and `MarkDroppedToCell` run the defensive
  recycle scan (which handles the new container synchronously via the
  handle the event provided), log the discard, and transition the slot to
  Free. The container handle from the event is consumed inside the function
  for the cleanup and not stored anywhere.
- New `LetterPool::ContainerChangedSink` — registered against
  `RE::ScriptEventSourceHolder::GetSingleton()`. Filters `baseObj` against
  `g_managedForms`. Dispatches based on `oldContainer` and `newContainer`:
  - `oldContainer == 0` and `newContainer == player`: courier delivery →
    `MarkDelivered`.
  - `oldContainer == player` and `newContainer != 0`: player gave/sold →
    `MarkDiscardedToContainer`.
  - `oldContainer == player` and `newContainer == 0`: player dropped to world →
    look up the dropped REFR via `RE::TESObjectREFR::Find` on the base form in the
    player's current cell, then `MarkDroppedToCell`.
- `src/Plugin.cpp` — register the sink in `kDataLoaded`.

**Specifics:**

- The sink runs off the main thread; marshal back via
  `AsyncDispatch::MarshalToMainThread` before mutating pool state or doing any
  RE-side reference manipulation.
- For the world-drop case: if the dropped REFR can't be found (cell already
  detached, or the player dropped immediately and the engine hasn't fully
  instantiated the world ref yet), retry on a 250ms delayed pass. If still not
  found, log a warning and run the eviction without the world-side disable/delete —
  the orphan REFR will reset with the cell.

**Verify:** Build clean. With the smoke-test-style flow no longer present, this
sink can't be exercised end-to-end yet — but logging `ContainerChanged: slot N
detected (action=<deliver/discard/drop>)` makes manual console testing possible
(use `player.additem` and `player.drop` against any of the pool FormIDs).

---

### Step 9 — Eviction logic in `Allocate`

**[CLAUDE]**

**Goal:** Replace the Step 4 placeholder `Allocate` with the full allocation
policy: Free → oldest Read (evict) → oldest InInventory (evict). Discarded slots
should never appear here in steady state but are handled defensively.

**Files:**

- `src/LetterPool.cpp` — rewrite `Allocate` per the **Allocation policy** section.
  Extract `EvictSlot(slotIndex)` as a helper that runs the full reference cleanup
  sequence and resets state. Reuses the cleanup logic added in Step 8 for the
  discard sinks (so the discard path and the eviction path share one
  implementation).
- The eviction's `kHasBeenRead` clear (proven by the smoke test) goes inside
  `EvictSlot` so every recycled form starts fresh in inventory.

**Specifics:**

- `EvictSlot` is the choke point for slot reset. Both the allocator (on eviction)
  and the container-changed sink (on discard) call it. Any other future entry
  point that needs to "clear a slot back to Free" should call it too.
- Eviction logs at the verbosity set by `iLetterPoolEvictionLogVerbosity` —
  default 1 logs the slot index, the prior state, and the reason; 2 also logs the
  current container handle and the cleanup operations that ran.

**Verify:** Build clean. Without a real action yet, allocation can be exercised
via a one-off debug helper that calls `Allocate` 25 times in a row and logs slot
indices — first 20 should succeed against Free slots; the 21st onwards should
log eviction events (no slots in any state yet beyond Free, so all evictions
will be Free → Free no-ops, but the code paths get exercised).

---

### Step 10 — Letter-compose prompt and content-LLM call wiring

**[CLAUDE]**

**Goal:** Author the `narrative_engine_letter_compose` prompt and wire the
content-generation LLM call as a standalone helper the action will use. No action
yet — this step verifies the prompt round-trips and the response parses.

**Files:**

- `statics/SKSE/Plugins/SkyrimNet/prompts/narrative_engine_letter_compose.prompt`
  — new prompt per the **Content-generation LLM call** section. Follows
  `docs/CUSTOM_PROMPTS.md`: domain-category content guidance only (no specific
  topic lists), instruct the LLM to invent the specific situation, output the
  five-key JSON object.
- `include/LetterComposer.h` / `src/LetterComposer.cpp` — new helper module. API:
  ```cpp
  struct LetterComposition
  {
      RE::FormID  senderNpcFormID;
      std::string senderLabel;
      std::string body;
      std::string mood;
      std::string topicTag;
  };

  // Async. Callback fires on a SkyrimNet worker thread; marshal back for main.
  void Compose(const ActionContext& ctx,
               int                  urgencyHint,    // 0=low, 1=med, 2=high
               std::function<void(std::optional<LetterComposition>)> callback);
  ```
- `src/LetterComposer.cpp` — builds the candidate pool by calling
  `PublicGetActorEngagement` (top 12 by engagement), then for each candidate
  calls `PublicGetMemoriesForActor(form_id, kPerCandidateMemoryCap,
  "Dragonborn")` to pull a player-relevant memory tail from that candidate's
  perspective. Assembles the per-candidate context per the
  **Content-generation LLM call** section, fires `SendCustomPromptToLLM`,
  parses the response with the `StripMarkdownFences` helper Phase 03 already
  exposed.

**Specifics:**

- Variant: `"narrative_engine_director"` so the call uses the same LLM-config
  overrides the rest of the Director uses (per the plugin manifest).
- Per-candidate memory cap: ~6 entries (`kPerCandidateMemoryCap = 6`). With 12
  candidates that's 72 memory snippets in the prompt — bounded, and each is
  short (a one-line text + small metadata block). The
  `contextQuery = "Dragonborn"` argument biases SkyrimNet's vector search
  toward player-involving entries; the alternative (empty contextQuery for
  recency-first) is also viable since candidates are already pre-filtered for
  high player engagement, but the semantic bias tightens the relevance signal.
- Validation rejects responses missing any required key, with body length outside
  bounds, or with `sender_npc_form_id` not in the candidate set. Failures pass
  `std::nullopt` to the callback.
- If `PublicGetMemoriesForActor` returns an empty array for a candidate, that
  candidate is still kept in the pool (the LLM might pick them based on name
  + engagement alone) but flagged in the prompt as having no recent
  player-involving memories — useful signal that the LLM should bias toward
  candidates that DO have memory to draw on.

**Verify:** Build clean. Add a temporary one-shot test (via a `console` command or
a 10-second timer like the smoke test had) that calls `LetterComposer::Compose`
once and logs the result. Boot Skyrim, trigger it, see a parsed
`LetterComposition` in the log with a plausible sender name and a 60-180 word body
— or a parse-failure log with the raw response for diagnosis. Remove the test
hook before merging.

---

### Step 11 — NPCLetterAction skeleton and registration

**[CLAUDE]**

**Goal:** Implement the action's metadata (name/description/polarity/IsAvailable)
and register it in `ActionRegistry`. `Start` is a stub that logs and returns
failure — the real implementation is Step 13.

**Files:**

- `include/NPCLetterAction.h` — class declaration.
- `src/NPCLetterAction.cpp` — `Name() = "npc_letter"`,
  `Description() = "..."` (one paragraph for the LLM), `Polarity() = Either`.
  `IsAvailable` checks the conditions per the **Preconditions** section,
  including a call to the existing `LocationKeywords::IsDangerous()` predicate
  (already wired and consumed by AmbushAction; the `kDangerous` keyword list at
  `include/LocationKeywords.h:69` covers the dungeon / lair / camp / crypt
  families this gate needs). `Start` logs "not yet implemented" and returns
  `started = false`.
- `src/Plugin.cpp` — register the action at `kDataLoaded` after `AmbushAction`.

**Specifics:**

- `IsAvailable` is the first action that gates on
  `PublicIsMemorySystemReady()` and on a min sender-candidate count. The
  candidate-count proxy is a quick call to `PublicGetActorEngagement` requesting
  top 5 — if it returns an array of length < `iLetterMinSenderCandidates`, the
  action declines.
- We use `IsDangerous` directly rather than `!IsSafe`. Most cells/locations
  carry neither label (a road in the wilderness, a generic exterior cell, etc.).
  The letter action wants "anywhere except dangerous"; the ambush action wants
  "anywhere except safe." Symmetric reasons, independent gates.

**Verify:** Build clean. Boot Skyrim; the action-select prompt should now
(after the Director picks ANY direction with `npc_letter` filtered in) include
`npc_letter` in candidates. With the stub `Start`, selecting it logs the
"not yet implemented" line and the dispatcher records a failed action. Walking
into a dungeon (e.g. Bleak Falls Barrow) and forcing a fresh `IsAvailable`
check should show the action declining with the `IsDangerous` reason.

---

### Step 12 — Dispatcher C++-side completion API + ActionContext propagation

**[CLAUDE]**

**Goal:** Expose `ActionDispatcher::CompleteAction(std::string_view)` as a public
C++ entry point that does what the existing `_ne_ActionCompleted` ModEvent sink
does (push to recency ring, set `g_lastActionCompletedAt`, clear in-flight, push
dashboard refresh). Refactor the existing sink to delegate to it. This is the
hook letters will use to mark themselves complete in pure C++ instead of doing a
Papyrus round-trip for an event we're synthesizing ourselves.

**Files:**

- `include/ActionDispatcher.h` — declare the new public function.
- `src/ActionDispatcher.cpp` — implement; extract the shared body of the existing
  ModEvent sink handler into the new function so the two paths share one
  implementation. Sink does FormID/name validation, then delegates. New C++ API
  asserts/logs if the name doesn't match the in-flight action and otherwise
  delegates. Both paths converge on the same mutex-guarded state update.

**Verify:** Build clean. Phase 03's ambush flow continues to work — verify by
firing an ambush and confirming the Papyrus completion still clears in-flight
and starts cooldown. (No new functionality is testable yet; this is plumbing
prep.)

---

### Step 13 — NPCLetterAction: `Start` + dispatch verification (`DetectAndRollbackFailedStart` + `DetectCompletion`)

**[CLAUDE]**

**Goal:** Wire `Start` to do real work, AND implement the two `IAction`
verification polls so the dispatcher only marks the action complete once the
letter is confirmed in the vanilla courier's inventory — and rolls back if it
never lands. The vanilla courier handles physical delivery on its own timeline,
which the action does not gate on.

**Files:**

- `src/NPCLetterAction.cpp` — `Start`, `DetectAndRollbackFailedStart`, and
  `DetectCompletion` implementations per the **`Start`** and **Dispatch
  verification** sections.
- Add `LetterPool::Free(slotIndex)` and `LetterPool::AbortPending(slotIndex)`
  if Step 4's scaffold didn't expose them — needed for the LLM-failure
  release path and the verification-rollback path respectively.
- Reuse `LetterComposer::Compose` from Step 10 for the LLM call.

**Specifics:**

- **WICourier quest resolution.** At `kDataLoaded`, the action caches a
  `RE::TESQuest*` pointer to the vanilla `WICourier` quest. The lookup is by
  hardcoded FormID (`Skyrim.esm`-side); resolution failure logs an error and
  permanently disables the action's `IsAvailable`. (Vanilla `WICourier` always
  exists; a failure here means an exceptionally broken install.) The action
  also caches the index or pointer of `WICourier`'s `CourierRef` reference
  alias so the verification polls can resolve the Courier NPC reference in
  O(1).
- **`Start`.** VM-dispatches to `WICourierScript.AddItemToContainer` per the
  **Vanilla courier dispatch** section. Returns `started=true` immediately;
  does NOT call `CompleteAction` (the dispatcher will see completion via
  `DetectCompletion` once verified). On LLM-callback failure or validation
  failure: release the slot via `LetterPool::Free` and call
  `ActionDispatcher::CompleteAction("npc_letter")` directly so the in-flight
  state unwinds and cooldown begins. Log the LLM response body for diagnosis.
  The dispatch itself happens on the main thread inside the LLM callback's
  marshaled continuation.
- **`DetectAndRollbackFailedStart`.** If
  `secondsSinceStart < Settings::Get().letterDispatchVerifyDelaySeconds`,
  return false (too early). Otherwise resolve the Courier NPC reference from
  `WICourier`'s `CourierRef` alias and call `GetItemCount(bookForm)` on it.
  If count > 0, the dispatch landed — return false and let `DetectCompletion`
  handle it. If count == 0, the dispatch silently failed: call
  `LetterPool::AbortPending(slotIndex)` to reset the slot and return true.
  The dispatcher will roll back its in-flight tracking; no cooldown applies.
- **`DetectCompletion`.** Resolve the Courier NPC reference and call
  `GetItemCount(bookForm)`. If count > 0, return true. The dispatcher clears
  in-flight, applies cooldown, and the LetterPool slot stays in
  `PendingDelivery` until `TESContainerChangedEvent` fires with the player as
  the receiving container (which transitions it to `InInventory` and triggers
  the sender-side memory write — see Step 14).
- **Logging.** Both polls log at debug level on every invocation
  (`LetterAction: verify@<secondsSinceStart>s — courier has N copies`) so the
  verification timeline is visible in the log.

**Verify:** Build clean. Boot Skyrim; trigger a phase advance so the dispatcher
picks `npc_letter`. SKSE log shows:

- action-select picks `npc_letter` → content LLM round-trip →
  `LetterPool: populated slot N (sender=<...>, body=N chars)` →
  `NPCLetterAction: VM-dispatched WICourierScript.AddItemToContainer(0xFE0E76xx, 1)`.
- Within ~1-5 seconds, `LetterAction: verify@Ns — courier has 1 copies` →
  `DetectCompletion: complete` → dispatcher applies cooldown. The slot is now
  in `PendingDelivery` state; the dashboard's `actionInFlight` indicator
  clears.
- Within a few in-game minutes, the vanilla courier finds the player and hands
  over the letter. Reading it shows the LLM-generated body.

To test the rollback path, temporarily break the dispatch by either passing a
non-existent FormID to the VM call or by disabling the `WICourier` quest via
console (`StopQuest WICourier`). The verification poll should log
`courier has 0 copies` → `DetectAndRollbackFailedStart: rolling back` → slot
returns to Free and the action is immediately re-selectable.

---

### Step 14 — SkyrimNet memory writes (sender at delivery, player at read)

**[CLAUDE]**

**Goal:** Fire both `PublicAddMemory` calls at their respective lifecycle
events, mirroring actual cognitive engagement: sender writes on delivery (when
their part of the act is complete), player writes on read (when they actually
process the contents). Director-side completion happened earlier (during
Step 13's `DetectCompletion` poll once the dispatch was verified); this step
is purely about the LetterPool's lifecycle hooks for memory integration.

**Files:**

- `src/LetterPool.cpp`:
  - `MarkDelivered` (stubbed in Step 8) now fires the **sender-side**
    `PublicAddMemory` call per the **SkyrimNet memory integration** section
    (formId = `senderNpcFormID` from the slot, text "Sent a letter to the
    Dragonborn about <topic_tag>.", importance per mood, emotion = mood,
    location = sender's known location via `SkyrimNetAPI::GetNPCLocation` if
    available, falling back to player's location).
  - `MarkRead` (stubbed in Step 7) now fires the **player-side**
    `PublicAddMemory` call (formId = player [0x14], text "Received a letter
    from <sender_label> — <topic_tag>.", importance per mood, emotion = mood,
    location = player's current location at read time).

**Specifics:**

- The two writes happen in different functions because they fire at different
  events, but the implementation logic — mood-to-importance map, memory-type
  selection, location resolution, error handling — is identical and should
  live in a shared `LetterPool::FireMemoryWrite(...)` helper that both
  functions call. Single source of truth for the formatting; single point of
  diagnostic logging.
- Both calls are gated by `PublicIsMemorySystemReady()` even though
  `IsAvailable` already checked it — the DB could in principle have torn down
  between selection and delivery/read, especially since vanilla courier
  delivery is asynchronous and can take several in-game minutes (an unlikely
  but easy guard).
- Failure to write either memory is logged but doesn't fail the lifecycle.
  The letter still works; we just lose that side of the recall.
- The sender's memory text is deliberately first-person from the NPC's frame
  ("Sent a letter to the Dragonborn about...") so it reads naturally when
  SkyrimNet surfaces the memory in that NPC's own conversational context
  later. Same first-person framing for the player ("Received a letter from
  Ysolda — debt repayment").

**Verify:** Build clean. Trigger a letter dispatch.
- After delivery (before the player opens the letter): log shows
  `LetterPool: sender NPC memory written (formId=0x..., importance=X)` but
  NOT a player memory write. Talk to the sender NPC and ask about a letter
  — the sender memory should surface.
- Talk to a different NPC (not the sender) about the letter — they should
  NOT have any memory of it, because the player never read it (so the
  player's memory was never written, so SkyrimNet has no recall to surface).
- Now read the letter. Log shows `LetterPool: player memory written
  (importance=X)`. Talk to any NPC about a recent letter — the player
  memory should surface.

---

### Step 15 — Dashboard: tab bar + Letters tab (pool + recent-dispatch detail)

**[CLAUDE]**

**Goal:** Split the dashboard into two tabs (Director, Letters), reparent the
existing panels into the Director tab, and build the Letters tab with its
recent-dispatch detail section and per-slot pool overview. Wire the C++ side
to emit the expanded `letter_pool` payload.

**Files:**

- `src/DashboardUIManager.cpp` — `ComposeFullStateJSON` emits the new
  `letter_pool` object per the **Schema additions** subsection of the
  Dashboard section. Iterates all 20 pool slots, dumps one entry each, plus
  computes `most_recent_dispatch_slot` by finding the slot with the largest
  `deliveredAt` among non-Free slots.
- `dashboard/src/types.ts` — extend `DirectorState` with the new `letter_pool`
  shape.
- `dashboard/src/components/TabBar.tsx` — new component: two-button tab bar
  pinned at the dashboard root. Takes `activeTab` and `onTabChange` props.
- `dashboard/src/components/tabs/DirectorTab.tsx` — new wrapper that hosts
  the existing StatusBanner / PhasePanel / LastEvaluation / DecisionList /
  EventList panels. Pure regrouping; the panel components themselves don't
  change.
- `dashboard/src/components/tabs/LettersTab.tsx` — new component, hosts the
  two letter sections.
- `dashboard/src/components/RecentDispatchDetail.tsx` — new component. Takes
  the slot from `letter_pool.slots[letter_pool.most_recent_dispatch_slot]`;
  unmounts when that index is null. Renders the sender label, topic,
  mood, state badge, delivered/read relative timestamps, and body preview
  per the **Tab 2 — Letters** subsection.
- `dashboard/src/components/LetterPoolOverview.tsx` — new component. Renders
  the stats header (counts derived from `letter_pool.slots` client-side) and
  the 20-row slot table per the **Tab 2 — Letters** subsection. Each row
  uses a small state-badge component for the color-coded state column.
- `dashboard/src/App.tsx` — adds `const [activeTab, setActiveTab] =
  useState<'director'|'letters'>('director')`. Renders, in order:
  `<StatusBanner status={s.status} />` (lifted out of DirectorTab so it's
  always visible), then `<TabBar active={activeTab} onChange={setActiveTab} />`,
  then either `<DirectorTab state={s} />` or `<LettersTab pool={s.letter_pool} />`
  based on `activeTab`.
- `dashboard/src/components/LastEvaluation.tsx` — revert the inline
  letter-action detail rendering. For letter dispatches, the row shows just
  `→ fired: npc_letter` (same shape as for ambush). The sender/topic
  details now live on the Letters tab.
- `dashboard/styles.css` — tab bar styling (chip-style buttons, active
  state, hover); state-badge color coding for the slot table (muted /
  amber / blue / green for free / pending / inventory / read); compact
  layout for the slot table (20 rows visible without scrolling at the
  default dashboard size).

**Specifics:**

- Active tab persists across re-renders within a single dashboard-open
  session but does NOT persist across dashboard close/reopen — that's
  acceptable for v1 (dashboard always opens to Director tab).
- Aggregate stats (Free/Pending/InInventory/Read counts) are computed
  client-side via a `useMemo` on `letter_pool.slots` — no extra C++-side
  rollup needed; the source data is the slots themselves.
- The recent-dispatch detail's body preview is the first ~200 chars of the
  cached body with the `<font face='$HandwrittenFont'>...</font>` wrapper
  stripped before display, so the dashboard reader sees the actual letter
  content without rendering directives.

**Verify:** Build clean (C++ + Rollup). Open the dashboard with F7. Confirm
the tab bar appears with two tabs, Director active by default. Switch to
Letters; with no letters dispatched yet, the recent-dispatch section is
absent and the pool table shows all 20 slots as Free. Trigger a letter
dispatch:

- Director tab's LastEvaluation shows `→ fired: npc_letter` (no inline
  detail).
- Letters tab's recent-dispatch detail appears with the LLM-generated
  sender label, topic, mood, and body preview. State badge shows
  PendingDelivery.
- After the vanilla courier delivers, the state badge updates to
  InInventory; the relative timestamp shows the seconds since delivery.
- After the player reads, the state badge updates to Read and a "read Ns
  ago" row appears.
- The pool table row for the slot reflects the same state and metadata
  in compact form throughout.

---

### Step 16 — End-to-end verification

**[USER]**

**Goal:** Play the integration loop in-game and confirm the experience matches the
design intent.

**Sub-tasks:**

1. Boot Skyrim. Open the dashboard (F7). Confirm the LetterPool panel shows
   20 Free slots.
2. Force-advance the Director to a tick where a lowering action is the most
   plausible pick — e.g. spend ~5 minutes in a calm city cell so phase dwell time
   exceeds `iIdealDurationExposition`. The action-select should pick
   `npc_letter`.
3. Within a few in-game minutes (vanilla courier timing — not real-time),
   the vanilla courier walks up and delivers a letter. Open inventory, find
   the new Notes-tab entry with the LLM-generated sender label. Confirm the
   dashboard's LetterPool panel shows the `PendingDelivery → InInventory`
   transition. The dashboard's `actionInFlight` indicator should already be
   clear at this point — the Director-side action completed at dispatch-queue
   time, not at physical delivery.
4. **Before reading the letter**, talk to the sender NPC and ask about it via
   SkyrimNet chat. The sender should recall having sent it (sender-side
   memory fires at delivery). Talk to a DIFFERENT NPC about the letter — they
   should have no idea, because the player-side memory hasn't been written
   yet.
5. Read the letter. Confirm the body rendered is the LLM-generated content
   (not the ESP placeholder). Confirm the dashboard now shows the slot as
   Read.
6. Now talk to any third-party NPC about the letter via SkyrimNet chat — the
   player-side memory should now be surfacing, so they should be able to
   reference the letter's contents.
7. Discard the letter (drop it on the ground or sell it to a vendor). Confirm
   the dashboard's LetterPool panel returns the slot to Free.
8. Save. Quit Skyrim. Relaunch. Load the save. Trigger another letter.
   Confirm letter content was preserved across the process restart for any
   still-in-inventory letters (via the co-save persistence).
9. Force the pool to exhaustion (use the console to dispatch ~22 letters in a row
   by manually triggering the action). Confirm the pool correctly evicts oldest
   Read first, then oldest InInventory, and that the evicted letter references
   are physically removed from wherever they were (player inventory, dropped on
   the ground, in a vendor's inventory).
10. Verify the unread-letter asymmetry directly: dispatch another letter, do NOT
    read it, then discard it. Talk to the sender NPC — they should still
    remember sending it. Talk to a third-party NPC about the letter — they
    should NOT have any recall (the player-side memory was never written
    because the player never read it).

**Verify:** Each sub-task's expected behavior happens. Edge cases logged but no
crashes, no soft-locks, no orphan references. The dashboard's counts always match
the actual in-game inventory state.

---

## Done condition

Phase 04 is complete when:

- All 16 implementation steps are checked off.
- The integration-verification run (Step 16) passes without intervention.
- The SKSE log over a 30-minute in-game session shows letters being dispatched,
  delivered, read, and slots being recycled without errors or stale-lock
  warnings.
- Save/load round-trip preserves in-flight letter contents across a process
  restart, verified by reading a delivered-but-unread letter after relaunching
  the game.
