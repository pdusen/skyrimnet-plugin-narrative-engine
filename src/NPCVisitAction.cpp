#include <NPCVisitAction.h>

#include <ActionDispatcher.h>
#include <AsyncDispatch.h>
#include <LocationKeywords.h>
#include <SenderCandidatePool.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <VisitComposer.h>
#include <VisitConclusionPoll.h>
#include <VisitState.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/B/BSFixedString.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/C/Calendar.h>
#include <RE/D/DialogueMenu.h>
#include <RE/F/FunctionArguments.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESCombatEvent.h>
#include <RE/T/TESDeathEvent.h>
#include <RE/T/TESFaction.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>
#include <RE/U/UI.h>
#include <RE/V/VirtualMachine.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace NarrativeEngine
{
    using namespace std::string_view_literals;

    namespace
    {
        // ---- Editor IDs & rank constants ---------------------------

        constexpr const char*   kVisitQuestEditorID   = "_ne_VisitQuest";
        constexpr const char*   kVisitFactionEditorID = "_ne_VisitSenderFaction";
        constexpr const char*   kSenderAliasName      = "Sender";
        constexpr const char*   kSpawnMarkerAliasName = "SpawnMarker";
        constexpr const char*   kReturnAnchorAliasName = "ReturnAnchor";

        // Faction rank scheme mirrors NPCLetterAction's — rank 0 =
        // prior candidate, rank 4 = currently designated. See
        // PHASE_05_NPC_VISIT_ACTION.md#the-marker-faction-mechanism.
        constexpr std::int8_t   kSenderRankCandidate  = 0;
        constexpr std::int8_t   kSenderRankDesignated = 4;

        // Stage constants (mirror the Papyrus stage table).
        // ---- SkyrimNet action names --------------------------------
        //
        // Plugin-owned turns fire SkyrimNet's built-in
        // `StartConversation` for now — it's the closest fit in the
        // stock SkyrimNet library ("initiate a new interaction between
        // two characters"). We pass our briefing argsJson so a future
        // SkyrimNet build that consumes it (or a plugin-registered
        // action replacing this) gets the context; the current build
        // ignores extra fields, so behavior is graceful degradation.
        //
        // TODO(Phase 05, post-Step 9): if the on-topic verification
        // in Step 9 shows the sender speaks off-topic reliably,
        // register a plugin-owned `_ne_VisitTurn` action via
        // SkyrimNetApi.RegisterAction (Papyrus) that consumes the
        // briefing argsJson and hands it to a prompt template we
        // ship in statics/.
        constexpr const char*   kSkyrimNetPluginTurnAction   = "StartConversation";

        constexpr std::uint32_t kStageSalutation      = 10;
        constexpr std::uint32_t kStageDiscuss         = 20;
        constexpr std::uint32_t kStageOnHold          = 25;
        constexpr std::uint32_t kStageReEngage        = 27;
        constexpr std::uint32_t kStageValediction     = 30;
        constexpr std::uint32_t kStageReturnHome      = 50;
        constexpr std::uint32_t kStageRollback        = 60;
        constexpr std::uint32_t kStageShutdown        = 200;

        // ---- Resolved engine handles (kDataLoaded) -----------------

        std::atomic<bool>       g_pointersResolved              = false;
        std::atomic<bool>       g_pointersCriticallyMissing     = false;
        RE::TESQuest*           g_visitQuest                     = nullptr;
        RE::TESFaction*         g_visitSenderFaction             = nullptr;
        RE::BGSRefAlias*        g_senderAlias                    = nullptr;
        RE::BGSRefAlias*        g_spawnMarkerAlias               = nullptr;
        RE::BGSRefAlias*        g_returnAnchorAlias              = nullptr;

        // ---- In-flight timing --------------------------------------
        //
        // Stamped by the compose callback once EnsureQuestStarted
        // succeeds; used by DetectAndRollbackFailedStart to measure
        // the Salutation window. Real-time seconds (steady_clock).
        // Zero = not yet stamped (compose in flight or start failed).
        std::atomic<double>     g_salutationEnteredAtRealSeconds = 0.0;

        // Throttle for the per-tick Salutation distance log (real-time
        // seconds). Zero = never logged; the log helper stamps on
        // every emission.
        std::atomic<double>     g_lastSalutationLogRealSeconds   = 0.0;

        // Set at the very top of Start (before the compose call fires)
        // and cleared once the compose callback has either succeeded
        // fully (EnsureQuestStarted returned true) or bailed out. Used
        // by DetectAndRollbackFailedStart to know the callback is
        // still mid-flight and shouldn't be treated as a failed start
        // even if the compose call outlives the composing flag's
        // window (unlikely, but the two flags together cover the
        // narrow race).
        std::atomic<bool>       g_startInProgress                = false;

        // ---- Small helpers -----------------------------------------

        double RealSecondsNow()
        {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count())
                / 1000.0;
        }

        // Same VM dispatch shape NPCLetterAction uses — args by value
        // (not forwarding-ref) so the deduced Args... are decayed and
        // MakeFunctionArguments' is_not_reference SFINAE passes.
        template <typename... Args>
        bool VMDispatchOnQuest(RE::TESQuest*    quest,
                               std::string_view scriptName,
                               std::string_view methodName,
                               Args...          args)
        {
            if (!quest) return false;
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return false;
            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) return false;
            const auto handle =
                policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest);
            auto* fnArgs = RE::MakeFunctionArguments(std::move(args)...);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return vm->DispatchMethodCall(
                handle,
                RE::BSFixedString(scriptName.data()),
                RE::BSFixedString(methodName.data()),
                fnArgs,
                callback);
        }

        bool VMDispatchQuestSetStage(RE::TESQuest* quest, std::uint32_t stage)
        {
            return VMDispatchOnQuest(
                quest, "Quest"sv, "SetStage"sv,
                static_cast<std::int32_t>(stage));
        }

        // Convenience wrapper — VM-dispatches the RunSenderAction
        // trampoline on `_ne_VisitQuest` with the two string args
        // (actionName + argsJson). See `_ne_VisitQuest.psc` for the
        // trampoline body.
        //
        // The BSFixedString conversion happens implicitly inside
        // MakeFunctionArguments; passing std::string by value gives
        // the deduced Args... type as std::string (decayed), which
        // MakeFunctionArguments accepts.
        bool VMDispatchRunSenderAction(RE::TESQuest*      quest,
                                        const std::string& actionName,
                                        const std::string& argsJson)
        {
            return VMDispatchOnQuest(
                quest, "_ne_VisitQuest"sv, "RunSenderAction"sv,
                RE::BSFixedString(actionName.c_str()),
                RE::BSFixedString(argsJson.c_str()));
        }

        // ---- Async poll helper -------------------------------------
        //
        // Mirrors NPCLetterAction's PollUntilOrTimeout. Polls the
        // main-thread predicate on `interval` cadence, running
        // onSuccess or onTimeout on the main thread when the predicate
        // returns true or `maxDuration` elapses.
        //
        // The worker thread runs the poll loop; each predicate
        // evaluation marshals to main and blocks on a promise so
        // engine access stays main-thread-only. The 5s safety timeout
        // on the future avoids hanging the worker if the main thread
        // wedges.
        void PollUntilOrTimeout(
            std::function<bool()>     predicate,
            std::function<void()>     onSuccess,
            std::function<void()>     onTimeout,
            std::chrono::milliseconds interval,
            std::chrono::milliseconds maxDuration,
            std::string               diagLabel = "")
        {
            AsyncDispatch::EnqueueWork(
                [predicate    = std::move(predicate),
                 onSuccess    = std::move(onSuccess),
                 onTimeout    = std::move(onTimeout),
                 interval, maxDuration,
                 diagLabel    = std::move(diagLabel)]() mutable {
                    const auto start = std::chrono::steady_clock::now();
                    while (true) {
                        auto promise = std::make_shared<std::promise<bool>>();
                        auto future  = promise->get_future();
                        AsyncDispatch::MarshalToMainThread(
                            [promise, &predicate]() {
                                bool ok = false;
                                try { ok = predicate(); } catch (...) { ok = false; }
                                promise->set_value(ok);
                            });

                        const auto status =
                            future.wait_for(std::chrono::seconds(5));
                        if (status != std::future_status::ready) {
                            logger::warn(
                                "NPCVisitAction::PollUntilOrTimeout[{}]: predicate "
                                "did not run within 5s; treating as timeout",
                                diagLabel);
                            AsyncDispatch::MarshalToMainThread(std::move(onTimeout));
                            return;
                        }
                        if (future.get()) {
                            AsyncDispatch::MarshalToMainThread(std::move(onSuccess));
                            return;
                        }
                        if (std::chrono::steady_clock::now() - start >= maxDuration) {
                            AsyncDispatch::MarshalToMainThread(std::move(onTimeout));
                            return;
                        }
                        std::this_thread::sleep_for(interval);
                    }
                });
        }

        // Visit-specific candidate viability filter used by
        // IsAvailable's cheap CountViable check. Kept in sync with
        // VisitComposer's filter so the count matches what Compose()
        // will end up building.
        bool VisitViabilityFilter_ForCountViable(RE::Actor* actor,
                                                  std::string* skipReasonOut)
        {
            if (!actor) {
                if (skipReasonOut) *skipReasonOut = "missing-actor";
                return false;
            }
            if (auto* base = actor->GetActorBase()) {
                if (!base->IsUnique()) {
                    if (skipReasonOut) *skipReasonOut = "not-unique";
                    return false;
                }
            }
            if (actor->IsInCombat()) {
                if (skipReasonOut) *skipReasonOut = "in-combat";
                return false;
            }
            if (actor->IsPlayerTeammate()) {
                if (skipReasonOut) *skipReasonOut = "player-follower";
                return false;
            }
            if (!actor->GetCurrentLocation()) {
                if (skipReasonOut) *skipReasonOut = "no-current-location";
                return false;
            }
            return true;
        }

        // ---- Faction bookkeeping -----------------------------------
        //
        // Same rank-0/rank-4 pattern NPCLetterAction uses on
        // `_ne_LetterSenderFaction`, applied to `_ne_VisitSenderFaction`.
        // Kept local because the letter action's helpers are file-scoped
        // there; refactoring both into a shared module is out of scope
        // for this phase.

        void SweepStaleDesignatedSenders(RE::TESFaction* fact,
                                          RE::Actor*      target)
        {
            if (!fact) return;
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl) return;

            std::size_t swept = 0;
            const auto walk = [&](const RE::BSTArray<RE::ActorHandle>& list) {
                for (const auto& h : list) {
                    auto ref = h.get();
                    auto* actor = ref.get();
                    if (!actor || actor == target) continue;
                    if (actor->GetFactionRank(fact, false) >= kSenderRankDesignated) {
                        actor->AddToFaction(fact, kSenderRankCandidate);
                        ++swept;
                        logger::info(
                            "NPCVisitAction[FACTION]: swept stale rank-{}+ "
                            "member 0x{:08X} -> rank {}",
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
            if (swept > 0) {
                logger::info(
                    "NPCVisitAction[FACTION]: pre-dispatch sweep demoted {} "
                    "stale rank-{}+ actor(s)",
                    swept, static_cast<int>(kSenderRankDesignated));
            }
        }

        void PromoteSenderToDesignated(RE::Actor* sender)
        {
            if (!g_visitSenderFaction || !sender) return;
            SweepStaleDesignatedSenders(g_visitSenderFaction, sender);

            const auto currentRank =
                sender->GetFactionRank(g_visitSenderFaction, false);
            if (currentRank == kSenderRankDesignated) {
                logger::info(
                    "NPCVisitAction[FACTION]: sender 0x{:08X} already at rank {}",
                    sender->GetFormID(),
                    static_cast<int>(kSenderRankDesignated));
                return;
            }
            sender->AddToFaction(g_visitSenderFaction, kSenderRankDesignated);
            logger::info(
                "NPCVisitAction[FACTION]: promoted sender 0x{:08X} from rank {} "
                "to rank {}",
                sender->GetFormID(), currentRank,
                static_cast<int>(kSenderRankDesignated));
        }

        void DemoteSenderToCandidate(RE::Actor* sender)
        {
            if (!g_visitSenderFaction || !sender) return;
            const auto currentRank =
                sender->GetFactionRank(g_visitSenderFaction, false);
            if (currentRank < 0) return;
            if (currentRank == kSenderRankCandidate) return;
            sender->AddToFaction(g_visitSenderFaction, kSenderRankCandidate);
            logger::info(
                "NPCVisitAction[FACTION]: demoted sender 0x{:08X} from rank {} "
                "back to rank {}",
                sender->GetFormID(), currentRank,
                static_cast<int>(kSenderRankCandidate));
        }

        // ---- Salutation watchdog helpers ---------------------------
        //
        // Build the argsJson payload for a plugin-owned turn per the
        // design's `ExecuteAction argsJson` shape. Reads the current
        // snapshot for topic / mood / briefing.
        std::string BuildTurnArgsJson(std::string_view turnKind,
                                       std::uint8_t     nudgeCount)
        {
            const auto snap = VisitState::GetSnapshot();
            nlohmann::json j;
            j["turn_kind"]   = turnKind;
            j["topic_tag"]   = snap.topicTag;
            j["mood"]        = snap.mood;
            j["briefing"]    = snap.briefingText;
            // `goal` is a separate field in the design; the composer
            // doesn't produce one distinct from the briefing yet, so
            // pass the briefing again — the sender's speech LLM can
            // treat briefing/goal as one input for now.
            j["goal"]        = snap.briefingText;
            j["nudge_count"] = nudgeCount;
            return j.dump();
        }

        // ---- Shared cross-block atomics ----------------------------
        //
        // Declared here (near the top of the anon namespace) so the
        // various watchdog / sink blocks below can reference each
        // other without extern/forward-declare gymnastics.

        // Terminal-shutdown latch — set once the visit's teardown
        // chain has advanced Stage 200 (either normal ReturnHome or
        // hard-abort). Consumed by DetectCompletion and the rollback
        // path.
        std::atomic<bool> g_terminalCleanupDone = false;

        // Hard-abort re-entry latch — HardAbortVisit is single-shot
        // per visit; multiple triggers landing near-simultaneously
        // still run teardown exactly once.
        std::atomic<bool> g_hardAbortFired = false;

        // Valediction re-entry latch — FireValediction fires exactly
        // once even if Stage 30 is entered twice.
        std::atomic<bool> g_valedictionFired = false;

        // OnHold combat-trigger timestamp (steady_clock seconds).
        // Zero = OnHold wasn't triggered by combat.
        std::atomic<double> g_onHoldCombatStartedAtRealSeconds = 0.0;

        // ReturnHome-entry wall-clock seconds. Consumed by the
        // ReturnHome watchdog's timeout branch.
        std::atomic<double> g_returnHomeStartedAtRealSeconds = 0.0;

        // Convenience flag for logging: true from OnHold entry
        // through ReEngage completion.
        std::atomic<bool> g_inDetour = false;

        // ---- Forward declarations for cross-block calls -------------
        void RegisterDiscussWatchdog(RE::FormID senderFormID);
        void RegisterReturnHomeWatchdog();
        void HardAbortVisit(const char* reason);
        bool CheckHardAbortConditions();

        // Register the 250ms Salutation watchdog. The poll returns
        // true if either (a) the sender got close enough to the
        // player to trigger the opening line, or (b) the quest is
        // no longer at Stage 10 (rolled back, or someone else
        // advanced it). Case (b) is a no-op in onSuccess — we
        // re-check the stage before firing the transition.
        //
        // The maxDuration equals `visitApproachTimeoutSeconds` so
        // the worker stops looping once the rollback path in
        // DetectAndRollbackFailedStart would have fired anyway.
        void RegisterSalutationWatchdog()
        {
            const int timeoutSeconds =
                std::max(1, Settings::Get().visitApproachTimeoutSeconds);
            const int approachDistance =
                std::max(1, Settings::Get().visitSalutationApproachDistanceUnits);

            PollUntilOrTimeout(
                // predicate — runs on main thread
                [approachDistance]() -> bool {
                    if (!g_visitQuest) return true;  // giving up
                    if (g_visitQuest->GetCurrentStageID() !=
                        static_cast<std::uint16_t>(kStageSalutation)) {
                        return true;  // stage changed under us
                    }
                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) return false;
                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    return dist <= static_cast<float>(approachDistance);
                },
                // onSuccess — runs on main thread
                []() {
                    if (!g_visitQuest) return;
                    if (g_visitQuest->GetCurrentStageID() !=
                        static_cast<std::uint16_t>(kStageSalutation)) {
                        // Rolled back or already advanced — nothing to do.
                        return;
                    }
                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) {
                        logger::warn(
                            "NPCVisitAction[SALUTATION]: onSuccess fired but "
                            "sender or player is null — leaving the DetectAndRollback "
                            "timeout to unwind");
                        return;
                    }
                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    logger::info(
                        "NPCVisitAction[SALUTATION]: sender-to-player distance {:.0f}u "
                        "reached — firing opening line and advancing to Discuss",
                        dist);

                    const auto argsJson = BuildTurnArgsJson("salutation", 0);
                    VMDispatchRunSenderAction(
                        g_visitQuest, kSkyrimNetPluginTurnAction, argsJson);
                    VMDispatchQuestSetStage(g_visitQuest, kStageDiscuss);

                    logger::info(
                        "NPCVisitAction: Salutation -> Discuss (executeAction "
                        "turn_kind=salutation, action='{}')",
                        kSkyrimNetPluginTurnAction);

                    // Arm the natural-conclusion poll and register
                    // the Discuss gate-tick watchdog. Snapshot is
                    // read from VisitState (populated by Start's
                    // compose callback).
                    const auto snap = VisitState::GetSnapshot();
                    VisitConclusionPoll::Arm(snap);
                    RegisterDiscussWatchdog(snap.senderFormID);
                },
                // onTimeout — do nothing; DetectAndRollbackFailedStart
                // owns the Salutation-timeout rollback branch.
                []() {
                    logger::debug(
                        "NPCVisitAction[SALUTATION]: watchdog worker exiting "
                        "(rollback path owns unwind)");
                },
                std::chrono::milliseconds(250),
                std::chrono::seconds(timeoutSeconds),
                "Salutation");
        }

        // ---- OnHold / ReEngage helpers (Step 14) -------------------
        //
        // OnHold and ReEngage are situational detours off the
        // Discuss main line. Entry conditions:
        //   * Player opened DialogueMenu (menu-open sink).
        //   * Player or sender entered combat (combat-event sink).
        //
        // Exit condition (checked live each OnHold tick): all of the
        // above are no longer true.
        //
        // The trigger sinks only fire the Stage 20 → 25 transition;
        // the OnHold watchdog polls live state each tick to decide
        // when to advance to Stage 27 (ReEngage). ReEngage watchdog
        // polls sender→player distance and fires the resumption line
        // + Stage 27 → 20 when the sender closes distance again.

        bool ObservePlayerInDialogue()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        bool ObserveAnyCombat()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && player->IsInCombat()) return true;
            if (auto* senderRef = g_senderAlias
                    ? g_senderAlias->GetReference() : nullptr) {
                if (auto* senderActor = senderRef->As<RE::Actor>()) {
                    if (senderActor->IsInCombat()) return true;
                }
            }
            return false;
        }

        // Forward-declared — RegisterOnHoldWatchdog calls
        // RegisterReEngageWatchdog on OnHold-clear.
        void RegisterReEngageWatchdog();

        void RegisterOnHoldWatchdog(const char* triggerReason)
        {
            const auto snap = VisitState::GetSnapshot();
            (void)snap; // reserved for future trigger-persistence

            logger::info(
                "NPCVisitAction: OnHold entry (trigger={})", triggerReason);
            g_inDetour.store(true);
            // Disarm the poll while we're paused — resumed at ReEngage
            // completion when Stage 20 is re-entered.
            VisitConclusionPoll::Disarm();

            const int combatMax =
                std::max(1, Settings::Get().visitOnHoldCombatMaxSeconds);
            const int hardTimeout =
                std::max(60, Settings::Get().visitHardTimeoutSeconds);

            PollUntilOrTimeout(
                // predicate — main thread
                [combatMax]() -> bool {
                    if (!g_visitQuest) return true;
                    if (CheckHardAbortConditions()) return true;
                    if (g_visitQuest->GetCurrentStageID() !=
                        static_cast<std::uint16_t>(kStageOnHold)) {
                        return true;
                    }
                    const bool inDialogue = ObservePlayerInDialogue();
                    const bool inCombat   = ObserveAnyCombat();
                    if (!inDialogue && !inCombat) {
                        logger::info(
                            "NPCVisitAction: OnHold triggers cleared, transitioning "
                            "to ReEngage");
                        VMDispatchQuestSetStage(g_visitQuest, kStageReEngage);
                        RegisterReEngageWatchdog();
                        return true;
                    }

                    // Combat-OnHold timeout — hard-abort.
                    const auto combatStart =
                        g_onHoldCombatStartedAtRealSeconds.load();
                    if (inCombat && combatStart > 0.0) {
                        const auto elapsed = RealSecondsNow() - combatStart;
                        if (elapsed >= static_cast<double>(combatMax)) {
                            HardAbortVisit("combat_stuck");
                            return true;
                        }
                    }
                    return false;
                },
                []() {
                    g_inDetour.store(false);
                    g_onHoldCombatStartedAtRealSeconds.store(0.0);
                },
                []() {
                    g_inDetour.store(false);
                    g_onHoldCombatStartedAtRealSeconds.store(0.0);
                    logger::warn(
                        "NPCVisitAction: OnHold watchdog outer timeout (safety net)");
                },
                std::chrono::milliseconds(500),
                std::chrono::seconds(hardTimeout),
                "OnHold");
        }

        void RegisterReEngageWatchdog()
        {
            const int approachDist =
                std::max(1, Settings::Get().visitReEngageApproachDistanceUnits);
            const int hardTimeout =
                std::max(60, Settings::Get().visitHardTimeoutSeconds);

            PollUntilOrTimeout(
                [approachDist]() -> bool {
                    if (!g_visitQuest) return true;
                    if (CheckHardAbortConditions()) return true;
                    const auto stage = g_visitQuest->GetCurrentStageID();
                    if (stage != static_cast<std::uint16_t>(kStageReEngage)) {
                        return true;
                    }

                    // Re-check OnHold triggers — if they re-trip
                    // during ReEngage, go back to Stage 25.
                    const bool inDialogue = ObservePlayerInDialogue();
                    const bool inCombat   = ObserveAnyCombat();
                    if (inDialogue || inCombat) {
                        logger::info(
                            "NPCVisitAction: ReEngage aborted — OnHold trigger "
                            "re-tripped ({}); returning to OnHold",
                            inDialogue ? "dialogue" : "combat");
                        VMDispatchQuestSetStage(g_visitQuest, kStageOnHold);
                        RegisterOnHoldWatchdog(
                            inDialogue ? "player_dialogue" : "combat");
                        return true;
                    }

                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) return false;

                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    if (dist <= static_cast<float>(approachDist)) {
                        logger::info(
                            "NPCVisitAction: ReEngage watchdog tripped ({:.0f}u <= "
                            "{}u), dispatching resumption line",
                            dist, approachDist);
                        const auto snap = VisitState::GetSnapshot();
                        const auto argsJson = BuildTurnArgsJson(
                            "reengage", snap.ignoreNudgeCount);
                        VMDispatchRunSenderAction(
                            g_visitQuest, kSkyrimNetPluginTurnAction, argsJson);
                        VMDispatchQuestSetStage(g_visitQuest, kStageDiscuss);
                        // Re-arm the poll for the resumed Discuss.
                        VisitConclusionPoll::Arm(snap);
                        // Re-register the Discuss watchdog so it
                        // picks up new speech turns.
                        RegisterDiscussWatchdog(snap.senderFormID);
                        return true;
                    }
                    return false;
                },
                []() {
                    g_inDetour.store(false);
                },
                []() {
                    g_inDetour.store(false);
                    logger::warn(
                        "NPCVisitAction: ReEngage watchdog outer timeout (safety net)");
                },
                std::chrono::milliseconds(250),
                std::chrono::seconds(hardTimeout),
                "ReEngage");
        }

        // ---- Detour trigger sinks (menu / combat) ------------------

        struct DialogueMenuOnHoldSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent*                a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*  /*src*/) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                if (a_event->menuName != RE::DialogueMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!a_event->opening) {
                    // Closing edge is picked up by the OnHold
                    // watchdog's live re-check.
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!g_visitQuest) return RE::BSEventNotifyControl::kContinue;
                if (g_visitQuest->GetCurrentStageID() !=
                    static_cast<std::uint16_t>(kStageDiscuss)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                VMDispatchQuestSetStage(g_visitQuest, kStageOnHold);
                RegisterOnHoldWatchdog("player_dialogue");
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct CombatOnHoldSink : public RE::BSTEventSink<RE::TESCombatEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCombatEvent*                a_event,
                RE::BSTEventSource<RE::TESCombatEvent>*  /*src*/) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                if (!g_visitQuest) return RE::BSEventNotifyControl::kContinue;
                if (g_visitQuest->GetCurrentStageID() !=
                    static_cast<std::uint16_t>(kStageDiscuss)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                // Only care about combat-entering transitions.
                // TESCombatEvent has newState = 0 (not combat), 1
                // (combat), 2 (searching). Fire OnHold on transitions
                // TO state 1 or 2.
                if (a_event->newState == RE::ACTOR_COMBAT_STATE::kNone) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!a_event->actor) return RE::BSEventNotifyControl::kContinue;

                auto* actor = a_event->actor->As<RE::Actor>();
                if (!actor) return RE::BSEventNotifyControl::kContinue;

                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* senderRef = g_senderAlias
                    ? g_senderAlias->GetReference() : nullptr;
                auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;

                const bool isRelevant =
                    (player && actor == player) ||
                    (senderActor && actor == senderActor);
                if (!isRelevant) return RE::BSEventNotifyControl::kContinue;

                const char* who = (actor == player) ? "player_combat"
                                                     : "sender_combat";
                g_onHoldCombatStartedAtRealSeconds.store(RealSecondsNow());
                VMDispatchQuestSetStage(g_visitQuest, kStageOnHold);
                RegisterOnHoldWatchdog(who);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        DialogueMenuOnHoldSink g_dialogueMenuSink;
        CombatOnHoldSink       g_combatSink;
        std::atomic<bool>      g_sinksRegistered = false;

        void RegisterOnHoldSinks()
        {
            if (g_sinksRegistered.exchange(true)) return;
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_dialogueMenuSink);
                logger::info(
                    "NPCVisitAction: MenuOpenCloseEvent sink registered "
                    "(DialogueMenu -> OnHold)");
            } else {
                logger::warn(
                    "NPCVisitAction: RE::UI singleton unavailable — DialogueMenu "
                    "OnHold sink not registered");
            }
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESCombatEvent>(&g_combatSink);
                logger::info(
                    "NPCVisitAction: TESCombatEvent sink registered "
                    "(combat -> OnHold)");
            } else {
                logger::warn(
                    "NPCVisitAction: ScriptEventSourceHolder unavailable — "
                    "combat OnHold sink not registered");
            }
        }

        // ---- Hard-abort (Step 15) ----------------------------------
        //
        // Unified teardown for the abort cases: sender/player death,
        // outer wall-clock timeout, combat-stuck, consecutive poll
        // failures. Skips the ReturnHome walk entirely; teleports
        // the sender home (if alive), demotes, SetStage(200), and
        // pushes an `aborted` history entry.
        //
        // Guarded so multiple triggers landing near-simultaneously
        // (e.g. sender_death right after outer_timeout) run
        // teardown exactly once. Latch atomic lives at the top of
        // the anon namespace.

        void HardAbortVisit(const char* reason)
        {
            if (g_hardAbortFired.exchange(true)) return;
            if (!g_visitQuest) return;

            const auto stage = g_visitQuest->GetCurrentStageID();
            logger::warn(
                "NPCVisitAction: hard-abort (reason={}, stage={})",
                reason, stage);

            // Disarm any live pollers / detour flags.
            VisitConclusionPoll::Disarm();
            g_inDetour.store(false);
            g_onHoldCombatStartedAtRealSeconds.store(0.0);

            auto* senderRef = g_senderAlias
                ? g_senderAlias->GetReference() : nullptr;
            auto* anchorRef = g_returnAnchorAlias
                ? g_returnAnchorAlias->GetReference() : nullptr;
            auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;

            if (senderActor && !senderActor->IsDead()) {
                if (anchorRef) {
                    senderActor->MoveTo(anchorRef);
                    logger::info(
                        "NPCVisitAction[HARD-ABORT]: teleported sender 0x{:08X} "
                        "to return anchor 0x{:08X}",
                        senderActor->GetFormID(), anchorRef->GetFormID());
                }
                senderActor->data.angle.z =
                    VisitState::GetSnapshot().returnAngleZ;
                DemoteSenderToCandidate(senderActor);
            } else if (senderActor) {
                logger::info(
                    "NPCVisitAction[HARD-ABORT]: sender dead; skipping teleport/demote");
            }

            VMDispatchQuestSetStage(g_visitQuest, kStageShutdown);

            // History entry with `aborted` outcome.
            {
                VisitState::HistoryEntry entry;
                const auto snap = VisitState::GetSnapshot();
                entry.dispatchedAt = snap.dispatchedAtRealSeconds;
                entry.topicTag     = snap.topicTag;
                entry.outcome      = VisitState::Outcome::Aborted;
                entry.durationSeconds =
                    snap.dispatchedAtRealSeconds > 0.0
                        ? (RealSecondsNow() - snap.dispatchedAtRealSeconds)
                        : 0.0;
                if (senderActor) {
                    if (auto* n = senderActor->GetName()) entry.senderName = n;
                }
                VisitState::PushHistory(std::move(entry));
            }

            g_terminalCleanupDone.store(true);

            // Give Papyrus Shutdown a moment then clear VisitState.
            AsyncDispatch::EnqueueWork([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                AsyncDispatch::MarshalToMainThread([]() {
                    VisitState::Reset();
                });
            });
        }

        struct VisitDeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESDeathEvent*                a_event,
                RE::BSTEventSource<RE::TESDeathEvent>*  /*src*/) override
            {
                if (!a_event || !a_event->actorDying) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!g_visitQuest) return RE::BSEventNotifyControl::kContinue;
                const auto stage = g_visitQuest->GetCurrentStageID();
                // Only care while the visit is running (any non-Idle
                // main-line stage). 0 / 60 / 200 = teardown running.
                if (stage == 0 || stage == 60 || stage == 200) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* dyingRefPtr = a_event->actorDying.get();
                if (!dyingRefPtr) return RE::BSEventNotifyControl::kContinue;

                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* senderRef = g_senderAlias
                    ? g_senderAlias->GetReference() : nullptr;

                if (player && dyingRefPtr == player) {
                    HardAbortVisit("player_death");
                } else if (senderRef && dyingRefPtr == senderRef) {
                    HardAbortVisit("sender_death");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        VisitDeathSink    g_deathSink;
        std::atomic<bool> g_deathSinkRegistered = false;

        void RegisterDeathSink()
        {
            if (g_deathSinkRegistered.exchange(true)) return;
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESDeathEvent>(&g_deathSink);
                logger::info(
                    "NPCVisitAction: TESDeathEvent sink registered "
                    "(sender/player death -> hard-abort)");
            } else {
                logger::warn(
                    "NPCVisitAction: ScriptEventSourceHolder unavailable — "
                    "TESDeathEvent sink not registered");
            }
        }

        // Cheap tick — checks the outer wall-clock hard-timeout and
        // the poll-broken threshold. Called from the Discuss and
        // OnHold watchdogs' predicate paths (both run each ~1s).
        // Returns true if hard-abort was fired.
        bool CheckHardAbortConditions()
        {
            if (g_hardAbortFired.load()) return true;
            if (!g_visitQuest) return false;
            const auto stage = g_visitQuest->GetCurrentStageID();
            if (stage == 0 || stage == 60 || stage == 200) return false;

            const auto snap = VisitState::GetSnapshot();
            const auto& cfg = Settings::Get();

            // Outer wall-clock timeout.
            if (snap.dispatchedAtRealSeconds > 0.0) {
                const auto elapsed =
                    RealSecondsNow() - snap.dispatchedAtRealSeconds;
                const auto limit =
                    static_cast<double>(std::max(60, cfg.visitHardTimeoutSeconds));
                if (elapsed >= limit) {
                    HardAbortVisit("outer_timeout");
                    return true;
                }
            }

            // Poll-broken threshold.
            const auto failures = VisitConclusionPoll::ConsecutivePollFailures();
            const auto cap = static_cast<std::uint32_t>(
                std::max(1, cfg.visitConclusionPollMaxConsecutiveFailures));
            if (failures >= cap) {
                HardAbortVisit("poll_broken");
                return true;
            }
            return false;
        }

        // ---- ReturnHome watchdog + shutdown (Step 13) --------------
        //
        // g_terminalCleanupDone and g_returnHomeStartedAtRealSeconds
        // live at the top of the anon namespace (shared with the
        // hard-abort path).

        // Push a history entry for the just-finished visit. Outcome
        // is `unsatisfied` if the sender left because the nudge cap
        // was reached (player ignored them); otherwise `completed`.
        // Called on the successful ReturnHome exit path.
        void PushCompletedHistory()
        {
            VisitState::HistoryEntry entry;
            const auto snap = VisitState::GetSnapshot();
            const int nudgeCap =
                std::max(1, Settings::Get().visitMaxIgnoreNudges);
            entry.dispatchedAt = snap.dispatchedAtRealSeconds;
            entry.topicTag     = snap.topicTag;
            entry.outcome      = (snap.ignoreNudgeCount >= nudgeCap)
                ? VisitState::Outcome::Unsatisfied
                : VisitState::Outcome::Completed;
            entry.durationSeconds =
                snap.dispatchedAtRealSeconds > 0.0
                    ? (RealSecondsNow() - snap.dispatchedAtRealSeconds)
                    : 0.0;
            if (auto* form = RE::TESForm::LookupByID(snap.senderFormID)) {
                if (auto* a = form->As<RE::Actor>()) {
                    if (auto* n = a->GetName()) entry.senderName = n;
                }
            }
            VisitState::PushHistory(std::move(entry));
        }

        // Terminal-shutdown chain for a successful ReturnHome exit.
        // Runs on the main thread.
        void RunReturnHomeShutdown(const char* triggerReason)
        {
            logger::info(
                "NPCVisitAction[RETURNHOME]: exit condition tripped ({}) — "
                "running shutdown chain",
                triggerReason);

            auto* senderRef = g_senderAlias
                ? g_senderAlias->GetReference() : nullptr;
            auto* anchorRef = g_returnAnchorAlias
                ? g_returnAnchorAlias->GetReference() : nullptr;
            auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;

            if (senderActor && !senderActor->IsDead()) {
                if (anchorRef) {
                    senderActor->MoveTo(anchorRef);
                    logger::info(
                        "NPCVisitAction[RETURNHOME]: teleported sender 0x{:08X} "
                        "to return anchor 0x{:08X}",
                        senderActor->GetFormID(), anchorRef->GetFormID());
                } else {
                    // Fallback: teleport to sender's current cell
                    // (no-op if same cell, but keeps behavior
                    // well-defined). Extremely rare — alias would
                    // have to have been externally invalidated.
                    senderActor->MoveTo(senderRef);
                    logger::warn(
                        "NPCVisitAction[RETURNHOME]: return anchor unresolved; "
                        "used self-MoveTo fallback for sender 0x{:08X}",
                        senderActor->GetFormID());
                }
                senderActor->data.angle.z =
                    VisitState::GetSnapshot().returnAngleZ;
                senderActor->EvaluatePackage();
                DemoteSenderToCandidate(senderActor);
            } else if (senderActor) {
                // Dead sender — skip teleport/demote (both would be
                // no-ops or worse). Anchor cleanup still runs via
                // Papyrus Shutdown fragment.
                logger::info(
                    "NPCVisitAction[RETURNHOME]: sender dead; skipping teleport/demote");
            }

            VMDispatchQuestSetStage(g_visitQuest, kStageShutdown);
            PushCompletedHistory();
            g_terminalCleanupDone.store(true);

            // Give the Papyrus VM ~100ms to run the Shutdown()
            // fragment (Disable+Delete anchor, Stop+Reset), then
            // clear VisitState.
            AsyncDispatch::EnqueueWork([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                AsyncDispatch::MarshalToMainThread([]() {
                    VisitState::Reset();
                });
            });
        }

        void RegisterReturnHomeWatchdog()
        {
            g_returnHomeStartedAtRealSeconds.store(RealSecondsNow());
            g_terminalCleanupDone.store(false);

            const int timeoutSec =
                std::max(1, Settings::Get().visitReturnHomeTimeoutSeconds);
            const int exitDist =
                std::max(1, Settings::Get().visitReturnHomeExitDistanceUnits);

            PollUntilOrTimeout(
                // predicate — main thread
                [exitDist, timeoutSec]() -> bool {
                    if (!g_visitQuest) return true;
                    if (g_visitQuest->GetCurrentStageID() !=
                        static_cast<std::uint16_t>(kStageReturnHome)) {
                        return true;
                    }
                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) return false;

                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    if (dist >= static_cast<float>(exitDist)) {
                        RunReturnHomeShutdown("distance");
                        return true;
                    }

                    if (auto* cell = senderRef->GetParentCell()) {
                        if (!cell->IsAttached()) {
                            RunReturnHomeShutdown("cell-unloaded");
                            return true;
                        }
                    }

                    const auto elapsed =
                        RealSecondsNow() -
                        g_returnHomeStartedAtRealSeconds.load();
                    if (elapsed >= static_cast<double>(timeoutSec)) {
                        RunReturnHomeShutdown("timeout");
                        return true;
                    }
                    return false;
                },
                // onSuccess — do nothing; RunReturnHomeShutdown
                // already ran from inside the predicate (a valid
                // pattern because the predicate is main-thread).
                []() {
                    logger::debug(
                        "NPCVisitAction[RETURNHOME]: watchdog exiting after shutdown");
                },
                // onTimeout — safety net. Duration is a hard cap
                // slightly above the watchdog's own timeout so this
                // usually never fires.
                []() {
                    logger::warn(
                        "NPCVisitAction[RETURNHOME]: watchdog outer safety-net timeout "
                        "hit (RunReturnHomeShutdown should have already fired)");
                    // Force shutdown so we don't leak an in-flight state.
                    RunReturnHomeShutdown("watchdog-safety-net");
                },
                std::chrono::milliseconds(500),
                std::chrono::seconds(
                    std::max(60, Settings::Get().visitReturnHomeTimeoutSeconds + 30)),
                "ReturnHome");
        }

        // ---- Valediction (Stage 30) handler (Step 12) --------------
        //
        // The verdict handler and nudge-cap path both funnel here
        // right after they VM-dispatch SetStage(30). Fires the
        // closing line via ExecuteAction with turn_kind=valediction
        // and nudge_count from the snapshot, then schedules a
        // one-shot dwell timer that VM-dispatches SetStage(50) to
        // begin ReturnHome.
        //
        // Guarded by g_valedictionFired (defined at the top of the
        // anon namespace) so a second entry to Stage 30 (should be
        // impossible in practice, but defensive) does not re-fire
        // the closing line.

        void FireValediction()
        {
            if (g_valedictionFired.exchange(true)) {
                logger::debug(
                    "NPCVisitAction[VALEDICTION]: already fired for this visit — "
                    "skipping duplicate trigger");
                return;
            }

            if (!g_visitQuest) {
                logger::warn(
                    "NPCVisitAction[VALEDICTION]: quest handle unresolved; "
                    "skipping trigger");
                return;
            }

            const auto snap = VisitState::GetSnapshot();
            logger::info(
                "NPCVisitAction: Valediction entry (nudge_count={})",
                snap.ignoreNudgeCount);

            const auto argsJson = BuildTurnArgsJson("valediction", snap.ignoreNudgeCount);
            VMDispatchRunSenderAction(
                g_visitQuest, kSkyrimNetPluginTurnAction, argsJson);
            logger::info(
                "NPCVisitAction[VALEDICTION]: RunSenderAction closing turn dispatched");

            const int dwellSeconds =
                std::max(1, Settings::Get().visitValedictionDwellSeconds);

            // One-shot delay via AsyncDispatch: worker sleeps
            // dwellSeconds, then marshals SetStage(50) back to main.
            AsyncDispatch::EnqueueWork([dwellSeconds]() {
                std::this_thread::sleep_for(std::chrono::seconds(dwellSeconds));
                AsyncDispatch::MarshalToMainThread([]() {
                    if (!g_visitQuest) return;
                    if (g_visitQuest->GetCurrentStageID() !=
                        static_cast<std::uint16_t>(kStageValediction)) {
                        // Stage moved out from under us (rollback,
                        // hard-abort). Nothing to do — the exit
                        // path already advanced past 30.
                        return;
                    }
                    logger::info(
                        "NPCVisitAction: Valediction dwell expired, advancing to "
                        "ReturnHome (stage 50)");
                    VMDispatchQuestSetStage(g_visitQuest, kStageReturnHome);
                    RegisterReturnHomeWatchdog();
                });
            });
        }

        // ---- Poll verdict handler (Step 11) ------------------------
        //
        // Runs on the main thread (marshaled from the poll's
        // SkyrimNet-worker callback). Three branches:
        //   * should_conclude=true → advance Stage 20 -> 30
        //     (Valediction). Disarm the poll.
        //   * should_conclude=false + silence-gate tripped → fire
        //     `ContinueConversation`, bump ignoreNudgeCount, persist.
        //     If ignoreNudgeCount >= visitMaxIgnoreNudges, force
        //     Valediction (SetStage(30)) so the sender leaves.
        //   * should_conclude=false + silence-gate NOT tripped →
        //     no-op; the exchange is alive.
        void HandleVisitPollVerdict(std::optional<VisitConclusionPoll::PollVerdict> verdict)
        {
            if (!verdict) {
                logger::warn("VisitPoll: parse failed");
                return;
            }
            logger::info(
                "VisitPoll: fired (verdict={}, rationale=\"{}\")",
                verdict->shouldConclude ? "true" : "false",
                verdict->rationale);

            // Guard: if we've already left Discuss for any reason
            // (rollback, hard-abort, someone else advancing), ignore
            // this late verdict.
            if (!g_visitQuest ||
                g_visitQuest->GetCurrentStageID() !=
                    static_cast<std::uint16_t>(kStageDiscuss)) {
                logger::debug(
                    "VisitPoll: verdict arrived after Discuss ended; discarding");
                return;
            }

            const auto& cfg = Settings::Get();

            if (verdict->shouldConclude) {
                logger::info(
                    "NPCVisitAction[DISCUSS]: verdict=concluded — advancing to Valediction");
                VMDispatchQuestSetStage(g_visitQuest, kStageValediction);
                VisitConclusionPoll::Disarm();
                FireValediction();
                return;
            }

            // should_conclude=false. Consult the silence gate.
            const double silence = VisitConclusionPoll::SilenceGameSeconds();
            const double silenceLimit =
                static_cast<double>(std::max(0, cfg.visitPollSilenceGameMinutes)) * 60.0;
            if (silenceLimit <= 0.0 || silence < silenceLimit) {
                logger::debug(
                    "NPCVisitAction[DISCUSS]: verdict=continue; silence gate "
                    "not tripped ({:.1f}s < {:.1f}s) — no nudge",
                    silence, silenceLimit);
                return;
            }

            // Silence gate tripped — fire ContinueConversation.
            auto snap = VisitState::GetSnapshot();
            const std::uint8_t nextNudge = static_cast<std::uint8_t>(
                std::min<int>(255, snap.ignoreNudgeCount + 1));
            snap.ignoreNudgeCount = nextNudge;
            VisitState::SetSnapshot(snap);
            logger::info(
                "NPCVisitAction[DISCUSS]: silence gate tripped ({:.1f}s >= {:.1f}s) — "
                "firing ContinueConversation (nudge #{})",
                silence, silenceLimit, nextNudge);
            VMDispatchRunSenderAction(g_visitQuest, "ContinueConversation", "");

            // Nudge cap — force Valediction with the accumulated
            // nudge_count so the closing line reads as frustrated.
            const int nudgeCap = std::max(1, cfg.visitMaxIgnoreNudges);
            if (nextNudge >= nudgeCap) {
                logger::info(
                    "NPCVisitAction[DISCUSS]: nudge cap reached ({} >= {}) — "
                    "forcing Valediction",
                    nextNudge, nudgeCap);
                VMDispatchQuestSetStage(g_visitQuest, kStageValediction);
                VisitConclusionPoll::Disarm();
                FireValediction();
            }
        }

        // ---- Discuss watchdog (gate tick + speech sampler) ---------
        //
        // Registered when Salutation -> Discuss fires. Fires every
        // `iVisitPollGateTickSeconds` (default 1s). On each tick:
        //   * Sample SkyrimNet's dialogue event history since the
        //     last check; call VisitConclusionPoll::RegisterSpeechTurn
        //     for each new turn from sender/player.
        //   * Call VisitConclusionPoll::GateTick. If it returns true,
        //     fire the LLM poll (async).
        //
        // Exit condition: quest stage no longer == 20. That covers
        // both natural conclusion (advance to 30) and any of the
        // OnHold / rollback / hard-abort side exits.

        // Local tracker so we don't double-count events across ticks.
        // Reset in RegisterDiscussWatchdog before the worker starts.
        std::atomic<double> g_lastSampledEventGameTime = 0.0;

        // How many recent events to pull per sampling tick. Small
        // enough to be cheap; large enough that a burst of quick
        // exchanges since the last tick doesn't overflow the window.
        constexpr int kDiscussSpeechSamplePerTick = 20;

        // Sample new dialogue turns since the last tick. Returns
        // true if at least one new turn was from the player — the
        // Step 11 verdict path uses this to reset the ignore-nudge
        // counter.
        bool SampleAndRegisterNewSpeechTurns(RE::FormID senderFormID)
        {
            if (!SkyrimNetAPI::IsAvailable()) return false;

            const auto raw = SkyrimNetAPI::GetRecentEvents(
                senderFormID, kDiscussSpeechSamplePerTick, "dialogue");
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) return false;

            auto* player = RE::PlayerCharacter::GetSingleton();
            const std::string playerName = player && player->GetName()
                ? std::string{player->GetName()} : std::string{};

            std::string senderName;
            if (auto* form = RE::TESForm::LookupByID(senderFormID)) {
                if (auto* a = form->As<RE::Actor>()) {
                    if (auto* n = a->GetName()) senderName = n;
                }
            }

            const double lastSeen = g_lastSampledEventGameTime.load();
            double maxSeen = lastSeen;
            int newCount = 0;
            bool sawPlayerTurn = false;

            for (const auto& entry : parsed) {
                if (!entry.is_object()) continue;
                double gameTime = 0.0;
                if (auto it = entry.find("gameTime");
                    it != entry.end() && it->is_number()) {
                    gameTime = it->get<double>();
                }
                if (gameTime <= lastSeen) continue;
                if (gameTime > maxSeen) maxSeen = gameTime;

                std::string speaker;
                if (auto it = entry.find("originatingActorName");
                    it != entry.end() && it->is_string()) {
                    speaker = it->get<std::string>();
                }
                // Only count turns from sender or player. Empty
                // speaker (rare) still counts — better to over-count
                // than miss a turn.
                if (!speaker.empty() &&
                    !senderName.empty() &&
                    !playerName.empty() &&
                    speaker != senderName && speaker != playerName) {
                    continue;
                }
                if (!speaker.empty() && !playerName.empty() &&
                    speaker == playerName) {
                    sawPlayerTurn = true;
                }
                VisitConclusionPoll::RegisterSpeechTurn();
                ++newCount;
            }
            if (maxSeen > lastSeen) {
                g_lastSampledEventGameTime.store(maxSeen);
            }
            if (newCount > 0 && Settings::Get().debugMode) {
                logger::debug(
                    "NPCVisitAction[DISCUSS]: sampled {} new dialogue turn(s) "
                    "(player_turn={})",
                    newCount, sawPlayerTurn);
            }
            return sawPlayerTurn;
        }

        // Reset the ignore-nudge counter and persist. Called by the
        // sampler when a player turn is observed.
        void ResetIgnoreNudgeCounter()
        {
            auto snap = VisitState::GetSnapshot();
            if (snap.ignoreNudgeCount == 0) return;
            logger::info(
                "NPCVisitAction[DISCUSS]: player spoke — resetting ignore-nudge counter "
                "from {} to 0",
                snap.ignoreNudgeCount);
            snap.ignoreNudgeCount = 0;
            VisitState::SetSnapshot(snap);
        }

        void RegisterDiscussWatchdog(RE::FormID senderFormID)
        {
            g_lastSampledEventGameTime.store(0.0);
            const int gateTickSeconds =
                std::max(1, Settings::Get().visitPollGateTickSeconds);
            // Discuss has no built-in real-time limit — only the
            // outer hard-timeout (Step 15) bounds it. Use a large
            // maxDuration so PollUntilOrTimeout's fallback is a
            // no-op; the predicate's stage-change branch is the
            // real exit condition.
            const auto maxDuration =
                std::chrono::seconds(std::max(60, Settings::Get().visitHardTimeoutSeconds));

            PollUntilOrTimeout(
                // predicate — runs main thread. Returns true when
                // Discuss ends (any stage change out of 20).
                [senderFormID]() -> bool {
                    if (!g_visitQuest) return true;
                    // Hard-abort check first — an outer-timeout or
                    // poll-broken trigger routes to Stage 200 and
                    // this predicate should exit.
                    if (CheckHardAbortConditions()) return true;
                    const auto stage = g_visitQuest->GetCurrentStageID();
                    if (stage != static_cast<std::uint16_t>(kStageDiscuss)) {
                        return true;
                    }
                    // Cheap-signal side effects during Discuss:
                    // sample new speech turns; if a player turn
                    // was among them, reset the ignore-nudge counter.
                    if (SampleAndRegisterNewSpeechTurns(senderFormID)) {
                        ResetIgnoreNudgeCounter();
                    }
                    if (VisitConclusionPoll::GateTick()) {
                        VisitConclusionPoll::FirePoll(
                            [](std::optional<VisitConclusionPoll::PollVerdict> verdict) {
                                AsyncDispatch::MarshalToMainThread(
                                    [verdict = std::move(verdict)]() mutable {
                                        HandleVisitPollVerdict(std::move(verdict));
                                    });
                            });
                    }
                    return false;
                },
                // onSuccess — Discuss ended. Disarm the poll cleanly.
                []() {
                    VisitConclusionPoll::Disarm();
                    logger::debug(
                        "NPCVisitAction[DISCUSS]: watchdog exiting (stage changed)");
                },
                // onTimeout — outer hard-timeout as a safety net.
                []() {
                    VisitConclusionPoll::Disarm();
                    logger::debug(
                        "NPCVisitAction[DISCUSS]: watchdog exiting (max duration)");
                },
                std::chrono::seconds(gateTickSeconds),
                maxDuration,
                "Discuss");
        }

        // ---- Compose callback (main thread, marshaled) -------------
        //
        // Invoked once the LLM round-trip finishes. On the failure
        // paths we clear the composing flag, wipe any partial
        // Snapshot, and hand completion back to the dispatcher via
        // CompleteAction. On the happy path we commit the snapshot,
        // promote the sender, and EnsureQuestStarted — the Papyrus
        // Stage 0 fragment then routes to Stage 10, which does the
        // MoveTo + EvaluatePackage.
        void HandleComposeResult(std::optional<VisitComposer::VisitBriefing> briefing)
        {
            auto finishWithFailure = [](const char* reason) {
                logger::warn("NPCVisitAction: Start failure — {}", reason);
                VisitConclusionPoll::Disarm();
                VisitState::Reset();
                VisitState::SetComposingSender(false);
                g_startInProgress.store(false);
                ActionDispatcher::CompleteAction("npc_visit");
            };

            if (!briefing) {
                finishWithFailure("VisitComposer returned no briefing");
                return;
            }

            // Sender must still resolve at callback time — the actor
            // could have been disabled / deleted between compose
            // start and callback in rare cases.
            auto* senderForm = RE::TESForm::LookupByID(briefing->senderNpcFormID);
            auto* sender = senderForm ? senderForm->As<RE::Actor>() : nullptr;
            if (!sender) {
                finishWithFailure("sender FormID no longer resolves to an Actor");
                return;
            }
            if (sender->IsDead()) {
                finishWithFailure("sender died during compose window");
                return;
            }

            // Snapshot the sender's pre-dispatch position/angle/cell so
            // ReturnHome can teleport them back after the visit ends.
            VisitState::Snapshot snap;
            snap.senderFormID    = sender->GetFormID();
            snap.returnPosition  = sender->GetPosition();
            snap.returnAngleZ    = sender->GetAngleZ();
            if (auto* parentCell = sender->GetParentCell()) {
                snap.returnCellFormID = parentCell->GetFormID();
            }
            snap.briefingText            = briefing->briefing;
            snap.topicTag                = briefing->topicTag;
            snap.mood                    = briefing->mood;
            snap.tags                    = briefing->tags;
            snap.dispatchedAtRealSeconds = RealSecondsNow();
            snap.ignoreNudgeCount        = 0;
            snap.consecutivePollFailures = 0;
            VisitState::SetSnapshot(snap);
            logger::info(
                "NPCVisitAction: snapshotted sender at ({:.1f},{:.1f},{:.1f}) "
                "in cell 0x{:08X}",
                snap.returnPosition.x, snap.returnPosition.y, snap.returnPosition.z,
                snap.returnCellFormID);

            // Faction promote — the Sender alias's FMR condition
            // (`GetFactionRank _ne_VisitSenderFaction >= 4`) matches
            // this actor during EnsureQuestStarted's alias-fill.
            PromoteSenderToDesignated(sender);

            // Start the quest natively. Sender fills via faction-rank
            // FMR; SpawnMarker fills via nearest-XMarkerHeading FMR;
            // ReturnAnchor spawns a new XMarkerHeading REFR at
            // Sender's position via `Create Reference to Object
            // At: Sender`.
            bool engineResult = false;
            const bool callOk =
                g_visitQuest->EnsureQuestStarted(engineResult, /*a_startNow=*/true);
            if (!callOk || !engineResult) {
                logger::warn(
                    "NPCVisitAction: EnsureQuestStarted failed "
                    "(callOk={}, engineResult={}) — demoting sender and rolling back",
                    callOk, engineResult);
                DemoteSenderToCandidate(sender);
                finishWithFailure("EnsureQuestStarted reported failure");
                return;
            }

            // Post-EnsureQuestStarted snapshot updates: capture the
            // spawned ReturnAnchor REFR's FormID so the dashboard
            // can display it (not load-bearing — the alias itself
            // holds the ref across saves).
            RE::FormID senderAliasFilledID = 0;
            RE::FormID spawnMarkerFilledID = 0;
            RE::FormID returnAnchorFilledID = 0;
            if (g_senderAlias) {
                if (auto* r = g_senderAlias->GetReference()) {
                    senderAliasFilledID = r->GetFormID();
                }
            }
            if (g_spawnMarkerAlias) {
                if (auto* r = g_spawnMarkerAlias->GetReference()) {
                    spawnMarkerFilledID = r->GetFormID();
                }
            }
            if (g_returnAnchorAlias) {
                if (auto* r = g_returnAnchorAlias->GetReference()) {
                    returnAnchorFilledID = r->GetFormID();
                }
            }
            snap.returnAnchorFormID = returnAnchorFilledID;
            VisitState::SetSnapshot(snap);

            logger::info(
                "NPCVisitAction: EnsureQuestStarted ok — Sender=0x{:08X}, "
                "SpawnMarker=0x{:08X}, ReturnAnchor=0x{:08X}",
                senderAliasFilledID, spawnMarkerFilledID, returnAnchorFilledID);

            // Sanity: warn if Sender fill picked someone other than
            // our designated target. Should never happen if the
            // pre-dispatch sweep worked.
            if (senderAliasFilledID != 0 &&
                senderAliasFilledID != sender->GetFormID()) {
                logger::warn(
                    "NPCVisitAction: Sender alias fill picked 0x{:08X} but we "
                    "designated 0x{:08X} — stale rank-{}+ member survived the "
                    "sweep?",
                    senderAliasFilledID, sender->GetFormID(),
                    static_cast<int>(kSenderRankDesignated));
            }

            // Stamp the Salutation window's clock BEFORE clearing the
            // composing flag / start-in-progress flag, so the poll
            // sees a fresh timestamp the moment those flags clear.
            g_salutationEnteredAtRealSeconds.store(RealSecondsNow());
            g_lastSalutationLogRealSeconds.store(0.0);
            VisitState::SetComposingSender(false);
            g_startInProgress.store(false);

            // Kick the Salutation distance watchdog. It polls at
            // 250ms and, on close-approach, fires RunSenderAction
            // (opening line) and advances Stage 10 -> 20. See
            // RegisterSalutationWatchdog for the details.
            RegisterSalutationWatchdog();
        }
    }

    // ---- NPCVisitAction_Init -------------------------------------

    namespace NPCVisitAction_Init
    {
        void Initialize()
        {
            if (g_pointersResolved.exchange(true)) return;

            bool ok = true;

            if (auto* form = RE::TESForm::LookupByEditorID(kVisitQuestEditorID)) {
                g_visitQuest = form->As<RE::TESQuest>();
            }
            if (!g_visitQuest) {
                logger::error(
                    "NPCVisitAction_Init: quest '{}' did not resolve — "
                    "IsAvailable will report false permanently",
                    kVisitQuestEditorID);
                ok = false;
            }

            if (auto* form = RE::TESForm::LookupByEditorID(kVisitFactionEditorID)) {
                g_visitSenderFaction = form->As<RE::TESFaction>();
            }
            if (!g_visitSenderFaction) {
                logger::error(
                    "NPCVisitAction_Init: sender faction '{}' did not resolve — "
                    "faction-mediated Sender alias fill is disabled",
                    kVisitFactionEditorID);
                ok = false;
            }

            if (g_visitQuest) {
                for (auto* a : g_visitQuest->aliases) {
                    if (!a) continue;
                    if (a->aliasName == kSenderAliasName) {
                        g_senderAlias = skyrim_cast<RE::BGSRefAlias*>(a);
                    } else if (a->aliasName == kSpawnMarkerAliasName) {
                        g_spawnMarkerAlias = skyrim_cast<RE::BGSRefAlias*>(a);
                    } else if (a->aliasName == kReturnAnchorAliasName) {
                        g_returnAnchorAlias = skyrim_cast<RE::BGSRefAlias*>(a);
                    }
                }
            }
            if (!g_senderAlias) {
                logger::error(
                    "NPCVisitAction_Init: '{}' alias '{}' did not resolve",
                    kVisitQuestEditorID, kSenderAliasName);
                ok = false;
            }
            if (!g_spawnMarkerAlias) {
                logger::error(
                    "NPCVisitAction_Init: '{}' alias '{}' did not resolve",
                    kVisitQuestEditorID, kSpawnMarkerAliasName);
                ok = false;
            }
            if (!g_returnAnchorAlias) {
                logger::error(
                    "NPCVisitAction_Init: '{}' alias '{}' did not resolve",
                    kVisitQuestEditorID, kReturnAnchorAliasName);
                ok = false;
            }

            g_pointersCriticallyMissing.store(!ok);

            if (ok) {
                logger::info(
                    "NPCVisitAction_Init: resolved quest=0x{:08X}, faction=0x{:08X}, "
                    "Sender/SpawnMarker/ReturnAnchor aliases bound",
                    g_visitQuest->GetFormID(),
                    g_visitSenderFaction->GetFormID());
                // Wire the Step 14 detour sinks (DialogueMenu +
                // combat) now that the quest handle is available for
                // the stage-gated triggers.
                RegisterOnHoldSinks();
                // Wire the Step 15 hard-abort death sink.
                RegisterDeathSink();
            } else {
                logger::error(
                    "NPCVisitAction_Init: one or more required forms missing — "
                    "NPCVisitAction disabled for the session");
            }
        }
    }

    // ---- IAction impl ------------------------------------------------

    std::string NPCVisitAction::Name() const
    {
        return "npc_visit";
    }

    std::string NPCVisitAction::Description() const
    {
        return
            "An NPC the player knows drops what they were doing and travels "
            "to the player's current location to speak in person. Best fit "
            "when the intended beat cannot survive being written down and "
            "folded into a letter — an urgent warning, an apology that needs "
            "eye contact, a confession, a threat delivered face-to-face — or "
            "when the sender's own state (grief, anger, love, contrition) "
            "demands they show up rather than write. Tone and polarity are "
            "driven by the generated content, so this action can serve "
            "either a raising direction (menacing / urgent visits) or a "
            "lowering direction (contrite / warm / mournful visits) "
            "depending on what the current phase calls for.\n"
            "\n"
            "Avoid when the player is in a dungeon, lair, jail cell, arena, "
            "or other cell where a stranger walking up would be jarring — "
            "the action already gates itself on those. Also avoid when the "
            "player has just received a letter or another visit; letting the "
            "cadence breathe between social beats reads more naturally.\n"
            "\n"
            "Prefer `npc_letter` when the beat could plausibly land on paper "
            "and reach the player at the courier's schedule. Prefer "
            "`npc_visit` when the sender needs to see the player's reaction, "
            "when the information is dangerous to write down, or when the "
            "situation is urgent and needs an answer now.\n"
            "\n"
            "Parameters:\n"
            "  - `urgency_hint` (optional, string): `low` / `medium` / "
            "`high`. Defaults to `medium`. One input among several to the "
            "brief-composition prompt; not a hard directive.\n"
            "\n"
            "Do NOT include other parameter fields — sender, briefing, "
            "topic, mood, and tags are decided by the action's own "
            "compose LLM call. Extra fields will be silently ignored.";
    }

    ActionPolarity NPCVisitAction::Polarity() const
    {
        return ActionPolarity::Either;
    }

    bool NPCVisitAction::IsAvailable(const ActionContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("NPCVisitAction::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        if (g_pointersCriticallyMissing.load()) {
            return blocked("critical forms missing at kDataLoaded");
        }

        // Location gate — dungeons/lairs/jails/arenas/barracks.
        if (ctx.player) {
            if (LocationKeywords::IsVisitHostile(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsVisitHostile");
            }
        }

        if (!SkyrimNetAPI::IsMemorySystemReady()) {
            return blocked("SkyrimNet memory system not ready");
        }

        // Candidate-count check — cheap CountViable walk (no per-
        // candidate memory fetch). Uses the same viability filter as
        // the compose path so the count matches what Compose() will
        // actually see.
        const std::size_t minCandidates =
            static_cast<std::size_t>(
                std::max(1, Settings::Get().visitMinSenderCandidates));
        const std::size_t viable = SenderCandidatePool::CountViable(
            &VisitViabilityFilter_ForCountViable, minCandidates);
        if (viable < minCandidates) {
            if (debug) {
                logger::debug(
                    "NPCVisitAction::IsAvailable: blocked (only {} viable "
                    "candidates, need {})",
                    viable, minCandidates);
            }
            return false;
        }

        return true;
    }

    StartResult NPCVisitAction::Start(const ActionContext&  ctx,
                                       const nlohmann::json& parameters)
    {
        // Re-validate — the dispatcher's ~250ms gap between candidate
        // manifest and Start can invalidate IsAvailable (player
        // entered a jail, sender pool shrank below floor, etc.).
        if (!IsAvailable(ctx)) {
            return {false, "preconditions failed at start time"};
        }

        // In-flight re-entrancy guard. DerivePhase reads both the
        // composing flag and the quest stage; either being non-Idle
        // means someone else's Start callback is still running or the
        // quest is mid-lifecycle.
        if (VisitState::DerivePhase() != VisitState::Mode::Idle) {
            return {false, "already in flight"};
        }
        if (g_startInProgress.load()) {
            return {false, "already in flight (start callback pending)"};
        }
        if (g_visitQuest && g_visitQuest->GetCurrentStageID() > 0) {
            return {false, "quest still cleaning up from a prior dispatch"};
        }

        // Decode urgency_hint (low / medium / high; default medium).
        VisitComposer::UrgencyHint urgency = VisitComposer::UrgencyHint::Medium;
        if (parameters.is_object()) {
            if (auto it = parameters.find("urgency_hint");
                it != parameters.end() && it->is_string()) {
                const auto v = it->get<std::string>();
                if      (v == "low")  urgency = VisitComposer::UrgencyHint::Low;
                else if (v == "high") urgency = VisitComposer::UrgencyHint::High;
            }
        }

        // Enter Composing pseudo-state — DerivePhase will report
        // Mode::Composing, and DetectAndRollbackFailedStart will
        // return false until the flag clears.
        VisitState::Reset();
        VisitState::SetComposingSender(true);
        g_startInProgress.store(true);
        g_salutationEnteredAtRealSeconds.store(0.0);
        g_lastSalutationLogRealSeconds.store(0.0);
        g_valedictionFired.store(false);
        g_hardAbortFired.store(false);

        logger::info("NPCVisitAction: composing briefing (urgency={})",
                     urgency == VisitComposer::UrgencyHint::High   ? "high"
                     : urgency == VisitComposer::UrgencyHint::Low  ? "low"
                                                                    : "medium");

        VisitComposer::Compose(
            ctx, urgency,
            [](std::optional<VisitComposer::VisitBriefing> briefing) {
                AsyncDispatch::MarshalToMainThread(
                    [briefing = std::move(briefing)]() mutable {
                        HandleComposeResult(std::move(briefing));
                    });
            });

        return {true, "composing briefing"};
    }

    bool NPCVisitAction::DetectAndRollbackFailedStart(
        const ActionContext& /*ctx*/, double /*secondsSinceStart*/)
    {
        // No cached quest / no in-flight state — nothing to check.
        if (!g_visitQuest) return false;

        // Compose still in flight, or the start callback is mid-
        // execution: both are the "committed to running" phase from
        // the dispatcher's perspective, but we haven't yet reached
        // Stage 10 so there's nothing to time out. HandleComposeResult
        // owns its own failure paths.
        if (VisitState::GetComposingSender()) return false;
        if (g_startInProgress.load()) return false;

        // Only Salutation (Stage 10) gets the timeout rollback here.
        // Post-Salutation stages (Discuss / OnHold / ReEngage /
        // Valediction / ReturnHome) are Step 11+/13's job.
        const auto stage = g_visitQuest->GetCurrentStageID();
        if (stage != static_cast<std::uint16_t>(kStageSalutation)) return false;

        const auto enteredAt = g_salutationEnteredAtRealSeconds.load();
        if (enteredAt <= 0.0) {
            // Stage 10 is set but the callback didn't get to stamp
            // the timer — unlikely (a save/load could technically
            // land here). Re-arm from now so the next poll has a
            // baseline.
            g_salutationEnteredAtRealSeconds.store(RealSecondsNow());
            return false;
        }

        const auto now     = RealSecondsNow();
        const auto elapsed = now - enteredAt;
        const auto timeout =
            static_cast<double>(std::max(1, Settings::Get().visitApproachTimeoutSeconds));

        // Within the window — throttled distance log so we can see
        // the sender approaching without spamming the log. The real
        // Salutation → Discuss transition wires in at Step 9; for
        // now we just watch the distance shrink.
        if (elapsed < timeout) {
            const auto lastLog = g_lastSalutationLogRealSeconds.load();
            if (now - lastLog >= 5.0) {
                g_lastSalutationLogRealSeconds.store(now);
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                float dist = -1.0f;
                if (player && senderRef) {
                    dist = senderRef->GetPosition().GetDistance(player->GetPosition());
                }
                logger::info(
                    "NPCVisitAction[SALUTATION]: elapsed={:.1f}s, sender-to-player "
                    "distance={:.0f}u (timeout at {:.0f}s)",
                    elapsed, dist, timeout);
            }
            return false;
        }

        // Timeout hit — sender never got close enough to trigger the
        // Salutation → Discuss transition (drew into combat, package
        // didn't bind, player fast-traveled, etc.). Roll back:
        // teleport sender to the return anchor, demote, route the
        // quest through Stage 60 → 200 (Papyrus Shutdown disables +
        // deletes the anchor + Stop/Reset the quest).
        logger::warn(
            "NPCVisitAction[SALUTATION]: timeout at {:.1f}s (limit {:.0f}s) — "
            "rolling back",
            elapsed, timeout);

        auto* senderRef  = g_senderAlias      ? g_senderAlias->GetReference()      : nullptr;
        auto* anchorRef  = g_returnAnchorAlias ? g_returnAnchorAlias->GetReference() : nullptr;
        auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;

        if (senderActor && anchorRef) {
            senderActor->MoveTo(anchorRef);
            // TESObjectREFR::data.angle is a public field on the base
            // (see RE/T/TESObjectREFR.h::OBJ_REFR); no SetAngleZ
            // helper exists in CommonLibSSE-NG. The change is visible
            // once the actor's 3D is re-attached (next tick).
            senderActor->data.angle.z = VisitState::GetSnapshot().returnAngleZ;
            logger::info(
                "NPCVisitAction[SALUTATION]: teleported sender 0x{:08X} to "
                "return anchor 0x{:08X}",
                senderActor->GetFormID(), anchorRef->GetFormID());
        } else if (senderActor) {
            logger::warn(
                "NPCVisitAction[SALUTATION]: return anchor unresolved; skipping "
                "teleport (sender=0x{:08X})",
                senderActor->GetFormID());
        }

        if (senderActor) {
            DemoteSenderToCandidate(senderActor);
        }

        VMDispatchQuestSetStage(g_visitQuest, kStageRollback);
        // Disarm the poll if it happens to be armed (shouldn't at
        // Stage 10, but defensive).
        VisitConclusionPoll::Disarm();
        // Rollback path is also a "cleanup done" signal —
        // DetectCompletion checks this same flag.
        g_terminalCleanupDone.store(true);

        // History entry for the dashboard's recent ring.
        {
            VisitState::HistoryEntry entry;
            const auto snap = VisitState::GetSnapshot();
            entry.dispatchedAt   = snap.dispatchedAtRealSeconds;
            entry.senderName     = senderActor ? senderActor->GetName() : "";
            entry.topicTag       = snap.topicTag;
            entry.outcome        = VisitState::Outcome::RolledBack;
            entry.durationSeconds =
                snap.dispatchedAtRealSeconds > 0.0
                    ? (RealSecondsNow() - snap.dispatchedAtRealSeconds)
                    : 0.0;
            VisitState::PushHistory(std::move(entry));
        }

        VisitState::Reset();
        g_salutationEnteredAtRealSeconds.store(0.0);
        g_lastSalutationLogRealSeconds.store(0.0);
        return true;
    }

    bool NPCVisitAction::DetectCompletion(const ActionContext& /*ctx*/,
                                           double /*secondsSinceStart*/)
    {
        // Only signal completion once the shutdown chain has fully
        // run — Papyrus Shutdown() cleared alias fills and the
        // quest is back at Stage 0. g_terminalCleanupDone is set
        // by RunReturnHomeShutdown (success) or the Salutation
        // rollback path (failure).
        if (!g_terminalCleanupDone.load()) return false;
        if (!g_visitQuest) return false;

        // Wait for the Papyrus Shutdown fragment to actually run
        // and drop the stage back to 0. Without this check we'd
        // signal completion while stage is still 200 (mid-shutdown)
        // and the dispatcher could try a fresh Start before Reset()
        // clears the alias fills.
        const auto stage = g_visitQuest->GetCurrentStageID();
        if (stage != 0) return false;

        // All clear — clear the flag so the next dispatch starts
        // from a clean state and signal completion.
        g_terminalCleanupDone.store(false);
        logger::info("NPCVisitAction: DetectCompletion — visit fully unwound");
        return true;
    }
}
