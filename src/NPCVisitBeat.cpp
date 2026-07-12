#include <NPCVisitBeat.h>

#include <AsyncDispatch.h>
#include <BeatParamHelpers.h>
#include <BeatUtils.h>
#include <CameraVisibility.h>
#include <EngineUtils.h>
#include <FactionDesignationUtils.h>
#include <LocationKeywords.h>
#include <QuestUtils.h>
#include <SenderCandidatePool.h>
#include <SenderCooldownTable.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <VisitComposer.h>
#include <VisitConclusionPoll.h>
#include <VisitState.h>
#include <logger.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/B/BGSBaseAlias.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/B/BSFixedString.h>
#include <RE/C/Calendar.h>
#include <RE/D/DialogueMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/T/TESCombatEvent.h>
#include <RE/T/TESDeathEvent.h>
#include <RE/T/TESFaction.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>
#include <RE/U/UI.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
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

        constexpr std::int8_t   kSenderRankCandidate  = 0;
        constexpr std::int8_t   kSenderRankDesignated = 4;

        constexpr std::uint32_t kStageSalutation  = 10;
        constexpr std::uint32_t kStageDiscuss     = 20;
        constexpr std::uint32_t kStageOnHold      = 25;
        constexpr std::uint32_t kStageReEngage    = 27;
        constexpr std::uint32_t kStageValediction = 30;
        constexpr std::uint32_t kStageReturnHome  = 50;
        constexpr std::uint32_t kStageRollback    = 60;
        constexpr std::uint32_t kStageShutdown    = 200;

        // ---- Resolved engine handles (kDataLoaded) -----------------

        std::atomic<bool>       g_pointersResolved              = false;
        std::atomic<bool>       g_pointersCriticallyMissing     = false;
        RE::TESQuest*           g_visitQuest                     = nullptr;
        RE::TESFaction*         g_visitSenderFaction             = nullptr;
        RE::BGSRefAlias*        g_senderAlias                    = nullptr;
        RE::BGSRefAlias*        g_spawnMarkerAlias               = nullptr;
        RE::BGSRefAlias*        g_returnAnchorAlias              = nullptr;

        // ---- Small helpers -----------------------------------------

        double RealSecondsNow()
        {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count())
                / 1000.0;
        }

        bool VMDispatchRunSenderAction(RE::TESQuest*      quest,
                                        const std::string& actionName,
                                        const std::string& argsJson)
        {
            return QuestUtils::VMDispatchOnQuest(
                quest, "_ne_VisitQuest"sv, "RunSenderAction"sv,
                RE::BSFixedString(actionName.c_str()),
                RE::BSFixedString(argsJson.c_str()));
        }

        bool VMDispatchRunSenderNarration(RE::TESQuest*      quest,
                                           const std::string& content)
        {
            return QuestUtils::VMDispatchOnQuest(
                quest, "_ne_VisitQuest"sv, "RunSenderNarration"sv,
                RE::BSFixedString(content.c_str()));
        }

        bool VMDispatchRunSenderSilentSceneEvent(RE::TESQuest*      quest,
                                                  const std::string& content)
        {
            return QuestUtils::VMDispatchOnQuest(
                quest, "_ne_VisitQuest"sv, "RunSenderSilentSceneEvent"sv,
                RE::BSFixedString(content.c_str()));
        }

        // Per-sender cooldowns — persisted via NPCVisitBeat_Persistence 'NBVS'.
        SenderCooldownTable g_senderCooldowns;

        // ---- Session state (not persisted; reset by OnStart / OnRevert)
        //
        // The beat's COMPOSE arm runs through a sub-state machine driven
        // by atomic flags flipped from marshaled main-thread tasks. The
        // RUNNING arm dispatches by quest stage each Normal-mode Tick
        // through a single main-thread stage-tick task.

        enum class ComposeSubPhase : std::uint8_t
        {
            Start,
            ComposingLLM,
            LLMResultReady,
            Dispatching,
            Succeeded,   // -> RUNNING
            Failed,      // -> CLEANUP
        };

        std::mutex        g_sessionMutex;
        BeatUtils::ComposeSubPhaseMachine<ComposeSubPhase>
            g_subPhase{ComposeSubPhase::Start};

        RE::FormID                     g_paramSenderFormID = 0;
        BeatParamHelpers::UrgencyHint  g_paramUrgency =
            BeatParamHelpers::UrgencyHint::Medium;
        std::string                    g_paramJustification;

        // Session flags — atomics used across the worker Tick and the
        // marshaled main-thread tasks.
        std::atomic<bool>   g_hardAbortFired = false;
        std::atomic<bool>   g_valedictionFired = false;
        std::atomic<bool>   g_terminalCleanupDone = false;
        std::atomic<double> g_salutationEnteredAtRealSeconds = 0.0;
        std::atomic<double> g_valedictionEnteredAtRealSeconds = 0.0;
        std::atomic<double> g_returnHomeStartedAtRealSeconds = 0.0;
        std::atomic<double> g_lastDistanceLogRealSeconds = 0.0;
        std::atomic<double> g_onHoldCombatStartedAtRealSeconds = 0.0;

        // Combat-stuck / poll_broken hard-abort reason surface, set by
        // the sink or the abort helper before HardAbort teardown runs.
        std::mutex          g_hardAbortReasonMutex;
        std::string         g_hardAbortReason;

        // Discuss sampler cursor and sender FormID cached at
        // Salutation → Discuss.
        std::atomic<double>     g_lastSampledEventGameTime = 0.0;
        std::atomic<RE::FormID> g_discussSenderFormID = 0;

        // RUNNING tick cadence — one marshaled main-thread stage tick
        // every kRunningCheckEveryNTicks worker ticks (4 = ~1s at
        // 250ms).
        constexpr int kRunningCheckEveryNTicks = 4;
        int g_runningTickCount = 0;
        std::atomic<bool> g_runningTaskInFlight = false;

        // Discuss speech sampler recent-lines cap.
        constexpr int kDiscussSpeechSamplePerTick = 20;

        void ResetSessionState()
        {
            {
                std::scoped_lock lock(g_sessionMutex);
                g_paramSenderFormID = 0;
                g_paramUrgency = BeatParamHelpers::UrgencyHint::Medium;
                g_paramJustification.clear();
            }
            g_subPhase.Reset();
            g_hardAbortFired.store(false, std::memory_order_release);
            g_valedictionFired.store(false, std::memory_order_release);
            g_terminalCleanupDone.store(false, std::memory_order_release);
            g_salutationEnteredAtRealSeconds.store(0.0, std::memory_order_release);
            g_valedictionEnteredAtRealSeconds.store(0.0, std::memory_order_release);
            g_returnHomeStartedAtRealSeconds.store(0.0, std::memory_order_release);
            g_lastDistanceLogRealSeconds.store(0.0, std::memory_order_release);
            g_onHoldCombatStartedAtRealSeconds.store(0.0, std::memory_order_release);
            g_lastSampledEventGameTime.store(0.0, std::memory_order_release);
            g_discussSenderFormID.store(0, std::memory_order_release);
            g_runningTickCount = 0;
            g_runningTaskInFlight.store(false, std::memory_order_release);
            {
                std::scoped_lock lock(g_hardAbortReasonMutex);
                g_hardAbortReason.clear();
            }
        }

        // Transition the sub-phase machine. On a Failed transition,
        // records `reason` as the failure-reason string; on any other
        // transition, `reason` is ignored.
        void SetSubPhase(ComposeSubPhase phase, const char* reason = nullptr)
        {
            if (reason && phase == ComposeSubPhase::Failed) {
                g_subPhase.Fail(phase, reason);
            } else {
                g_subPhase.Set(phase);
            }
        }

        // ---- Sender turn / event bookkeeping shared with sinks -----

        bool ObservePlayerInDialogue()
        {
            return EngineUtils::IsPlayerInDialogue();
        }

        bool ObserveAnyCombat()
        {
            if (EngineUtils::IsPlayerInCombat()) return true;
            if (auto* senderRef = g_senderAlias
                    ? g_senderAlias->GetReference() : nullptr) {
                if (auto* senderActor = senderRef->As<RE::Actor>()) {
                    if (senderActor->IsInCombat()) return true;
                }
            }
            return false;
        }

        std::string BuildTurnArgsJson(std::string_view turnKind,
                                       std::uint8_t     nudgeCount)
        {
            const auto snap = VisitState::GetSnapshot();

            auto* player = RE::PlayerCharacter::GetSingleton();
            std::string playerName;
            if (player) {
                if (const auto* n = player->GetName()) playerName = n;
            }
            std::string senderName;
            if (auto* form = RE::TESForm::LookupByID(snap.senderFormID)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    if (const auto* n = actor->GetName()) senderName = n;
                }
            }
            nlohmann::json j;
            j["speaker"]     = senderName;
            j["target"]      = playerName;
            j["topic"]       = snap.topicTag;
            j["turn_kind"]   = turnKind;
            j["topic_tag"]   = snap.topicTag;
            j["mood"]        = snap.mood;
            j["briefing"]    = snap.briefingText;
            j["goal"]        = snap.briefingText;
            j["nudge_count"] = nudgeCount;
            return j.dump();
        }

        void PromoteSenderToDesignated(RE::Actor* sender)
        {
            FactionDesignationUtils::PromoteToDesignated(
                g_visitSenderFaction, sender,
                kSenderRankDesignated, kSenderRankCandidate,
                "NPCVisitBeat");
        }

        void DemoteSenderToCandidate(RE::Actor* sender)
        {
            FactionDesignationUtils::DemoteToCandidate(
                g_visitSenderFaction, sender,
                kSenderRankCandidate, "NPCVisitBeat");
        }

        // Visit-specific candidate viability filter used by
        // IsAvailable's cheap CountViable walk. Kept in sync with
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

        // ---- Hard-abort helper -------------------------------------

        void HardAbortVisit(const char* reason);

        bool CheckHardAbortConditions()
        {
            if (g_hardAbortFired.load()) return true;
            if (!g_visitQuest) return false;
            const auto stage = g_visitQuest->GetCurrentStageID();
            if (stage == 0 || stage == kStageRollback ||
                stage == kStageShutdown) return false;

            const auto& cfg = Settings::Get();
            const auto failures = VisitConclusionPoll::ConsecutivePollFailures();
            const auto cap = static_cast<std::uint32_t>(
                std::max(1, cfg.visitConclusionPollMaxConsecutiveFailures));
            if (failures >= cap) {
                logger::warn(
                    "NPCVisitBeat[HARD-ABORT-CHECK]: poll_broken — "
                    "consecutivePollFailures={} >= cap={}",
                    failures, cap);
                HardAbortVisit("poll_broken");
                return true;
            }
            return false;
        }

        // ---- Sinks (main-thread; fire stage transitions) -----------

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
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!g_visitQuest) return RE::BSEventNotifyControl::kContinue;
                const auto stage = g_visitQuest->GetCurrentStageID();
                if (stage != static_cast<std::uint16_t>(kStageDiscuss)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                logger::info(
                    "NPCVisitBeat[SINK]: DialogueMenu opened during Discuss — "
                    "dispatching SetStage(OnHold)");
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageOnHold);
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
                const auto stage = g_visitQuest->GetCurrentStageID();
                if (stage != static_cast<std::uint16_t>(kStageDiscuss)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
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
                logger::info(
                    "NPCVisitBeat[SINK]: {} entered combat during Discuss — "
                    "dispatching SetStage(OnHold)",
                    who);
                g_onHoldCombatStartedAtRealSeconds.store(RealSecondsNow());
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageOnHold);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

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
                if (stage == 0 || stage == kStageRollback ||
                    stage == kStageShutdown)
                {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* dyingRefPtr = a_event->actorDying.get();
                if (!dyingRefPtr) return RE::BSEventNotifyControl::kContinue;
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* senderRef = g_senderAlias
                    ? g_senderAlias->GetReference() : nullptr;
                if (player && dyingRefPtr == player) {
                    logger::warn(
                        "NPCVisitBeat[SINK]: player death observed during visit "
                        "(stage={}) — triggering hard-abort", stage);
                    HardAbortVisit("player_death");
                } else if (senderRef && dyingRefPtr == senderRef) {
                    logger::warn(
                        "NPCVisitBeat[SINK]: sender death observed during visit "
                        "(stage={}, sender=0x{:08X}) — triggering hard-abort",
                        stage, dyingRefPtr->GetFormID());
                    HardAbortVisit("sender_death");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        DialogueMenuOnHoldSink g_dialogueMenuSink;
        CombatOnHoldSink       g_combatSink;
        VisitDeathSink         g_deathSink;
        std::atomic<bool>      g_sinksRegistered = false;

        void RegisterSinks()
        {
            if (g_sinksRegistered.exchange(true)) return;
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_dialogueMenuSink);
                logger::info(
                    "NPCVisitBeat: MenuOpenCloseEvent sink registered "
                    "(DialogueMenu -> OnHold)");
            }
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESCombatEvent>(&g_combatSink);
                holder->AddEventSink<RE::TESDeathEvent>(&g_deathSink);
                logger::info(
                    "NPCVisitBeat: combat + death sinks registered");
            } else {
                logger::warn(
                    "NPCVisitBeat: ScriptEventSourceHolder unavailable — "
                    "combat / death sinks NOT registered");
            }
        }

        // ---- Push history + hard-abort teardown --------------------

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

        void PushRolledBackHistory(RE::Actor* senderActor)
        {
            VisitState::HistoryEntry entry;
            const auto snap = VisitState::GetSnapshot();
            entry.dispatchedAt = snap.dispatchedAtRealSeconds;
            entry.senderName   = senderActor ? senderActor->GetName() : "";
            entry.topicTag     = snap.topicTag;
            entry.outcome      = VisitState::Outcome::RolledBack;
            entry.durationSeconds =
                snap.dispatchedAtRealSeconds > 0.0
                    ? (RealSecondsNow() - snap.dispatchedAtRealSeconds)
                    : 0.0;
            VisitState::PushHistory(std::move(entry));
        }

        void PushAbortedHistory(RE::Actor* senderActor)
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

        void HardAbortVisit(const char* reason)
        {
            if (g_hardAbortFired.exchange(true)) {
                logger::debug(
                    "NPCVisitBeat[HARD-ABORT]: already fired for this visit "
                    "(second trigger reason='{}' ignored)", reason);
                return;
            }
            if (!g_visitQuest) {
                logger::warn(
                    "NPCVisitBeat[HARD-ABORT]: quest handle null (reason='{}')",
                    reason);
                return;
            }
            const auto stage = g_visitQuest->GetCurrentStageID();
            logger::warn(
                "NPCVisitBeat: hard-abort (reason={}, stage={})", reason, stage);
            {
                std::scoped_lock lock(g_hardAbortReasonMutex);
                g_hardAbortReason = reason;
            }
            VisitConclusionPoll::Disarm();
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
                        "NPCVisitBeat[HARD-ABORT]: teleported sender 0x{:08X} to "
                        "return anchor 0x{:08X}",
                        senderActor->GetFormID(), anchorRef->GetFormID());
                }
                senderActor->data.angle.z =
                    VisitState::GetSnapshot().returnAngleZ;
                DemoteSenderToCandidate(senderActor);
            } else if (senderActor) {
                logger::info(
                    "NPCVisitBeat[HARD-ABORT]: sender dead; skipping teleport/demote");
            }
            QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageShutdown);
            PushAbortedHistory(senderActor);
            g_terminalCleanupDone.store(true);
        }

        // ---- Poll verdict handler (Discuss) ------------------------

        void FireValediction(bool closingAlreadySpoken);

        void HandleVisitPollVerdict(
            std::optional<VisitConclusionPoll::PollVerdict> verdict)
        {
            if (!verdict) {
                logger::warn("VisitPoll: parse failed");
                return;
            }
            logger::info(
                "VisitPoll: fired (verdict={}, rationale=\"{}\", "
                "closing_already_spoken={})",
                verdict->shouldConclude ? "true" : "false",
                verdict->rationale, verdict->closingAlreadySpoken);
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
                    "NPCVisitBeat[DISCUSS]: verdict=concluded — advancing to "
                    "Valediction");
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageValediction);
                VisitConclusionPoll::Disarm();
                FireValediction(verdict->closingAlreadySpoken);
                return;
            }
            const double silence = VisitConclusionPoll::SilenceRealSeconds();
            const double silenceLimit =
                static_cast<double>(std::max(0, cfg.visitPollSilenceRealSeconds));
            if (silenceLimit <= 0.0 || silence < silenceLimit) {
                logger::debug(
                    "NPCVisitBeat[DISCUSS]: verdict=continue; silence gate not "
                    "tripped ({:.1f}s < {:.1f}s real) — no nudge",
                    silence, silenceLimit);
                return;
            }
            auto snap = VisitState::GetSnapshot();
            const std::uint8_t nextNudge = static_cast<std::uint8_t>(
                std::min<int>(255, snap.ignoreNudgeCount + 1));
            snap.ignoreNudgeCount = nextNudge;
            VisitState::SetSnapshot(snap);
            logger::info(
                "NPCVisitBeat[DISCUSS]: silence gate tripped ({:.1f}s >= {:.1f}s "
                "real) — firing ContinueConversation (nudge #{})",
                silence, silenceLimit, nextNudge);
            VMDispatchRunSenderAction(g_visitQuest, "ContinueConversation", "");
            const int nudgeCap = std::max(1, cfg.visitMaxIgnoreNudges);
            if (nextNudge >= nudgeCap) {
                logger::info(
                    "NPCVisitBeat[DISCUSS]: nudge cap reached ({} >= {}) — "
                    "forcing Valediction", nextNudge, nudgeCap);
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageValediction);
                VisitConclusionPoll::Disarm();
                FireValediction(false);
            }
        }

        void FireValediction(bool closingAlreadySpoken)
        {
            if (g_valedictionFired.exchange(true)) {
                logger::debug(
                    "NPCVisitBeat[VALEDICTION]: already fired — skipping duplicate");
                return;
            }
            if (!g_visitQuest) return;
            const auto snap = VisitState::GetSnapshot();
            logger::info(
                "NPCVisitBeat: Valediction entry (nudge_count={}, "
                "closing_already_spoken={})",
                snap.ignoreNudgeCount, closingAlreadySpoken);
            const int nudgeCap =
                std::max(1, Settings::Get().visitMaxIgnoreNudges);
            const bool ignored = snap.ignoreNudgeCount >= nudgeCap;
            // Resolve the sender's display name so the closing beat
            // refers to them by name instead of the abstract "the sender".
            // Falls back to "the sender" only if resolution fails
            // (mid-flight sender deletion, corrupted snapshot, etc.).
            std::string senderName;
            if (auto* form = RE::TESForm::LookupByID(snap.senderFormID)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    if (const auto* n = actor->GetName()) senderName = n;
                }
            }
            if (senderName.empty()) senderName = "the sender";
            const std::string closingNarration = ignored
                ? "Having failed to hold the conversation, " + senderName +
                  " prepares to leave, frustrated by being ignored."
                : "Having said what needed saying, " + senderName +
                  " prepares to take their leave.";
            if (closingAlreadySpoken) {
                logger::info(
                    "NPCVisitBeat[VALEDICTION]: dispatching SILENT closing scene "
                    "event ({} chars, ignored={})",
                    closingNarration.size(), ignored);
                VMDispatchRunSenderSilentSceneEvent(g_visitQuest, closingNarration);
            } else {
                logger::info(
                    "NPCVisitBeat[VALEDICTION]: dispatching closing narration "
                    "({} chars, ignored={})",
                    closingNarration.size(), ignored);
                VMDispatchRunSenderNarration(g_visitQuest, closingNarration);
            }
            g_valedictionEnteredAtRealSeconds.store(RealSecondsNow());
        }

        // ---- Discuss speech sampler --------------------------------

        bool SampleAndRegisterNewSpeechTurns(RE::FormID senderFormID)
        {
            if (!SkyrimNetAPI::IsAvailable()) return false;
            const auto raw = SkyrimNetAPI::GetRecentDialogue(
                senderFormID, kDiscussSpeechSamplePerTick);
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
                std::string speakerRaw;
                if (auto it = entry.find("speaker");
                    it != entry.end() && it->is_string()) {
                    speakerRaw = it->get<std::string>();
                }
                bool isPlayerTurn = false;
                if (speakerRaw == "player") {
                    isPlayerTurn = true;
                } else if (speakerRaw == "npc") {
                    // sender turn — nothing extra
                } else if (!speakerRaw.empty()) {
                    if (!playerName.empty() && speakerRaw == playerName) {
                        isPlayerTurn = true;
                    } else if (!senderName.empty() && speakerRaw == senderName) {
                        // sender turn
                    } else {
                        continue;
                    }
                }
                if (isPlayerTurn) sawPlayerTurn = true;
                VisitConclusionPoll::RegisterSpeechTurn();
                ++newCount;
            }
            if (maxSeen > lastSeen) {
                g_lastSampledEventGameTime.store(maxSeen);
            }
            if (newCount > 0 && Settings::Get().debugMode) {
                logger::debug(
                    "NPCVisitBeat[DISCUSS]: sampled {} new dialogue turn(s) "
                    "(player_turn={})", newCount, sawPlayerTurn);
            }
            return sawPlayerTurn;
        }

        void ResetIgnoreNudgeCounter()
        {
            auto snap = VisitState::GetSnapshot();
            if (snap.ignoreNudgeCount == 0) return;
            logger::info(
                "NPCVisitBeat[DISCUSS]: player spoke — resetting ignore-nudge "
                "counter from {} to 0", snap.ignoreNudgeCount);
            snap.ignoreNudgeCount = 0;
            VisitState::SetSnapshot(snap);
        }

        // ---- ReturnHome shutdown chain -----------------------------

        void RunReturnHomeShutdown(const char* triggerReason)
        {
            logger::info(
                "NPCVisitBeat[RETURNHOME]: exit condition tripped ({}) — running "
                "shutdown chain", triggerReason);
            auto* senderRef = g_senderAlias
                ? g_senderAlias->GetReference() : nullptr;
            auto* anchorRef = g_returnAnchorAlias
                ? g_returnAnchorAlias->GetReference() : nullptr;
            auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;
            if (senderActor && !senderActor->IsDead()) {
                if (anchorRef) {
                    senderActor->MoveTo(anchorRef);
                    logger::info(
                        "NPCVisitBeat[RETURNHOME]: teleported sender 0x{:08X} to "
                        "return anchor 0x{:08X}",
                        senderActor->GetFormID(), anchorRef->GetFormID());
                } else {
                    senderActor->MoveTo(senderRef);
                    logger::warn(
                        "NPCVisitBeat[RETURNHOME]: return anchor unresolved; used "
                        "self-MoveTo fallback for sender 0x{:08X}",
                        senderActor->GetFormID());
                }
                senderActor->data.angle.z =
                    VisitState::GetSnapshot().returnAngleZ;
                senderActor->EvaluatePackage();
                DemoteSenderToCandidate(senderActor);
            } else if (senderActor) {
                logger::info(
                    "NPCVisitBeat[RETURNHOME]: sender dead; skipping teleport/demote");
            }
            QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageShutdown);
            PushCompletedHistory();
            g_terminalCleanupDone.store(true);
        }

        // ---- Compose main-thread tasks -----------------------------

        void MainThreadFireComposeLLM();
        void MainThreadDispatchQuest();

        void MainThreadFireComposeLLM()
        {
            RE::FormID senderFormID = 0;
            VisitComposer::UrgencyHint urgency =
                VisitComposer::UrgencyHint::Medium;
            std::string justification;
            {
                std::scoped_lock lock(g_sessionMutex);
                senderFormID  = g_paramSenderFormID;
                urgency       = g_paramUrgency;
                justification = g_paramJustification;
            }
            BeatContext composeCtx;
            composeCtx.desiredDirection = PhaseTracker::Direction::Raise;
            composeCtx.tensionDelta     = 0;
            VisitComposer::Compose(
                composeCtx, urgency, senderFormID, std::move(justification),
                [](std::optional<VisitComposer::VisitBriefing> briefing) {
                    AsyncDispatch::MarshalToMainThread(
                        [briefing = std::move(briefing)]() mutable {
                            if (!briefing) {
                                SetSubPhase(ComposeSubPhase::Failed,
                                            "compose_llm_failed");
                                return;
                            }
                            // Store composition on the VisitState
                            // snapshot; the dispatch task will read it.
                            VisitState::Snapshot snap;
                            {
                                std::scoped_lock lock(g_sessionMutex);
                                snap.senderFormID = g_paramSenderFormID;
                            }
                            snap.briefingText  = briefing->briefing;
                            snap.narrationText = briefing->narration;
                            snap.topicTag      = briefing->topicTag;
                            snap.mood          = briefing->mood;
                            snap.dispatchedAtRealSeconds = RealSecondsNow();
                            VisitState::SetSnapshot(snap);
                            SetSubPhase(ComposeSubPhase::LLMResultReady);
                        });
                });
        }

        void MainThreadDispatchQuest()
        {
            auto snap = VisitState::GetSnapshot();
            if (snap.senderFormID == 0) {
                SetSubPhase(ComposeSubPhase::Failed, "no_composition_at_dispatch");
                return;
            }
            std::string liveResolveReason;
            RE::Actor* sender = BeatParamHelpers::ResolveLiveSenderActor(
                snap.senderFormID, &liveResolveReason);
            if (!sender) {
                g_subPhase.Fail(ComposeSubPhase::Failed,
                                std::move(liveResolveReason));
                return;
            }

            // Snapshot pre-dispatch pose so ReturnHome can teleport back.
            snap.returnPosition = sender->GetPosition();
            snap.returnAngleZ   = sender->GetAngleZ();
            if (auto* parentCell = sender->GetParentCell()) {
                snap.returnCellFormID = parentCell->GetFormID();
            }
            snap.ignoreNudgeCount        = 0;
            snap.consecutivePollFailures = 0;
            VisitState::SetSnapshot(snap);
            logger::info(
                "NPCVisitBeat: snapshotted sender at ({:.1f},{:.1f},{:.1f}) in "
                "cell 0x{:08X}",
                snap.returnPosition.x, snap.returnPosition.y,
                snap.returnPosition.z, snap.returnCellFormID);

            PromoteSenderToDesignated(sender);

            bool engineResult = false;
            const bool callOk =
                g_visitQuest->EnsureQuestStarted(engineResult, /*a_startNow=*/true);
            if (!callOk || !engineResult) {
                const bool senderFilled = g_senderAlias
                    && g_senderAlias->GetReference() != nullptr;
                const bool spawnFilled  = g_spawnMarkerAlias
                    && g_spawnMarkerAlias->GetReference() != nullptr;
                const bool anchorFilled = g_returnAnchorAlias
                    && g_returnAnchorAlias->GetReference() != nullptr;
                logger::warn(
                    "NPCVisitBeat: EnsureQuestStarted failed (callOk={}, "
                    "engineResult={}) alias_state after failure: Sender_filled={} "
                    "SpawnMarker_filled={} ReturnAnchor_filled={} — demoting and "
                    "rolling back",
                    callOk, engineResult, senderFilled, spawnFilled, anchorFilled);
                DemoteSenderToCandidate(sender);
                SetSubPhase(ComposeSubPhase::Failed, "ensure_quest_started_failed");
                return;
            }
            const RE::FormID senderAliasFilledID =
                (g_senderAlias && g_senderAlias->GetReference())
                    ? g_senderAlias->GetReference()->GetFormID() : 0u;
            const RE::FormID spawnMarkerFilledID =
                (g_spawnMarkerAlias && g_spawnMarkerAlias->GetReference())
                    ? g_spawnMarkerAlias->GetReference()->GetFormID() : 0u;
            const RE::FormID returnAnchorFilledID =
                (g_returnAnchorAlias && g_returnAnchorAlias->GetReference())
                    ? g_returnAnchorAlias->GetReference()->GetFormID() : 0u;
            snap.returnAnchorFormID = returnAnchorFilledID;
            VisitState::SetSnapshot(snap);
            logger::info(
                "NPCVisitBeat: EnsureQuestStarted ok — Sender=0x{:08X}, "
                "SpawnMarker=0x{:08X}, ReturnAnchor=0x{:08X}",
                senderAliasFilledID, spawnMarkerFilledID, returnAnchorFilledID);
            if (senderAliasFilledID == 0 || returnAnchorFilledID == 0) {
                logger::warn(
                    "NPCVisitBeat: EnsureQuestStarted reported success but a "
                    "required alias is unfilled (Sender=0x{:08X}, "
                    "ReturnAnchor=0x{:08X}) — rolling back",
                    senderAliasFilledID, returnAnchorFilledID);
                DemoteSenderToCandidate(sender);
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageRollback);
                SetSubPhase(ComposeSubPhase::Failed,
                            "ensure_quest_started_unfilled_alias");
                return;
            }
            g_salutationEnteredAtRealSeconds.store(RealSecondsNow());
            g_lastDistanceLogRealSeconds.store(0.0);
            VisitState::SetComposingSender(false);
            SetSubPhase(ComposeSubPhase::Succeeded);
        }

        // ---- RUNNING stage-tick (main thread; called every ~1s) ----

        void MainThreadRunningTick(TickMode mode)
        {
            g_runningTaskInFlight.store(false, std::memory_order_release);
            if (!g_visitQuest) return;
            if (g_hardAbortFired.load()) return;
            if (CheckHardAbortConditions()) return;

            const auto stage = g_visitQuest->GetCurrentStageID();
            const auto& cfg = Settings::Get();

            switch (stage) {
                case kStageSalutation: {
                    if (mode != TickMode::Normal) return;
                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) return;
                    const int approachDist =
                        std::max(1, cfg.visitSalutationApproachDistanceUnits);
                    const int timeoutSec =
                        std::max(1, cfg.visitApproachTimeoutSeconds);
                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    const auto now = RealSecondsNow();
                    const auto enteredAt = g_salutationEnteredAtRealSeconds.load();
                    const auto elapsed = enteredAt > 0.0 ? (now - enteredAt) : 0.0;
                    const auto lastLog = g_lastDistanceLogRealSeconds.load();
                    if (now - lastLog >= 5.0) {
                        g_lastDistanceLogRealSeconds.store(now);
                        logger::info(
                            "NPCVisitBeat[SALUTATION]: elapsed={:.1f}s, "
                            "sender-to-player distance={:.0f}u (timeout at {}s, "
                            "approach<={}u)",
                            elapsed, dist, timeoutSec, approachDist);
                    }
                    if (dist <= static_cast<float>(approachDist)) {
                        logger::info(
                            "NPCVisitBeat[SALUTATION]: approach reached "
                            "({:.0f}u) — firing opening line and advancing to "
                            "Discuss", dist);
                        const auto snap = VisitState::GetSnapshot();
                        VMDispatchRunSenderNarration(
                            g_visitQuest, snap.narrationText);
                        QuestUtils::VMDispatchQuestSetStage(
                            g_visitQuest, kStageDiscuss);
                        VisitConclusionPoll::Arm(snap);
                        g_discussSenderFormID.store(snap.senderFormID);
                        // Initialize the speech sampler cursor to now
                        // so we don't count pre-Salutation dialogue.
                        if (auto* cal = RE::Calendar::GetSingleton()) {
                            g_lastSampledEventGameTime.store(
                                static_cast<double>(cal->GetHoursPassed()) * 3600.0);
                        }
                        NPCVisitBeat_Cooldowns::OnVisitCompleted(snap.senderFormID);
                        return;
                    }
                    if (elapsed >= static_cast<double>(timeoutSec)) {
                        logger::warn(
                            "NPCVisitBeat[SALUTATION]: timeout at {:.1f}s (limit "
                            "{}s) — rolling back", elapsed, timeoutSec);
                        auto* anchorRef = g_returnAnchorAlias
                            ? g_returnAnchorAlias->GetReference() : nullptr;
                        auto* senderActor = senderRef->As<RE::Actor>();
                        if (senderActor && anchorRef) {
                            senderActor->MoveTo(anchorRef);
                            senderActor->data.angle.z =
                                VisitState::GetSnapshot().returnAngleZ;
                        }
                        if (senderActor) DemoteSenderToCandidate(senderActor);
                        QuestUtils::VMDispatchQuestSetStage(
                            g_visitQuest, kStageRollback);
                        VisitConclusionPoll::Disarm();
                        PushRolledBackHistory(senderActor);
                        g_terminalCleanupDone.store(true);
                    }
                    return;
                }
                case kStageDiscuss: {
                    if (mode != TickMode::Normal) return;
                    const auto senderFormID = g_discussSenderFormID.load();
                    if (SampleAndRegisterNewSpeechTurns(senderFormID)) {
                        ResetIgnoreNudgeCounter();
                    }
                    if (VisitConclusionPoll::GateTick()) {
                        logger::info(
                            "NPCVisitBeat[DISCUSS]: gate tripped — firing "
                            "conclusion poll");
                        VisitConclusionPoll::FirePoll(
                            [](std::optional<VisitConclusionPoll::PollVerdict> v) {
                                AsyncDispatch::MarshalToMainThread(
                                    [v = std::move(v)]() mutable {
                                        HandleVisitPollVerdict(std::move(v));
                                    });
                            });
                    }
                    return;
                }
                case kStageOnHold: {
                    const bool inDialogue = ObservePlayerInDialogue();
                    const bool inCombat   = ObserveAnyCombat();
                    if (!inDialogue && !inCombat) {
                        logger::info(
                            "NPCVisitBeat[ONHOLD]: triggers cleared — dispatching "
                            "SetStage(ReEngage)");
                        QuestUtils::VMDispatchQuestSetStage(
                            g_visitQuest, kStageReEngage);
                        g_onHoldCombatStartedAtRealSeconds.store(0.0);
                        return;
                    }
                    // Combat-stuck check runs under Combat mode.
                    if (mode == TickMode::Combat && inCombat) {
                        const int combatMax =
                            std::max(1, cfg.visitOnHoldCombatMaxSeconds);
                        const auto combatStart =
                            g_onHoldCombatStartedAtRealSeconds.load();
                        if (combatStart <= 0.0) {
                            g_onHoldCombatStartedAtRealSeconds.store(RealSecondsNow());
                        } else {
                            const auto elapsed = RealSecondsNow() - combatStart;
                            if (elapsed >= static_cast<double>(combatMax)) {
                                logger::warn(
                                    "NPCVisitBeat[ONHOLD]: combat_stuck — elapsed "
                                    "{:.1f}s >= {}s", elapsed, combatMax);
                                HardAbortVisit("combat_stuck");
                            }
                        }
                    }
                    return;
                }
                case kStageReEngage: {
                    if (mode != TickMode::Normal) return;
                    const bool inDialogue = ObservePlayerInDialogue();
                    const bool inCombat   = ObserveAnyCombat();
                    if (inDialogue || inCombat) {
                        logger::info(
                            "NPCVisitBeat[REENGAGE]: OnHold trigger re-tripped "
                            "(dialogue={}, combat={}) — returning to OnHold",
                            inDialogue, inCombat);
                        QuestUtils::VMDispatchQuestSetStage(
                            g_visitQuest, kStageOnHold);
                        if (inCombat) {
                            g_onHoldCombatStartedAtRealSeconds.store(RealSecondsNow());
                        }
                        return;
                    }
                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) return;
                    const int approachDist =
                        std::max(1, cfg.visitReEngageApproachDistanceUnits);
                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    const auto now = RealSecondsNow();
                    const auto lastLog = g_lastDistanceLogRealSeconds.load();
                    if (now - lastLog >= 5.0) {
                        g_lastDistanceLogRealSeconds.store(now);
                        logger::info(
                            "NPCVisitBeat[REENGAGE]: distance={:.0f}u (threshold {}u)",
                            dist, approachDist);
                    }
                    if (dist <= static_cast<float>(approachDist)) {
                        logger::info(
                            "NPCVisitBeat[REENGAGE]: approach reached ({:.0f}u) — "
                            "dispatching resumption narration + SetStage(Discuss)",
                            dist);
                        const auto snap = VisitState::GetSnapshot();
                        VMDispatchRunSenderNarration(
                            g_visitQuest, snap.narrationText);
                        QuestUtils::VMDispatchQuestSetStage(
                            g_visitQuest, kStageDiscuss);
                        VisitConclusionPoll::Arm(snap);
                        g_discussSenderFormID.store(snap.senderFormID);
                    }
                    return;
                }
                case kStageValediction: {
                    if (mode != TickMode::Normal) return;
                    const auto enteredAt = g_valedictionEnteredAtRealSeconds.load();
                    if (enteredAt <= 0.0) return;
                    const int dwellSec =
                        std::max(1, cfg.visitValedictionDwellSeconds);
                    const auto elapsed = RealSecondsNow() - enteredAt;
                    if (elapsed >= static_cast<double>(dwellSec)) {
                        logger::info(
                            "NPCVisitBeat[VALEDICTION]: dwell expired ({:.1f}s "
                            ">= {}s) — advancing to ReturnHome",
                            elapsed, dwellSec);
                        QuestUtils::VMDispatchQuestSetStage(
                            g_visitQuest, kStageReturnHome);
                        g_returnHomeStartedAtRealSeconds.store(RealSecondsNow());
                    }
                    return;
                }
                case kStageReturnHome: {
                    if (mode != TickMode::Normal) return;
                    auto* senderRef = g_senderAlias
                        ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player) return;
                    const int exitDist =
                        std::max(1, cfg.visitReturnHomeExitDistanceUnits);
                    const int timeoutSec =
                        std::max(1, cfg.visitReturnHomeTimeoutSeconds);
                    const auto dist =
                        senderRef->GetPosition().GetDistance(player->GetPosition());
                    const bool attached = senderRef->GetParentCell()
                        ? senderRef->GetParentCell()->IsAttached() : true;
                    const auto startAt =
                        g_returnHomeStartedAtRealSeconds.load();
                    const auto elapsed = startAt > 0.0
                        ? (RealSecondsNow() - startAt) : 0.0;
                    const bool losToSender =
                        CameraVisibility::IsAnyPartVisibleFromCamera(senderRef);
                    const auto now = RealSecondsNow();
                    const auto lastLog = g_lastDistanceLogRealSeconds.load();
                    if (now - lastLog >= 5.0) {
                        g_lastDistanceLogRealSeconds.store(now);
                        logger::info(
                            "NPCVisitBeat[RETURNHOME]: dist={:.0f}u (exit>={}), "
                            "cell_attached={}, los={}, elapsed={:.1f}s (timeout={}s)",
                            dist, exitDist, attached, losToSender, elapsed, timeoutSec);
                    }
                    if (dist >= static_cast<float>(exitDist)) {
                        RunReturnHomeShutdown("distance"); return;
                    }
                    if (!attached) {
                        RunReturnHomeShutdown("cell-unloaded"); return;
                    }
                    if (!losToSender && dist >= 2000.0f) {
                        RunReturnHomeShutdown("los-lost"); return;
                    }
                    if (elapsed >= static_cast<double>(timeoutSec)) {
                        RunReturnHomeShutdown("timeout"); return;
                    }
                    return;
                }
                case kStageRollback:
                case kStageShutdown:
                    // Terminal — cleanup handles the wait.
                    return;
                default:
                    return;
            }
        }

        void MainThreadCleanup()
        {
            // If cleanup wasn't already triggered by a rollback / abort
            // path, run the terminal shutdown chain now — this covers
            // the "COMPOSE failed before any stage was set" case where
            // no other path has cleared state.
            if (!g_visitQuest) {
                VisitState::Reset();
                return;
            }
            const auto stage = g_visitQuest->GetCurrentStageID();
            if (stage != 0 && stage != kStageShutdown &&
                stage != kStageRollback)
            {
                // Force it into rollback so the Papyrus Shutdown
                // fragment tears down alias fills. Guarded so we don't
                // double-drive a stage that's already terminal.
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageRollback);
            }
            // Give Papyrus a beat, then clear VisitState.
            VisitState::Reset();
        }
    }

    // ---------------------------------------------------------------
    // NPCVisitBeat_Init
    // ---------------------------------------------------------------

    namespace NPCVisitBeat_Init
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
                    "NPCVisitBeat_Init: quest '{}' did not resolve — "
                    "IsAvailable will report false permanently",
                    kVisitQuestEditorID);
                ok = false;
            }
            if (auto* form = RE::TESForm::LookupByEditorID(kVisitFactionEditorID)) {
                g_visitSenderFaction = form->As<RE::TESFaction>();
            }
            if (!g_visitSenderFaction) {
                logger::error(
                    "NPCVisitBeat_Init: sender faction '{}' did not resolve",
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
            if (!g_senderAlias || !g_spawnMarkerAlias || !g_returnAnchorAlias) {
                logger::error(
                    "NPCVisitBeat_Init: one or more required aliases unresolved "
                    "(Sender={} SpawnMarker={} ReturnAnchor={})",
                    g_senderAlias ? "ok" : "MISSING",
                    g_spawnMarkerAlias ? "ok" : "MISSING",
                    g_returnAnchorAlias ? "ok" : "MISSING");
                ok = false;
            }
            g_pointersCriticallyMissing.store(!ok);
            if (ok) {
                logger::info(
                    "NPCVisitBeat_Init: resolved quest=0x{:08X}, faction=0x{:08X}, "
                    "aliases bound",
                    g_visitQuest->GetFormID(),
                    g_visitSenderFaction->GetFormID());
                RegisterSinks();
            } else {
                logger::error(
                    "NPCVisitBeat_Init: one or more required forms missing — "
                    "NPCVisitBeat disabled for the session");
            }
        }
    }

    // ---------------------------------------------------------------
    // IBeat impl
    // ---------------------------------------------------------------

    std::string NPCVisitBeat::Name() const { return "npc_visit"; }

    std::string NPCVisitBeat::Description() const
    {
        return
            "An NPC the player knows drops what they were doing and travels "
            "to the player's current location to speak in person. Best fit "
            "when the intended beat cannot survive being written down and "
            "folded into a letter — an urgent warning, an apology that needs "
            "eye contact, a confession, a threat delivered face-to-face — or "
            "when the sender's own state (grief, anger, love, contrition) "
            "demands they show up rather than write. Tone and polarity are "
            "driven by the generated content, so this beat can serve either "
            "a raising direction (menacing / urgent visits) or a lowering "
            "direction (contrite / warm / mournful visits) depending on what "
            "the current phase calls for.\n"
            "\n"
            "Avoid when the player is in a dungeon, lair, jail cell, arena, "
            "or other cell where a stranger walking up would be jarring — "
            "the beat already gates itself on those. Also avoid when the "
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
            "topic, mood, and tags are decided by the beat's own compose "
            "LLM call. Extra fields will be silently ignored.";
    }

    BeatPolarity NPCVisitBeat::Polarity() const { return BeatPolarity::Either; }

    bool NPCVisitBeat::IsAvailable(const BeatContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("NPCVisitBeat::IsAvailable: blocked ({})", reason);
            }
            return false;
        };
        if (g_pointersCriticallyMissing.load()) {
            return blocked("critical forms missing at kDataLoaded");
        }
        if (ctx.player) {
            if (LocationKeywords::IsVisitHostile(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsVisitHostile");
            }
        }
        if (!SkyrimNetAPI::IsMemorySystemReady()) {
            return blocked("SkyrimNet memory system not ready");
        }
        const std::size_t minCandidates =
            static_cast<std::size_t>(
                std::max(1, Settings::Get().visitMinSenderCandidates));
        const std::size_t viable = SenderCandidatePool::CountViable(
            &VisitViabilityFilter_ForCountViable, minCandidates);
        if (viable < minCandidates) {
            if (debug) {
                logger::debug(
                    "NPCVisitBeat::IsAvailable: blocked (only {} viable "
                    "candidates, need {})", viable, minCandidates);
            }
            return false;
        }
        return true;
    }

    void NPCVisitBeat::OnStart(const BeatContext&    /*ctx*/,
                                const nlohmann::json& parameters)
    {
        std::string failureReason;
        const auto senderParsed =
            BeatParamHelpers::ParseSenderFormID(parameters, &failureReason);
        const auto urgency = BeatParamHelpers::ParseUrgencyHint(parameters);

        // parameter_justification is optional; missing / non-string is
        // treated as "compose LLM invents motivation from memory tail."
        std::string justification;
        if (parameters.is_object()) {
            if (auto it = parameters.find("parameter_justification");
                it != parameters.end() && it->is_string())
            {
                justification = it->get<std::string>();
            }
        }

        ResetSessionState();
        VisitState::Reset();
        VisitState::SetComposingSender(true);
        if (!senderParsed) {
            g_subPhase.Fail(ComposeSubPhase::Failed, std::move(failureReason));
        } else {
            std::scoped_lock lock(g_sessionMutex);
            g_paramSenderFormID  = *senderParsed;
            g_paramUrgency       = urgency;
            g_paramJustification = std::move(justification);
        }
        logger::info(
            "NPCVisitBeat::OnStart: sender=0x{:08X} urgency={}",
            senderParsed.value_or(0),
            urgency == BeatParamHelpers::UrgencyHint::Low    ? "low"
          : urgency == BeatParamHelpers::UrgencyHint::High   ? "high"
                                                              : "medium");
    }

    TickResult NPCVisitBeat::Tick(TickMode mode, BeatState state)
    {
        // Paused/Dialogue freeze the whole beat.
        if (mode == TickMode::Paused || mode == TickMode::Dialogue) return {};

        switch (state) {
            case BeatState::COMPOSE: {
                if (mode != TickMode::Normal) return {};
                const auto sub = g_subPhase.Get();
                switch (sub) {
                    case ComposeSubPhase::Start:
                        SetSubPhase(ComposeSubPhase::ComposingLLM);
                        AsyncDispatch::MarshalToMainThread(&MainThreadFireComposeLLM);
                        return {};
                    case ComposeSubPhase::ComposingLLM:
                        return {};
                    case ComposeSubPhase::LLMResultReady:
                        SetSubPhase(ComposeSubPhase::Dispatching);
                        AsyncDispatch::MarshalToMainThread(&MainThreadDispatchQuest);
                        return {};
                    case ComposeSubPhase::Dispatching:
                        return {};
                    case ComposeSubPhase::Succeeded:
                        logger::info(
                            "NPCVisitBeat: COMPOSE succeeded; advancing to RUNNING");
                        g_runningTickCount = 0;
                        return {BeatState::RUNNING};
                    case ComposeSubPhase::Failed: {
                        logger::warn(
                            "NPCVisitBeat: COMPOSE failed ({}); advancing to CLEANUP",
                            g_subPhase.FailureReason());
                        return {BeatState::CLEANUP};
                    }
                }
                return {};
            }

            case BeatState::RUNNING: {
                // Terminal-cleanup latch flipped by rollback / hard-abort /
                // normal ReturnHome shutdown paths (all run on the main
                // thread via marshaled tasks).
                if (g_terminalCleanupDone.load()) {
                    return {BeatState::CLEANUP};
                }
                // Fire a stage-tick every N ticks. Combat mode is
                // forwarded so Stage 25 (OnHold) can track combat-stuck.
                if (++g_runningTickCount >= kRunningCheckEveryNTicks) {
                    g_runningTickCount = 0;
                    if (!g_runningTaskInFlight.exchange(
                            true, std::memory_order_acq_rel))
                    {
                        AsyncDispatch::MarshalToMainThread(
                            [mode]() { MainThreadRunningTick(mode); });
                    }
                }
                return {};
            }

            case BeatState::CLEANUP: {
                // Wait for the quest's terminal shutdown fragment to
                // land the stage back at 0, then return to NOT_RUNNING.
                if (g_visitQuest) {
                    const auto stage = g_visitQuest->GetCurrentStageID();
                    if (stage == 0) {
                        logger::info(
                            "NPCVisitBeat: CLEANUP done; returning to NOT_RUNNING");
                        VisitState::Reset();
                        VisitState::SetComposingSender(false);
                        return {BeatState::NOT_RUNNING};
                    }
                    // If we haven't yet dispatched a terminal stage
                    // (COMPOSE failed before EnsureQuestStarted, etc.)
                    // and the quest isn't running, we can exit
                    // immediately.
                    if (stage != kStageShutdown && stage != kStageRollback &&
                        !g_terminalCleanupDone.load())
                    {
                        AsyncDispatch::MarshalToMainThread(&MainThreadCleanup);
                        g_terminalCleanupDone.store(true);
                    }
                } else {
                    VisitState::Reset();
                    VisitState::SetComposingSender(false);
                    return {BeatState::NOT_RUNNING};
                }
                return {};
            }

            case BeatState::NOT_RUNNING:
            default:
                return {};
        }
    }

    // ---------------------------------------------------------------
    // Cooldowns + Persistence
    // ---------------------------------------------------------------

    namespace NPCVisitBeat_Cooldowns
    {
        void OnVisitCompleted(RE::FormID senderNpcFormID)
        {
            if (senderNpcFormID == 0) return;
            g_senderCooldowns.Stamp(senderNpcFormID);
            logger::info(
                "NPCVisitBeat: per-sender cooldown stamp set for 0x{:08X}",
                senderNpcFormID);
        }

        bool IsSenderOnCooldown(RE::FormID senderNpcFormID)
        {
            return g_senderCooldowns.IsOnCooldown(
                senderNpcFormID,
                Settings::Get().visitSenderCooldownGameHours);
        }
    }

    namespace NPCVisitBeat_Persistence
    {
        constexpr std::uint32_t kRecordVersion = 1;

        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) return;
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
                logger::error("NPCVisitBeat::OnSave: OpenRecord failed");
                return;
            }
            g_senderCooldowns.Serialize(intfc);
        }

        void OnLoad(SKSE::SerializationInterface* intfc,
                    std::uint32_t version, std::uint32_t length)
        {
            if (!intfc) return;
            if (version != kRecordVersion) {
                logger::warn(
                    "NPCVisitBeat::OnLoad: unknown version {} (length={}); "
                    "clearing cooldown state", version, length);
                OnRevert();
                return;
            }
            if (!g_senderCooldowns.Deserialize(intfc)) {
                logger::error(
                    "NPCVisitBeat::OnLoad: sender-cooldown deserialize failed; "
                    "cleared");
                return;
            }
            logger::info(
                "NPCVisitBeat::OnLoad: restored per-sender cooldowns");
        }

        void OnRevert()
        {
            g_senderCooldowns.Clear();
        }
    }
}
