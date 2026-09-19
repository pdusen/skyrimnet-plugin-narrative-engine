#include <NPCVisitBeat.h>

#include <AsyncDispatch.h>
#include <BeatParamHelpers.h>
#include <BeatUtils.h>
#include <CameraVisibility.h>
#include <EngineUtils.h>
#include <LocationKeywords.h>
#include <logger.h>
#include <MainThread.h>
#include <QuestUtils.h>
#include <SenderCandidatePool.h>
#include <SenderCooldownTable.h>
#include <Settings.h>
#include <SKSECosaveIO.h>
#include <SkyrimNetAPI.h>
#include <StuckRecovery.h>
#include <VisitArrivalPoint.h>
#include <VisitComposer.h>
#include <VisitConclusionPoll.h>
#include <VisitState.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/B/BGSBaseAlias.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/B/BSFixedString.h>
#include <RE/C/Calendar.h>
#include <RE/P/PlayerCharacter.h>
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

        constexpr const char* kVisitQuestEditorID = "_ne_VisitQuest";
        constexpr const char* kSenderAliasName = "Sender";
        constexpr const char* kReturnAnchorAliasName = "ReturnAnchor";
        constexpr const char* kQuestScriptName = "_ne_VisitQuest";

        // XMarkerHeading, the static both runtime markers are made
        // from. Carries a facing as well as a position, which is what
        // lets the arrival marker point the sender at the player.
        // Verified against the Spriggit export: 000034 is
        // XMarkerHeading and 00003B is the plain XMarker.
        constexpr RE::FormID kXMarkerHeadingFormID = 0x00000034;

        // Ticks to wait for both force-fills to land before giving up.
        // The master poll is ~250ms, so this is ~5 seconds — the same
        // bound AmbushBeat uses for the same dispatch-then-read shape.
        constexpr int kFillVerifyMaxTicks = 20;

        constexpr std::uint32_t kStageSalutation = 10;
        constexpr std::uint32_t kStageDiscuss = 20;
        constexpr std::uint32_t kStageValediction = 30;
        constexpr std::uint32_t kStageReturnHome = 50;
        constexpr std::uint32_t kStageRollback = 60;
        constexpr std::uint32_t kStageShutdown = 200;

        // ---- Resolved engine handles (kDataLoaded) -----------------

        std::atomic<bool> g_pointersResolved = false;
        std::atomic<bool> g_pointersCriticallyMissing = false;
        RE::TESQuest* g_visitQuest = nullptr;
        RE::BGSRefAlias* g_senderAlias = nullptr;
        RE::BGSRefAlias* g_returnAnchorAlias = nullptr;
        RE::TESBoundObject* g_xMarkerBase = nullptr;

        // ---- Small helpers -----------------------------------------

        // Null in production; a test may point it at a clock it drives.
        std::atomic<double (*)()> g_clockOverride = nullptr;

        double RealSecondsNow()
        {
            if (auto* clock = g_clockOverride.load(std::memory_order_acquire)) {
                return clock();
            }
            return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count())
                   / 1000.0;
        }

        // Beat-lifetime accumulators of wall-clock seconds, advanced at
        // the top of Tick() by the delta since the previous Tick. Each
        // accumulator advances only under the TickMode whose gates
        // consume it — so a timer never registers "elapsed" during ticks
        // that couldn't have fired its check.
        //
        // g_normalElapsedSec drives salutation approach, valediction
        // dwell, return-home timeout, and the distance-log throttle.
        // Those live in stage bodies that early-return unless mode ==
        // Normal, so counting only Normal ticks keeps the gate and its
        // clock in sync.
        //
        // g_combatElapsedSec drives the OnHold combat-stuck watchdog,
        // which only ever evaluates under mode == Combat. It needs its
        // own timebase — a Normal-only accumulator would be frozen for
        // the entire duration of a combat episode and the watchdog
        // would never fire.
        //
        // Both freeze on Paused (Paused precludes Normal/Combat) and on
        // Dialogue (Dialogue precludes Normal/Combat). Same shape as
        // VisitConclusionPoll's silence accumulator. Reset by
        // ResetSessionState at the start of every visit.
        std::atomic<double> g_normalElapsedSec = 0.0;
        std::atomic<double> g_combatElapsedSec = 0.0;

        // Wall-clock timestamp captured on the previous Tick, used to
        // compute the "seconds since last Tick" delta credited to
        // whichever accumulator matches the current TickMode. 0.0
        // sentinel = "no prior tick this visit" — the first Tick after
        // OnStart establishes the baseline and adds nothing (so a long
        // stall before the first Tick can't retroactively pile up
        // elapsed time on either clock).
        std::atomic<double> g_lastAccumulatorSampleRealSec = 0.0;

        double NormalElapsedNow()
        {
            return g_normalElapsedSec.load(std::memory_order_acquire);
        }

        double CombatElapsedNow()
        {
            return g_combatElapsedSec.load(std::memory_order_acquire);
        }

        bool VMDispatchRunSenderAction(RE::TESQuest* quest, const std::string& actionName, const std::string& argsJson)
        {
            return QuestUtils::VMDispatchOnQuest(quest,
                                                 "_ne_VisitQuest"sv,
                                                 "RunSenderAction"sv,
                                                 RE::BSFixedString(actionName.c_str()),
                                                 RE::BSFixedString(argsJson.c_str()));
        }

        bool VMDispatchRunSenderNarration(RE::TESQuest* quest, const std::string& content)
        {
            return QuestUtils::VMDispatchOnQuest(
                quest, "_ne_VisitQuest"sv, "RunSenderNarration"sv, RE::BSFixedString(content.c_str()));
        }

        bool VMDispatchRunSenderSilentSceneEvent(RE::TESQuest* quest, const std::string& content)
        {
            return QuestUtils::VMDispatchOnQuest(
                quest, "_ne_VisitQuest"sv, "RunSenderSilentSceneEvent"sv, RE::BSFixedString(content.c_str()));
        }

        // Per-sender cooldowns — persisted via NPCVisitBeat_Persistence 'NBVS'.
        SenderCooldownTable g_senderCooldowns;

        // Per-sender memory watermark. Stamped at Valediction entry
        // (the moment we know the visit's beat has landed). The
        // SenderCandidatePool memory filter drops any memories whose
        // absolute game-hours are at or below this stamp — used to
        // exclude prior memories from re-motivating a follow-up visit
        // with the same sender about the same topic. Same storage
        // shape as the cooldown table; reused purely for its map +
        // persistence machinery. Callers use GetStampGameHours()
        // instead of IsOnCooldown().
        SenderCooldownTable g_senderMemoryWatermarks;

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
            SelectingPoint, // road-graph arrival search
            StartingQuest,  // snapshot pose, place anchor, EnsureQuestStarted
            Warping,        // place arrival marker, MoveTo, dispatch both fills
            VerifyingFill,  // read both aliases back on a later tick
            Arming,         // bind the package, begin the escort
            Succeeded,      // -> RUNNING
            Failed,         // -> CLEANUP
        };

        std::mutex g_sessionMutex;
        BeatUtils::ComposeSubPhaseMachine<ComposeSubPhase> g_subPhase{ComposeSubPhase::Start};

        RE::FormID g_paramSenderFormID = 0;
        BeatParamHelpers::UrgencyHint g_paramUrgency = BeatParamHelpers::UrgencyHint::Medium;
        std::string g_paramJustification;

        // Session flags — atomics used across the worker Tick and the
        // marshaled main-thread tasks.
        std::atomic<bool> g_hardAbortFired = false;
        std::atomic<bool> g_valedictionFired = false;
        std::atomic<bool> g_terminalCleanupDone = false;
        std::atomic<double> g_salutationEnteredAtNormalSec = 0.0;
        std::atomic<double> g_valedictionEnteredAtNormalSec = 0.0;
        std::atomic<double> g_returnHomeStartedAtNormalSec = 0.0;
        std::atomic<double> g_lastDistanceLogNormalSec = 0.0;
        std::atomic<double> g_onHoldCombatStartedAtCombatSec = 0.0;

        // Combat-stuck / poll_broken hard-abort reason surface, set by
        // the sink or the abort helper before HardAbort teardown runs.
        std::mutex g_hardAbortReasonMutex;
        std::string g_hardAbortReason;

        // Discuss sampler cursor and sender FormID cached at
        // Salutation → Discuss.
        std::atomic<double> g_lastSampledEventGameTime = 0.0;
        std::atomic<RE::FormID> g_discussSenderFormID = 0;

        // One-shot latch for the Salutation "narration + SetStage(Discuss)"
        // dispatch. The Papyrus VM stage change is async, so a bare stage
        // tick would keep re-firing the dispatch every ~1s until the stage
        // lands, which would turn the opening narration into a spam loop.
        // The latch is set the first time the dispatch fires and cleared
        // automatically when the observed stage moves off Salutation.
        // While set, the tick emits a periodic warn (every 5s) and
        // escalates to error at 30s to surface a stuck loop. Not
        // persisted — resets on OnStart / OnRevert.
        struct StageTransitionLatch
        {
            std::atomic<bool> dispatched{false};
            std::atomic<double> dispatchedAtNormalSec{0.0};
            std::atomic<double> lastWarningAtNormalSec{0.0};

            void Reset()
            {
                dispatched.store(false, std::memory_order_release);
                dispatchedAtNormalSec.store(0.0, std::memory_order_release);
                lastWarningAtNormalSec.store(0.0, std::memory_order_release);
            }

            bool IsDispatched() const
            {
                return dispatched.load(std::memory_order_acquire);
            }

            void MarkDispatched(double nowNormalSec)
            {
                dispatched.store(true, std::memory_order_release);
                dispatchedAtNormalSec.store(nowNormalSec, std::memory_order_release);
                lastWarningAtNormalSec.store(nowNormalSec, std::memory_order_release);
            }
        };

        StageTransitionLatch g_salutationDiscussLatch;

        // Discuss sub-state machine. Cyclic and mostly one-directional:
        //   Discussing --(hold trigger tripped)--------------> OnHold
        //   OnHold     --(all hold triggers cleared)---------> ReEngage
        //   ReEngage   --(sender within approach distance)---> Discussing
        //                (fires resumption narration)
        //   ReEngage   --(hold trigger re-tripped)-----------> OnHold
        // The only backward transition permitted is ReEngage → OnHold on
        // re-trip; Discussing cannot skip directly to ReEngage and OnHold
        // cannot skip directly back to Discussing.
        //
        // Hold triggers: either-participant combat OR the player being in
        // a vanilla dialogue menu (with anyone). Not persisted — resets
        // to Discussing on OnStart / OnRevert. On save/reload mid-cycle,
        // the next tick re-derives the correct substate from live state.
        enum class DiscussSubPhase : std::uint8_t
        {
            Discussing,
            OnHold,
            ReEngage,
        };
        std::atomic<DiscussSubPhase> g_discussSubPhase = DiscussSubPhase::Discussing;

        // RUNNING tick cadence — one marshaled main-thread stage tick
        // every kRunningCheckEveryNTicks worker ticks (4 = ~1s at
        // 250ms).
        // Where the sender is warped to, and the road behind it that
        // StuckRecovery escalates through. Written by SelectingPoint,
        // read by Warping and Arming. Guarded by g_sessionMutex.
        VisitArrivalPoint::Result g_arrival;

        // The two references this beat creates. The arrival marker is a
        // MoveTo target and is deleted a tick later; the anchor lives for
        // the whole visit and is deleted by the quest's Shutdown
        // fragment. Both are tracked here so a failure path can clean up
        // whatever had been made by the time it fired.
        std::atomic<RE::FormID> g_arrivalMarkerFormID = 0;

        std::atomic<int> g_fillVerifyTicks = 0;

        // Last Normal-mode reading handed to the escort's clock, so the
        // gap between checks is measured rather than assumed from the
        // tick rate.
        std::atomic<double> g_lastEscortSampleNormalSec = 0.0;

        // Supervises the walk in, fed the arrival search's fallbacks.
        // Only the approach is escorted -- once the sender is talking to
        // the player there is nothing left to be stuck on.
        StuckRecovery::Escort g_escort{"visit"};

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
            g_salutationEnteredAtNormalSec.store(0.0, std::memory_order_release);
            g_valedictionEnteredAtNormalSec.store(0.0, std::memory_order_release);
            g_returnHomeStartedAtNormalSec.store(0.0, std::memory_order_release);
            g_lastDistanceLogNormalSec.store(0.0, std::memory_order_release);
            g_onHoldCombatStartedAtCombatSec.store(0.0, std::memory_order_release);
            g_normalElapsedSec.store(0.0, std::memory_order_release);
            g_combatElapsedSec.store(0.0, std::memory_order_release);
            g_lastAccumulatorSampleRealSec.store(0.0, std::memory_order_release);
            g_lastSampledEventGameTime.store(0.0, std::memory_order_release);
            g_discussSenderFormID.store(0, std::memory_order_release);
            g_runningTickCount = 0;
            g_runningTaskInFlight.store(false, std::memory_order_release);
            g_fillVerifyTicks.store(0, std::memory_order_release);
            g_arrivalMarkerFormID.store(0, std::memory_order_release);
            g_escort.Clear();
            g_lastEscortSampleNormalSec.store(0.0, std::memory_order_release);
            {
                std::scoped_lock lock(g_sessionMutex);
                g_arrival = {};
            }
            g_salutationDiscussLatch.Reset();
            g_discussSubPhase.store(DiscussSubPhase::Discussing, std::memory_order_release);
            {
                std::scoped_lock lock(g_hardAbortReasonMutex);
                g_hardAbortReason.clear();
            }
        }

        const char* ComposeSubPhaseName(ComposeSubPhase phase)
        {
            switch (phase) {
            case ComposeSubPhase::Start:
                return "Start";
            case ComposeSubPhase::ComposingLLM:
                return "ComposingLLM";
            case ComposeSubPhase::LLMResultReady:
                return "LLMResultReady";
            case ComposeSubPhase::SelectingPoint:
                return "SelectingPoint";
            case ComposeSubPhase::StartingQuest:
                return "StartingQuest";
            case ComposeSubPhase::Warping:
                return "Warping";
            case ComposeSubPhase::VerifyingFill:
                return "VerifyingFill";
            case ComposeSubPhase::Arming:
                return "Arming";
            case ComposeSubPhase::Succeeded:
                return "Succeeded";
            case ComposeSubPhase::Failed:
                return "Failed";
            }
            return "?";
        }

        // Transition the sub-phase machine. On a Failed transition,
        // records `reason` as the failure-reason string; on any other
        // transition, `reason` is ignored.
        //
        // Every transition is logged, not just the failing ones. COMPOSE
        // is six steps deep and most of what can go wrong leaves the beat
        // sitting in one of them rather than announcing anything, so the
        // last transition in the log is how anyone reading it afterwards
        // knows how far the beat actually got.
        void SetSubPhase(ComposeSubPhase phase, const char* reason = nullptr)
        {
            const auto previous = g_subPhase.Get();
            if (reason && phase == ComposeSubPhase::Failed) {
                logger::warn("NPCVisitBeat: COMPOSE {} -> {} ({})",
                             ComposeSubPhaseName(previous),
                             ComposeSubPhaseName(phase),
                             reason);
                g_subPhase.Fail(phase, reason);
            } else {
                logger::info(
                    "NPCVisitBeat: COMPOSE {} -> {}", ComposeSubPhaseName(previous), ComposeSubPhaseName(phase));
                g_subPhase.Set(phase);
            }
        }

        // ---- Sender turn / event bookkeeping shared with sinks -----

        bool ObserveAnyCombat()
        {
            if (EngineUtils::IsPlayerInCombat())
                return true;
            if (auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr) {
                if (auto* senderActor = senderRef->As<RE::Actor>()) {
                    if (senderActor->IsInCombat())
                        return true;
                }
            }
            return false;
        }

        std::string BuildTurnArgsJson(std::string_view turnKind, std::uint8_t nudgeCount)
        {
            const auto snap = VisitState::GetSnapshot();

            auto* player = RE::PlayerCharacter::GetSingleton();
            std::string playerName;
            if (player) {
                if (const auto* n = player->GetName())
                    playerName = n;
            }
            std::string senderName;
            if (auto* form = RE::TESForm::LookupByID(snap.senderFormID)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    if (const auto* n = actor->GetName())
                        senderName = n;
                }
            }
            nlohmann::json j;
            j["speaker"] = senderName;
            j["target"] = playerName;
            j["topic"] = snap.topicTag;
            j["turn_kind"] = turnKind;
            j["topic_tag"] = snap.topicTag;
            j["mood"] = snap.mood;
            j["briefing"] = snap.briefingText;
            j["goal"] = snap.briefingText;
            j["nudge_count"] = nudgeCount;
            return j.dump();
        }

        // Visit-specific candidate viability filter used by
        // IsAvailable's cheap CountViable walk. Kept in sync with
        // VisitComposer's filter so the count matches what Compose()
        // will end up building.
        bool VisitViabilityFilter_ForCountViable(RE::Actor* actor, std::string* skipReasonOut)
        {
            if (!actor) {
                if (skipReasonOut)
                    *skipReasonOut = "missing-actor";
                return false;
            }
            if (auto* base = actor->GetActorBase()) {
                if (!base->IsUnique()) {
                    if (skipReasonOut)
                        *skipReasonOut = "not-unique";
                    return false;
                }
            }
            if (actor->IsInCombat()) {
                if (skipReasonOut)
                    *skipReasonOut = "in-combat";
                return false;
            }
            if (actor->IsPlayerTeammate()) {
                if (skipReasonOut)
                    *skipReasonOut = "player-follower";
                return false;
            }
            if (!actor->GetCurrentLocation()) {
                if (skipReasonOut)
                    *skipReasonOut = "no-current-location";
                return false;
            }
            return true;
        }

        // Put the sender back where the beat found them.
        //
        // The anchor is the good answer: it is a real reference, so MoveTo
        // handles the cell change for us. When it is missing -- the fill
        // never landed, or nothing could be placed -- the snapshot still
        // knows where they were standing, and putting them there is far
        // better than the alternative this replaced, which was to skip the
        // move entirely and abandon them wherever the visit ended.
        //
        // KNOWN LIMIT: the fallback is a position, not a reference, so it
        // cannot carry the sender across a cell boundary. A visit that
        // started indoors and lost its anchor leaves them outside their own
        // front door rather than inside it.
        void SendSenderHome(RE::Actor* senderActor, const char* tag)
        {
            if (!senderActor) {
                return;
            }
            const auto snap = VisitState::GetSnapshot();
            auto* anchorRef = g_returnAnchorAlias ? g_returnAnchorAlias->GetReference() : nullptr;
            if (anchorRef) {
                senderActor->MoveTo(anchorRef);
                logger::info("NPCVisitBeat[{}]: sent sender 0x{:08X} home to anchor 0x{:08X}",
                             tag,
                             senderActor->GetFormID(),
                             anchorRef->GetFormID());
            } else {
                senderActor->SetPosition(snap.returnPosition, /*a_updateCharController=*/true);
                senderActor->Update3DPosition(/*a_warp=*/true);
                logger::warn("NPCVisitBeat[{}]: no return anchor for sender 0x{:08X} — placed them at the "
                             "snapshotted position ({:.0f},{:.0f},{:.0f}) instead",
                             tag,
                             senderActor->GetFormID(),
                             snap.returnPosition.x,
                             snap.returnPosition.y,
                             snap.returnPosition.z);
            }
            senderActor->data.angle.z = snap.returnAngleZ;
        }

        // ---- Hard-abort helper -------------------------------------

        void HardAbortVisit(const char* reason);

        bool CheckHardAbortConditions()
        {
            if (g_hardAbortFired.load())
                return true;
            if (!g_visitQuest)
                return false;
            const auto stage = g_visitQuest->GetCurrentStageID();
            if (stage == 0 || stage == kStageRollback || stage == kStageShutdown)
                return false;

            const auto& cfg = Settings::Get();
            const auto failures = VisitConclusionPoll::ConsecutivePollFailures();
            const auto cap = static_cast<std::uint32_t>(std::max(1, cfg.visitConclusionPollMaxConsecutiveFailures));
            if (failures >= cap) {
                logger::warn("NPCVisitBeat[HARD-ABORT-CHECK]: poll_broken — "
                             "consecutivePollFailures={} >= cap={}",
                             failures,
                             cap);
                HardAbortVisit("poll_broken");
                return true;
            }
            return false;
        }

        // ---- Sinks (main-thread; fire stage transitions) -----------

        struct VisitDeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
                                                  RE::BSTEventSource<RE::TESDeathEvent>* /*src*/) override
            {
                if (!a_event || !a_event->actorDying) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!g_visitQuest)
                    return RE::BSEventNotifyControl::kContinue;
                const auto stage = g_visitQuest->GetCurrentStageID();
                if (stage == 0 || stage == kStageRollback || stage == kStageShutdown) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* dyingRefPtr = a_event->actorDying.get();
                if (!dyingRefPtr)
                    return RE::BSEventNotifyControl::kContinue;
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                if (player && dyingRefPtr == player) {
                    logger::warn("NPCVisitBeat[SINK]: player death observed during visit "
                                 "(stage={}) — triggering hard-abort",
                                 stage);
                    HardAbortVisit("player_death");
                } else if (senderRef && dyingRefPtr == senderRef) {
                    logger::warn("NPCVisitBeat[SINK]: sender death observed during visit "
                                 "(stage={}, sender=0x{:08X}) — triggering hard-abort",
                                 stage,
                                 dyingRefPtr->GetFormID());
                    HardAbortVisit("sender_death");
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        VisitDeathSink g_deathSink;
        std::atomic<bool> g_sinksRegistered = false;

        void RegisterSinks()
        {
            if (g_sinksRegistered.exchange(true))
                return;
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESDeathEvent>(&g_deathSink);
                logger::info("NPCVisitBeat: death sink registered");
            } else {
                logger::warn("NPCVisitBeat: ScriptEventSourceHolder unavailable — "
                             "death sink NOT registered");
            }
        }

        // ---- Push history + hard-abort teardown --------------------

        void PushCompletedHistory()
        {
            VisitState::HistoryEntry entry;
            const auto snap = VisitState::GetSnapshot();
            const int nudgeCap = std::max(1, Settings::Get().visitMaxIgnoreNudges);
            entry.dispatchedAt = snap.dispatchedAtRealSeconds;
            entry.topicTag = snap.topicTag;
            entry.outcome =
                (snap.ignoreNudgeCount >= nudgeCap) ? VisitState::Outcome::Unsatisfied : VisitState::Outcome::Completed;
            entry.durationSeconds =
                snap.dispatchedAtRealSeconds > 0.0 ? (RealSecondsNow() - snap.dispatchedAtRealSeconds) : 0.0;
            if (auto* form = RE::TESForm::LookupByID(snap.senderFormID)) {
                if (auto* a = form->As<RE::Actor>()) {
                    if (auto* n = a->GetName())
                        entry.senderName = n;
                }
            }
            VisitState::PushHistory(std::move(entry));
        }

        void PushRolledBackHistory(RE::Actor* senderActor)
        {
            VisitState::HistoryEntry entry;
            const auto snap = VisitState::GetSnapshot();
            entry.dispatchedAt = snap.dispatchedAtRealSeconds;
            entry.senderName = senderActor ? senderActor->GetName() : "";
            entry.topicTag = snap.topicTag;
            entry.outcome = VisitState::Outcome::RolledBack;
            entry.durationSeconds =
                snap.dispatchedAtRealSeconds > 0.0 ? (RealSecondsNow() - snap.dispatchedAtRealSeconds) : 0.0;
            VisitState::PushHistory(std::move(entry));
        }

        void PushAbortedHistory(RE::Actor* senderActor)
        {
            VisitState::HistoryEntry entry;
            const auto snap = VisitState::GetSnapshot();
            entry.dispatchedAt = snap.dispatchedAtRealSeconds;
            entry.topicTag = snap.topicTag;
            entry.outcome = VisitState::Outcome::Aborted;
            entry.durationSeconds =
                snap.dispatchedAtRealSeconds > 0.0 ? (RealSecondsNow() - snap.dispatchedAtRealSeconds) : 0.0;
            if (senderActor) {
                if (auto* n = senderActor->GetName())
                    entry.senderName = n;
            }
            VisitState::PushHistory(std::move(entry));
        }

        void HardAbortVisit(const char* reason)
        {
            if (g_hardAbortFired.exchange(true)) {
                logger::debug("NPCVisitBeat[HARD-ABORT]: already fired for this visit "
                              "(second trigger reason='{}' ignored)",
                              reason);
                return;
            }
            if (!g_visitQuest) {
                logger::warn("NPCVisitBeat[HARD-ABORT]: quest handle null (reason='{}')", reason);
                return;
            }
            const auto stage = g_visitQuest->GetCurrentStageID();
            logger::warn("NPCVisitBeat: hard-abort (reason={}, stage={})", reason, stage);
            {
                std::scoped_lock lock(g_hardAbortReasonMutex);
                g_hardAbortReason = reason;
            }
            VisitConclusionPoll::Disarm();
            g_onHoldCombatStartedAtCombatSec.store(0.0);

            auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
            auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;

            if (senderActor && !senderActor->IsDead()) {
                SendSenderHome(senderActor, "HARD-ABORT");
            } else if (senderActor) {
                logger::info("NPCVisitBeat[HARD-ABORT]: sender dead; skipping teleport/demote");
            }
            QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageShutdown);
            PushAbortedHistory(senderActor);
            g_terminalCleanupDone.store(true);
        }

        // ---- Poll verdict handler (Discuss) ------------------------

        void FireValediction(bool closingAlreadySpoken);

        void HandleVisitPollVerdict(std::optional<VisitConclusionPoll::PollVerdict> verdict)
        {
            if (!verdict) {
                logger::warn("VisitPoll: parse failed");
                return;
            }
            logger::info("VisitPoll: fired (verdict={}, rationale=\"{}\", "
                         "closing_already_spoken={})",
                         verdict->shouldConclude ? "true" : "false",
                         verdict->rationale,
                         verdict->closingAlreadySpoken);
            if (!g_visitQuest || g_visitQuest->GetCurrentStageID() != static_cast<std::uint16_t>(kStageDiscuss)) {
                logger::debug("VisitPoll: verdict arrived after Discuss ended; discarding");
                return;
            }
            const auto& cfg = Settings::Get();
            if (verdict->shouldConclude) {
                logger::info("NPCVisitBeat[DISCUSS]: verdict=concluded — advancing to "
                             "Valediction");
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageValediction);
                VisitConclusionPoll::Disarm();
                FireValediction(verdict->closingAlreadySpoken);
                return;
            }
            const double silence = VisitConclusionPoll::SilenceRealSeconds();
            const double silenceLimit = static_cast<double>(std::max(0, cfg.visitPollSilenceRealSeconds));
            if (silenceLimit <= 0.0 || silence < silenceLimit) {
                logger::debug("NPCVisitBeat[DISCUSS]: verdict=continue; silence gate not "
                              "tripped ({:.1f}s < {:.1f}s real) — no nudge",
                              silence,
                              silenceLimit);
                return;
            }
            auto snap = VisitState::GetSnapshot();
            const std::uint8_t nextNudge = static_cast<std::uint8_t>(std::min<int>(255, snap.ignoreNudgeCount + 1));
            snap.ignoreNudgeCount = nextNudge;
            VisitState::SetSnapshot(snap);
            logger::info("NPCVisitBeat[DISCUSS]: silence gate tripped ({:.1f}s >= {:.1f}s "
                         "real) — firing ContinueConversation (nudge #{})",
                         silence,
                         silenceLimit,
                         nextNudge);
            VMDispatchRunSenderAction(g_visitQuest, "ContinueConversation", "");
            const int nudgeCap = std::max(1, cfg.visitMaxIgnoreNudges);
            if (nextNudge >= nudgeCap) {
                logger::info("NPCVisitBeat[DISCUSS]: nudge cap reached ({} >= {}) — "
                             "forcing Valediction",
                             nextNudge,
                             nudgeCap);
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageValediction);
                VisitConclusionPoll::Disarm();
                FireValediction(false);
            }
        }

        void FireValediction(bool closingAlreadySpoken)
        {
            if (g_valedictionFired.exchange(true)) {
                logger::debug("NPCVisitBeat[VALEDICTION]: already fired — skipping duplicate");
                return;
            }
            if (!g_visitQuest)
                return;
            const auto snap = VisitState::GetSnapshot();
            // Watermark the sender's memory pool at Valediction entry.
            // Any dialogue-turn memories from this visit's Discuss
            // phase have absolute game-hours strictly less than this
            // stamp, so they filter out of the sender's memory tail on
            // subsequent visit-beat candidate builds — the same
            // sender can't be re-picked to visit about a topic they
            // already visited about. Deliberately AFTER the guard
            // above so rolled-back / hard-aborted visits don't stamp.
            NPCVisitBeat_Cooldowns::OnVisitReachedValediction(snap.senderFormID);
            logger::info("NPCVisitBeat: Valediction entry (nudge_count={}, "
                         "closing_already_spoken={})",
                         snap.ignoreNudgeCount,
                         closingAlreadySpoken);
            const int nudgeCap = std::max(1, Settings::Get().visitMaxIgnoreNudges);
            const bool ignored = snap.ignoreNudgeCount >= nudgeCap;
            // Resolve the sender's display name so the closing beat
            // refers to them by name instead of the abstract "the sender".
            // Falls back to "the sender" only if resolution fails
            // (mid-flight sender deletion, corrupted snapshot, etc.).
            std::string senderName;
            if (auto* form = RE::TESForm::LookupByID(snap.senderFormID)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    if (const auto* n = actor->GetName())
                        senderName = n;
                }
            }
            if (senderName.empty())
                senderName = "the sender";
            const std::string closingNarration =
                ignored ? "Having failed to hold the conversation, " + senderName
                              + " prepares to leave, frustrated by being ignored."
                        : "Having said what needed saying, " + senderName + " prepares to take their leave.";
            if (closingAlreadySpoken) {
                logger::info("NPCVisitBeat[VALEDICTION]: dispatching SILENT closing scene "
                             "event ({} chars, ignored={})",
                             closingNarration.size(),
                             ignored);
                VMDispatchRunSenderSilentSceneEvent(g_visitQuest, closingNarration);
            } else {
                logger::info("NPCVisitBeat[VALEDICTION]: dispatching closing narration "
                             "({} chars, ignored={})",
                             closingNarration.size(),
                             ignored);
                VMDispatchRunSenderNarration(g_visitQuest, closingNarration);
            }
            g_valedictionEnteredAtNormalSec.store(NormalElapsedNow());
        }

        // ---- Discuss speech sampler --------------------------------

        bool SampleAndRegisterNewSpeechTurns(RE::FormID senderFormID)
        {
            if (!SkyrimNetAPI::IsAvailable())
                return false;
            const auto raw = SkyrimNetAPI::GetRecentDialogue(senderFormID, kDiscussSpeechSamplePerTick);
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array())
                return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const std::string playerName = player && player->GetName() ? std::string{player->GetName()} : std::string{};
            std::string senderName;
            if (auto* form = RE::TESForm::LookupByID(senderFormID)) {
                if (auto* a = form->As<RE::Actor>()) {
                    if (auto* n = a->GetName())
                        senderName = n;
                }
            }
            const double lastSeen = g_lastSampledEventGameTime.load();
            double maxSeen = lastSeen;
            int newCount = 0;
            bool sawPlayerTurn = false;
            for (const auto& entry : parsed) {
                if (!entry.is_object())
                    continue;
                double gameTime = 0.0;
                if (auto it = entry.find("gameTime"); it != entry.end() && it->is_number()) {
                    gameTime = it->get<double>();
                }
                if (gameTime <= lastSeen)
                    continue;
                if (gameTime > maxSeen)
                    maxSeen = gameTime;
                std::string speakerRaw;
                if (auto it = entry.find("speaker"); it != entry.end() && it->is_string()) {
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
                if (isPlayerTurn)
                    sawPlayerTurn = true;
                VisitConclusionPoll::RegisterSpeechTurn();
                ++newCount;
            }
            if (maxSeen > lastSeen) {
                g_lastSampledEventGameTime.store(maxSeen);
            }
            if (newCount > 0 && Settings::Get().debugMode) {
                logger::debug("NPCVisitBeat[DISCUSS]: sampled {} new dialogue turn(s) "
                              "(player_turn={})",
                              newCount,
                              sawPlayerTurn);
            }
            return sawPlayerTurn;
        }

        void ResetIgnoreNudgeCounter()
        {
            auto snap = VisitState::GetSnapshot();
            if (snap.ignoreNudgeCount == 0)
                return;
            logger::info("NPCVisitBeat[DISCUSS]: player spoke — resetting ignore-nudge "
                         "counter from {} to 0",
                         snap.ignoreNudgeCount);
            snap.ignoreNudgeCount = 0;
            VisitState::SetSnapshot(snap);
        }

        // ---- ReturnHome shutdown chain -----------------------------

        void RunReturnHomeShutdown(const char* triggerReason)
        {
            logger::info("NPCVisitBeat[RETURNHOME]: exit condition tripped ({}) — running "
                         "shutdown chain",
                         triggerReason);
            auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
            auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;
            if (senderActor && !senderActor->IsDead()) {
                // Was a MoveTo onto the sender's own reference when the
                // anchor was missing, which moves nobody anywhere.
                SendSenderHome(senderActor, "RETURNHOME");
                senderActor->EvaluatePackage();
            } else if (senderActor) {
                logger::info("NPCVisitBeat[RETURNHOME]: sender dead; skipping teleport/demote");
            }
            QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageShutdown);
            PushCompletedHistory();
            g_terminalCleanupDone.store(true);
        }

        void FireComposeLLM();

        void FireComposeLLM()
        {
            RE::FormID senderFormID = 0;
            VisitComposer::UrgencyHint urgency = VisitComposer::UrgencyHint::Medium;
            std::string justification;
            {
                std::scoped_lock lock(g_sessionMutex);
                senderFormID = g_paramSenderFormID;
                urgency = g_paramUrgency;
                justification = g_paramJustification;
            }
            BeatContext composeCtx;
            composeCtx.desiredDirection = PhaseTracker::Direction::Raise;
            composeCtx.tensionDelta = 0;
            // SkyrimNetAPI's async adapter routes the completion callback
            // through AsyncDispatch::EnqueueWork before invoking it, so
            // the body below runs on the plugin thread.
            VisitComposer::Compose(composeCtx,
                                   urgency,
                                   senderFormID,
                                   std::move(justification),
                                   [](std::optional<VisitComposer::VisitBriefing> briefing) {
                                       if (!briefing) {
                                           SetSubPhase(ComposeSubPhase::Failed, "compose_llm_failed");
                                           return;
                                       }
                                       VisitState::Snapshot snap;
                                       {
                                           std::scoped_lock lock(g_sessionMutex);
                                           snap.senderFormID = g_paramSenderFormID;
                                       }
                                       snap.briefingText = briefing->briefing;
                                       snap.narrationText = briefing->narration;
                                       snap.topicTag = briefing->topicTag;
                                       snap.mood = briefing->mood;
                                       snap.dispatchedAtRealSeconds = RealSecondsNow();
                                       VisitState::SetSnapshot(snap);
                                       SetSubPhase(ComposeSubPhase::LLMResultReady);
                                   });
        }

        // ---- COMPOSE sub-phases ------------------------------------
        //
        // Each one does its work through a blocking main-thread hop, sets
        // the next sub-phase, and returns; the Tick after it picks up
        // where this left off. Same shape as AmbushBeat's COMPOSE, and
        // the reason VerifyingFill is a phase of its own rather than a
        // line at the end of Warping: VMDispatchOnQuest reports that the
        // call was QUEUED, not that it ran, so the aliases can only be
        // read back on a later tick.

        // Delete the temporary marker the sender was warped onto, if one
        // is still outstanding. Safe to call more than once.
        void DeleteArrivalMarker(const MainThread::Token&)
        {
            const RE::FormID markerID = g_arrivalMarkerFormID.exchange(0, std::memory_order_acq_rel);
            if (markerID == 0) {
                return;
            }
            auto* form = RE::TESForm::LookupByID(markerID);
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!ref) {
                logger::warn("NPCVisitBeat: arrival marker 0x{:08X} no longer resolves", markerID);
                return;
            }
            // Disable + SetDelete, not the Papyrus DisableNoWait /
            // Delete pair -- neither of those has a CommonLibSSE-NG
            // binding. This is the same teardown AmbushBeat uses on its
            // own spawned references.
            ref->Disable();
            ref->SetDelete(true);
            logger::info("NPCVisitBeat: arrival marker 0x{:08X} deleted", markerID);
        }

        // SelectingPoint -- where should this visitor come from?
        //
        // Deliberately ahead of StartingQuest: this is the step most
        // likely to fail, and failing it here means there is no quest to
        // tear down afterwards.
        TickResult ComposeSelectPoint(const PluginThread::Token& pt)
        {
            const auto snap = VisitState::GetSnapshot();
            if (snap.senderFormID == 0) {
                logger::warn("NPCVisitBeat: reached SelectingPoint with no sender in the snapshot — the "
                             "compose result never landed");
                SetSubPhase(ComposeSubPhase::Failed, "no_composition_at_dispatch");
                return {};
            }

            std::string liveResolveReason;
            auto* sender = MainThread::Run(pt, [&](const MainThread::Token&) {
                return BeatParamHelpers::ResolveLiveSenderActor(snap.senderFormID, &liveResolveReason);
            });
            if (!sender) {
                g_subPhase.Fail(ComposeSubPhase::Failed, std::move(liveResolveReason));
                return {};
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto arrival = VisitArrivalPoint::Find(pt, sender, player);
            if (!arrival.Ok()) {
                // Not an error. There is no road they could have walked
                // in on, and arriving from nowhere in particular is the
                // thing this beat exists to stop doing.
                logger::info("NPCVisitBeat: no arrival point for sender 0x{:08X} -- declining the visit",
                             snap.senderFormID);
                SetSubPhase(ComposeSubPhase::Failed, "arrival_no_point");
                return {};
            }

            logger::info("NPCVisitBeat: arrival tier={} at ({:.0f},{:.0f},{:.0f}) with {} fallback(s)",
                         VisitArrivalPoint::TierName(arrival.tier),
                         arrival.point.x,
                         arrival.point.y,
                         arrival.point.z,
                         arrival.fallbacks.size());
            {
                std::scoped_lock lock(g_sessionMutex);
                g_arrival = std::move(arrival);
            }
            SetSubPhase(ComposeSubPhase::StartingQuest);
            return {};
        }

        // StartingQuest -- snapshot the pose, plant the anchor, start the
        // quest.
        //
        // The anchor is created BEFORE anything moves, because it marks
        // where the sender was standing when the beat picked them. Once
        // Warping has run, that place is no longer anywhere the sender
        // can be asked about.
        TickResult ComposeStartQuest(const PluginThread::Token& pt)
        {
            const bool ok = MainThread::Run(pt, [](const MainThread::Token& mt) {
                auto snap = VisitState::GetSnapshot();
                std::string reason;
                auto* sender = BeatParamHelpers::ResolveLiveSenderActor(snap.senderFormID, &reason);
                if (!sender) {
                    logger::warn("NPCVisitBeat: sender no longer resolves at quest start ({})", reason);
                    return false;
                }

                snap.returnPosition = sender->GetPosition();
                snap.returnAngleZ = sender->GetAngleZ();
                if (auto* parentCell = sender->GetParentCell()) {
                    snap.returnCellFormID = parentCell->GetFormID();
                }
                snap.ignoreNudgeCount = 0;
                snap.consecutivePollFailures = 0;

                // PlaceObjectAtMe on the SENDER, so the anchor lands in
                // the sender's own cell rather than the player's.
                snap.returnAnchorFormID = 0;
                if (g_xMarkerBase) {
                    if (auto anchor = sender->PlaceObjectAtMe(g_xMarkerBase, /*a_forcePersist=*/true)) {
                        snap.returnAnchorFormID = anchor->GetFormID();
                    }
                }
                VisitState::SetSnapshot(snap);
                logger::info("NPCVisitBeat: snapshotted sender at ({:.1f},{:.1f},{:.1f}) in cell "
                             "0x{:08X}, anchor=0x{:08X}",
                             snap.returnPosition.x,
                             snap.returnPosition.y,
                             snap.returnPosition.z,
                             snap.returnCellFormID,
                             snap.returnAnchorFormID);
                if (snap.returnAnchorFormID == 0) {
                    // Survivable: every cleanup path falls back to the
                    // snapshotted position, and only the walk home is
                    // lost. Worth a warning, not a failure.
                    logger::warn("NPCVisitBeat: no return anchor could be placed -- the sender will be "
                                 "teleported home rather than walking");
                }

                bool engineResult = false;
                const bool callOk = g_visitQuest->EnsureQuestStarted(engineResult, /*a_startNow=*/true);
                if (!callOk || !engineResult) {
                    logger::warn(
                        "NPCVisitBeat: EnsureQuestStarted failed (callOk={}, engineResult={})", callOk, engineResult);
                    DeleteArrivalMarker(mt);
                    return false;
                }
                return true;
            });

            if (!ok) {
                SetSubPhase(ComposeSubPhase::Failed, "quest_start_failed");
                return {};
            }
            SetSubPhase(ComposeSubPhase::Warping);
            return {};
        }

        // Warping -- put the sender on the arrival point and dispatch
        // both force-fills.
        //
        // MoveTo rather than SetPosition: the sender may be in a
        // different worldspace entirely, which raw coordinates would not
        // survive. That needs a reference to move ONTO, hence the
        // throwaway marker.
        TickResult ComposeWarp(const PluginThread::Token& pt)
        {
            RE::NiPoint3 point{};
            {
                std::scoped_lock lock(g_sessionMutex);
                point = g_arrival.point;
            }

            const bool ok = MainThread::Run(pt, [point](const MainThread::Token&) {
                auto snap = VisitState::GetSnapshot();
                std::string reason;
                auto* sender = BeatParamHelpers::ResolveLiveSenderActor(snap.senderFormID, &reason);
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!sender || !player || !g_xMarkerBase) {
                    logger::warn("NPCVisitBeat: cannot warp (sender={} player={} markerBase={})",
                                 sender != nullptr,
                                 player != nullptr,
                                 g_xMarkerBase != nullptr);
                    return false;
                }

                auto marker = player->PlaceObjectAtMe(g_xMarkerBase, /*a_forcePersist=*/true);
                if (!marker) {
                    logger::warn("NPCVisitBeat: PlaceObjectAtMe returned no arrival marker");
                    return false;
                }
                marker->SetPosition(point);
                // Face the player, so the sender arrives looking the way
                // someone walking toward them would. MoveTo copies the
                // target's rotation, which is why the marker is an
                // XMarkerHeading rather than a plain XMarker.
                const auto playerPos = player->GetPosition();
                marker->data.angle.z = std::atan2(playerPos.x - point.x, playerPos.y - point.y);
                marker->Update3DPosition(/*a_warp=*/true);
                g_arrivalMarkerFormID.store(marker->GetFormID(), std::memory_order_release);

                sender->MoveTo(marker.get());
                logger::info("NPCVisitBeat: warped sender 0x{:08X} to ({:.0f},{:.0f},{:.0f}) via marker "
                             "0x{:08X}",
                             snap.senderFormID,
                             point.x,
                             point.y,
                             point.z,
                             marker->GetFormID());

                // ForceRefTo has no native binding, so both fills go
                // through the quest script. Fire-and-forget, hence the
                // readback a tick later. Passed as FormIDs, not
                // references; see
                // docs/engine-findings/passing-references-to-papyrus-from-cpp.md.
                const bool senderQueued = QuestUtils::VMDispatchOnQuest(
                    g_visitQuest, kQuestScriptName, "FillSenderSlot", static_cast<std::int32_t>(sender->GetFormID()));
                bool anchorQueued = true;
                if (snap.returnAnchorFormID != 0) {
                    anchorQueued = QuestUtils::VMDispatchOnQuest(g_visitQuest,
                                                                 kQuestScriptName,
                                                                 "FillReturnAnchorSlot",
                                                                 static_cast<std::int32_t>(snap.returnAnchorFormID));
                }
                // Queued is not landed -- that is what VerifyingFill is
                // for -- but a dispatch that could not even be queued is a
                // different fault with the same symptom, and only this
                // line tells them apart.
                logger::info("NPCVisitBeat: fill dispatches queued (sender={} anchor={})", senderQueued, anchorQueued);
                if (!senderQueued) {
                    logger::error("NPCVisitBeat: could not queue FillSenderSlot on '{}' — the quest script "
                                  "is probably not attached",
                                  kQuestScriptName);
                }
                return true;
            });

            if (!ok) {
                MainThread::Run(pt, [](const MainThread::Token& mt) {
                    DeleteArrivalMarker(mt);
                    return 0;
                });
                SetSubPhase(ComposeSubPhase::Failed, "warp_failed");
                return {};
            }
            g_fillVerifyTicks.store(0, std::memory_order_release);
            SetSubPhase(ComposeSubPhase::VerifyingFill);
            return {};
        }

        struct AliasFillState
        {
            RE::FormID sender = 0;
            RE::FormID anchor = 0;
        };

        // VerifyingFill -- read both aliases back, a tick or more after
        // the dispatch.
        TickResult ComposeVerifyFill(const PluginThread::Token& pt)
        {
            const auto snap = VisitState::GetSnapshot();
            const bool anchorExpected = snap.returnAnchorFormID != 0;

            const auto filled = MainThread::Run(pt, [](const MainThread::Token& mt) {
                // The marker has done its job the moment the move landed.
                // Deleted here rather than at the end of Warping so a
                // full tick has passed first -- MoveTo is believed
                // synchronous, and the cost of not relying on that is one
                // tick.
                DeleteArrivalMarker(mt);
                AliasFillState out;
                if (g_senderAlias && g_senderAlias->GetReference()) {
                    out.sender = g_senderAlias->GetReference()->GetFormID();
                }
                if (g_returnAnchorAlias && g_returnAnchorAlias->GetReference()) {
                    out.anchor = g_returnAnchorAlias->GetReference()->GetFormID();
                }
                return out;
            });

            if (filled.sender != 0 && (!anchorExpected || filled.anchor != 0)) {
                logger::info("NPCVisitBeat: fills verified -- Sender=0x{:08X} ReturnAnchor=0x{:08X}",
                             filled.sender,
                             filled.anchor);
                SetSubPhase(ComposeSubPhase::Arming);
                return {};
            }

            const int ticks = g_fillVerifyTicks.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (ticks < kFillVerifyMaxTicks) {
                return {};
            }

            // Both FormIDs we ASKED for, alongside what actually landed.
            // The trampolines themselves trace to Papyrus.0.log rather than
            // here, so these are what let the two logs be lined up without
            // running the visit again.
            logger::warn("NPCVisitBeat: fills did not land after {} ticks — dispatched sender=0x{:08X} "
                         "anchor=0x{:08X}, read back sender=0x{:08X} anchor=0x{:08X}",
                         ticks,
                         snap.senderFormID,
                         snap.returnAnchorFormID,
                         filled.sender,
                         filled.anchor);
            MainThread::Run(pt, [](const MainThread::Token&) {
                auto snapshot = VisitState::GetSnapshot();
                std::string reason;
                if (auto* sender = BeatParamHelpers::ResolveLiveSenderActor(snapshot.senderFormID, &reason)) {}
                QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageRollback);
                return 0;
            });
            // The sender's own fill is what the beat cannot proceed
            // without; a missing anchor only costs the walk home, so the
            // two are reported apart.
            SetSubPhase(ComposeSubPhase::Failed,
                        filled.sender == 0 ? "sender_fill_unverified" : "anchor_fill_unverified");
            return {};
        }

        // Arming -- bind the package and hand the approach to the escort.
        TickResult ComposeArm(const PluginThread::Token& pt)
        {
            std::vector<RE::NiPoint3> fallbacks;
            RE::NiPoint3 placedAt{};
            {
                std::scoped_lock lock(g_sessionMutex);
                fallbacks = g_arrival.fallbacks;
                placedAt = g_arrival.point;
            }

            // VerifyingFill proved the alias holds SOMETHING. Whether
            // that something is an actor is a separate question, and the
            // answer decides whether anyone can be sent walking. Arming
            // used to skip its work quietly when the cast failed and
            // report success anyway, which produces a visit that runs its
            // full length with nobody ever arriving -- indistinguishable,
            // from the outside, from a sender who simply could not path.
            const bool armed = MainThread::Run(pt, [&](const MainThread::Token&) {
                auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                if (!senderRef) {
                    logger::error("NPCVisitBeat: Sender alias empty at Arming");
                    return false;
                }
                auto* senderActor = senderRef->As<RE::Actor>();
                if (!senderActor) {
                    auto* base = senderRef->GetBaseObject();
                    logger::error("NPCVisitBeat: Sender alias holds ref 0x{:08X} (base 0x{:08X}) which is not "
                                  "an actor — nobody would ever walk over",
                                  senderRef->GetFormID(),
                                  base ? base->GetFormID() : 0u);
                    return false;
                }
                senderActor->EvaluatePackage();
                // Fallbacks are further along the visitor's own road, so
                // escalating through them walks a stuck sender BACK ALONG
                // THEIR ROUTE rather than sideways onto unrelated terrain.
                g_escort.Begin(fallbacks);
                g_escort.Track(senderActor, placedAt);
                logger::info("NPCVisitBeat: armed sender 0x{:08X} '{}' at ({:.0f},{:.0f},{:.0f}); escort has "
                             "{} fallback(s)",
                             senderActor->GetFormID(),
                             senderRef->GetDisplayFullName(),
                             placedAt.x,
                             placedAt.y,
                             placedAt.z,
                             fallbacks.size());
                return true;
            });

            if (!armed) {
                SetSubPhase(ComposeSubPhase::Failed, "sender_not_actor");
                return {};
            }

            // Start the escort's clock HERE, not at OnStart.
            //
            // The clock measures the gap between checks as a delta against
            // this stamp, and leaving it at zero made the first delta the
            // whole of COMPOSE -- an LLM call, several seconds -- which
            // cleared the check interval instantly. Every visit therefore
            // got its first stall check about half a second after the
            // visitor was put down, saw them barely moved because they had
            // barely had time to move, and warped them to a fallback they
            // never needed. One check interval has to pass between being
            // placed and being judged for failing to walk.
            g_lastEscortSampleNormalSec.store(NormalElapsedNow(), std::memory_order_release);
            g_salutationEnteredAtNormalSec.store(NormalElapsedNow());
            g_lastDistanceLogNormalSec.store(0.0);
            VisitState::SetComposingSender(false);
            SetSubPhase(ComposeSubPhase::Succeeded);
            return {};
        }

        // Move the sender on when the walk in has stalled.
        //
        // Only while they are still walking: the escort supervises the
        // APPROACH and nothing else. Once they are talking to the player
        // there is nothing left to be stuck on, and warping someone
        // mid-conversation is worse than any stall.
        //
        // The clock is plugin-thread arithmetic, so waiting costs no
        // main-thread hop -- only the check it gates does.
        void DriveApproachEscort(const PluginThread::Token& pt, TickMode mode)
        {
            if (mode != TickMode::Normal || !g_visitQuest) {
                return;
            }
            const auto stage = g_visitQuest->GetCurrentStageID();
            if (stage != kStageSalutation) {
                return;
            }
            const auto& cfg = Settings::Get();
            StuckRecovery::Options opts;
            opts.movementThresholdUnits = static_cast<float>(cfg.stuckRecoveryMovementThresholdUnits);
            opts.checkIntervalSeconds = static_cast<double>(cfg.stuckRecoveryCheckIntervalSeconds);
            opts.arrivedDistanceUnits = static_cast<float>(cfg.visitSalutationApproachDistanceUnits);

            const double nowNormal = NormalElapsedNow();
            const double sincePrev = nowNormal - g_lastEscortSampleNormalSec.exchange(nowNormal);
            if (sincePrev <= 0.0 || !g_escort.DueForCheck(sincePrev, opts)) {
                return;
            }

            MainThread::Run(pt, [&opts](const MainThread::Token& mt) {
                auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                auto* senderActor = senderRef ? senderRef->As<RE::Actor>() : nullptr;
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!senderActor || senderActor->IsDead() || !player) {
                    // Once per due check rather than once per tick, so a
                    // sender who died or fell out of the alias mid-approach
                    // leaves a trail without flooding the log.
                    logger::warn("NPCVisitBeat: escort check found no live sender to escort (ref={} actor={} "
                                 "dead={})",
                                 senderRef != nullptr,
                                 senderActor != nullptr,
                                 senderActor && senderActor->IsDead());
                    return 0;
                }
                // StuckRecovery logs each warp itself.
                g_escort.Update(mt, senderActor, player->GetPosition(), opts);
                return 0;
            });
        }

        // ---- RUNNING stage-tick (main thread; called every ~1s) ----

        // Emit a periodic warn/error while a stage-transition dispatch
        // is still waiting for the Papyrus VM to land. Called from a
        // stage handler AFTER it has already fired its transition
        // dispatch. Warn every 5s of Normal-mode time, escalating to
        // error at 30s.
        void LogPendingStageTransition(const char* stageName,
                                       std::uint32_t expectedStageAfter,
                                       std::uint32_t currentStageID,
                                       StageTransitionLatch& latch)
        {
            const auto now = NormalElapsedNow();
            const auto lastWarn = latch.lastWarningAtNormalSec.load();
            if (now - lastWarn < 5.0)
                return;
            const auto dispatchedAt = latch.dispatchedAtNormalSec.load();
            const auto elapsed = dispatchedAt > 0.0 ? (now - dispatchedAt) : 0.0;
            latch.lastWarningAtNormalSec.store(now, std::memory_order_release);
            if (elapsed >= 30.0) {
                logger::error("NPCVisitBeat[{}]: SetStage({}) dispatched {:.1f}s ago "
                              "(Normal-mode) but quest stage is still {} — Papyrus "
                              "VM stage change has not landed; suppressing repeat "
                              "dispatch to avoid narration spam loop",
                              stageName,
                              expectedStageAfter,
                              elapsed,
                              currentStageID);
            } else {
                logger::warn("NPCVisitBeat[{}]: SetStage({}) dispatched {:.1f}s ago "
                             "(Normal-mode) but quest stage is still {} — waiting "
                             "for Papyrus VM (suppressing repeat dispatch)",
                             stageName,
                             expectedStageAfter,
                             elapsed,
                             currentStageID);
            }
        }

        void RunningTick(const PluginThread::Token& pt, TickMode mode)
        {
            MainThread::FireAndForget(pt, [mode](const MainThread::Token&) {
                g_runningTaskInFlight.store(false, std::memory_order_release);
                if (!g_visitQuest)
                    return;
                if (g_hardAbortFired.load())
                    return;
                if (CheckHardAbortConditions())
                    return;

                const auto stage = g_visitQuest->GetCurrentStageID();
                const auto& cfg = Settings::Get();

                // Clear the salutation latch once the SetStage(Discuss) has
                // landed and we've moved off Salutation.
                if (stage != kStageSalutation)
                    g_salutationDiscussLatch.Reset();

                switch (stage) {
                case kStageSalutation: {
                    if (mode != TickMode::Normal)
                        return;
                    if (g_salutationDiscussLatch.IsDispatched()) {
                        LogPendingStageTransition("SALUTATION", kStageDiscuss, stage, g_salutationDiscussLatch);
                        return;
                    }
                    auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player)
                        return;
                    const int approachDist = std::max(1, cfg.visitSalutationApproachDistanceUnits);
                    const int timeoutSec = std::max(1, cfg.visitApproachTimeoutSeconds);
                    const auto dist = senderRef->GetPosition().GetDistance(player->GetPosition());
                    const auto now = NormalElapsedNow();
                    const auto enteredAt = g_salutationEnteredAtNormalSec.load();
                    const auto elapsed = enteredAt > 0.0 ? (now - enteredAt) : 0.0;
                    const auto lastLog = g_lastDistanceLogNormalSec.load();
                    if (now - lastLog >= 5.0) {
                        g_lastDistanceLogNormalSec.store(now);
                        logger::info("NPCVisitBeat[SALUTATION]: elapsed={:.1f}s, "
                                     "sender-to-player distance={:.0f}u (timeout at {}s, "
                                     "approach<={}u)",
                                     elapsed,
                                     dist,
                                     timeoutSec,
                                     approachDist);
                    }
                    if (dist <= static_cast<float>(approachDist)) {
                        logger::info("NPCVisitBeat[SALUTATION]: approach reached "
                                     "({:.0f}u) — firing opening line and advancing to "
                                     "Discuss",
                                     dist);
                        const auto snap = VisitState::GetSnapshot();
                        VMDispatchRunSenderNarration(g_visitQuest, snap.narrationText);
                        QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageDiscuss);
                        g_salutationDiscussLatch.MarkDispatched(now);
                        VisitConclusionPoll::Arm(snap);
                        g_discussSenderFormID.store(snap.senderFormID);
                        // Initialize the speech sampler cursor to now
                        // so we don't count pre-Salutation dialogue.
                        if (auto* cal = RE::Calendar::GetSingleton()) {
                            g_lastSampledEventGameTime.store(static_cast<double>(cal->GetHoursPassed()) * 3600.0);
                        }
                        NPCVisitBeat_Cooldowns::OnVisitCompleted(snap.senderFormID);
                        return;
                    }
                    if (elapsed >= static_cast<double>(timeoutSec)) {
                        logger::warn("NPCVisitBeat[SALUTATION]: timeout at {:.1f}s (limit "
                                     "{}s) — rolling back",
                                     elapsed,
                                     timeoutSec);
                        auto* senderActor = senderRef->As<RE::Actor>();
                        if (senderActor) {
                            SendSenderHome(senderActor, "SALUTATION");
                        }
                        QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageRollback);
                        VisitConclusionPoll::Disarm();
                        PushRolledBackHistory(senderActor);
                        g_terminalCleanupDone.store(true);
                    }
                    return;
                }
                case kStageDiscuss: {
                    // Sub-state machine (all C++, non-persisted). See enum
                    // definition for the full transition table.
                    const auto subPhase = g_discussSubPhase.load(std::memory_order_acquire);
                    const bool inCombat = ObserveAnyCombat();
                    const bool inDialogue = EngineUtils::IsPlayerInDialogue();
                    const bool holdTripped = inCombat || inDialogue;

                    switch (subPhase) {
                    case DiscussSubPhase::Discussing: {
                        if (holdTripped) {
                            logger::info("NPCVisitBeat[DISCUSS/Discussing]: hold trigger "
                                         "tripped (combat={}, dialogue={}) — transitioning "
                                         "to OnHold",
                                         inCombat,
                                         inDialogue);
                            g_discussSubPhase.store(DiscussSubPhase::OnHold, std::memory_order_release);
                            if (inCombat) {
                                g_onHoldCombatStartedAtCombatSec.store(CombatElapsedNow());
                            }
                            return;
                        }
                        if (mode != TickMode::Normal)
                            return;
                        const auto senderFormID = g_discussSenderFormID.load();
                        if (SampleAndRegisterNewSpeechTurns(senderFormID)) {
                            ResetIgnoreNudgeCounter();
                        }
                        if (VisitConclusionPoll::GateTick()) {
                            logger::info("NPCVisitBeat[DISCUSS/Discussing]: gate tripped — "
                                         "firing conclusion poll");
                            VisitConclusionPoll::FirePoll([](std::optional<VisitConclusionPoll::PollVerdict> v) {
                                AsyncDispatch::EnqueueWork([v = std::move(v)](const PluginThread::Token& pt) mutable {
                                    MainThread::FireAndForget(pt, [v = std::move(v)](const MainThread::Token&) mutable {
                                        HandleVisitPollVerdict(std::move(v));
                                    });
                                });
                            });
                        }
                        return;
                    }
                    case DiscussSubPhase::OnHold: {
                        if (!holdTripped) {
                            logger::info("NPCVisitBeat[DISCUSS/OnHold]: hold triggers cleared "
                                         "— transitioning to ReEngage");
                            g_discussSubPhase.store(DiscussSubPhase::ReEngage, std::memory_order_release);
                            g_onHoldCombatStartedAtCombatSec.store(0.0);
                            g_lastDistanceLogNormalSec.store(0.0);
                            return;
                        }
                        // Combat-stuck watchdog runs under Combat mode.
                        if (mode == TickMode::Combat && inCombat) {
                            const int combatMax = std::max(1, cfg.visitOnHoldCombatMaxSeconds);
                            const auto combatStart = g_onHoldCombatStartedAtCombatSec.load();
                            if (combatStart <= 0.0) {
                                g_onHoldCombatStartedAtCombatSec.store(CombatElapsedNow());
                            } else {
                                const auto elapsed = CombatElapsedNow() - combatStart;
                                if (elapsed >= static_cast<double>(combatMax)) {
                                    logger::warn("NPCVisitBeat[DISCUSS/OnHold]: combat_stuck — "
                                                 "elapsed {:.1f}s >= {}s",
                                                 elapsed,
                                                 combatMax);
                                    HardAbortVisit("combat_stuck");
                                }
                            }
                        }
                        return;
                    }
                    case DiscussSubPhase::ReEngage: {
                        if (holdTripped) {
                            logger::info("NPCVisitBeat[DISCUSS/ReEngage]: hold trigger "
                                         "re-tripped (combat={}, dialogue={}) — returning to "
                                         "OnHold",
                                         inCombat,
                                         inDialogue);
                            g_discussSubPhase.store(DiscussSubPhase::OnHold, std::memory_order_release);
                            if (inCombat) {
                                g_onHoldCombatStartedAtCombatSec.store(CombatElapsedNow());
                            }
                            return;
                        }
                        if (mode != TickMode::Normal)
                            return;
                        auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (!senderRef || !player)
                            return;
                        const int approachDist = std::max(1, cfg.visitReEngageApproachDistanceUnits);
                        const auto dist = senderRef->GetPosition().GetDistance(player->GetPosition());
                        const auto now = NormalElapsedNow();
                        const auto lastLog = g_lastDistanceLogNormalSec.load();
                        if (now - lastLog >= 5.0) {
                            g_lastDistanceLogNormalSec.store(now);
                            logger::info("NPCVisitBeat[DISCUSS/ReEngage]: distance={:.0f}u "
                                         "(threshold {}u)",
                                         dist,
                                         approachDist);
                        }
                        if (dist <= static_cast<float>(approachDist)) {
                            logger::info("NPCVisitBeat[DISCUSS/ReEngage]: approach reached "
                                         "({:.0f}u) — firing resumption narration and returning "
                                         "to Discussing",
                                         dist);
                            const auto snap = VisitState::GetSnapshot();
                            // Build a fresh resumption line rather than
                            // replaying the composed opener (which would
                            // narrate the sender arriving all over again).
                            std::string senderName;
                            if (auto* senderActor = senderRef->As<RE::Actor>()) {
                                if (const auto* n = senderActor->GetName())
                                    senderName = n;
                            }
                            if (senderName.empty())
                                senderName = "the sender";
                            std::string playerName;
                            if (const auto* n = player->GetName())
                                playerName = n;
                            if (playerName.empty())
                                playerName = "the player";
                            const std::string resumptionNarration =
                                "Now that the interruption has ended, " + senderName + " turns back to " + playerName
                                + " to try and resume their discussion where it left off.";
                            VMDispatchRunSenderNarration(g_visitQuest, resumptionNarration);
                            VisitConclusionPoll::Arm(snap);
                            g_discussSenderFormID.store(snap.senderFormID);
                            g_discussSubPhase.store(DiscussSubPhase::Discussing, std::memory_order_release);
                        }
                        return;
                    }
                    }
                    return;
                }
                case kStageValediction: {
                    if (mode != TickMode::Normal)
                        return;
                    const auto enteredAt = g_valedictionEnteredAtNormalSec.load();
                    if (enteredAt <= 0.0)
                        return;
                    const int dwellSec = std::max(1, cfg.visitValedictionDwellSeconds);
                    const auto elapsed = NormalElapsedNow() - enteredAt;
                    if (elapsed >= static_cast<double>(dwellSec)) {
                        logger::info("NPCVisitBeat[VALEDICTION]: dwell expired ({:.1f}s "
                                     ">= {}s) — advancing to ReturnHome",
                                     elapsed,
                                     dwellSec);
                        QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageReturnHome);
                        g_returnHomeStartedAtNormalSec.store(NormalElapsedNow());
                    }
                    return;
                }
                case kStageReturnHome: {
                    if (mode != TickMode::Normal)
                        return;
                    auto* senderRef = g_senderAlias ? g_senderAlias->GetReference() : nullptr;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!senderRef || !player)
                        return;
                    const int exitDist = std::max(1, cfg.visitReturnHomeExitDistanceUnits);
                    const int timeoutSec = std::max(1, cfg.visitReturnHomeTimeoutSeconds);
                    const auto dist = senderRef->GetPosition().GetDistance(player->GetPosition());
                    const bool attached = senderRef->GetParentCell() ? senderRef->GetParentCell()->IsAttached() : true;
                    const auto startAt = g_returnHomeStartedAtNormalSec.load();
                    const auto now = NormalElapsedNow();
                    const auto elapsed = startAt > 0.0 ? (now - startAt) : 0.0;
                    const bool losToSender = CameraVisibility::IsAnyPartVisibleFromCamera(senderRef);
                    const auto lastLog = g_lastDistanceLogNormalSec.load();
                    if (now - lastLog >= 5.0) {
                        g_lastDistanceLogNormalSec.store(now);
                        logger::info("NPCVisitBeat[RETURNHOME]: dist={:.0f}u (exit>={}), "
                                     "cell_attached={}, los={}, elapsed={:.1f}s (timeout={}s)",
                                     dist,
                                     exitDist,
                                     attached,
                                     losToSender,
                                     elapsed,
                                     timeoutSec);
                    }
                    if (dist >= static_cast<float>(exitDist)) {
                        RunReturnHomeShutdown("distance");
                        return;
                    }
                    if (!attached) {
                        RunReturnHomeShutdown("cell-unloaded");
                        return;
                    }
                    if (!losToSender && dist >= 2000.0f) {
                        RunReturnHomeShutdown("los-lost");
                        return;
                    }
                    if (elapsed >= static_cast<double>(timeoutSec)) {
                        RunReturnHomeShutdown("timeout");
                        return;
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
            });
        }

        void Cleanup(const PluginThread::Token& pt)
        {
            MainThread::FireAndForget(pt, [](const MainThread::Token&) {
                // If cleanup wasn't already triggered by a rollback / abort
                // path, run the terminal shutdown chain now — this covers
                // the "COMPOSE failed before any stage was set" case where
                // no other path has cleared state.
                if (!g_visitQuest) {
                    VisitState::Reset();
                    return;
                }
                const auto stage = g_visitQuest->GetCurrentStageID();
                if (stage != 0 && stage != kStageShutdown && stage != kStageRollback) {
                    // Force it into rollback so the Papyrus Shutdown
                    // fragment tears down alias fills. Guarded so we don't
                    // double-drive a stage that's already terminal.
                    QuestUtils::VMDispatchQuestSetStage(g_visitQuest, kStageRollback);
                }
                // Give Papyrus a beat, then clear VisitState.
                VisitState::Reset();
            });
        }
    } // namespace

    // ---------------------------------------------------------------
    // NPCVisitBeat_Init
    // ---------------------------------------------------------------

    namespace NPCVisitBeat_Init
    {
        void Initialize()
        {
            // Resolve every time rather than latching on the first call.
            //
            // Production calls this exactly once, at kDataLoaded, so nothing
            // changes there. The latch that used to sit here was guarding
            // nothing -- RegisterSinks has its own -- and it silently made
            // the module keep pointers from the FIRST call forever, which in
            // the test harness means pointers into storage a later case has
            // already recycled. Every assertion about the faction or the
            // aliases after the first one was reading whatever now sat at
            // that address.
            g_pointersResolved.store(true, std::memory_order_release);
            bool ok = true;
            if (auto* form = RE::TESForm::LookupByEditorID(kVisitQuestEditorID)) {
                g_visitQuest = form->As<RE::TESQuest>();
            }
            if (!g_visitQuest) {
                logger::error("NPCVisitBeat_Init: quest '{}' did not resolve — "
                              "IsAvailable will report false permanently",
                              kVisitQuestEditorID);
                ok = false;
            }
            if (g_visitQuest) {
                for (auto* a : g_visitQuest->aliases) {
                    if (!a)
                        continue;
                    if (a->aliasName == kSenderAliasName) {
                        g_senderAlias = skyrim_cast<RE::BGSRefAlias*>(a);
                    } else if (a->aliasName == kReturnAnchorAliasName) {
                        g_returnAnchorAlias = skyrim_cast<RE::BGSRefAlias*>(a);
                    }
                }
            }
            if (!g_senderAlias || !g_returnAnchorAlias) {
                logger::error("NPCVisitBeat_Init: one or more required aliases unresolved "
                              "(Sender={} ReturnAnchor={})",
                              g_senderAlias ? "ok" : "MISSING",
                              g_returnAnchorAlias ? "ok" : "MISSING");
                ok = false;
            }
            // Both runtime markers -- the arrival point the sender is
            // warped onto, and the anchor they walk back to -- are made
            // from this one static.
            if (auto* form = RE::TESForm::LookupByID(kXMarkerHeadingFormID)) {
                g_xMarkerBase = form->As<RE::TESBoundObject>();
            }
            if (!g_xMarkerBase) {
                logger::error("NPCVisitBeat_Init: XMarkerHeading (0x{:08X}) did not resolve", kXMarkerHeadingFormID);
                ok = false;
            }
            g_pointersCriticallyMissing.store(!ok);
            if (ok) {
                logger::info("NPCVisitBeat_Init: resolved quest=0x{:08X}, aliases bound", g_visitQuest->GetFormID());
                RegisterSinks();
            } else {
                logger::error("NPCVisitBeat_Init: one or more required forms missing — "
                              "NPCVisitBeat disabled for the session");
            }
        }
    } // namespace NPCVisitBeat_Init

    // ---------------------------------------------------------------
    // IBeat impl
    // ---------------------------------------------------------------

    std::string NPCVisitBeat::Name() const
    {
        return "npc_visit";
    }

    std::string NPCVisitBeat::Description() const
    {
        return "An NPC the player knows drops what they were doing and travels "
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

    BeatPolarity NPCVisitBeat::Polarity() const
    {
        return BeatPolarity::Either;
    }

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
            static_cast<std::size_t>(std::max(1, Settings::Get().visitMinSenderCandidates));
        const std::size_t viable =
            SenderCandidatePool::CountViable(&VisitViabilityFilter_ForCountViable, minCandidates);
        if (viable < minCandidates) {
            if (debug) {
                logger::debug("NPCVisitBeat::IsAvailable: blocked (only {} viable "
                              "candidates, need {})",
                              viable,
                              minCandidates);
            }
            return false;
        }
        return true;
    }

    void NPCVisitBeat::OnStart(const BeatContext& /*ctx*/, const nlohmann::json& parameters)
    {
        std::string failureReason;
        const auto senderParsed = BeatParamHelpers::ParseSenderFormID(parameters, &failureReason);
        const auto urgency = BeatParamHelpers::ParseUrgencyHint(parameters);

        // parameter_justification is optional; missing / non-string is
        // treated as "compose LLM invents motivation from memory tail."
        std::string justification;
        if (parameters.is_object()) {
            if (auto it = parameters.find("parameter_justification"); it != parameters.end() && it->is_string()) {
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
            g_paramSenderFormID = *senderParsed;
            g_paramUrgency = urgency;
            g_paramJustification = std::move(justification);
        }
        logger::info("NPCVisitBeat::OnStart: sender=0x{:08X} urgency={}",
                     senderParsed.value_or(0),
                     urgency == BeatParamHelpers::UrgencyHint::Low    ? "low"
                     : urgency == BeatParamHelpers::UrgencyHint::High ? "high"
                                                                      : "medium");
    }

    TickResult NPCVisitBeat::Tick(const PluginThread::Token& pt, TickMode mode, BeatState state)
    {
        // Advance the mode-scoped accumulators that feed the stage-gate
        // timers. Sample wall-clock on every Tick and credit the delta
        // to whichever accumulator matches this Tick's mode — Normal
        // ticks feed g_normalElapsedSec (the four "sender is actively
        // working toward the player" gates); Combat ticks feed
        // g_combatElapsedSec (the OnHold combat-stuck watchdog).
        // Dialogue and Paused ticks freeze both, but must still refresh
        // g_lastAccumulatorSampleRealSec so the first tick after
        // returning to Normal/Combat doesn't retroactively credit the
        // paused / dialogue interval.
        {
            const double nowReal = RealSecondsNow();
            const double prevReal = g_lastAccumulatorSampleRealSec.exchange(nowReal, std::memory_order_acq_rel);
            if (prevReal > 0.0) {
                const double delta = nowReal - prevReal;
                if (delta > 0.0) {
                    if (mode == TickMode::Normal) {
                        g_normalElapsedSec.fetch_add(delta, std::memory_order_acq_rel);
                    } else if (mode == TickMode::Combat) {
                        g_combatElapsedSec.fetch_add(delta, std::memory_order_acq_rel);
                    }
                }
            }
        }

        // Advance VisitConclusionPoll's silence accumulator baseline
        // for the same reason and on the same schedule — call on every
        // Tick regardless of mode so its baseline stays fresh across
        // non-Normal intervals. It's a no-op when the poll isn't
        // armed (i.e., outside Discuss stage).
        VisitConclusionPoll::TickAccumulator(mode);

        // Paused/Dialogue freeze the whole beat.
        if (mode == TickMode::Paused || mode == TickMode::Dialogue)
            return {};

        switch (state) {
        case BeatState::COMPOSE: {
            if (mode != TickMode::Normal)
                return {};
            const auto sub = g_subPhase.Get();
            switch (sub) {
            case ComposeSubPhase::Start:
                SetSubPhase(ComposeSubPhase::ComposingLLM);
                FireComposeLLM();
                return {};
            case ComposeSubPhase::ComposingLLM:
                return {};
            case ComposeSubPhase::LLMResultReady:
                SetSubPhase(ComposeSubPhase::SelectingPoint);
                return {};
            case ComposeSubPhase::SelectingPoint:
                return ComposeSelectPoint(pt);
            case ComposeSubPhase::StartingQuest:
                return ComposeStartQuest(pt);
            case ComposeSubPhase::Warping:
                return ComposeWarp(pt);
            case ComposeSubPhase::VerifyingFill:
                return ComposeVerifyFill(pt);
            case ComposeSubPhase::Arming:
                return ComposeArm(pt);
            case ComposeSubPhase::Succeeded:
                logger::info("NPCVisitBeat: COMPOSE succeeded; advancing to RUNNING");
                g_runningTickCount = 0;
                return {BeatState::RUNNING};
            case ComposeSubPhase::Failed: {
                logger::warn("NPCVisitBeat: COMPOSE failed ({}); advancing to CLEANUP", g_subPhase.FailureReason());
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
            DriveApproachEscort(pt, mode);
            // Fire a stage-tick every N ticks. Combat mode is forwarded
            // so the Discuss/OnHold substate can drive combat-stuck.
            if (++g_runningTickCount >= kRunningCheckEveryNTicks) {
                g_runningTickCount = 0;
                if (!g_runningTaskInFlight.exchange(true, std::memory_order_acq_rel)) {
                    RunningTick(pt, mode);
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
                    logger::info("NPCVisitBeat: CLEANUP done; returning to NOT_RUNNING");
                    VisitState::Reset();
                    VisitState::SetComposingSender(false);
                    return {BeatState::NOT_RUNNING};
                }
                // If we haven't yet dispatched a terminal stage
                // (COMPOSE failed before EnsureQuestStarted, etc.)
                // and the quest isn't running, we can exit
                // immediately.
                if (stage != kStageShutdown && stage != kStageRollback && !g_terminalCleanupDone.load()) {
                    Cleanup(pt);
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

    void NPCVisitBeat::Abort(const MainThread::Token& /*mt*/)
    {
        logger::warn("NPCVisitBeat: Abort() invoked — running terminal cleanup");
        // HardAbortVisit teleports the sender home (if alive), demotes
        // them from the marker faction, dispatches kStageShutdown on the
        // visit quest,
        // them from the marker faction, dispatches kStageShutdown on the
        // visit quest, pushes an Aborted history entry, and sets the
        // terminal-cleanup latch. Idempotent — a second call after the
        // first no-ops via g_hardAbortFired. When the beat is mid-COMPOSE
        // (quest handle null or stage 0), the guard clauses inside
        // HardAbortVisit return early; VisitState::Reset covers that
        // case. Already-spoken narrations and any dialogue memories
        // SkyrimNet has committed are left intact.
        HardAbortVisit("user_abort");
        VisitState::Reset();
        VisitState::SetComposingSender(false);
    }

    // ---------------------------------------------------------------
    // Query surface
    // ---------------------------------------------------------------

    namespace NPCVisitBeat_Testing
    {
        void SetClock(double (*clock)())
        {
            g_clockOverride.store(clock, std::memory_order_release);
        }
    } // namespace NPCVisitBeat_Testing

    namespace NPCVisitBeat_Query
    {
        DiscussSubPhase GetDiscussSubPhase()
        {
            using Internal = ::NarrativeEngine::DiscussSubPhase;
            switch (g_discussSubPhase.load(std::memory_order_acquire)) {
            case Internal::OnHold:
                return DiscussSubPhase::OnHold;
            case Internal::ReEngage:
                return DiscussSubPhase::ReEngage;
            case Internal::Discussing:
            default:
                return DiscussSubPhase::Discussing;
            }
        }
    } // namespace NPCVisitBeat_Query

    // ---------------------------------------------------------------
    // Cooldowns + Persistence
    // ---------------------------------------------------------------

    namespace NPCVisitBeat_Cooldowns
    {
        void OnVisitCompleted(RE::FormID senderNpcFormID)
        {
            if (senderNpcFormID == 0)
                return;
            g_senderCooldowns.Stamp(senderNpcFormID, EngineUtils::GetCurrentGameHours());
            logger::info("NPCVisitBeat: per-sender cooldown stamp set for 0x{:08X}", senderNpcFormID);
        }

        void OnVisitReachedValediction(RE::FormID senderNpcFormID)
        {
            if (senderNpcFormID == 0)
                return;
            g_senderMemoryWatermarks.Stamp(senderNpcFormID, EngineUtils::GetCurrentGameHours());
            logger::info("NPCVisitBeat: per-sender memory watermark stamped at Valediction for 0x{:08X}",
                         senderNpcFormID);
        }

        bool IsSenderOnCooldown(RE::FormID senderNpcFormID)
        {
            return g_senderCooldowns.IsOnCooldown(
                senderNpcFormID, Settings::Get().visitSenderCooldownGameHours, EngineUtils::GetCurrentGameHours());
        }

        std::optional<double> GetSenderMemoryWatermarkGameHours(RE::FormID senderNpcFormID)
        {
            return g_senderMemoryWatermarks.GetStampGameHours(senderNpcFormID);
        }
    } // namespace NPCVisitBeat_Cooldowns

    // ---------------------------------------------------------------
    // Cosave — 'NBVS' record.
    // Layout (v2, current):
    //   u32    senderCooldownCount
    //   [FormID(u32) + stamp(double)] * senderCooldownCount
    //   u32    senderMemoryWatermarkCount
    //   [FormID(u32) + stamp(double)] * senderMemoryWatermarkCount
    // Layout (v1, legacy — accepted on load):
    //   u32    senderCooldownCount
    //   [FormID(u32) + stamp(double)] * senderCooldownCount
    // v1 loads leave the watermark table empty; the first Valediction
    // reached on the loaded save re-arms the filter from there.
    // ---------------------------------------------------------------

    namespace NPCVisitBeat_Persistence
    {
        constexpr std::uint32_t kRecordVersion = 2;

        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc)
                return;
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
                logger::error("NPCVisitBeat::OnSave: OpenRecord failed");
                return;
            }
            SKSECosaveIO io{intfc};
            g_senderCooldowns.Serialize(io);
            g_senderMemoryWatermarks.Serialize(io);
        }

        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
        {
            if (!intfc)
                return;
            if (version != 1 && version != kRecordVersion) {
                logger::warn("NPCVisitBeat::OnLoad: unknown version {} (length={}); "
                             "clearing cooldown state",
                             version,
                             length);
                OnRevert();
                return;
            }
            SKSECosaveIO io{intfc};
            if (!g_senderCooldowns.Deserialize(io)) {
                logger::error("NPCVisitBeat::OnLoad: sender-cooldown deserialize failed; "
                              "cleared");
                g_senderMemoryWatermarks.Clear();
                return;
            }
            // Watermark table was added in v2; older saves have no
            // trailing bytes so we skip the read and leave the table
            // empty.
            if (version >= 2) {
                if (!g_senderMemoryWatermarks.Deserialize(io)) {
                    logger::error("NPCVisitBeat::OnLoad: sender-memory-watermark deserialize failed; cleared");
                    g_senderMemoryWatermarks.Clear();
                }
            } else {
                g_senderMemoryWatermarks.Clear();
            }
            logger::info("NPCVisitBeat::OnLoad: restored per-sender cooldowns (record v{})", version);
        }

        void OnRevert()
        {
            g_senderCooldowns.Clear();
            g_senderMemoryWatermarks.Clear();
        }
    } // namespace NPCVisitBeat_Persistence
} // namespace NarrativeEngine
