#include <LetterPool.h>

#include <NPCLetterAction.h>
#include <Settings.h>
#include <logger.h>

#include <AsyncDispatch.h>

#include <SKSE/Interfaces.h>

// RE/Skyrim.h (via the PCH) doesn't pull in RE/Offsets.h, only
// RE/Offsets_VTABLE.h. Pull in explicitly so RE::Offset::TESDescription
// is in scope below. Namespace is `RE::Offset` (singular), not Offsets.
#include <RE/Offsets.h>

// BookMenu::OpenBookMenu is the path through which the rendered book
// body actually reaches the Scaleform page renderer. The smoke test
// established BookMenu doesn't go through TESDescription::GetDescription
// at all, so we need this second hook to make the substitution visible
// to the player.
#include <RE/B/BookMenu.h>

#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace NarrativeEngine::LetterPool
{
    namespace
    {
        // -----------------------------------------------------------------
        // Slot definition
        // -----------------------------------------------------------------

        struct Slot
        {
            RE::FormID  bookFormID      = 0;  // 0 means "EditorID failed to resolve"
            State       state           = State::Free;
            std::string body;
            std::string senderLabel;
            RE::FormID  senderNpcFormID = 0;
            std::string topicTag;
            double      deliveredAt     = 0.0;  // Unix-epoch seconds
            double      readAt          = 0.0;  // Unix-epoch seconds; 0 if unread
        };

        // -----------------------------------------------------------------
        // State
        // -----------------------------------------------------------------

        std::mutex                       g_mutex;
        std::array<Slot, kPoolSize>      g_slots{};
        bool                             g_initialized = false;

        // Hot lookup table for the MinHook detours. Repopulated by
        // Initialize from the slot table; guarded by g_mutex (writers
        // hold the lock, the hooks read briefly). An unordered_set is
        // overkill at N=20 but keeps the membership check O(1) without
        // requiring callers to iterate the full slot array on every
        // engine book read.
        std::unordered_set<RE::FormID>   g_managedForms;

        // Hook-install idempotency.
        std::atomic<bool>                g_hooksInstalled = false;

        // Step 7: slot index of the pool letter currently open in
        // BookMenu, or -1 if none. Set by the OpenBookMenu hook when
        // the matched FormID resolves to a slot, consumed by the
        // MenuOpenCloseEvent sink on the matching close edge. Atomic
        // because the hook fires on the engine thread and the menu
        // sink can fire on a separate UI thread.
        std::atomic<int>                 g_currentOpenSlot{-1};

        // Sink-registration idempotency.
        std::atomic<bool>                g_menuSinkRegistered = false;
        std::atomic<bool>                g_containerSinkRegistered = false;

        // Resolve a Book FormID to its pool slot index, or -1 if not
        // pooled. Caller must hold g_mutex.
        int FindSlotByFormIDLocked(RE::FormID formID)
        {
            if (formID == 0) return -1;
            for (std::size_t i = 0; i < kPoolSize; ++i) {
                if (g_slots[i].bookFormID == formID) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // Forward declaration — EvictSlot's body sits with the
        // container-tracking implementation further down (it shares
        // the recycle scan with the discard sinks). Allocate calls
        // it for the oldest-Read / oldest-InInventory eviction path.
        void EvictSlot(std::size_t slotIndex);

        // -----------------------------------------------------------------
        // Helpers
        // -----------------------------------------------------------------

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // EditorID for slot index `i` (0..19). Lifetime is the program; the
        // returned buffer is internal static and rotates per call (callers
        // must consume immediately).
        const char* SlotEditorID(std::size_t i)
        {
            static thread_local char buf[32];
            std::snprintf(buf, sizeof(buf), "_ne_PooledLetter%02zu", i + 1);
            return buf;
        }

        // Look up the Book TESForm for a slot's FormID. Returns nullptr
        // if the FormID was never resolved, or the form is no longer a
        // BOOK (e.g. the ESP was removed mid-session, which is a hard
        // failure mode we accept by no-op'ing the operation).
        RE::TESObjectBOOK* LookupBook(RE::FormID formID)
        {
            if (formID == 0) return nullptr;
            auto* form = RE::TESForm::LookupByID(formID);
            if (!form) return nullptr;
            return form->As<RE::TESObjectBOOK>();
        }

        const char* StateName(State s)
        {
            switch (s) {
                case State::Free:            return "Free";
                case State::PendingDelivery: return "PendingDelivery";
                case State::InInventory:     return "InInventory";
                case State::Read:            return "Read";
                case State::Discarded:       return "Discarded";
            }
            return "<unknown>";
        }
    }

    // -----------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------

    void Initialize()
    {
        std::scoped_lock lock(g_mutex);

        std::size_t resolved = 0;
        std::size_t failed   = 0;

        g_managedForms.clear();

        for (std::size_t i = 0; i < kPoolSize; ++i) {
            const char* edid = SlotEditorID(i);
            auto* form = RE::TESForm::LookupByEditorID(edid);
            if (!form) {
                g_slots[i].bookFormID = 0;
                ++failed;
                logger::error(
                    "LetterPool: EditorID '{}' did not resolve; slot {} disabled",
                    edid, i);
                continue;
            }
            auto* book = form->As<RE::TESObjectBOOK>();
            if (!book) {
                g_slots[i].bookFormID = 0;
                ++failed;
                logger::error(
                    "LetterPool: EditorID '{}' resolved to a non-BOOK form "
                    "(formType={}); slot {} disabled",
                    edid, static_cast<int>(form->GetFormType()), i);
                continue;
            }
            // Idempotent: a second Initialize call rewires the FormID
            // without clobbering an in-flight slot's state / body.
            g_slots[i].bookFormID = book->GetFormID();
            g_managedForms.insert(book->GetFormID());
            ++resolved;
        }

        g_initialized = true;
        logger::info("LetterPool: pool resolved ({} forms; {} failed)",
                     resolved, failed);
    }

    std::expected<AllocatedSlot, AllocationFailure> Allocate()
    {
        // Allocation policy (per PHASE_04 plan):
        //   1. A slot in state Free with a resolved FormID.
        //   2. If none free: oldest slot in state Read (smallest
        //      non-zero readAt) — EvictSlot, return.
        //   3. If none read: oldest slot in state InInventory
        //      (smallest deliveredAt) — EvictSlot, return.
        //   4. If none of those (everything PendingDelivery or
        //      unresolved): fail.
        //
        // EvictSlot needs the main thread because it sweeps engine
        // refs; we drop the lock around it and re-check the slot's
        // FormID before returning to defend against concurrent
        // mutation.

        std::size_t chosen = kPoolSize;  // sentinel
        RE::FormID  chosenFormID = 0;
        bool        needsEvict = false;

        {
            std::scoped_lock lock(g_mutex);
            if (!g_initialized) {
                return std::unexpected(AllocationFailure::PoolNotResolved);
            }

            // 1. Free slot.
            for (std::size_t i = 0; i < kPoolSize; ++i) {
                auto& slot = g_slots[i];
                if (slot.state == State::Free && slot.bookFormID != 0) {
                    chosen       = i;
                    chosenFormID = slot.bookFormID;
                    needsEvict   = false;
                    break;
                }
            }

            // 2. Oldest Read.
            if (chosen == kPoolSize) {
                double oldest = 0.0;
                for (std::size_t i = 0; i < kPoolSize; ++i) {
                    const auto& slot = g_slots[i];
                    if (slot.state != State::Read || slot.bookFormID == 0) continue;
                    if (chosen == kPoolSize || slot.readAt < oldest) {
                        chosen = i;
                        oldest = slot.readAt;
                    }
                }
                if (chosen != kPoolSize) {
                    chosenFormID = g_slots[chosen].bookFormID;
                    needsEvict   = true;
                }
            }

            // 3. Oldest InInventory.
            if (chosen == kPoolSize) {
                double oldest = 0.0;
                for (std::size_t i = 0; i < kPoolSize; ++i) {
                    const auto& slot = g_slots[i];
                    if (slot.state != State::InInventory || slot.bookFormID == 0) continue;
                    if (chosen == kPoolSize || slot.deliveredAt < oldest) {
                        chosen = i;
                        oldest = slot.deliveredAt;
                    }
                }
                if (chosen != kPoolSize) {
                    chosenFormID = g_slots[chosen].bookFormID;
                    needsEvict   = true;
                }
            }
        }

        if (chosen == kPoolSize) {
            // 4. Everything in PendingDelivery (or unresolved). Fail.
            return std::unexpected(AllocationFailure::EvictionFailed);
        }

        if (needsEvict) {
            // Synchronously Stop() + Reset() the per-slot delivery
            // quest. Allocate is followed immediately (same frame) by
            // PopulateSlot + PromoteSender + EnsureQuestStarted; a
            // VM-dispatched Stage 60 → 200 → Shutdown would race that
            // and EnsureQuestStarted would see the quest still running
            // and skip the alias-fill pass. The native Stop+Reset is
            // idempotent on an already-stopped quest, so the Free-slot
            // case is fine too.
            NPCLetterAction_QuestControl::ShutdownSlotQuestSync(chosen);
            EvictSlot(chosen);
            // Re-check FormID under the lock — defensive against the
            // (very unlikely) case where the slot's form unloaded
            // between candidate selection and eviction.
            std::scoped_lock lock(g_mutex);
            chosenFormID = g_slots[chosen].bookFormID;
            if (chosenFormID == 0) {
                return std::unexpected(AllocationFailure::EvictionFailed);
            }
        }

        return AllocatedSlot{ .slotIndex = chosen, .bookFormID = chosenFormID };
    }

    void PopulateSlot(std::size_t  slotIndex,
                      std::string  senderLabel,
                      std::string  body,
                      RE::FormID   senderNpcFormID,
                      std::string  topicTag)
    {
        if (slotIndex >= kPoolSize) {
            logger::error("LetterPool::PopulateSlot: slotIndex {} out of range", slotIndex);
            return;
        }

        RE::TESObjectBOOK* book = nullptr;
        {
            std::scoped_lock lock(g_mutex);
            auto& slot = g_slots[slotIndex];
            if (slot.bookFormID == 0) {
                logger::error(
                    "LetterPool::PopulateSlot: slot {} has no resolved FormID; aborting",
                    slotIndex);
                return;
            }
            slot.senderLabel     = std::move(senderLabel);
            slot.body            = std::move(body);
            slot.senderNpcFormID = senderNpcFormID;
            slot.topicTag        = std::move(topicTag);
            slot.deliveredAt     = NowUnixSeconds();  // queued-with-courier
                                                      // time; overwritten by
                                                      // MarkDelivered (Step 8).
            slot.readAt          = 0.0;
            slot.state           = State::PendingDelivery;

            // The SetFullName call must touch the engine's Book record,
            // which is a main-thread operation. We hold the mutex through
            // it because the form pointer is stable and the call is cheap;
            // dropping the lock would just open a race where another
            // PopulateSlot for the same index (which can't happen with
            // single-allocator usage anyway) could interleave with the
            // SetFullName below.
            book = LookupBook(slot.bookFormID);
        }

        if (!book) {
            logger::error(
                "LetterPool::PopulateSlot: slot {} FormID lookup failed at SetFullName",
                slotIndex);
            return;
        }

        // SetFullName takes a C string; the slot's senderLabel is the
        // canonical store. Read it back under the lock to keep the call
        // self-consistent if a future caller changes the field shape.
        std::string labelCopy;
        {
            std::scoped_lock lock(g_mutex);
            labelCopy = g_slots[slotIndex].senderLabel;
        }
        book->SetFullName(labelCopy.c_str());

        logger::info(
            "LetterPool: populated slot {} (book=0x{:08X}, sender='{}', body={} chars, topic='{}')",
            slotIndex, book->GetFormID(), labelCopy,
            // body length is read separately to avoid holding the lock
            // across the format call; relock briefly.
            ([slotIndex] {
                std::scoped_lock l(g_mutex);
                return g_slots[slotIndex].body.size();
            })(),
            ([slotIndex] {
                std::scoped_lock l(g_mutex);
                return g_slots[slotIndex].topicTag;
            })());
    }

    void Free(std::size_t slotIndex)
    {
        if (slotIndex >= kPoolSize) {
            logger::error("LetterPool::Free: slotIndex {} out of range", slotIndex);
            return;
        }

        std::scoped_lock lock(g_mutex);
        auto& slot = g_slots[slotIndex];
        const auto priorState = slot.state;

        // Preserve bookFormID (it's a resolved-form pointer to the ESP
        // record, not slot-specific runtime state); clear everything else.
        slot.state           = State::Free;
        slot.body.clear();
        slot.senderLabel.clear();
        slot.senderNpcFormID = 0;
        slot.topicTag.clear();
        slot.deliveredAt     = 0.0;
        slot.readAt          = 0.0;

        logger::debug("LetterPool: slot {} freed (was {})",
                      slotIndex, StateName(priorState));
    }

    void AbortPending(std::size_t slotIndex)
    {
        if (slotIndex >= kPoolSize) {
            logger::error("LetterPool::AbortPending: slotIndex {} out of range", slotIndex);
            return;
        }
        std::scoped_lock lock(g_mutex);
        auto& slot = g_slots[slotIndex];
        const auto priorState = slot.state;
        slot.state           = State::Free;
        slot.body.clear();
        slot.senderLabel.clear();
        slot.senderNpcFormID = 0;
        slot.topicTag.clear();
        slot.deliveredAt     = 0.0;
        slot.readAt          = 0.0;
        logger::info(
            "LetterPool: slot {} aborted (was {}; dispatch never landed in world)",
            slotIndex, StateName(priorState));
    }

    PoolStats GetStats()
    {
        std::scoped_lock lock(g_mutex);
        PoolStats s;
        for (const auto& slot : g_slots) {
            if (slot.bookFormID != 0) {
                ++s.resolved;
            }
            switch (slot.state) {
                case State::Free:            ++s.free;            break;
                case State::PendingDelivery: ++s.pendingDelivery; break;
                case State::InInventory:     ++s.inInventory;     break;
                case State::Read:            ++s.read;            break;
                case State::Discarded:                            break;
            }
        }
        return s;
    }

    // -----------------------------------------------------------------
    // Hook integration
    // -----------------------------------------------------------------

    bool IsManagedForm(RE::FormID formID)
    {
        if (formID == 0) return false;
        std::scoped_lock lock(g_mutex);
        return g_managedForms.contains(formID);
    }

    bool TryGetBody(RE::FormID formID, std::string& outBody)
    {
        if (formID == 0) return false;
        std::scoped_lock lock(g_mutex);
        // Linear scan over 20 slots is fine and avoids maintaining a
        // second formID→slotIndex map.
        for (const auto& slot : g_slots) {
            if (slot.bookFormID == formID) {
                if (slot.body.empty()) return false;
                outBody = slot.body;
                return true;
            }
        }
        return false;
    }

    namespace
    {
        // ---------- TESDescription::GetDescription hook ----------
        //
        // TESObjectBOOK inherits two TESDescription instances — DESC
        // (book body, fieldType 'CSED') and CNAM (item-card tooltip,
        // fieldType 'MANC'). We only substitute on the body read.

        constexpr std::uint32_t kFieldType_DESC = 'CSED';

        using GetDescription_t = void (*)(RE::TESDescription*,
                                          RE::BSString&,
                                          RE::TESForm*,
                                          std::uint32_t);

        GetDescription_t g_origGetDescription = nullptr;

        void HookedGetDescription(RE::TESDescription* self,
                                  RE::BSString&       a_out,
                                  RE::TESForm*        a_parent,
                                  std::uint32_t       a_fieldType)
        {
            // Cheap gates first so non-pool reads (almost every read)
            // fall through to the engine with one hash hit.
            if (a_fieldType == kFieldType_DESC && a_parent &&
                IsManagedForm(a_parent->GetFormID())) {
                std::string body;
                if (TryGetBody(a_parent->GetFormID(), body)) {
                    a_out = body.c_str();
                    return;
                }
                // Pool form with no cached body (slot in Free state
                // before population, or after Free). Fall through so
                // the player sees the ESP placeholder rather than an
                // empty page.
            }
            g_origGetDescription(self, a_out, a_parent, a_fieldType);
        }

        // ---------- BookMenu::OpenBookMenu hook ----------
        //
        // BookMenu doesn't read the rendered body via
        // TESDescription::GetDescription — it has its own path that
        // takes the page text as a function argument. This hook
        // substitutes our cached body for the description argument
        // when the Book FormID is one of ours.

        using OpenBookMenu_t = void(*)(const RE::BSString&,
                                       const RE::ExtraDataList*,
                                       RE::TESObjectREFR*,
                                       RE::TESObjectBOOK*,
                                       const RE::NiPoint3&,
                                       const RE::NiMatrix3&,
                                       float,
                                       bool);

        OpenBookMenu_t g_origOpenBookMenu = nullptr;

        void HookedOpenBookMenu(const RE::BSString&      a_description,
                                const RE::ExtraDataList* a_extraList,
                                RE::TESObjectREFR*       a_ref,
                                RE::TESObjectBOOK*       a_book,
                                const RE::NiPoint3&      a_pos,
                                const RE::NiMatrix3&     a_rot,
                                float                    a_scale,
                                bool                     a_useDefaultPos)
        {
            if (a_book && IsManagedForm(a_book->GetFormID())) {
                // Stamp the slot index so the close-edge sink (Step 7)
                // can fire MarkRead on it. We stamp even when TryGetBody
                // returns false (slot has no cached body) — opening a
                // pool letter is still a "read" lifecycle event from
                // the player's perspective.
                {
                    std::scoped_lock lock(g_mutex);
                    const int slotIdx = FindSlotByFormIDLocked(a_book->GetFormID());
                    if (slotIdx >= 0) {
                        g_currentOpenSlot.store(slotIdx, std::memory_order_relaxed);
                    }
                }

                std::string body;
                if (TryGetBody(a_book->GetFormID(), body)) {
                    RE::BSString substitute = body.c_str();
                    g_origOpenBookMenu(substitute, a_extraList, a_ref, a_book,
                                       a_pos, a_rot, a_scale, a_useDefaultPos);
                    return;
                }
            }
            g_origOpenBookMenu(a_description, a_extraList, a_ref, a_book,
                               a_pos, a_rot, a_scale, a_useDefaultPos);
        }

        // Single helper for both hooks — wraps the MH_Initialize /
        // MH_CreateHook / MH_EnableHook chain with logging. Returns
        // true on success, false on any failure (already logged).
        bool InstallHook(const char*    name,
                         std::uintptr_t targetAddr,
                         void*          detour,
                         void**         origOut)
        {
            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                logger::error(
                    "LetterPool: MH_Initialize failed for {} hook (status={})",
                    name, static_cast<int>(initStatus));
                return false;
            }

            const auto createStatus = MH_CreateHook(
                reinterpret_cast<LPVOID>(targetAddr), detour,
                reinterpret_cast<LPVOID*>(origOut));
            if (createStatus != MH_OK) {
                logger::error(
                    "LetterPool: MH_CreateHook failed for {} (status={})",
                    name, static_cast<int>(createStatus));
                return false;
            }

            const auto enableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(targetAddr));
            if (enableStatus != MH_OK) {
                logger::error(
                    "LetterPool: MH_EnableHook failed for {} (status={})",
                    name, static_cast<int>(enableStatus));
                return false;
            }

            logger::info(
                "LetterPool: {} hook installed (target=0x{:016X}, "
                "hook=0x{:016X}, orig trampoline=0x{:016X})",
                name, targetAddr,
                reinterpret_cast<std::uintptr_t>(detour),
                reinterpret_cast<std::uintptr_t>(*origOut));
            return true;
        }
    }

    void InstallHooks()
    {
        // Idempotent — second call is a no-op.
        bool expected = false;
        if (!g_hooksInstalled.compare_exchange_strong(expected, true)) {
            return;
        }

        // See docs/engine-findings/hooking-engine-functions-with-minhook.md
        // for why we use MinHook here rather than SKSE's trampoline
        // (the latter is a call-site patcher, not a function-entry detour).

        {
            REL::Relocation<std::uintptr_t> target{ RE::Offset::TESDescription::GetDescription };
            InstallHook("TESDescription::GetDescription",
                        target.address(),
                        reinterpret_cast<void*>(&HookedGetDescription),
                        reinterpret_cast<void**>(&g_origGetDescription));
        }
        {
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
            InstallHook("BookMenu::OpenBookMenu",
                        target.address(),
                        reinterpret_cast<void*>(&HookedOpenBookMenu),
                        reinterpret_cast<void**>(&g_origOpenBookMenu));
        }
    }

    // -----------------------------------------------------------------
    // Read detection
    // -----------------------------------------------------------------

    void MarkRead(std::size_t slotIndex)
    {
        if (slotIndex >= kPoolSize) {
            logger::error("LetterPool::MarkRead: slotIndex {} out of range", slotIndex);
            return;
        }
        std::scoped_lock lock(g_mutex);
        auto& slot = g_slots[slotIndex];
        // Only InInventory → Read is the canonical transition. Other
        // states (Free / PendingDelivery / already-Read) can fire here
        // if the player opens the same letter twice in quick succession
        // or opens a letter before delivery completes — neither is a
        // useful event, so we no-op rather than re-stamp readAt.
        if (slot.state != State::InInventory) {
            return;
        }
        slot.state  = State::Read;
        slot.readAt = NowUnixSeconds();
        logger::info("LetterPool: slot {} marked Read", slotIndex);
        // Step 14 layers the player-side SkyrimNet memory write here.
        // Advance the per-slot delivery quest to Stage 40 ("read by
        // player"). The quest stays running until disposal (Stage 50)
        // or allocator eviction (Stage 60).
        NPCLetterAction_QuestControl::AdvanceSlotStage(slotIndex, 40);
    }

    namespace
    {
        struct BookMenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent*                a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*src*/) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                if (a_event->menuName != RE::BookMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (a_event->opening) {
                    // OpenBookMenu hook handles the open edge.
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Closing edge — consume the stamped slot index. The
                // exchange-to-(-1) keeps a stale stamp from firing on
                // the next close.
                const int slotIdx = g_currentOpenSlot.exchange(
                    -1, std::memory_order_acq_rel);
                if (slotIdx < 0) {
                    // Player closed BookMenu for a non-pool book, or
                    // we never saw the open (hook wasn't installed
                    // when the menu opened).
                    return RE::BSEventNotifyControl::kContinue;
                }
                MarkRead(static_cast<std::size_t>(slotIdx));
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        BookMenuCloseSink g_bookMenuCloseSink;
    }

    void RegisterMenuEventSink()
    {
        bool expected = false;
        if (!g_menuSinkRegistered.compare_exchange_strong(expected, true)) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::error("LetterPool::RegisterMenuEventSink: RE::UI singleton unavailable");
            g_menuSinkRegistered.store(false);
            return;
        }
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_bookMenuCloseSink);
        logger::info("LetterPool: MenuOpenCloseEvent sink registered");
    }

    // -----------------------------------------------------------------
    // Container tracking
    // -----------------------------------------------------------------

    namespace
    {
        // The choke point for slot reset. Removes every copy of the
        // slot's book from the player, every loaded actor, and every
        // loaded cell; clears the kHasBeenRead flag on the base form so
        // a future delivery looks "new"; resets slot state to Free.
        // Called both by the allocator (when it needs to evict an
        // older slot) and by the discard sink (when the player
        // sells/gives/drops a letter). Same semantics in both cases:
        // the slot returns to the pool ready for re-allocation, and
        // any lingering REFRs in loaded space are cleaned up.
        //
        // Main-thread only — touches engine APIs (RemoveItem, cell
        // ForEachReference, Disable/SetDelete) that aren't safe to
        // call from sink-side worker threads. Sinks marshal first.
        void EvictSlot(std::size_t slotIndex)
        {
            if (slotIndex >= kPoolSize) return;

            RE::FormID bookFormID = 0;
            {
                std::scoped_lock lock(g_mutex);
                bookFormID = g_slots[slotIndex].bookFormID;
            }
            auto* book = LookupBook(bookFormID);
            if (book) {
                // 1. Sweep the player's inventory.
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->RemoveItem(
                        book,
                        std::numeric_limits<std::int32_t>::max(),
                        RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                }

                // 2. Sweep every loaded actor (high + middle-high +
                // middle-low + low process lists). These are the only
                // actors whose inventories are simulated; unloaded NPCs
                // aren't accessible. The unloaded-follower edge case is
                // documented in PHASE_04_LETTER_POOL_AND_NPC_LETTER_ACTION.md
                // (Slot container tracking) as a known minor cosmetic
                // limit.
                if (auto* pl = RE::ProcessLists::GetSingleton()) {
                    auto sweep = [book](RE::BSTArray<RE::ActorHandle>& list) {
                        for (auto& handle : list) {
                            auto actorPtr = handle.get();
                            if (!actorPtr) continue;
                            auto* actor = actorPtr.get();
                            if (!actor) continue;
                            actor->RemoveItem(
                                book,
                                std::numeric_limits<std::int32_t>::max(),
                                RE::ITEM_REMOVE_REASON::kRemove,
                                nullptr, nullptr);
                        }
                    };
                    sweep(pl->highActorHandles);
                    sweep(pl->middleHighActorHandles);
                    sweep(pl->middleLowActorHandles);
                    sweep(pl->lowActorHandles);
                }

                // 3. Sweep the player's current cell for any world refs
                // (i.e. the letter sitting on the ground after a drop).
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* cell = player->GetParentCell()) {
                        cell->ForEachReference([book](RE::TESObjectREFR* r) {
                            if (r && r->GetBaseObject() == book) {
                                r->Disable();
                                r->SetDelete(true);
                            }
                            return RE::BSContainer::ForEachResult::kContinue;
                        });
                    }
                }

                // 4. Clear kHasBeenRead on the base form so a future
                // delivery doesn't appear pre-read in the inventory list
                // (proven by the smoke test).
                if (book->data.flags.all(RE::OBJ_BOOK::Flag::kHasBeenRead)) {
                    book->data.flags.reset(RE::OBJ_BOOK::Flag::kHasBeenRead);
                }
            }

            // 5. Clear slot fields, reset state to Free.
            std::scoped_lock lock(g_mutex);
            auto& slot = g_slots[slotIndex];
            const auto priorState = slot.state;
            slot.state           = State::Free;
            slot.body.clear();
            slot.senderLabel.clear();
            slot.senderNpcFormID = 0;
            slot.topicTag.clear();
            slot.deliveredAt     = 0.0;
            slot.readAt          = 0.0;
            if (Settings::Get().letterPoolEvictionLogVerbosity >= 1) {
                logger::info(
                    "LetterPool: slot {} recycled (was {})",
                    slotIndex, StateName(priorState));
            }
        }
    }

    void MarkDelivered(std::size_t slotIndex)
    {
        if (slotIndex >= kPoolSize) {
            logger::error("LetterPool::MarkDelivered: slotIndex {} out of range", slotIndex);
            return;
        }
        RE::FormID senderFormID = 0;
        {
            std::scoped_lock lock(g_mutex);
            auto& slot = g_slots[slotIndex];
            if (slot.state != State::PendingDelivery) {
                // Defensive — duplicate container events or moves on a
                // slot that already advanced shouldn't re-trigger
                // delivery memory writes (Step 14 hangs the sender-
                // side memory off this transition).
                return;
            }
            slot.state       = State::InInventory;
            slot.deliveredAt = NowUnixSeconds();  // overwrite the queued-
                                                  // with-courier timestamp
                                                  // set by PopulateSlot.
            senderFormID     = slot.senderNpcFormID;
            logger::info("LetterPool: slot {} marked Delivered (InInventory)", slotIndex);
        }
        // Fire the per-sender cooldown stamp outside our mutex to avoid
        // any risk of lock inversion with NPCLetterAction's own mutex.
        // Step 14 layers the sender-side SkyrimNet memory write here.
        NPCLetterAction_Cooldowns::OnLetterDelivered(senderFormID);
        NPCLetterAction_QuestControl::AdvanceSlotStage(slotIndex, 30);
    }

    void MarkDiscardedToContainer(std::size_t slotIndex, RE::TESObjectREFR* destination)
    {
        if (slotIndex >= kPoolSize) {
            logger::error(
                "LetterPool::MarkDiscardedToContainer: slotIndex {} out of range",
                slotIndex);
            return;
        }
        logger::info(
            "LetterPool: slot {} discarded to container (destFormID=0x{:08X})",
            slotIndex,
            destination ? destination->GetFormID() : 0u);

        // Delete the letter from the destination container BEFORE
        // EvictSlot clears slot state. EvictSlot's sweep only walks
        // actors (via ProcessLists) and the current cell, which misses
        // merchant chests (container REFRs, not actors) — the exact
        // path a sold letter takes. The destination REFR is already
        // handed to us here; do the removal directly.
        //
        // kRemove with a null moveToRef despawns the item outright,
        // matching the player-inventory sweep pattern in EvictSlot.
        RE::FormID bookFormID = 0;
        {
            std::scoped_lock lock(g_mutex);
            bookFormID = g_slots[slotIndex].bookFormID;
        }
        if (destination && bookFormID != 0) {
            auto* book = LookupBook(bookFormID);
            if (book) {
                destination->RemoveItem(
                    book,
                    std::numeric_limits<std::int32_t>::max(),
                    RE::ITEM_REMOVE_REASON::kRemove,
                    nullptr, nullptr);
            }
        }

        // Advance the per-slot delivery quest to Stage 50 ("disposed by
        // player"). The Stage 50 fragment routes to Stage 200, which
        // runs Shutdown() (Stop + Reset). Must fire before EvictSlot
        // wipes state so the quest advances against a still-populated
        // slot; the terminal shutdown races EvictSlot but the two are
        // independent (quest lifecycle vs. slot lifecycle).
        NPCLetterAction_QuestControl::AdvanceSlotStage(slotIndex, 50);
        EvictSlot(slotIndex);
    }

    void MarkDroppedToCell(std::size_t slotIndex, RE::TESObjectREFR* worldRef)
    {
        if (slotIndex >= kPoolSize) {
            logger::error(
                "LetterPool::MarkDroppedToCell: slotIndex {} out of range",
                slotIndex);
            return;
        }
        logger::info(
            "LetterPool: slot {} dropped to cell (worldRefFormID=0x{:08X})",
            slotIndex,
            worldRef ? worldRef->GetFormID() : 0u);
        NPCLetterAction_QuestControl::AdvanceSlotStage(slotIndex, 50);
        EvictSlot(slotIndex);
    }

    namespace
    {
        // TESContainerChangedEvent fires for every container move in
        // loaded space. Filter on baseObj being one of our pool forms,
        // then marshal to the main thread (sinks can run off-thread on
        // some events) before mutating pool state or touching engine APIs.
        struct ContainerChangedSink :
            public RE::BSTEventSink<RE::TESContainerChangedEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESContainerChangedEvent*                a_event,
                RE::BSTEventSource<RE::TESContainerChangedEvent>* /*src*/) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;

                if (!IsManagedForm(a_event->baseObj)) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                int slotIdx = -1;
                {
                    std::scoped_lock lock(g_mutex);
                    slotIdx = FindSlotByFormIDLocked(a_event->baseObj);
                }
                if (slotIdx < 0) return RE::BSEventNotifyControl::kContinue;

                const auto oldContainer = a_event->oldContainer;
                const auto newContainer = a_event->newContainer;
                const auto baseObj      = a_event->baseObj;
                const auto playerID     =
                    RE::PlayerCharacter::GetSingleton()
                        ? RE::PlayerCharacter::GetSingleton()->GetFormID()
                        : 0u;

                AsyncDispatch::MarshalToMainThread(
                    [slotIdx, oldContainer, newContainer, baseObj, playerID] {
                        // Any → player: courier delivery (or any other
                        // arrival into the player's inventory). The
                        // MarkDelivered guard no-ops if the slot isn't
                        // PendingDelivery, so other transitions are safe.
                        if (newContainer == playerID && oldContainer != playerID) {
                            MarkDelivered(static_cast<std::size_t>(slotIdx));
                            return;
                        }

                        // Player → other container: discard (sold,
                        // given, transferred).
                        if (oldContainer == playerID && newContainer != 0 &&
                            newContainer != playerID) {
                            auto* form = RE::TESForm::LookupByID(newContainer);
                            auto* ref  = form ? form->AsReference() : nullptr;
                            MarkDiscardedToContainer(
                                static_cast<std::size_t>(slotIdx), ref);
                            return;
                        }

                        // Player → world (drop). newContainer == 0;
                        // look up the dropped REFR in the player's
                        // current cell so the recycle can disable it.
                        if (oldContainer == playerID && newContainer == 0) {
                            RE::TESObjectREFR* worldRef = nullptr;
                            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                                if (auto* cell = player->GetParentCell()) {
                                    cell->ForEachReference(
                                        [&](RE::TESObjectREFR* r) {
                                            if (!worldRef && r &&
                                                r->GetBaseObject() &&
                                                r->GetBaseObject()->GetFormID() == baseObj) {
                                                worldRef = r;
                                                return RE::BSContainer::ForEachResult::kStop;
                                            }
                                            return RE::BSContainer::ForEachResult::kContinue;
                                        });
                                }
                            }
                            MarkDroppedToCell(
                                static_cast<std::size_t>(slotIdx), worldRef);
                            return;
                        }
                        // Other transitions (NPC↔NPC, world→world) —
                        // ignore.
                    });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        ContainerChangedSink g_containerSink;
    }

    void RegisterContainerEventSink()
    {
        bool expected = false;
        if (!g_containerSinkRegistered.compare_exchange_strong(expected, true)) {
            return;
        }
        auto* src = RE::ScriptEventSourceHolder::GetSingleton();
        if (!src) {
            logger::error(
                "LetterPool::RegisterContainerEventSink: ScriptEventSourceHolder unavailable");
            g_containerSinkRegistered.store(false);
            return;
        }
        src->AddEventSink<RE::TESContainerChangedEvent>(&g_containerSink);
        logger::info("LetterPool: TESContainerChangedEvent sink registered");
    }

    // -----------------------------------------------------------------
    // Co-save persistence
    // -----------------------------------------------------------------

    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
        {
            const auto len = static_cast<std::uint16_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0) intfc->WriteRecordData(s.data(), len);
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint16_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len)) return false;
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len) return false;
            return true;
        }

        // Demote a slot to Free without running any reference cleanup —
        // used by the on-load reconciliation pass when we detect the
        // slot's persisted state can't be honored anymore. The full
        // recycle scan introduced in Step 9 isn't safe to call here
        // because OnLoad fires before the world fully streams in.
        void DemoteToFreeLocked(std::size_t i, std::string_view reason)
        {
            auto& slot = g_slots[i];
            const auto priorState = slot.state;
            slot.state           = State::Free;
            slot.body.clear();
            slot.senderLabel.clear();
            slot.senderNpcFormID = 0;
            slot.topicTag.clear();
            slot.deliveredAt     = 0.0;
            slot.readAt          = 0.0;
            logger::warn(
                "LetterPool::OnLoad: slot {} demoted to Free (was {}; reason: {})",
                i, StateName(priorState), reason);
        }
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) return;
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("LetterPool::OnSave: OpenRecord failed");
            return;
        }

        // Snapshot under the lock; emit without the lock to keep the
        // mutex window short.
        std::array<Slot, kPoolSize> snapshot;
        {
            std::scoped_lock lock(g_mutex);
            snapshot = g_slots;
        }

        const auto slotCount = static_cast<std::uint32_t>(kPoolSize);
        intfc->WriteRecordData(slotCount);
        for (const auto& slot : snapshot) {
            const auto stateByte = static_cast<std::uint8_t>(slot.state);
            intfc->WriteRecordData(stateByte);
            intfc->WriteRecordData(slot.bookFormID);
            intfc->WriteRecordData(slot.senderNpcFormID);
            intfc->WriteRecordData(slot.deliveredAt);
            intfc->WriteRecordData(slot.readAt);
            WriteString(intfc, slot.body);
            WriteString(intfc, slot.senderLabel);
            WriteString(intfc, slot.topicTag);
        }
    }

    void OnLoad(SKSE::SerializationInterface* intfc,
                std::uint32_t                 version,
                std::uint32_t                 length)
    {
        if (!intfc) return;
        if (version != kRecordVersion) {
            logger::warn(
                "LetterPool::OnLoad: unknown version {} (length={}); clearing pool",
                version, length);
            OnRevert();
            return;
        }

        std::uint32_t slotCount = 0;
        if (intfc->ReadRecordData(slotCount) != sizeof(slotCount)) {
            logger::error("LetterPool::OnLoad: failed to read slot_count");
            OnRevert();
            return;
        }
        if (slotCount != kPoolSize) {
            logger::warn(
                "LetterPool::OnLoad: slot_count {} != current kPoolSize {}; clearing pool "
                "(co-save predates a pool-size change)",
                slotCount, kPoolSize);
            OnRevert();
            return;
        }

        std::array<Slot, kPoolSize> loaded;
        for (std::size_t i = 0; i < kPoolSize; ++i) {
            Slot& s = loaded[i];
            std::uint8_t stateByte = 0;
            if (intfc->ReadRecordData(stateByte)        != sizeof(stateByte) ||
                intfc->ReadRecordData(s.bookFormID)     != sizeof(s.bookFormID) ||
                intfc->ReadRecordData(s.senderNpcFormID)!= sizeof(s.senderNpcFormID) ||
                intfc->ReadRecordData(s.deliveredAt)    != sizeof(s.deliveredAt) ||
                intfc->ReadRecordData(s.readAt)         != sizeof(s.readAt)) {
                logger::error("LetterPool::OnLoad: short read on slot {} header", i);
                OnRevert();
                return;
            }
            s.state = static_cast<State>(stateByte);

            if (!ReadString(intfc, s.body) ||
                !ReadString(intfc, s.senderLabel) ||
                !ReadString(intfc, s.topicTag)) {
                logger::error("LetterPool::OnLoad: short read on slot {} strings", i);
                OnRevert();
                return;
            }

            // Resolve persisted FormIDs through SKSE's mapping table so
            // load-order changes between save and load don't corrupt
            // the references.
            RE::FormID resolvedBook = 0;
            if (s.bookFormID != 0) {
                if (!intfc->ResolveFormID(s.bookFormID, resolvedBook)) {
                    resolvedBook = 0;
                }
            }
            s.bookFormID = resolvedBook;

            RE::FormID resolvedSender = 0;
            if (s.senderNpcFormID != 0) {
                if (!intfc->ResolveFormID(s.senderNpcFormID, resolvedSender)) {
                    resolvedSender = 0;
                }
            }
            s.senderNpcFormID = resolvedSender;
        }

        // Commit the loaded snapshot, then run the reconciliation pass.
        std::size_t demoted = 0;
        {
            std::scoped_lock lock(g_mutex);
            g_slots = loaded;

            for (std::size_t i = 0; i < kPoolSize; ++i) {
                auto& slot = g_slots[i];

                // If the Book form itself failed to resolve, no slot
                // state is meaningful; force Free.
                if (slot.bookFormID == 0 && slot.state != State::Free) {
                    DemoteToFreeLocked(i, "bookFormID failed to resolve");
                    ++demoted;
                    continue;
                }

                switch (slot.state) {
                    case State::Free:
                        // Nothing to reconcile.
                        break;

                    case State::PendingDelivery: {
                        const auto timeoutSeconds = static_cast<double>(
                            Settings::Get().letterPendingDeliveryTimeoutSeconds);
                        if (NowUnixSeconds() - slot.deliveredAt > timeoutSeconds) {
                            DemoteToFreeLocked(i, "PendingDelivery aged past timeout");
                            ++demoted;
                        }
                        break;
                    }

                    case State::InInventory:
                    case State::Read: {
                        // Verify the player still actually holds the
                        // letter. If not (console removal, save editor,
                        // external mod), demote.
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        auto* book = LookupBook(slot.bookFormID);
                        std::int32_t count = 0;
                        if (player && book) {
                            count = player->GetItemCount(book);
                        }
                        if (count <= 0) {
                            DemoteToFreeLocked(i,
                                slot.state == State::InInventory
                                    ? "InInventory but player has 0 copies"
                                    : "Read but player has 0 copies");
                            ++demoted;
                        }
                        break;
                    }

                    case State::Discarded:
                        // Should never persist (transitions to Free
                        // synchronously in Step 8). Treat as stale.
                        DemoteToFreeLocked(i, "Discarded shouldn't persist");
                        ++demoted;
                        break;
                }
            }
        }

        logger::info(
            "LetterPool::OnLoad: restored {} slot(s) ({} demoted)",
            kPoolSize, demoted);
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_mutex);
        for (auto& slot : g_slots) {
            // Keep bookFormID — it's resolved at kDataLoaded from the
            // ESP and is independent of save-game state. Everything
            // else resets.
            slot.state           = State::Free;
            slot.body.clear();
            slot.senderLabel.clear();
            slot.senderNpcFormID = 0;
            slot.topicTag.clear();
            slot.deliveredAt     = 0.0;
            slot.readAt          = 0.0;
        }
    }
}
