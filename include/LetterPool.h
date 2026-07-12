#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>

namespace SKSE
{
    class SerializationInterface;
}

// LetterPool — a fixed pool of 20 pre-authored Book records whose runtime
// content (FULL field, body text) is mutated at allocation time and read
// back through MinHook detours on the engine's book-rendering path. The
// pool is allocated by NPCLetterBeat and any other beat that needs to
// put dynamic-content book / note items into the world.
//
// This header is the Step 4 scaffold: just the data structures and the
// scalar lifecycle. The hook integration (Step 6), event-driven
// transitions (Steps 7-8), co-save persistence (Step 5), and eviction
// logic (Step 9) layer on top of this baseline.
//
// Threading: most calls happen from the main thread, but co-save
// callbacks (Step 5 onward) and SKSE event sinks (Step 7-8 onward) can
// fire off-thread. The module guards its state with an internal mutex
// so callers don't need to coordinate.
namespace NarrativeEngine::LetterPool
{
    // Pool size — fixed at 20 by the ESP. The Settings value
    // `letterPoolSize` is informational only; this constant is the
    // source of truth for the in-memory layout.
    inline constexpr std::size_t kPoolSize = 20;

    // SKSE co-save record type ID — 'NELP' = NarrativeEngine Letter Pool.
    // Frozen; changing it would orphan every previously-saved pool
    // payload.
    inline constexpr std::uint32_t kRecordTypeId = 'NELP';

    // Slot lifecycle. See PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md
    // for the full state-machine description.
    enum class State : std::uint8_t
    {
        Free,            // slot unused; available for allocation
        PendingDelivery, // populated; courier has not yet handed off
        InInventory,     // courier delivered; player has not yet read
        Read,            // player closed BookMenu after opening
        Discarded,       // player removed via sell/drop/transfer (transient;
                         // recycle scan returns to Free synchronously)
    };

    // What the allocator returns on success.
    struct AllocatedSlot
    {
        std::size_t slotIndex = 0;
        RE::FormID bookFormID = 0;
    };

    // Why the allocator returned no slot.
    enum class AllocationFailure : std::uint8_t
    {
        PoolNotResolved, // Initialize never ran, or no Book forms resolved
        EvictionFailed,  // pool exhausted (everything in flight pre-delivery)
    };

    // Dashboard-facing aggregate counts.
    struct PoolStats
    {
        std::size_t free = 0;
        std::size_t pendingDelivery = 0;
        std::size_t inInventory = 0;
        std::size_t read = 0;
        // Discarded is transient (recycled synchronously) — no stat.

        // How many of the 20 slots resolved to a real Book form at
        // Initialize. If < kPoolSize, the missing forms can't allocate.
        std::size_t resolved = 0;
    };

    // Resolve the 20 `_ne_PooledLetterNN` EditorIDs to Book FormIDs and
    // populate the slot table. Idempotent: a second call rewires the
    // FormIDs but leaves slot state untouched (so a kDataLoaded re-fire
    // doesn't wipe an in-flight letter).
    //
    // Logs `LetterPool: pool resolved (N forms; M failed)` at the end so
    // ESP wiring issues surface in the SKSE log.
    //
    // A Book form on its own is not sufficient to allocate a slot —
    // the caller must ALSO hand the pool the per-slot delivery quest
    // pointers via SetPerSlotQuests. Until that happens, every slot
    // is undispatchable and Allocate refuses everything. In practice
    // NPCLetterBeat_Init resolves the quests and hands them off
    // within the same kDataLoaded tick.
    void Initialize();

    // Hand the pool the resolved per-slot delivery quests. `quests[i]`
    // is either the quest for slot i, or nullptr if that slot's quest
    // didn't resolve. The pool stores these pointers and uses them
    // for both the dispatchability check inside Allocate and the
    // Stop+Reset sequence during eviction. Idempotent; state derived
    // from ESP wiring at kDataLoaded and re-derived every session
    // (not persisted).
    void SetPerSlotQuests(const std::array<RE::TESQuest*, kPoolSize>& quests);

    // Look up a slot's per-slot delivery quest, or nullptr if the
    // slot is undispatchable / out of range. Available to any caller
    // that needs to advance stages, start the quest, poll aliases,
    // etc. — LetterPool is now the single source of truth for the
    // per-slot quest identity.
    RE::TESQuest* GetPerSlotQuest(std::size_t slotIndex);

    // Allocate the next Free slot. Step 4 implementation is strictly
    // "first slot in state Free with a resolved FormID"; the
    // eviction-aware policy (oldest Read, then oldest InInventory) lands
    // in Step 9.
    //
    // Returns the slot index + the Book FormID to use, or a failure
    // reason. The caller follows up with PopulateSlot to set content.
    std::expected<AllocatedSlot, AllocationFailure> Allocate();

    // Populate an allocated slot with LLM-supplied content. Mutates the
    // Book form's FULL field via SetFullName so the inventory shows the
    // generated title; caches the body in the in-memory map the hooks
    // (Step 6) will read from. `mood` and `tags` are stored on the slot
    // so the later MarkDelivered / MarkRead memory-write hooks (Step 16)
    // can produce symmetric memories without re-invoking the composer.
    //
    // Caller must have just received `slotIndex` from a successful
    // Allocate. Transitions the slot to PendingDelivery and stamps the
    // current real-time onto deliveredAt (initial value; overwritten by
    // MarkDelivered in Step 8 when the courier actually hands off).
    void PopulateSlot(std::size_t slotIndex,
                      std::string senderLabel,
                      std::string body,
                      RE::FormID senderNpcFormID,
                      std::string topicTag,
                      std::string mood,
                      std::vector<std::string> tags);

    // Manual release back to Free. Step 4 testing entry point — Steps 8
    // (discard sinks), 9 (eviction inside Allocate), and 13 (action
    // LLM-failure / abort paths) are the real callers in production.
    // No reference cleanup happens at this layer; the scan-and-cleanup
    // sequence lives in EvictSlot, which Step 9 introduces.
    void Free(std::size_t slotIndex);

    // Release a slot whose VM dispatch never actually placed a letter
    // in the world — the LLM-callback-failure path and the
    // verification-rollback path use this rather than the heavier
    // recycle scan. Semantically identical to Free in Step 13's
    // implementation; named separately so the rollback log line is
    // distinct from a normal Free in diagnostics.
    void AbortPending(std::size_t slotIndex);

    // Aggregate counts for the dashboard / log diagnostics.
    PoolStats GetStats();

    // Per-slot snapshot shape for the Letters dashboard tab.
    // A trimmed projection of the internal Slot record — only the
    // fields the dashboard actually renders, so the internal type
    // (which carries FormIDs and other implementation state) stays
    // private to the .cpp.
    struct SlotSnapshot
    {
        std::size_t index = 0;
        State state = State::Free;
        std::string senderLabel;
        std::string topicTag;
        std::string mood;
        std::string body;         // full cached body; the dashboard trims to
                                  // a preview client-side or in the JSON
                                  // encoder.
        double deliveredAt = 0.0; // Unix-epoch seconds
        double readAt = 0.0;      // Unix-epoch seconds
    };

    // Returns a snapshot of every slot (Free slots included, sender/topic
    // fields empty) for the Letters dashboard tab. Taken under the pool
    // mutex, then released before returning — the caller sees a consistent
    // point-in-time view.
    std::array<SlotSnapshot, kPoolSize> GetSlotSnapshots();

    // -----------------------------------------------------------------
    // Hook integration (Step 6)
    // -----------------------------------------------------------------
    //
    // MinHook detours on TESDescription::GetDescription and
    // BookMenu::OpenBookMenu route the engine's reads of any pool Book's
    // body text through LetterPool::TryGetBody, which returns the
    // LLM-generated body. The smoke test established that two hook
    // points are needed:
    //   - TESDescription::GetDescription (filtered on fieldType == 'CSED'):
    //     SkyrimNet's `book.GetDescription()` Papyrus grab and other
    //     plugin-side reads. Substituting here makes the LLM body show
    //     up in SkyrimNet event logs, player thoughts, decorators.
    //   - BookMenu::OpenBookMenu: the actual rendered page the player
    //     reads. BookMenu does NOT use GetDescription internally.

    // Install both MinHook detours. Idempotent — second call is a no-op.
    // Call after Initialize so the form set is populated.
    void InstallHooks();

    // True iff `formID` is one of the resolved pool Book FormIDs. Used
    // by the hooks for an O(1) gate before the full slot lookup.
    bool IsManagedForm(RE::FormID formID);

    // If `formID` matches a pool slot AND the slot has cached body
    // content, write the body to `outBody` and return true. Returns
    // false (and leaves outBody untouched) for non-pool forms or pool
    // forms with no current content (e.g. Free slots).
    //
    // Called from hot paths inside the MinHook detours, so the lock
    // window is kept minimal.
    bool TryGetBody(RE::FormID formID, std::string& outBody);

    // -----------------------------------------------------------------
    // Read detection (Step 7)
    // -----------------------------------------------------------------
    //
    // BookMenu's open/close edges are how we detect that the player
    // actually read a letter (TESBookReadEvent doesn't fire for
    // inventory reads — established empirically by the smoke test).
    // The OpenBookMenu hook installed in Step 6 stamps the currently-
    // open slot index, and a MenuOpenCloseEvent sink fires MarkRead on
    // the matching close edge.

    // Register the MenuOpenCloseEvent sink against RE::UI. Idempotent;
    // call after InstallHooks at kDataLoaded.
    void RegisterMenuEventSink();

    // Mark a slot as read: transitions InInventory → Read and stamps
    // readAt. Step 14 layers the player-side SkyrimNet memory write
    // on top.
    void MarkRead(std::size_t slotIndex);

    // -----------------------------------------------------------------
    // Container tracking (Step 8)
    // -----------------------------------------------------------------
    //
    // A TESContainerChangedEvent sink (registered via
    // RegisterContainerEventSink below) maps engine container moves
    // for pool Book FormIDs onto these three transitions:
    //   - courier → player:    MarkDelivered
    //   - player → container:  MarkDiscardedToContainer
    //   - player → world drop: MarkDroppedToCell
    //
    // Direct callers should be rare — most lifecycle transitions go
    // through the sink. The dispatch helpers are exposed publicly so
    // future testing / debug paths can drive them too.

    // Register the TESContainerChangedEvent sink against
    // RE::ScriptEventSourceHolder. Idempotent; call after
    // RegisterMenuEventSink at kDataLoaded.
    void RegisterContainerEventSink();

    // Courier has delivered the letter to the player. Transitions
    // PendingDelivery → InInventory and overwrites deliveredAt with
    // the actual delivery time. Step 14 layers the sender-side
    // SkyrimNet memory write here.
    void MarkDelivered(std::size_t slotIndex);

    // Player removed the letter via sell / give / transfer-to-container.
    // The destination container is consumed inside the function for the
    // cleanup scan and not stored. After cleanup the slot returns to
    // Free and is available for reallocation.
    void MarkDiscardedToContainer(std::size_t slotIndex, RE::TESObjectREFR* destination);

    // Player dropped the letter into the world. The world ref the
    // engine instantiated is consumed for cleanup and not stored.
    // Slot returns to Free.
    void MarkDroppedToCell(std::size_t slotIndex, RE::TESObjectREFR* worldRef);

    // -----------------------------------------------------------------
    // Co-save persistence ('NELP' record, version 1)
    // -----------------------------------------------------------------
    //
    // Slot state survives save/load via the SKSE serialization
    // interface. Both FormID fields (bookFormID, senderNpcFormID) go
    // through ResolveFormID on load so the pool survives a player
    // adding or removing mods between save and load.
    //
    // OnLoad runs the reconciliation pass described in the
    // "Persistence" section of PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md
    // — slots in unreachable states get demoted to Free with a logged
    // reason. Step 5 includes the basic reconciliation logic; future
    // steps may layer richer recovery (e.g. defensive recycle scan
    // once Step 9 introduces EvictSlot).

    void OnSave(SKSE::SerializationInterface* intfc);
    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);
    void OnRevert();
} // namespace NarrativeEngine::LetterPool
