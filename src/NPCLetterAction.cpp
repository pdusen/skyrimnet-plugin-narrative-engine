#include <NPCLetterAction.h>

#include <ActionDispatcher.h>
#include <AsyncDispatch.h>
#include <LetterComposer.h>
#include <LetterPool.h>
#include <LocationKeywords.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <RE/B/BSFixedString.h>
#include <RE/F/FunctionArguments.h>
#include <RE/V/VirtualMachine.h>

#include <atomic>
#include <mutex>
#include <string_view>

namespace NarrativeEngine
{
    using namespace std::string_view_literals;

    namespace
    {
        // Vanilla WICourier quest EditorID — Bethesda-side, doesn't
        // change. Looked up lazily on first need; cached for the
        // session.
        constexpr const char* kWICourierEditorID         = "WICourier";

        // Where `WICourierScript.AddItemToContainer(item, count)` writes
        // items. Two resolution paths, tried in order:
        //
        //   1. The "Container" reference alias on the WICourier quest.
        //      Pinned in vanilla to `WICourierContainerRef`, but
        //      mod-authors who replace the courier system politely tend
        //      to repoint this alias at their own container — so honoring
        //      it gives compatibility with the "extends through alias"
        //      modding pattern.
        //
        //   2. Direct EditorID lookup of `WICourierContainerRef` (vanilla
        //      FormID 0x00039FB9, backing CONT 0x00039FB8). Used as a
        //      fallback if a mod renames or removes the alias.
        //
        // Caveat: `WICourierScript`'s `pCourierContainer` property is
        // pinned directly to the REFR (not the alias) in vanilla. If a
        // mod repoints the alias but leaves the script alone, items
        // go to the script's REFR while we verify against the modded
        // alias and every dispatch falsely rolls back. There's no
        // C++-side way to introspect the script's property to detect
        // this, so we accept it as a known edge case and log which
        // path resolved so a misconfigured install is at least
        // diagnosable.
        constexpr const char* kCourierContainerAliasName = "Container";
        constexpr const char* kCourierContainerEditorID  = "WICourierContainerRef";

        // In-flight bookkeeping. Only one NPCLetterAction can be in
        // flight at a time (the dispatcher single-flights actions), so
        // a single slot index and a single Book FormID suffice. -1 /
        // 0 = no in-flight.
        std::mutex   g_inFlightMutex;
        int          g_inFlightSlot       = -1;
        RE::FormID   g_inFlightBookFormID = 0;
        double       g_lastVerifyLogTime  = 0.0;  // throttle the per-poll log line

        // Lazily-resolved engine pointers. Cached behind a flag so we
        // try once and remember the result (success or failure).
        // g_courierContainerAlias is preferred when available — it
        // re-resolves on each call (the alias's reference can be
        // refilled by the engine across save/load), so we cache the
        // alias pointer rather than the reference it points at.
        // g_courierContainerRefFallback is the direct REFR we
        // looked up at init as a backup if the alias is missing.
        std::atomic<bool>   g_courierResolved              = false;
        RE::TESQuest*       g_courierQuest                 = nullptr;
        RE::BGSBaseAlias*   g_courierContainerAlias        = nullptr;
        RE::TESObjectREFR*  g_courierContainerRefFallback  = nullptr;

        RE::TESQuest* ResolveCourierQuest()
        {
            if (g_courierResolved.load(std::memory_order_acquire)) {
                return g_courierQuest;
            }
            auto* form = RE::TESForm::LookupByEditorID(kWICourierEditorID);
            RE::TESQuest* quest = form ? form->As<RE::TESQuest>() : nullptr;
            if (!quest) {
                logger::error(
                    "NPCLetterAction: vanilla WICourier quest did not resolve "
                    "(LookupByEditorID '{}' failed); action permanently disabled",
                    kWICourierEditorID);
            } else {
                // Preferred: the "Container" alias on the quest.
                for (auto* alias : quest->aliases) {
                    if (alias && alias->aliasName == kCourierContainerAliasName) {
                        g_courierContainerAlias = alias;
                        break;
                    }
                }

                // Fallback: direct REFR lookup by EditorID. Used if the
                // alias isn't present (modded quest that renamed or
                // dropped it).
                if (auto* containerForm =
                        RE::TESForm::LookupByEditorID(kCourierContainerEditorID)) {
                    g_courierContainerRefFallback = containerForm->AsReference();
                }

                logger::info(
                    "NPCLetterAction: WICourier resolved (formID=0x{:08X}, "
                    "isRunning={}, stage={}); alias '{}' = {}, "
                    "fallback REFR '{}' = {}",
                    quest->GetFormID(),
                    quest->IsRunning(),
                    quest->GetCurrentStageID(),
                    kCourierContainerAliasName,
                    g_courierContainerAlias ? "found" : "MISSING",
                    kCourierContainerEditorID,
                    g_courierContainerRefFallback
                        ? fmt::format("0x{:08X}",
                                      g_courierContainerRefFallback->GetFormID())
                        : std::string{"NOT FOUND"});

                if (!g_courierContainerAlias && !g_courierContainerRefFallback) {
                    logger::warn(
                        "NPCLetterAction: neither the '{}' alias nor a '{}' "
                        "REFR resolved; verification polls will fail-soft and "
                        "every dispatch will roll back. Confirm vanilla "
                        "Skyrim.esm is loaded and no mod has renamed both the "
                        "alias and the staging container.",
                        kCourierContainerAliasName, kCourierContainerEditorID);
                }
            }
            g_courierQuest = quest;
            g_courierResolved.store(true, std::memory_order_release);
            return quest;
        }

        // The container reference WICourierScript.AddItemToContainer
        // writes to. Re-resolves the alias on every call so that an
        // alias the engine re-filled across save/load is followed
        // correctly; falls back to the EditorID REFR if the alias
        // isn't present at all.
        RE::TESObjectREFR* GetCourierContainerRef()
        {
            if (g_courierContainerAlias) {
                if (auto* refAlias =
                        skyrim_cast<RE::BGSRefAlias*>(g_courierContainerAlias)) {
                    if (auto* r = refAlias->GetReference()) {
                        return r;
                    }
                }
            }
            return g_courierContainerRefFallback;
        }

        std::int32_t CourierInventoryCount(RE::FormID bookFormID)
        {
            if (bookFormID == 0) return 0;
            auto* containerRef = GetCourierContainerRef();
            if (!containerRef) return 0;
            auto* bookForm = RE::TESForm::LookupByID(bookFormID);
            auto* book = bookForm ? bookForm->As<RE::TESBoundObject>() : nullptr;
            if (!book) return 0;
            auto* changes = containerRef->GetInventoryChanges();
            if (!changes) return 0;
            return static_cast<std::int32_t>(changes->GetItemCount(book));
        }

        // VM-dispatch into vanilla WICourierScript.AddItemToContainer.
        // Fire-and-forget; success is verified later by the polls.
        // Returns true if the dispatch was queued (engine-side), false
        // if the VM / handle resolution failed up-front.
        bool VMDispatchAddItemToContainer(RE::TESQuest*      /*courierQuest*/,
                                          RE::TESObjectBOOK* book,
                                          RE::FormID         senderNpcFormID)
        {
            // TODO(phase 4 step 13 followup): spawn an instance of the
            // pool book in the sender NPC's inventory using the CK
            // Wiki courier pattern (reference alias with
            // "Create Reference to Object"), retrieve the spawned
            // REFR from the alias, and pass that REFR to
            // WICourierScript.AddItemToContainer via VM dispatch.
            //
            // See: https://ck.uesp.net/wiki/Using_the_Vanilla_Courier
            //
            // Previous attempts that did NOT work and why:
            //   - Passing the base book Form: engine's AddItem merge
            //     path crashed (EXCEPTION_ACCESS_VIOLATION inside
            //     inventory iteration, 0xCC poison pointer).
            //   - PlaceObjectAtMe + pass REFR: same crash signature
            //     in the inventory move path.
            //   - C++ AddObjectToContainer with nullptr extras: creates
            //     an inventory entry with no extra-data lists at all,
            //     so no ExtraReferenceHandle to retrieve a per-instance
            //     REFR from.
            //   - TESContainerChangedEvent capture: event fires with
            //     empty `reference` handle for nullptr-extras adds.
            //
            // The correct path appears to require a quest with a
            // reference alias configured for "Create Reference to
            // Object" pointing at the pool book, with "Create In" set
            // to the sender NPC. Setting that up dynamically per
            // dispatch is what needs designing.
            //
            // For now: stub returns false so the action's
            // !queued path runs immediately — slot rolls back via
            // AbortPending, no cooldown applied, no engine state
            // mutated.
            logger::warn(
                "NPCLetterAction: VMDispatchAddItemToContainer is currently "
                "a stub (book=0x{:08X}, sender=0x{:08X}); returning false so "
                "the dispatcher rolls back cleanly. See TODO in source.",
                book ? book->GetFormID() : 0u,
                senderNpcFormID);
            return false;
        }
    }

    std::string NPCLetterAction::Name() const
    {
        return "npc_letter";
    }

    std::string NPCLetterAction::Description() const
    {
        return
            "An NPC who knows the player character — picked from the player's recent "
            "interaction history — sends a personal letter via the vanilla "
            "courier system. Tone and polarity are driven by the generated "
            "content: warm thank-yous, mournful condolences, businesslike "
            "follow-ups, and urgent or menacing demands are all in scope, so "
            "this action can serve either a raising or lowering direction "
            "depending on what the situation calls for. The courier reaches "
            "the player anywhere a vanilla courier can — exteriors, "
            "settlements, inns, player homes — so this is a good catch-all "
            "for non-dangerous space. Avoid when the player is in a "
            "dungeon, lair, or other hostile area (the courier can't get "
            "there anyway, and a letter beat doesn't fit those moments).\n"
            "\n"
            "Parameters: this action accepts EXACTLY ONE optional "
            "parameter, `urgency_hint`, a string of `low` / `medium` / "
            "`high`. Defaults to `medium`. Do NOT include any other "
            "parameter fields — sender NPC, letter body, tone, mood, "
            "topic, and recipient are all decided by the action's own "
            "internal pipeline using the player's recent SkyrimNet "
            "engagement history. Any other fields you put in "
            "`parameters` will be silently ignored and won't influence "
            "the letter that gets sent.";
    }

    ActionPolarity NPCLetterAction::Polarity() const
    {
        return ActionPolarity::Either;
    }

    bool NPCLetterAction::IsAvailable(const ActionContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("NPCLetterAction::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        // Hard fail if WICourier never resolved (very broken install).
        if (!ResolveCourierQuest()) {
            return blocked("WICourier quest not resolved");
        }

        // Location must not be flagged dangerous (dungeon / lair / camp).
        if (ctx.player) {
            if (LocationKeywords::IsDangerous(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsDangerous");
            }
        }

        if (!SkyrimNetAPI::IsMemorySystemReady()) {
            return blocked("SkyrimNet memory system not ready");
        }

        const int minCandidates = Settings::Get().letterMinSenderCandidates;
        const auto enrolledJson = SkyrimNetAPI::GetActorEngagement(
            /*maxCount=*/5,
            /*excludePlayer=*/true,
            /*playerEventsOnly=*/false,
            /*shortWindowSeconds=*/86400.0,
            /*mediumWindowSeconds=*/604800.0);
        auto enrolled = nlohmann::json::parse(enrolledJson, nullptr, false);
        if (!enrolled.is_array()) {
            return blocked("GetActorEngagement returned non-array");
        }
        if (static_cast<int>(enrolled.size()) < minCandidates) {
            if (debug) {
                logger::debug(
                    "NPCLetterAction::IsAvailable: blocked (only {} candidates, "
                    "need {})", enrolled.size(), minCandidates);
            }
            return false;
        }

        return true;
    }

    StartResult NPCLetterAction::Start(const ActionContext&  ctx,
                                       const nlohmann::json& parameters)
    {
        // 1. Allocate a pool slot up-front. If this fails the dispatcher
        // gets an immediate started=false with no LLM round-trip cost.
        auto alloc = LetterPool::Allocate();
        if (!alloc) {
            const char* reason =
                (alloc.error() == LetterPool::AllocationFailure::PoolNotResolved)
                    ? "letter pool not resolved (Initialize never ran or all forms failed)"
                    : "letter pool exhausted (all slots PendingDelivery)";
            logger::warn("NPCLetterAction::Start: {}", reason);
            return StartResult{ .started = false, .detail = reason };
        }

        const std::size_t slotIndex  = alloc->slotIndex;
        const RE::FormID  bookFormID = alloc->bookFormID;

        // Stash in-flight bookkeeping. The verification polls
        // (DetectAndRollbackFailedStart / DetectCompletion) read this
        // to know which book to check the courier's inventory for.
        {
            std::scoped_lock lock(g_inFlightMutex);
            g_inFlightSlot       = static_cast<int>(slotIndex);
            g_inFlightBookFormID = bookFormID;
            g_lastVerifyLogTime  = 0.0;
        }

        // 2. Decode urgency_hint from parameters (low/medium/high).
        // Defaults to medium when missing or out of range — the action's
        // own LLM call gets the final say on emotional weight via the
        // generated mood.
        LetterComposer::UrgencyHint urgency = LetterComposer::UrgencyHint::Medium;
        if (parameters.is_object()) {
            if (auto it = parameters.find("urgency_hint");
                it != parameters.end() && it->is_string()) {
                const auto v = it->get<std::string>();
                if      (v == "low")  urgency = LetterComposer::UrgencyHint::Low;
                else if (v == "high") urgency = LetterComposer::UrgencyHint::High;
            }
        }

        // 3. Fire the async content-LLM call. The callback fires on a
        // SkyrimNet worker thread; we marshal back to main before
        // touching engine state. While composing, the action is
        // formally in-flight from the dispatcher's perspective.
        const std::size_t slotForLambda = slotIndex;
        const RE::FormID  bookForLambda = bookFormID;
        LetterComposer::Compose(
            ctx, urgency,
            [slotForLambda, bookForLambda]
            (std::optional<LetterComposer::LetterComposition> comp) {
                AsyncDispatch::MarshalToMainThread(
                    [slotForLambda, bookForLambda, comp = std::move(comp)]() mutable {
                        if (!comp) {
                            // LLM failure / validation failure. Release
                            // the slot and unwind the dispatcher's
                            // in-flight state via the public C++
                            // CompleteAction so cooldown begins (we did
                            // burn an action-select tick).
                            logger::warn(
                                "NPCLetterAction: LetterComposer returned no composition; "
                                "releasing slot {} and completing action with no dispatch",
                                slotForLambda);
                            LetterPool::Free(slotForLambda);
                            {
                                std::scoped_lock lock(g_inFlightMutex);
                                g_inFlightSlot       = -1;
                                g_inFlightBookFormID = 0;
                            }
                            ActionDispatcher::CompleteAction("npc_letter");
                            return;
                        }

                        // Populate the slot with the generated content.
                        // The hooks now substitute the body when the
                        // engine reads it; SetFullName updates the
                        // inventory title.
                        LetterPool::PopulateSlot(
                            slotForLambda,
                            comp->senderLabel,
                            comp->body,
                            comp->senderNpcFormID,
                            comp->topicTag);

                        // VM-dispatch to vanilla
                        // WICourierScript.AddItemToContainer. The
                        // dispatch is fire-and-forget; verification
                        // happens via the two IAction polls.
                        auto* courierQuest = ResolveCourierQuest();
                        auto* bookForm = RE::TESForm::LookupByID(bookForLambda);
                        auto* book = bookForm ? bookForm->As<RE::TESObjectBOOK>() : nullptr;

                        const bool queued =
                            VMDispatchAddItemToContainer(
                                courierQuest, book, comp->senderNpcFormID);
                        // Note: the actual VM call passes the spawned
                        // REFR (logged inside the dispatch function),
                        // not the base book form. This line only
                        // reports the queue result; the REFR's FormID
                        // is in the "spawned book instance REFR ..."
                        // line just above.
                        logger::info(
                            "NPCLetterAction: WICourierScript.AddItemToContainer "
                            "dispatch queued={}",
                            queued ? "true" : "false");

                        if (!queued) {
                            // Dispatch refused to even queue (VM down,
                            // handle policy missing, etc.). Roll back
                            // without waiting for the verification
                            // window — we know the letter will never
                            // appear.
                            LetterPool::AbortPending(slotForLambda);
                            {
                                std::scoped_lock lock(g_inFlightMutex);
                                g_inFlightSlot       = -1;
                                g_inFlightBookFormID = 0;
                            }
                            ActionDispatcher::CompleteAction("npc_letter");
                            return;
                        }
                        // Otherwise: dispatcher's verification polls
                        // will see the letter appear (DetectCompletion)
                        // or fail to appear (DetectAndRollbackFailedStart).
                    });
            });

        return StartResult{ .started = true, .detail = "composing letter" };
    }

    bool NPCLetterAction::DetectAndRollbackFailedStart(
        const ActionContext& /*ctx*/, double secondsSinceStart)
    {
        const double verifyDelay = static_cast<double>(
            Settings::Get().letterDispatchVerifyDelaySeconds);
        if (secondsSinceStart < verifyDelay) {
            return false;  // too early to give up
        }

        RE::FormID bookFormID = 0;
        int        slotIndex  = -1;
        {
            std::scoped_lock lock(g_inFlightMutex);
            bookFormID = g_inFlightBookFormID;
            slotIndex  = g_inFlightSlot;
        }
        if (bookFormID == 0 || slotIndex < 0) {
            // No in-flight tracking (LLM still composing, or already
            // cleaned up). Not a rollback case.
            return false;
        }

        const auto count = CourierInventoryCount(bookFormID);
        if (Settings::Get().debugMode) {
            logger::debug(
                "NPCLetterAction: verify@{:.1f}s — courier has {} copies of 0x{:08X}",
                secondsSinceStart, count, bookFormID);
        }

        if (count > 0) {
            // Letter landed; let DetectCompletion handle it.
            return false;
        }

        // Past the verification window and the courier never received
        // the letter. Roll back: free the slot, clear bookkeeping,
        // return true so the dispatcher unwinds in-flight without
        // applying a cooldown.
        logger::warn(
            "NPCLetterAction: dispatch verification failed (courier had 0 copies "
            "after {:.1f}s window); rolling back slot {}",
            secondsSinceStart, slotIndex);
        LetterPool::AbortPending(static_cast<std::size_t>(slotIndex));
        {
            std::scoped_lock lock(g_inFlightMutex);
            g_inFlightSlot       = -1;
            g_inFlightBookFormID = 0;
        }
        return true;
    }

    bool NPCLetterAction::DetectCompletion(const ActionContext& /*ctx*/,
                                           double                secondsSinceStart)
    {
        RE::FormID bookFormID = 0;
        {
            std::scoped_lock lock(g_inFlightMutex);
            bookFormID = g_inFlightBookFormID;
        }
        if (bookFormID == 0) return false;

        const auto count = CourierInventoryCount(bookFormID);
        if (count <= 0) return false;

        logger::info(
            "NPCLetterAction: DetectCompletion — courier has letter 0x{:08X} "
            "after {:.1f}s; action complete (slot stays PendingDelivery until "
            "courier hands off to player)",
            bookFormID, secondsSinceStart);

        // Clear in-flight bookkeeping but leave the slot in
        // PendingDelivery — physical delivery happens on the vanilla
        // courier's timeline (potentially minutes), and the
        // TESContainerChangedEvent sink will transition the slot to
        // InInventory when the courier hands off to the player.
        {
            std::scoped_lock lock(g_inFlightMutex);
            g_inFlightSlot       = -1;
            g_inFlightBookFormID = 0;
        }
        return true;
    }
}
