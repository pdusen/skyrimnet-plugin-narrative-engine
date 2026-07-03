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

#include <RE/A/Actor.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/F/FunctionArguments.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESFaction.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>
#include <RE/V/VirtualMachine.h>

#include <RE/C/Calendar.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace NarrativeEngine
{
    using namespace std::string_view_literals;

    namespace
    {
        // Vanilla WICourier quest EditorID — Bethesda-side, doesn't
        // change. Looked up lazily on first need; cached for the
        // session.
        constexpr const char *kWICourierEditorID = "WICourier";

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
        constexpr const char *kCourierContainerAliasName = "Container";
        constexpr const char *kCourierContainerEditorID = "WICourierContainerRef";

        // In-flight bookkeeping. Only one NPCLetterAction can be in
        // flight at a time (the dispatcher single-flights actions), so
        // a single slot index and a single Book FormID suffice. -1 /
        // 0 = no in-flight.
        std::mutex g_inFlightMutex;
        int g_inFlightSlot = -1;
        RE::FormID g_inFlightBookFormID = 0;
        double g_lastVerifyLogTime = 0.0; // throttle the per-poll log line

        // Lazily-resolved engine pointers. Cached behind a flag so we
        // try once and remember the result (success or failure).
        // g_courierContainerAlias is preferred when available — it
        // re-resolves on each call (the alias's reference can be
        // refilled by the engine across save/load), so we cache the
        // alias pointer rather than the reference it points at.
        // g_courierContainerRefFallback is the direct REFR we
        // looked up at init as a backup if the alias is missing.
        std::atomic<bool> g_courierResolved = false;
        RE::TESQuest *g_courierQuest = nullptr;
        RE::BGSBaseAlias *g_courierContainerAlias = nullptr;
        RE::TESObjectREFR *g_courierContainerRefFallback = nullptr;

        RE::TESQuest *ResolveCourierQuest()
        {
            if (g_courierResolved.load(std::memory_order_acquire))
            {
                return g_courierQuest;
            }
            auto *form = RE::TESForm::LookupByEditorID(kWICourierEditorID);
            RE::TESQuest *quest = form ? form->As<RE::TESQuest>() : nullptr;
            if (!quest)
            {
                logger::error(
                    "NPCLetterAction: vanilla WICourier quest did not resolve "
                    "(LookupByEditorID '{}' failed); action permanently disabled",
                    kWICourierEditorID);
            }
            else
            {
                // Preferred: the "Container" alias on the quest.
                for (auto *alias : quest->aliases)
                {
                    if (alias && alias->aliasName == kCourierContainerAliasName)
                    {
                        g_courierContainerAlias = alias;
                        break;
                    }
                }

                // Fallback: direct REFR lookup by EditorID. Used if the
                // alias isn't present (modded quest that renamed or
                // dropped it).
                if (auto *containerForm =
                        RE::TESForm::LookupByEditorID(kCourierContainerEditorID))
                {
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

                if (!g_courierContainerAlias && !g_courierContainerRefFallback)
                {
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
        RE::TESObjectREFR *GetCourierContainerRef()
        {
            if (g_courierContainerAlias)
            {
                if (auto *refAlias =
                        skyrim_cast<RE::BGSRefAlias *>(g_courierContainerAlias))
                {
                    if (auto *r = refAlias->GetReference())
                    {
                        return r;
                    }
                }
            }
            return g_courierContainerRefFallback;
        }

        std::int32_t CourierInventoryCount(RE::FormID bookFormID)
        {
            if (bookFormID == 0)
                return 0;
            auto *containerRef = GetCourierContainerRef();
            if (!containerRef)
                return 0;
            auto *bookForm = RE::TESForm::LookupByID(bookFormID);
            auto *book = bookForm ? bookForm->As<RE::TESBoundObject>() : nullptr;
            if (!book)
                return 0;

            // Use TESObjectREFR::GetInventoryCounts() (absolute counts =
            // base CONT contents + InventoryChanges delta). The earlier
            // `InventoryChanges::GetItemCount` returned only the delta,
            // which for a stock CONT-backed container like
            // WICourierContainerRef can be wildly negative (we saw
            // -15360 on the first dispatch test) when the engine has
            // done internal accounting against the base list. The
            // absolute count is the only thing we can sensibly compare
            // against a "letter present?" threshold.
            //
            // GetInventoryCounts is filterable; passing a predicate that
            // accepts only our specific book avoids the cost of iterating
            // every CONT entry on every poll.
            const auto counts = containerRef->GetInventoryCounts(
                [book](RE::TESBoundObject &obj)
                { return &obj == book; });
            auto it = counts.find(book);
            return it != counts.end() ? it->second : 0;
        }

        // --- Per-slot delivery quest cache (Step 15) -----------------
        //
        // 20 entries, parallel to LetterPool's slot array. Each entry
        // is the `_ne_PooledLetterQuestNN` quest pointer (resolved at
        // kDataLoaded via LookupByEditorID), or nullptr if that slot's
        // quest wasn't authored yet. nullptr disables that slot from
        // dispatch — the allocator already skips slots whose
        // bookFormID == 0 for unresolved books; the dispatch path here
        // adds the symmetric check for unresolved quests.
        //
        // During Phase 4 Step 14/15 bring-up, only `_ne_PooledLetterQuest01`
        // is authored, so only slot 0 should resolve. Step 16
        // duplicates the rest.

        std::array<RE::TESQuest *, LetterPool::kPoolSize> g_perSlotQuests{};
        std::array<RE::BGSRefAlias *, LetterPool::kPoolSize> g_perSlotSenderAlias{};
        std::array<RE::BGSRefAlias *, LetterPool::kPoolSize> g_perSlotLetterRefAlias{};
        std::atomic<bool> g_perSlotResolved = false;

        // The async dispatch chain (Step 15) runs in two polled phases
        // off the main thread: poll-for-Sender-filled, then poll-for-
        // LetterRef-filled. Until the chain completes, the IAction
        // verification polls must not yet count the dispatch as failed
        // (they'd time out on the courier-container check while we're
        // still waiting on the alias). g_dispatchChainCompletedAt is
        // stamped (real-time seconds) at the moment LetterRef fills,
        // and the IAction polls use `now - g_dispatchChainCompletedAt`
        // as the verification window instead of `secondsSinceStart`.
        //
        // g_dispatchChainFailed signals that our async chain already
        // diagnosed and rolled back the in-flight letter (sender alias
        // never filled, LetterRef never spawned, etc.), so the IAction
        // poll should treat that as the rollback and not try a second
        // rollback against the same slot.
        std::atomic<double> g_dispatchChainCompletedAt = 0.0;
        std::atomic<bool> g_dispatchChainFailed = false;

        // --- Cooldowns (in-game hours) ------------------------------
        //
        // Two independent cooldowns, both persisted via the action's
        // co-save record (kRecordTypeId 'NELE'):
        //
        //   g_lastDispatchGameHours — stamped by DetectCompletion when
        //     the letter reaches WICourierContainerRef. Gates
        //     IsAvailable so the action can't fire again for
        //     `iLetterActionCooldownGameHours` in-game hours.
        //
        //   g_senderLastDeliveryGameHours — stamped by
        //     NPCLetterAction_Cooldowns::OnLetterDelivered (called
        //     from LetterPool::MarkDelivered) when the vanilla courier
        //     actually hands the letter to the player. LetterComposer
        //     filters candidates whose entry here is within
        //     `iLetterSenderCooldownGameHours` in-game hours.
        //
        // Both are guarded by g_cooldownMutex — reads/writes happen
        // from the main thread (IsAvailable, DetectCompletion,
        // LetterComposer callbacks) and the SKSE serialization thread
        // (OnSave / OnLoad).
        std::mutex g_cooldownMutex;
        double g_lastDispatchGameHours = 0.0;
        std::unordered_map<RE::FormID, double> g_senderLastDeliveryGameHours;

        // Current game-time in hours since the calendar epoch. Returns
        // 0 if the calendar isn't available (very early in plugin
        // lifecycle); the cooldown gates treat a 0 stamp as "never
        // dispatched" so this fail-open behavior is intentional.
        double LetterCurrentGameHours()
        {
            auto *calendar = RE::Calendar::GetSingleton();
            if (!calendar)
                return 0.0;
            return static_cast<double>(calendar->GetHoursPassed());
        }

        // Stage IDs the C++ side directly sets on the per-slot quest.
        // The full lifecycle from Step 14's stage map:
        //   Stage  0 — Startup; alias fills happen here.
        //   Stage 10 — Papyrus fragment dispatches the letter to WICourier.
        //   Stage 20 — C++ DetectCompletion verified the letter reached
        //              WICourierContainerRef.
        //   Stage 30 — C++ TESContainerChangedEvent sink saw the letter
        //              land in the player's inventory.
        //   Stage 40 — C++ MenuOpenCloseEvent sink saw the player close
        //              BookMenu on the letter (read).
        //   Stage 50 — C++ TESContainerChangedEvent sink saw the letter
        //              leave the player's inventory (sold / dropped /
        //              given). Fragment routes → Stage 200.
        //   Stage 60 — C++ allocator evicted the slot. Fragment routes
        //              → Stage 200.
        //   Stage 200 — Terminal shutdown; fragment calls Shutdown() to
        //              Stop+Reset the quest for reuse.
        constexpr std::uint32_t kStageInCourierContainer = 20;
        constexpr std::uint32_t kStageDeliveredToPlayer = 30;
        constexpr std::uint32_t kStageReadByPlayer = 40;
        constexpr std::uint32_t kStageDisposedByPlayer = 50;
        constexpr std::uint32_t kStageRecycledByCpp = 60;
        constexpr std::uint32_t kStageTerminalShutdown = 200;

        void ResolvePerSlotQuests()
        {
            if (g_perSlotResolved.exchange(true))
                return;

            std::size_t resolved = 0;
            std::vector<std::string> missing;
            std::vector<std::string> missingAliases;
            for (std::size_t i = 0; i < LetterPool::kPoolSize; ++i)
            {
                char editorId[64];
                std::snprintf(editorId, sizeof(editorId),
                              "_ne_PooledLetterQuest%02zu", i + 1);
                auto *form = RE::TESForm::LookupByEditorID(editorId);
                auto *quest = form ? form->As<RE::TESQuest>() : nullptr;
                g_perSlotQuests[i] = quest;
                if (!quest)
                {
                    missing.emplace_back(editorId);
                    continue;
                }
                ++resolved;

                // Cache the Sender and LetterRef aliases by name. The
                // dispatch chain polls these directly to know when the
                // VM-side ForceFillSender has actually landed and when
                // the Created-in-Sender REFR has been spawned.
                for (auto *a : quest->aliases)
                {
                    if (!a)
                        continue;
                    if (a->aliasName == "Sender")
                    {
                        g_perSlotSenderAlias[i] = skyrim_cast<RE::BGSRefAlias *>(a);
                    }
                    else if (a->aliasName == "LetterRef")
                    {
                        g_perSlotLetterRefAlias[i] = skyrim_cast<RE::BGSRefAlias *>(a);
                    }
                }
                if (!g_perSlotSenderAlias[i] || !g_perSlotLetterRefAlias[i])
                {
                    char detail[128];
                    std::snprintf(detail, sizeof(detail),
                                  "%s (Sender=%s LetterRef=%s)",
                                  editorId,
                                  g_perSlotSenderAlias[i] ? "ok" : "MISSING",
                                  g_perSlotLetterRefAlias[i] ? "ok" : "MISSING");
                    missingAliases.emplace_back(detail);
                }
            }
            logger::info(
                "NPCLetterAction: resolved per-slot delivery quests ({} of {})",
                resolved, LetterPool::kPoolSize);
            for (const auto &name : missing)
            {
                logger::warn(
                    "NPCLetterAction: per-slot quest '{}' unresolved; slot disabled "
                    "from dispatch",
                    name);
            }
            for (const auto &detail : missingAliases)
            {
                logger::warn(
                    "NPCLetterAction: per-slot quest has missing alias(es): {}",
                    detail);
            }
        }

        RE::TESQuest *GetPerSlotQuest(std::size_t slotIndex)
        {
            if (slotIndex >= g_perSlotQuests.size())
                return nullptr;
            return g_perSlotQuests[slotIndex];
        }

        // --- VM dispatch helpers ------------------------------------
        //
        // Each call queues a Papyrus member function for execution on
        // the VM. The VM processes pending calls FIFO, so sequential C++
        // dispatches against the same handle run in order. We do not
        // wait for return values — every call here is fire-and-forget;
        // the verification polls observe the engine state effect.

        // Note: args are taken by value (not forwarding-reference) so the
        // deduced Args... are the decayed types, not references.
        // CommonLibSSE-NG's MakeFunctionArguments → FunctionArguments goes
        // through an `is_parameter_convertible` SFINAE check that requires
        // `is_not_reference<T>`; passing `Actor*` lvalues through
        // forwarding refs would deduce `Actor*&` and fail that check. We
        // only ever pass pointer / integral arguments here, so the by-
        // value copy is cheap and the type collapses cleanly.
        template <typename... Args>
        bool VMDispatchOnQuest(RE::TESQuest *quest,
                               std::string_view scriptName,
                               std::string_view methodName,
                               Args... args)
        {
            if (!quest)
                return false;
            auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm)
                return false;
            auto *policy = vm->GetObjectHandlePolicy();
            if (!policy)
                return false;
            const auto handle = policy->GetHandleForObject(
                RE::TESQuest::FORMTYPE, quest);
            auto *fnArgs = RE::MakeFunctionArguments(std::move(args)...);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return vm->DispatchMethodCall(
                handle,
                RE::BSFixedString(scriptName.data()),
                RE::BSFixedString(methodName.data()),
                fnArgs,
                callback);
        }

        // Helper: VM-dispatch Quest.SetStage(int) on the per-slot quest.
        // Used by the verification polls to advance the quest to its
        // terminal-shutdown stage (200) on success or its
        // recycled-by-C++ stage (60) on rollback.
        bool VMDispatchQuestSetStage(RE::TESQuest *quest, std::uint32_t stage)
        {
            return VMDispatchOnQuest(
                quest, "Quest"sv, "SetStage"sv,
                static_cast<std::int32_t>(stage));
        }

        // --- Polling helper -----------------------------------------
        //
        // Poll a main-thread predicate at fixed intervals until it
        // returns true or the deadline elapses, then run either
        // onSuccess or onTimeout on the main thread.
        //
        // The polling itself runs on the AsyncDispatch worker thread
        // (sleeping between iterations); each predicate evaluation is
        // marshaled to the main thread and synchronized via a
        // promise/future. This keeps the engine off-limits to the
        // worker (only main-thread code touches RE:: objects) while
        // not blocking the main thread on the wait.
        //
        // The predicate, onSuccess, and onTimeout closures are
        // captured by-move into the worker-thread lambda; they must
        // own everything they touch (no dangling references).
        void PollUntilOrTimeout(
            std::function<bool()> predicate,
            std::function<void()> onSuccess,
            std::function<void()> onTimeout,
            std::chrono::milliseconds interval,
            std::chrono::milliseconds maxDuration,
            std::string diagLabel = "")
        {
            AsyncDispatch::EnqueueWork(
                [predicate = std::move(predicate),
                 onSuccess = std::move(onSuccess),
                 onTimeout = std::move(onTimeout),
                 interval, maxDuration,
                 diagLabel = std::move(diagLabel)]() mutable
                {
                    const auto start = std::chrono::steady_clock::now();
                    int iter = 0;
                    while (true)
                    {
                        ++iter;
                        auto promise = std::make_shared<std::promise<bool>>();
                        auto future = promise->get_future();
                        AsyncDispatch::MarshalToMainThread(
                            [promise, &predicate]()
                            {
                                bool ok = false;
                                try
                                {
                                    ok = predicate();
                                }
                                catch (...)
                                {
                                    ok = false;
                                }
                                promise->set_value(ok);
                            });

                        // Wait for the marshaled predicate to run. A
                        // long wait timeout here is a safety net in
                        // case the main thread is wedged; we don't
                        // want the worker to hang forever.
                        const auto status = future.wait_for(std::chrono::seconds(5));
                        if (status != std::future_status::ready)
                        {
                            logger::warn(
                                "NPCLetterAction::PollUntilOrTimeout[{}]: "
                                "main-thread predicate did not return within 5s; "
                                "treating as timeout",
                                diagLabel);
                            AsyncDispatch::MarshalToMainThread(std::move(onTimeout));
                            return;
                        }
                        if (future.get())
                        {
                            AsyncDispatch::MarshalToMainThread(std::move(onSuccess));
                            return;
                        }
                        if (std::chrono::steady_clock::now() - start >= maxDuration)
                        {
                            AsyncDispatch::MarshalToMainThread(std::move(onTimeout));
                            return;
                        }
                        std::this_thread::sleep_for(interval);
                    }
                });
        }

        // --- Sender faction (rank-based alias fill) ------------------
        //
        // Each per-slot delivery quest's Sender alias fills via
        // `Find Matching Reference` with condition
        // `GetFactionRank _ne_LetterSenderFaction >= 4`. The engine's
        // alias-fill pass therefore picks whichever loaded actor is
        // currently at rank 4 in this faction. C++ promotes the chosen
        // sender to rank 4 before calling EnsureQuestStarted, and
        // demotes them back to rank 0 once the dispatch chain resolves
        // (success or failure). Any other actor lingering at rank 4
        // (from a crash-mid-dispatch, a rollback that didn't finish, or
        // a mid-flight save) is demoted by the pre-dispatch sweep, so
        // there's exactly one rank-4 actor when EnsureQuestStarted runs.
        //
        // Rank semantics:
        //   -1 (not in faction): NPC has never been touched.
        //    0: NPC was previously touched; not currently designated.
        //    4: NPC is the currently-designated sender.
        //
        // The 0/4 pair keeps the ExtraFactionChanges record on the actor
        // as a single stable entry (Add/Remove would toggle the entry in
        // and out with the same effective co-save cost, plus a lifecycle
        // gotcha where a version-specific "removed" sentinel can bring
        // back the bloat we were trying to avoid).
        constexpr const char *kSenderFactionEditorID = "_ne_LetterSenderFaction";
        constexpr std::int8_t kSenderRankCandidate = 0;
        constexpr std::int8_t kSenderRankDesignated = 4;

        std::atomic<bool> g_senderFactionResolved = false;
        RE::TESFaction *g_senderFaction = nullptr;

        RE::TESFaction *ResolveSenderFaction()
        {
            if (g_senderFactionResolved.load(std::memory_order_acquire))
                return g_senderFaction;
            auto *form = RE::TESForm::LookupByEditorID(kSenderFactionEditorID);
            auto *fact = form ? form->As<RE::TESFaction>() : nullptr;
            if (!fact)
            {
                logger::error(
                    "NPCLetterAction: sender faction '{}' did not resolve; "
                    "faction-mediated Sender alias fill is disabled and every "
                    "dispatch will roll back with Sender empty",
                    kSenderFactionEditorID);
            }
            else
            {
                logger::info(
                    "NPCLetterAction: sender faction resolved (formID=0x{:08X})",
                    fact->GetFormID());
            }
            g_senderFaction = fact;
            g_senderFactionResolved.store(true, std::memory_order_release);
            return fact;
        }

        // Sweep loaded actors: for anyone at rank >= kSenderRankDesignated
        // in the sender faction that isn't the intended target, demote
        // to kSenderRankCandidate. Runs on the main thread (we call it
        // from the pre-dispatch main-thread path).
        //
        // Loaded-only: senders are typically remote (that's the whole
        // point of a courier system), so unloaded persistent actors that
        // were previously designated but never demoted (crash-mid-dispatch,
        // save-mid-dispatch, etc.) would be missed by this sweep. Since
        // `finish()` demotes on every terminal path in the normal flow,
        // the only way a stragler survives is a crash inside the
        // dispatch chain — a small window (~100ms typical, up to 10s in
        // timeout). If we start seeing stale rank-4 members surviving
        // across reloads in practice, add co-save persistence of the
        // currently-designated sender FormID and demote on OnLoad.
        void SweepStaleDesignatedSenders(RE::TESFaction *fact,
                                         RE::Actor *target)
        {
            if (!fact)
                return;
            auto *pl = RE::ProcessLists::GetSingleton();
            if (!pl)
                return;

            std::size_t swept = 0;
            const auto walk =
                [&](const RE::BSTArray<RE::ActorHandle> &list)
            {
                for (const auto &h : list)
                {
                    auto ref = h.get();
                    auto *actor = ref.get();
                    if (!actor || actor == target)
                        continue;
                    // GetFactionRank's `a_isPlayer` argument is only
                    // consulted by the engine for the player-crime path;
                    // for all other actors it's ignored. Passing false is
                    // correct here regardless of whether target happens
                    // to be the player (which shouldn't happen but is a
                    // valid Actor pointer).
                    if (actor->GetFactionRank(fact, false) >= kSenderRankDesignated)
                    {
                        actor->AddToFaction(fact, kSenderRankCandidate);
                        ++swept;
                        logger::info(
                            "NPCLetterAction[FACTION]: swept stale rank-{}+ "
                            "member 0x{:08X} → rank {}",
                            static_cast<int>(kSenderRankDesignated),
                            actor->GetFormID(),
                            static_cast<int>(kSenderRankCandidate));
                    }
                }
            };
            walk(pl->highActorHandles);
            walk(pl->middleHighActorHandles);
            walk(pl->middleLowActorHandles);
            walk(pl->lowActorHandles);
            if (swept > 0)
            {
                logger::info(
                    "NPCLetterAction[FACTION]: pre-dispatch sweep demoted "
                    "{} stale rank-{}+ actor(s)",
                    swept,
                    static_cast<int>(kSenderRankDesignated));
            }
        }

        // Elevate the chosen sender to the designated rank. Handles the
        // three cases the user's spec calls out:
        //   not in faction → AddToFaction(..., rank=4)
        //   in faction, rank < 4 → AddToFaction(..., rank=4) (promotes)
        //   in faction, rank == 4 → no-op
        //
        // Actor::AddToFaction serves as both add-and-set and set-rank in
        // CommonLibSSE-NG (same underlying engine function). It updates
        // the ExtraFactionChanges record in place without churning the
        // extra-data list.
        void PromoteSenderToDesignated(RE::Actor *sender)
        {
            auto *fact = ResolveSenderFaction();
            if (!fact || !sender)
                return;

            SweepStaleDesignatedSenders(fact, sender);

            const auto currentRank = sender->GetFactionRank(fact, false);
            if (currentRank == kSenderRankDesignated)
            {
                logger::info(
                    "NPCLetterAction[FACTION]: sender 0x{:08X} already at "
                    "rank {}; no promotion needed",
                    sender->GetFormID(),
                    static_cast<int>(kSenderRankDesignated));
                return;
            }
            sender->AddToFaction(fact, kSenderRankDesignated);
            if (currentRank < 0)
            {
                logger::info(
                    "NPCLetterAction[FACTION]: added sender 0x{:08X} to "
                    "faction at rank {}",
                    sender->GetFormID(),
                    static_cast<int>(kSenderRankDesignated));
            }
            else
            {
                logger::info(
                    "NPCLetterAction[FACTION]: promoted sender 0x{:08X} "
                    "from rank {} to rank {}",
                    sender->GetFormID(),
                    currentRank,
                    static_cast<int>(kSenderRankDesignated));
            }
        }

        // Return the designated sender to the candidate rank. Called on
        // both the success and failure completion paths so the faction
        // state is always cleaned up.
        void DemoteSenderToCandidate(RE::Actor *sender)
        {
            auto *fact = ResolveSenderFaction();
            if (!fact || !sender)
                return;
            const auto currentRank = sender->GetFactionRank(fact, false);
            if (currentRank < 0)
            {
                // Not in faction at all — nothing to do. Shouldn't happen
                // on the normal path (we just promoted them), but a
                // defensive early-out avoids spurious extra-data churn.
                return;
            }
            if (currentRank == kSenderRankCandidate)
                return;
            sender->AddToFaction(fact, kSenderRankCandidate);
            logger::info(
                "NPCLetterAction[FACTION]: demoted sender 0x{:08X} from "
                "rank {} back to rank {}",
                sender->GetFormID(),
                currentRank,
                static_cast<int>(kSenderRankCandidate));
        }

        // --- The async dispatch chain --------------------------------
        //
        // Replaces the prior synchronous `VMDispatchAddItemToContainer`
        // stub. Three observable phases the IAction polls + the Papyrus
        // side care about:
        //
        //   Phase A — Sender alias fill
        //     VM-dispatch `_ne_PooledLetterQuest.ForceFillSender(actor)`
        //     and POLL the alias's GetReference() until it returns
        //     non-null (up to 5s). VM dispatches are async — they queue
        //     on the Papyrus VM and run on a later tick — so doing
        //     EnsureQuestStarted immediately would beat the fill and
        //     leave Sender empty. Polling is the only way to know the
        //     fill actually landed before continuing.
        //
        //   Phase B — Quest start + LetterRef alias fill
        //     With Sender filled, call `TESQuest::EnsureQuestStarted`
        //     natively (the C++ entry point AmbushAction uses, the only
        //     path that runs the engine's full alias-fill pass — see
        //     docs/engine-findings/starting-a-quest-from-cpp.md). Then
        //     POLL the LetterRef alias's GetReference() until it returns
        //     non-null (up to 5s). The `Create Reference to Object` rule
        //     for LetterRef runs inside EnsureQuestStarted's alias-fill
        //     pass, so this usually succeeds on the first poll — but if
        //     the sender isn't world-instantiated (e.g. unloaded cell),
        //     the Create silently no-ops and LetterRef stays empty. The
        //     timeout here is the discriminator the user asked for:
        //     "letter didn't spawn at all" vs "spawned but didn't reach
        //     courier" (the latter is the IAction polls' job).
        //
        //   Phase C — Stage 10 dispatches to courier container
        //     Once LetterRef is filled, Papyrus advances Stage 0 → 10
        //     within a VM tick, and Stage 10's fragment calls the
        //     script's DispatchLetterToCourier function which moves
        //     the REFR into WICourierContainerRef. C++ does nothing
        //     for this phase; the IAction polls (DetectCompletion /
        //     DetectAndRollbackFailedStart) observe the container's
        //     inventory and decide success/failure.
        //
        // The function takes an onComplete(success) callback. On true,
        // the chain reached Phase C and the IAction polls take over.
        // On false, the chain diagnosed and logged a specific failure
        // and the caller should roll back the slot + dispatcher state.
        void DispatchLetterViaPerSlotQuest(
            std::size_t slotIndex,
            RE::FormID senderActorFormID,
            std::function<void(bool)> onComplete)
        {
            auto *quest = GetPerSlotQuest(slotIndex);
            if (!quest)
            {
                logger::warn(
                    "NPCLetterAction: slot {} has no resolved per-slot delivery "
                    "quest; dispatch refused (expected during Step 14/15 bring-up "
                    "for slots > 0)",
                    slotIndex);
                onComplete(false);
                return;
            }

            auto *senderAlias = (slotIndex < g_perSlotSenderAlias.size())
                                    ? g_perSlotSenderAlias[slotIndex]
                                    : nullptr;
            auto *letterRefAlias = (slotIndex < g_perSlotLetterRefAlias.size())
                                       ? g_perSlotLetterRefAlias[slotIndex]
                                       : nullptr;
            if (!senderAlias || !letterRefAlias)
            {
                logger::warn(
                    "NPCLetterAction: slot {} per-slot quest is missing "
                    "Sender or LetterRef alias (Sender={}, LetterRef={}); "
                    "dispatch refused",
                    slotIndex,
                    senderAlias ? "ok" : "MISSING",
                    letterRefAlias ? "ok" : "MISSING");
                onComplete(false);
                return;
            }

            auto *form = RE::TESForm::LookupByID(senderActorFormID);
            auto *sender = form ? form->As<RE::Actor>() : nullptr;
            if (!sender)
            {
                logger::warn(
                    "NPCLetterAction: sender FormID 0x{:08X} did not resolve to "
                    "a live Actor; dispatch refused",
                    senderActorFormID);
                onComplete(false);
                return;
            }
            // Belt-and-suspenders: LetterComposer already filters dead /
            // disabled candidates, but the LLM round-trip means the chosen
            // sender could die in combat or be disabled by console between
            // candidate collection and dispatch. The per-slot quest's
            // "Create Reference in Sender" alias needs a live, enabled
            // sender; refuse dispatch otherwise so the slot rolls back
            // cleanly instead of stranding inside Papyrus.
            if (sender->IsDead())
            {
                logger::warn(
                    "NPCLetterAction: sender 0x{:08X} is dead at dispatch "
                    "time; refusing (will roll back, no cooldown)",
                    senderActorFormID);
                onComplete(false);
                return;
            }
            if (sender->IsDisabled())
            {
                logger::warn(
                    "NPCLetterAction: sender 0x{:08X} is disabled at dispatch "
                    "time; refusing (will roll back, no cooldown)",
                    senderActorFormID);
                onComplete(false);
                return;
            }

            // Reset the dispatch-chain state flags before kicking off.
            g_dispatchChainCompletedAt.store(0.0);
            g_dispatchChainFailed.store(false);

            const auto questFormID = quest->GetFormID();

            // Wrap the caller's onComplete so every terminal path first
            // demotes the sender out of the designated rank, whether the
            // chain succeeded or failed. This is the only cleanup site —
            // both `callFailed` and the LetterRef-success continuation
            // route here so the faction state is guaranteed consistent.
            auto finish = [sender, onComplete](bool ok)
            {
                DemoteSenderToCandidate(sender);
                onComplete(ok);
            };
            auto callFailed =
                [slotIndex, finish](const char *reason)
            {
                logger::warn(
                    "NPCLetterAction: dispatch chain FAILED for slot {} — {}",
                    slotIndex, reason);
                g_dispatchChainFailed.store(true);
                finish(false);
            };

            // Phase A: promote sender to the designated rank in
            // _ne_LetterSenderFaction, sweeping any stale rank-4+ members
            // that were left behind by a prior crash / interrupted
            // dispatch / mid-flight save. Once EnsureQuestStarted runs,
            // the Sender alias's Find-Matching-Reference condition
            // (GetFactionRank >= 4) picks this actor natively during the
            // engine's alias-fill pass — no ForceRefTo, no pre-start
            // alias hackery.
            logger::info(
                "NPCLetterAction: dispatching slot {} (quest=0x{:08X}) → "
                "Phase A: faction promote (sender=0x{:08X} → rank {})",
                slotIndex, questFormID, senderActorFormID,
                static_cast<int>(kSenderRankDesignated));
            PromoteSenderToDesignated(sender);

            // Phase B: EnsureQuestStarted natively. The engine's alias
            // fill pass runs Sender's Find-Matching-Reference rule (picks
            // our rank-4 actor), then LetterRef's Create-in-Sender rule
            // (spawns the pooled book in the sender's inventory).
            bool engineResult = false;
            const bool callOk =
                quest->EnsureQuestStarted(engineResult, /*a_startNow=*/true);
            if (!callOk || !engineResult)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "EnsureQuestStarted reported failure "
                              "(callOk=%s engineResult=%s)",
                              callOk ? "true" : "false",
                              engineResult ? "true" : "false");
                callFailed(buf);
                return;
            }
            logger::info(
                "NPCLetterAction: slot {} EnsureQuestStarted ok "
                "(quest=0x{:08X}); polling for Sender then LetterRef fill",
                slotIndex, questFormID);

            // Phase B poll — the Sender alias's Find-Matching-Reference
            // rule usually completes synchronously inside
            // EnsureQuestStarted, but poll defensively in case the engine
            // defers the match. Then Phase C polls for LetterRef fill.
            PollUntilOrTimeout(
                // predicate
                [senderAlias]()
                { return senderAlias->GetReference() != nullptr; },
                // onSuccess (Sender filled, kick off LetterRef poll)
                [slotIndex, senderActorFormID, letterRefAlias, senderAlias,
                 callFailed, finish]()
                {
                    auto *senderRef = senderAlias->GetReference();
                    const auto filledID =
                        senderRef ? senderRef->GetFormID() : 0u;
                    logger::info(
                        "NPCLetterAction: slot {} Phase B complete — Sender "
                        "alias filled (REFR=0x{:08X}, expected 0x{:08X}); "
                        "Phase C: polling for LetterRef fill",
                        slotIndex, filledID, senderActorFormID);

                    // Sanity: warn (do not fail) if the alias picked
                    // someone other than our target. Should never happen
                    // if the pre-dispatch sweep worked, but it's a
                    // symptom worth surfacing.
                    if (senderRef && senderRef->GetFormID() != senderActorFormID)
                    {
                        logger::warn(
                            "NPCLetterAction: slot {} Sender fill picked "
                            "0x{:08X} but we designated 0x{:08X}; stale "
                            "rank-{}+ member survived the sweep?",
                            slotIndex,
                            filledID,
                            senderActorFormID,
                            static_cast<int>(kSenderRankDesignated));
                    }

                    PollUntilOrTimeout(
                        // predicate
                        [letterRefAlias]()
                        { return letterRefAlias->GetReference() != nullptr; },
                        // onSuccess (LetterRef filled, chain reaches Phase D)
                        [slotIndex, letterRefAlias, finish]()
                        {
                            auto *ref = letterRefAlias->GetReference();
                            logger::info(
                                "NPCLetterAction: slot {} Phase C complete — "
                                "LetterRef filled (REFR=0x{:08X}); the letter "
                                "REFR is spawned in the sender's inventory and "
                                "Papyrus Stage 10 will hand it off to vanilla "
                                "WICourier within one VM tick. Awaiting "
                                "courier-container verification via IAction polls.",
                                slotIndex,
                                ref ? ref->GetFormID() : 0u);
                            g_dispatchChainCompletedAt.store(
                                static_cast<double>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now()
                                            .time_since_epoch())
                                        .count()) /
                                1000.0);
                            finish(true);
                        },
                        // onTimeout (LetterRef never filled)
                        [slotIndex, callFailed]()
                        {
                            char buf[256];
                            std::snprintf(
                                buf, sizeof(buf),
                                "Phase C timeout (slot %zu): LetterRef alias "
                                "did NOT fill within 5s of EnsureQuestStarted. "
                                "The Create-Reference-in-Sender rule silently "
                                "no-op'd. Likely causes: sender NPC not world-"
                                "instantiated (e.g. unloaded cell), or sender's "
                                "inventory rejected the create. The letter "
                                "REFR was NEVER spawned (so this is NOT a "
                                "courier-handoff failure).",
                                slotIndex);
                            callFailed(buf);
                        },
                        std::chrono::milliseconds(100),
                        std::chrono::seconds(5),
                        std::string{"LetterRef-fill"});
                },
                // onTimeout (Sender never filled by faction condition)
                [slotIndex, callFailed]()
                {
                    char buf[256];
                    std::snprintf(
                        buf, sizeof(buf),
                        "Phase B timeout (slot %zu): Sender alias did NOT "
                        "fill within 5s of EnsureQuestStarted. The "
                        "Find-Matching-Reference rule with "
                        "GetFactionRank _ne_LetterSenderFaction >= %d "
                        "matched no persistent reference. Confirm CK setup: "
                        "alias fill type is Find Matching Reference, the "
                        "condition function/comparison are correct, and "
                        "'In Loaded Area' is UNCHECKED (senders are "
                        "expected to be remote and unloaded).",
                        slotIndex,
                        static_cast<int>(kSenderRankDesignated));
                    callFailed(buf);
                },
                std::chrono::milliseconds(100),
                std::chrono::seconds(5),
                std::string{"Sender-fill"});
        }
    }

    namespace NPCLetterAction_Init
    {
        void Initialize()
        {
            // Warm the WICourier / courier-container resolution so the
            // very first verification poll doesn't pay the lookup cost.
            // The lazy path inside ResolveCourierQuest still handles the
            // case where this Initialize isn't called.
            ResolveCourierQuest();

            // Resolve the 20 per-slot delivery quests.
            ResolvePerSlotQuests();

            // Warm the sender-faction resolution. Emits an error log at
            // kDataLoaded if the faction is missing, so a mis-authored
            // ESP fails loudly at load time rather than silently at
            // first dispatch.
            ResolveSenderFaction();
        }
    }

    namespace NPCLetterAction_QuestControl
    {
        void AdvanceSlotStage(std::size_t slotIndex, std::uint32_t stage)
        {
            auto *quest = GetPerSlotQuest(slotIndex);
            if (!quest)
                return;
            VMDispatchQuestSetStage(quest, stage);
        }

        void ShutdownSlotQuestSync(std::size_t slotIndex)
        {
            auto *quest = GetPerSlotQuest(slotIndex);
            if (!quest)
                return;
            const bool wasRunning = quest->IsRunning();
            quest->Stop();
            quest->Reset();
            logger::info(
                "NPCLetterAction: slot {} recycled by allocator "
                "(quest=0x{:08X}, wasRunning={}, native Stop+Reset)",
                slotIndex, quest->GetFormID(), wasRunning);
        }
    }

    namespace NPCLetterAction_Cooldowns
    {
        void OnLetterDelivered(RE::FormID senderNpcFormID)
        {
            if (senderNpcFormID == 0)
                return;
            const double nowHours = LetterCurrentGameHours();
            {
                std::scoped_lock lock(g_cooldownMutex);
                g_senderLastDeliveryGameHours[senderNpcFormID] = nowHours;
            }
            logger::info(
                "NPCLetterAction: per-sender cooldown stamp set for 0x{:08X} at "
                "gameHours={:.2f}",
                senderNpcFormID, nowHours);
        }

        bool IsSenderOnCooldown(RE::FormID senderNpcFormID)
        {
            const int cooldownHours = Settings::Get().letterSenderCooldownGameHours;
            if (cooldownHours <= 0 || senderNpcFormID == 0)
                return false;
            double stamp = 0.0;
            {
                std::scoped_lock lock(g_cooldownMutex);
                auto it = g_senderLastDeliveryGameHours.find(senderNpcFormID);
                if (it == g_senderLastDeliveryGameHours.end())
                    return false;
                stamp = it->second;
            }
            if (stamp <= 0.0)
                return false;
            const double elapsed = LetterCurrentGameHours() - stamp;
            return elapsed < static_cast<double>(cooldownHours);
        }
    }

    namespace NPCLetterAction_Persistence
    {
        constexpr std::uint32_t kRecordVersion = 1;

        void OnSave(SKSE::SerializationInterface *intfc)
        {
            if (!intfc)
                return;
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion))
            {
                logger::error("NPCLetterAction::OnSave: OpenRecord failed");
                return;
            }

            // Snapshot under lock, then write outside the lock so
            // SKSE's serialization thread doesn't hold g_cooldownMutex
            // during file I/O.
            double lastDispatch = 0.0;
            std::vector<std::pair<RE::FormID, double>> senderStamps;
            {
                std::scoped_lock lock(g_cooldownMutex);
                lastDispatch = g_lastDispatchGameHours;
                senderStamps.reserve(g_senderLastDeliveryGameHours.size());
                for (const auto &kv : g_senderLastDeliveryGameHours)
                {
                    senderStamps.emplace_back(kv.first, kv.second);
                }
            }

            intfc->WriteRecordData(lastDispatch);
            const std::uint32_t count = static_cast<std::uint32_t>(senderStamps.size());
            intfc->WriteRecordData(count);
            for (const auto &kv : senderStamps)
            {
                intfc->WriteRecordData(kv.first);
                intfc->WriteRecordData(kv.second);
            }
        }

        void OnLoad(SKSE::SerializationInterface *intfc,
                    std::uint32_t version,
                    std::uint32_t length)
        {
            if (!intfc)
                return;
            if (version != kRecordVersion)
            {
                logger::warn(
                    "NPCLetterAction::OnLoad: unknown version {} (length={}); "
                    "clearing cooldown state",
                    version, length);
                OnRevert();
                return;
            }

            double lastDispatch = 0.0;
            if (intfc->ReadRecordData(lastDispatch) != sizeof(lastDispatch))
            {
                logger::error(
                    "NPCLetterAction::OnLoad: short read on lastDispatch stamp; clearing");
                OnRevert();
                return;
            }
            std::uint32_t count = 0;
            if (intfc->ReadRecordData(count) != sizeof(count))
            {
                logger::error(
                    "NPCLetterAction::OnLoad: short read on sender-count; clearing");
                OnRevert();
                return;
            }

            std::unordered_map<RE::FormID, double> loaded;
            loaded.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                RE::FormID fid = 0;
                double h = 0.0;
                if (intfc->ReadRecordData(fid) != sizeof(fid) ||
                    intfc->ReadRecordData(h) != sizeof(h))
                {
                    logger::error(
                        "NPCLetterAction::OnLoad: short read on sender entry {}; "
                        "clearing everything and bailing",
                        i);
                    OnRevert();
                    return;
                }
                // ResolveFormID converts saved formIDs across load-order
                // changes. If the sender's mod is no longer loaded, skip
                // the entry rather than resurrecting a stale FormID.
                RE::FormID resolved = 0;
                if (intfc->ResolveFormID(fid, resolved) && resolved != 0)
                {
                    loaded[resolved] = h;
                }
            }

            {
                std::scoped_lock lock(g_cooldownMutex);
                g_lastDispatchGameHours = lastDispatch;
                g_senderLastDeliveryGameHours = std::move(loaded);
            }
            logger::info(
                "NPCLetterAction::OnLoad: restored lastDispatchGameHours={:.2f}, "
                "senderCount={}",
                lastDispatch,
                count);
        }

        void OnRevert()
        {
            std::scoped_lock lock(g_cooldownMutex);
            g_lastDispatchGameHours = 0.0;
            g_senderLastDeliveryGameHours.clear();
        }
    }

    std::string NPCLetterAction::Name() const
    {
        return "npc_letter";
    }

    std::string NPCLetterAction::Description() const
    {
        return "An NPC who knows the player character — chosen by you at "
               "action-select time from the letter sender candidates listed "
               "below — sends a personal letter via the vanilla courier "
               "system. Tone and polarity are driven by the generated "
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
               "Parameters:\n"
               "  - `sender_npc_form_id` (REQUIRED, string): hex FormID of "
               "ONE entry from the `letter_sender_candidates` list at the "
               "end of this prompt (e.g. `\"0xA2C8E\"`). Pick the NPC "
               "whose recent memories about the player best carry the tone "
               "this moment needs. If this field is missing, unparseable, "
               "or names a form outside the candidate list, the action "
               "fails to start.\n"
               "  - `urgency_hint` (optional, string): `low` / `medium` / "
               "`high`. Defaults to `medium`. One input among several to "
               "the letter-writing prompt — not a hard directive.\n"
               "\n"
               "Do NOT include any other parameter fields — letter body, "
               "tone, mood, topic, and recipient are all decided by the "
               "action's own internal letter-writing pipeline, which "
               "embodies the sender you chose. Extra fields will be "
               "silently ignored.";
    }

    ActionPolarity NPCLetterAction::Polarity() const
    {
        return ActionPolarity::Either;
    }

    bool NPCLetterAction::IsAvailable(const ActionContext &ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char *reason)
        {
            if (debug)
            {
                logger::debug("NPCLetterAction::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        // Hard fail if WICourier never resolved (very broken install).
        if (!ResolveCourierQuest())
        {
            return blocked("WICourier quest not resolved");
        }

        // Per-action in-game-hours cooldown. Stamped by DetectCompletion
        // when a dispatch verifies as landed in the courier container.
        const int cooldownHours = Settings::Get().letterActionCooldownGameHours;
        if (cooldownHours > 0)
        {
            double lastDispatch = 0.0;
            {
                std::scoped_lock lock(g_cooldownMutex);
                lastDispatch = g_lastDispatchGameHours;
            }
            if (lastDispatch > 0.0)
            {
                const double nowHours = LetterCurrentGameHours();
                const double elapsed = nowHours - lastDispatch;
                if (elapsed < static_cast<double>(cooldownHours))
                {
                    if (debug)
                    {
                        logger::debug(
                            "NPCLetterAction::IsAvailable: blocked (per-action cooldown: "
                            "elapsed={:.2f}h < cooldown={}h, lastDispatch={:.2f}h current={:.2f}h)",
                            elapsed, cooldownHours, lastDispatch, nowHours);
                    }
                    return false;
                }
            }
        }

        // Location must not be flagged dangerous (dungeon / lair / camp).
        if (ctx.player)
        {
            if (LocationKeywords::IsDangerous(ctx.player->GetCurrentLocation()))
            {
                return blocked("LocationKeywords::IsDangerous");
            }
        }

        if (!SkyrimNetAPI::IsMemorySystemReady())
        {
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
        if (!enrolled.is_array())
        {
            return blocked("GetActorEngagement returned non-array");
        }
        if (static_cast<int>(enrolled.size()) < minCandidates)
        {
            if (debug)
            {
                logger::debug(
                    "NPCLetterAction::IsAvailable: blocked (only {} candidates, "
                    "need {})",
                    enrolled.size(), minCandidates);
            }
            return false;
        }

        return true;
    }

    StartResult NPCLetterAction::Start(const ActionContext &ctx,
                                       const nlohmann::json &parameters)
    {
        // 1. Parse the required sender_npc_form_id parameter. The
        // action-select LLM chose the sender from the candidate list;
        // if that's missing or malformed, fail Start immediately
        // (before allocating a pool slot) so we don't burn a slot on
        // a request we can't fulfill.
        if (!parameters.is_object())
        {
            return StartResult{.started = false,
                               .detail  = "parameters is not a JSON object"};
        }
        RE::FormID senderNpcFormID = 0;
        {
            auto it = parameters.find("sender_npc_form_id");
            if (it == parameters.end() || !it->is_string())
            {
                return StartResult{
                    .started = false,
                    .detail  = "parameters.sender_npc_form_id missing or not a string"};
            }
            const auto idStr = it->get<std::string>();
            try
            {
                senderNpcFormID = static_cast<RE::FormID>(
                    std::stoul(idStr, nullptr, /*base=*/0));
            }
            catch (...)
            {
                return StartResult{
                    .started = false,
                    .detail  = "parameters.sender_npc_form_id unparseable: '" + idStr + "'"};
            }
            if (senderNpcFormID == 0)
            {
                return StartResult{
                    .started = false,
                    .detail  = "parameters.sender_npc_form_id resolved to 0"};
            }
        }

        // Reset the dispatch-chain state atomics at the very top of
        // Start, before any allocator or IAction poll can observe stale
        // state left over from a prior successful dispatch. If we only
        // reset inside DispatchLetterViaPerSlotQuest, the ~1–2s
        // LetterComposer LLM window between Allocate and the actual
        // dispatch runs with the previous dispatch's completion
        // timestamp still live, so the IAction poll computes
        // `sinceChainDone = now - <old timestamp>` (arbitrarily large)
        // and fires a spurious "Phase C failure" rollback against the
        // brand-new in-flight slot. Zeroing here means the poll's
        // "chainCompletedAt <= 0 → still setting up, don't fail yet"
        // gate keeps it silent until this dispatch's chain actually
        // completes.
        g_dispatchChainCompletedAt.store(0.0);
        g_dispatchChainFailed.store(false);

        // 2. Allocate a pool slot up-front. If this fails the dispatcher
        // gets an immediate started=false with no LLM round-trip cost.
        auto alloc = LetterPool::Allocate();
        if (!alloc)
        {
            const char *reason =
                (alloc.error() == LetterPool::AllocationFailure::PoolNotResolved)
                    ? "letter pool not resolved (Initialize never ran or all forms failed)"
                    : "letter pool exhausted (all slots PendingDelivery)";
            logger::warn("NPCLetterAction::Start: {}", reason);
            return StartResult{.started = false, .detail = reason};
        }

        const std::size_t slotIndex = alloc->slotIndex;
        const RE::FormID bookFormID = alloc->bookFormID;

        // Stash in-flight bookkeeping. The verification polls
        // (DetectAndRollbackFailedStart / DetectCompletion) read this
        // to know which book to check the courier's inventory for.
        {
            std::scoped_lock lock(g_inFlightMutex);
            g_inFlightSlot = static_cast<int>(slotIndex);
            g_inFlightBookFormID = bookFormID;
            g_lastVerifyLogTime = 0.0;
        }

        // 3. Decode urgency_hint from parameters (low/medium/high).
        // Defaults to medium when missing or out of range — the action's
        // own LLM call gets the final say on emotional weight via the
        // generated mood.
        LetterComposer::UrgencyHint urgency = LetterComposer::UrgencyHint::Medium;
        if (auto it = parameters.find("urgency_hint");
            it != parameters.end() && it->is_string())
        {
            const auto v = it->get<std::string>();
            if (v == "low")
                urgency = LetterComposer::UrgencyHint::Low;
            else if (v == "high")
                urgency = LetterComposer::UrgencyHint::High;
        }

        // 4. Fire the async content-LLM call. The callback fires on a
        // SkyrimNet worker thread; we marshal back to main before
        // touching engine state. While composing, the action is
        // formally in-flight from the dispatcher's perspective.
        const std::size_t slotForLambda = slotIndex;
        const RE::FormID bookForLambda = bookFormID;
        LetterComposer::Compose(
            ctx, urgency, senderNpcFormID,
            [slotForLambda, bookForLambda](std::optional<LetterComposer::LetterComposition> comp)
            {
                AsyncDispatch::MarshalToMainThread(
                    [slotForLambda, bookForLambda, comp = std::move(comp)]() mutable
                    {
                        if (!comp)
                        {
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
                                g_inFlightSlot = -1;
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

                        // Drive the slot's per-slot delivery quest via
                        // the async chain (Phase A: ForceFillSender +
                        // poll Sender; Phase B: EnsureQuestStarted +
                        // poll LetterRef). The onComplete callback fires
                        // back on the main thread; on failure it
                        // performs the slot/dispatcher rollback.
                        (void)bookForLambda; // logged by the chain + polls
                        DispatchLetterViaPerSlotQuest(
                            slotForLambda, comp->senderNpcFormID,
                            [slotForLambda](bool success)
                            {
                                if (success)
                                {
                                    // The IAction polls will take it
                                    // from here, checking the courier
                                    // container for the letter.
                                    logger::info(
                                        "NPCLetterAction: dispatch chain "
                                        "complete for slot {}; awaiting "
                                        "courier-container verification",
                                        slotForLambda);
                                    return;
                                }
                                // Chain failed (already logged inside
                                // the chain with the specific phase /
                                // reason). Roll back here.
                                LetterPool::AbortPending(slotForLambda);
                                {
                                    std::scoped_lock lock(g_inFlightMutex);
                                    g_inFlightSlot = -1;
                                    g_inFlightBookFormID = 0;
                                }
                                ActionDispatcher::CompleteAction("npc_letter");
                            });
                    });
            });

        return StartResult{.started = true, .detail = "composing letter"};
    }

    bool NPCLetterAction::DetectAndRollbackFailedStart(
        const ActionContext & /*ctx*/, double secondsSinceStart)
    {
        // If the async dispatch chain already diagnosed a failure (e.g.
        // Phase A or Phase B timeout), the Start-callback's onComplete
        // handler has ALREADY cleared in-flight, freed the slot, and
        // called CompleteAction. Nothing left for the poll to roll back.
        if (g_dispatchChainFailed.load())
        {
            return false;
        }

        // The verify-delay window starts at the moment the dispatch chain
        // reaches Phase C (LetterRef filled, Papyrus about to dispatch).
        // Counting from action start would race the chain itself: the
        // chain can take up to ~10s (5s Phase A + 5s Phase B), during
        // which the courier container is correctly empty.
        const double chainCompletedAt = g_dispatchChainCompletedAt.load();
        if (chainCompletedAt <= 0.0)
        {
            // Chain still running (or already failed and the flag isn't
            // visible yet). Not yet time to consider this a failure.
            return false;
        }

        const double nowSec =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()) /
            1000.0;
        const double sinceChainDone = nowSec - chainCompletedAt;
        const double verifyDelay = static_cast<double>(
            Settings::Get().letterDispatchVerifyDelaySeconds);
        if (sinceChainDone < verifyDelay)
        {
            return false; // too early to give up on the courier handoff
        }

        RE::FormID bookFormID = 0;
        int slotIndex = -1;
        {
            std::scoped_lock lock(g_inFlightMutex);
            bookFormID = g_inFlightBookFormID;
            slotIndex = g_inFlightSlot;
        }
        if (bookFormID == 0 || slotIndex < 0)
        {
            return false;
        }

        const auto count = CourierInventoryCount(bookFormID);
        if (Settings::Get().debugMode)
        {
            logger::debug(
                "NPCLetterAction: verify (sinceChainDone={:.1f}s, action@{:.1f}s) — "
                "courier container has {} copies of 0x{:08X}",
                sinceChainDone, secondsSinceStart, count, bookFormID);
        }

        if (count > 0)
        {
            // Letter landed; let DetectCompletion handle it.
            return false;
        }

        // Phase C failure: LetterRef was filled (sender's inventory got
        // the REFR) but the courier container never received it. The
        // letter spawned but didn't reach the destination — Stage 10's
        // DispatchLetterToCourier ran and AddItemToContainer either
        // failed silently or moved the REFR somewhere else. Distinct
        // failure mode from "letter never spawned" (which the Phase B
        // poll catches).
        logger::warn(
            "NPCLetterAction: Phase C failure for slot {} — letter REFR spawned "
            "successfully but courier container has 0 copies of 0x{:08X} after "
            "{:.1f}s past dispatch-chain completion. The Papyrus Stage 10 "
            "AddItemToContainer call did not deposit the letter into "
            "WICourierContainerRef. Rolling back.",
            slotIndex, bookFormID, sinceChainDone);
        if (auto *quest = GetPerSlotQuest(
                static_cast<std::size_t>(slotIndex)))
        {
            VMDispatchQuestSetStage(quest, kStageRecycledByCpp);
        }
        LetterPool::AbortPending(static_cast<std::size_t>(slotIndex));
        {
            std::scoped_lock lock(g_inFlightMutex);
            g_inFlightSlot = -1;
            g_inFlightBookFormID = 0;
        }
        return true;
    }

    bool NPCLetterAction::DetectCompletion(const ActionContext & /*ctx*/,
                                           double secondsSinceStart)
    {
        RE::FormID bookFormID = 0;
        int slotIndex = -1;
        {
            std::scoped_lock lock(g_inFlightMutex);
            bookFormID = g_inFlightBookFormID;
            slotIndex = g_inFlightSlot;
        }
        if (bookFormID == 0)
            return false;

        const auto count = CourierInventoryCount(bookFormID);
        if (count <= 0)
            return false;

        logger::info(
            "NPCLetterAction: DetectCompletion — courier has letter 0x{:08X} "
            "after {:.1f}s; action complete (slot stays PendingDelivery until "
            "courier hands off to player)",
            bookFormID, secondsSinceStart);

        // Stamp the per-action in-game-hours cooldown. Done here (on
        // successful courier deposit), not in the delivery-to-player
        // path, so the action-level pacing starts as soon as we know
        // the letter is in flight — not when it arrives, which can be
        // an arbitrarily long game-time delay depending on where the
        // player wanders.
        const double dispatchHours = LetterCurrentGameHours();
        {
            std::scoped_lock lock(g_cooldownMutex);
            g_lastDispatchGameHours = dispatchHours;
        }
        logger::info(
            "NPCLetterAction: per-action cooldown stamp set to gameHours={:.2f}",
            dispatchHours);

        // Advance the per-slot quest to Stage 20 ("in courier
        // container, verified"). The quest stays running through the
        // rest of the lifecycle (30 = delivered, 40 = read, 50 =
        // disposed) driven by LetterPool's TESContainerChangedEvent
        // and MenuOpenCloseEvent sinks; only Stage 50 (or Stage 60,
        // from the allocator's recycle path) routes to the terminal
        // Stage 200 Shutdown. See kStage* constants above for the map.
        if (slotIndex >= 0)
        {
            if (auto *quest = GetPerSlotQuest(
                    static_cast<std::size_t>(slotIndex)))
            {
                VMDispatchQuestSetStage(quest, kStageInCourierContainer);
            }
        }

        // Clear in-flight bookkeeping but leave the slot in
        // PendingDelivery — physical delivery happens on the vanilla
        // courier's timeline (potentially minutes), and the
        // TESContainerChangedEvent sink will transition the slot to
        // InInventory when the courier hands off to the player.
        {
            std::scoped_lock lock(g_inFlightMutex);
            g_inFlightSlot = -1;
            g_inFlightBookFormID = 0;
        }
        return true;
    }
}
