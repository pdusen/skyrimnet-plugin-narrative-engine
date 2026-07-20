#include <NPCLetterBeat.h>

#include <AsyncDispatch.h>
#include <BeatParamHelpers.h>
#include <BeatUtils.h>
#include <CourierUtils.h>
#include <EngineUtils.h>
#include <FactionDesignationUtils.h>
#include <LetterComposer.h>
#include <LetterPool.h>
#include <LocationKeywords.h>
#include <logger.h>
#include <QuestUtils.h>
#include <SenderCooldownTable.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <RE/A/Actor.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESFaction.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace NarrativeEngine
{
    using namespace std::string_view_literals;

    namespace
    {
        // -----------------------------------------------------------------
        // Per-slot delivery quest cache
        // -----------------------------------------------------------------

        std::array<RE::BGSRefAlias*, LetterPool::kPoolSize> g_perSlotSenderAlias{};
        std::array<RE::BGSRefAlias*, LetterPool::kPoolSize> g_perSlotLetterRefAlias{};
        std::atomic<bool> g_perSlotResolved = false;

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
            std::array<RE::TESQuest*, LetterPool::kPoolSize> quests{};
            for (std::size_t i = 0; i < LetterPool::kPoolSize; ++i) {
                char editorId[64];
                std::snprintf(editorId, sizeof(editorId), "_ne_PooledLetterQuest%02zu", i);
                auto* form = RE::TESForm::LookupByEditorID(editorId);
                auto* quest = form ? form->As<RE::TESQuest>() : nullptr;
                quests[i] = quest;
                if (!quest) {
                    missing.emplace_back(editorId);
                    continue;
                }
                ++resolved;
                for (auto* a : quest->aliases) {
                    if (!a)
                        continue;
                    if (a->aliasName == "Sender") {
                        g_perSlotSenderAlias[i] = skyrim_cast<RE::BGSRefAlias*>(a);
                    } else if (a->aliasName == "LetterRef") {
                        g_perSlotLetterRefAlias[i] = skyrim_cast<RE::BGSRefAlias*>(a);
                    }
                }
                if (!g_perSlotSenderAlias[i] || !g_perSlotLetterRefAlias[i]) {
                    char detail[128];
                    std::snprintf(detail,
                                  sizeof(detail),
                                  "%s (Sender=%s LetterRef=%s)",
                                  editorId,
                                  g_perSlotSenderAlias[i] ? "ok" : "MISSING",
                                  g_perSlotLetterRefAlias[i] ? "ok" : "MISSING");
                    missingAliases.emplace_back(detail);
                }
            }
            logger::info(
                "NPCLetterBeat: resolved per-slot delivery quests ({} of {})", resolved, LetterPool::kPoolSize);
            for (const auto& name : missing) {
                logger::warn("NPCLetterBeat: per-slot quest '{}' unresolved; slot disabled", name);
            }
            for (const auto& detail : missingAliases) {
                logger::warn("NPCLetterBeat: per-slot quest has missing alias(es): {}", detail);
            }
            LetterPool::SetPerSlotQuests(quests);
        }

        RE::TESQuest* GetPerSlotQuest(std::size_t slotIndex)
        {
            return LetterPool::GetPerSlotQuest(slotIndex);
        }

        // -----------------------------------------------------------------
        // Sender faction (rank-based alias fill)
        // -----------------------------------------------------------------

        constexpr const char* kSenderFactionEditorID = "_ne_LetterSenderFaction";
        constexpr std::int8_t kSenderRankCandidate = 0;
        constexpr std::int8_t kSenderRankDesignated = 4;

        std::atomic<bool> g_senderFactionResolved = false;
        RE::TESFaction* g_senderFaction = nullptr;

        RE::TESFaction* ResolveSenderFaction()
        {
            if (g_senderFactionResolved.load(std::memory_order_acquire)) {
                return g_senderFaction;
            }
            auto* form = RE::TESForm::LookupByEditorID(kSenderFactionEditorID);
            auto* fact = form ? form->As<RE::TESFaction>() : nullptr;
            if (!fact) {
                logger::error("NPCLetterBeat: sender faction '{}' did not resolve; "
                              "faction-mediated Sender alias fill is disabled",
                              kSenderFactionEditorID);
            } else {
                logger::info("NPCLetterBeat: sender faction resolved (formID=0x{:08X})", fact->GetFormID());
            }
            g_senderFaction = fact;
            g_senderFactionResolved.store(true, std::memory_order_release);
            return fact;
        }

        void PromoteSenderToDesignated(RE::Actor* sender)
        {
            FactionDesignationUtils::PromoteToDesignated(
                ResolveSenderFaction(), sender, kSenderRankDesignated, kSenderRankCandidate, "NPCLetterBeat");
        }

        void DemoteSenderToCandidate(RE::Actor* sender)
        {
            FactionDesignationUtils::DemoteToCandidate(
                ResolveSenderFaction(), sender, kSenderRankCandidate, "NPCLetterBeat");
        }

        // -----------------------------------------------------------------
        // Persistent state (cosave-backed)
        // -----------------------------------------------------------------

        std::mutex g_cooldownMutex;
        double g_lastDispatchGameHours = 0.0;
        SenderCooldownTable g_senderCooldowns;

        // -----------------------------------------------------------------
        // Session state (not persisted; reset by OnStart / OnRevert)
        //
        // The beat runs its COMPOSE arm through a sub-state machine driven
        // by atomic flags flipped from marshaled main-thread tasks. The
        // structure mirrors AmbushBeat's compose/complete/cleanup atomics
        // but adds sub-phase enum to track the multi-step letter-dispatch
        // chain (Compose LLM -> allocate + populate -> promote sender +
        // EnsureQuestStarted -> poll Sender fill -> poll LetterRef fill).
        // -----------------------------------------------------------------

        enum class ComposeSubPhase : std::uint8_t
        {
            Start,             // no work started
            ComposingLLM,      // LetterComposer LLM in flight
            LLMResultReady,    // composition arrived (or failed)
            DispatchRequested, // main-thread dispatch task queued
            DispatchLaunched,  // EnsureQuestStarted returned true
            PollingSender,     // waiting on Sender alias fill
            PollingLetterRef,  // waiting on LetterRef alias fill
            Succeeded,         // -> RUNNING
            Failed,            // -> CLEANUP (failure_reason set)
        };

        std::mutex g_sessionMutex;
        BeatUtils::ComposeSubPhaseMachine<ComposeSubPhase> g_subPhase{ComposeSubPhase::Start};

        // Parameters resolved in OnStart.
        RE::FormID g_paramSenderFormID = 0;
        BeatParamHelpers::UrgencyHint g_paramUrgency = BeatParamHelpers::UrgencyHint::Medium;

        // Populated after the LLM call returns.
        std::optional<LetterComposer::LetterComposition> g_composition;

        // Populated by the dispatch main-thread task.
        int g_inFlightSlot = -1;
        RE::FormID g_inFlightBookFormID = 0;
        RE::FormID g_dispatchedSenderFormID = 0;

        // Tick counters for the sub-phase polls. All in poll ticks (250ms
        // each by default). At kPollBudgetTicks the sub-phase times out.
        int g_subPhaseTickCount = 0;
        constexpr int kSubPhaseCheckEveryNTicks = 4; // ~1s at 250ms
        constexpr int kSubPhaseTimeoutTicks = 20;    // ~5s at 250ms

        // RUNNING arm counters — after CLEANUP fires the courier-container
        // verification, wait letterDispatchVerifyDelaySeconds worth of
        // ticks before giving up.
        int g_runningTickCount = 0;
        constexpr int kRunningCheckEveryNTicks = 4; // ~1s

        BeatUtils::CleanupLatch g_cleanupLatch;
        // Which CLEANUP path to run — set once at the COMPOSE/RUNNING →
        // CLEANUP transition.
        std::atomic<bool> g_cleanupWasSuccess{false};

        void ResetSessionState()
        {
            {
                std::scoped_lock lock(g_sessionMutex);
                g_paramSenderFormID = 0;
                g_paramUrgency = BeatParamHelpers::UrgencyHint::Medium;
                g_composition.reset();
                g_inFlightSlot = -1;
                g_inFlightBookFormID = 0;
                g_dispatchedSenderFormID = 0;
                g_subPhaseTickCount = 0;
                g_runningTickCount = 0;
            }
            g_subPhase.Reset();
            g_cleanupLatch.Reset();
            g_cleanupWasSuccess.store(false, std::memory_order_release);
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
            std::scoped_lock lock(g_sessionMutex);
            g_subPhaseTickCount = 0;
        }

        // -----------------------------------------------------------------
        // Main-thread tasks
        // -----------------------------------------------------------------

        void MainThreadFireComposeLLM()
        {
            RE::FormID senderFormID = 0;
            BeatParamHelpers::UrgencyHint urgency = BeatParamHelpers::UrgencyHint::Medium;
            {
                std::scoped_lock lock(g_sessionMutex);
                senderFormID = g_paramSenderFormID;
                urgency = g_paramUrgency;
            }

            // Minimal BeatContext — LetterComposer only reads
            // desiredDirection + tensionDelta out of it. If a future
            // Compose path needs richer context we can plumb it through
            // OnStart's ctx via a persisted snapshot.
            BeatContext composeCtx;
            composeCtx.desiredDirection = PhaseTracker::Direction::Raise;
            composeCtx.tensionDelta = 0;

            LetterComposer::Compose(
                composeCtx, urgency, senderFormID, [](std::optional<LetterComposer::LetterComposition> comp) {
                    AsyncDispatch::MarshalToMainThread([comp = std::move(comp)]() mutable {
                        if (comp) {
                            std::scoped_lock lock(g_sessionMutex);
                            g_composition = std::move(comp);
                        }
                        SetSubPhase(comp ? ComposeSubPhase::LLMResultReady : ComposeSubPhase::Failed,
                                    comp ? nullptr : "compose_llm_failed");
                    });
                });
        }

        void MainThreadDispatchQuest()
        {
            std::optional<LetterComposer::LetterComposition> comp;
            {
                std::scoped_lock lock(g_sessionMutex);
                comp = g_composition; // copy so we don't hold the lock through allocator
            }
            if (!comp) {
                SetSubPhase(ComposeSubPhase::Failed, "no_composition_at_dispatch");
                return;
            }

            auto alloc = LetterPool::Allocate();
            if (!alloc) {
                const char* reason = (alloc.error() == LetterPool::AllocationFailure::PoolNotResolved)
                                         ? "letter_pool_not_resolved"
                                         : "letter_pool_exhausted";
                logger::warn("NPCLetterBeat: allocation failed: {}", reason);
                SetSubPhase(ComposeSubPhase::Failed, reason);
                return;
            }
            const std::size_t slotIndex = alloc->slotIndex;
            const RE::FormID bookFormID = alloc->bookFormID;

            LetterPool::PopulateSlot(slotIndex,
                                     comp->senderLabel,
                                     comp->body,
                                     comp->senderNpcFormID,
                                     comp->topicTag,
                                     comp->mood,
                                     comp->tags);

            {
                std::scoped_lock lock(g_sessionMutex);
                g_inFlightSlot = static_cast<int>(slotIndex);
                g_inFlightBookFormID = bookFormID;
                g_dispatchedSenderFormID = comp->senderNpcFormID;
            }

            auto* quest = GetPerSlotQuest(slotIndex);
            if (!quest) {
                LetterPool::AbortPending(slotIndex);
                SetSubPhase(ComposeSubPhase::Failed, "per_slot_quest_missing");
                return;
            }
            auto* senderAlias = g_perSlotSenderAlias[slotIndex];
            auto* letterRefAlias = g_perSlotLetterRefAlias[slotIndex];
            if (!senderAlias || !letterRefAlias) {
                LetterPool::AbortPending(slotIndex);
                SetSubPhase(ComposeSubPhase::Failed, "per_slot_alias_missing");
                return;
            }

            std::string liveResolveReason;
            RE::Actor* sender = BeatParamHelpers::ResolveLiveSenderActor(comp->senderNpcFormID, &liveResolveReason);
            if (!sender) {
                LetterPool::AbortPending(slotIndex);
                g_subPhase.Fail(ComposeSubPhase::Failed, std::move(liveResolveReason));
                return;
            }

            logger::info("NPCLetterBeat: dispatching slot {} (quest=0x{:08X}) -> Phase A: "
                         "faction promote (sender=0x{:08X} -> rank {})",
                         slotIndex,
                         quest->GetFormID(),
                         comp->senderNpcFormID,
                         static_cast<int>(kSenderRankDesignated));
            PromoteSenderToDesignated(sender);

            bool engineResult = false;
            const bool callOk = quest->EnsureQuestStarted(engineResult, true);
            if (!callOk || !engineResult) {
                DemoteSenderToCandidate(sender);
                LetterPool::AbortPending(slotIndex);
                logger::warn("NPCLetterBeat: EnsureQuestStarted failed for slot {} "
                             "(callOk={} engineResult={})",
                             slotIndex,
                             callOk,
                             engineResult);
                SetSubPhase(ComposeSubPhase::Failed, "ensure_quest_started_failed");
                return;
            }

            logger::info("NPCLetterBeat: slot {} EnsureQuestStarted ok (quest=0x{:08X}); "
                         "polling for Sender then LetterRef fill",
                         slotIndex,
                         quest->GetFormID());
            SetSubPhase(ComposeSubPhase::PollingSender);
        }

        void MainThreadCheckSenderFill()
        {
            int slotIndex = -1;
            {
                std::scoped_lock lock(g_sessionMutex);
                slotIndex = g_inFlightSlot;
            }
            if (slotIndex < 0) {
                SetSubPhase(ComposeSubPhase::Failed, "no_in_flight_slot");
                return;
            }
            auto* senderAlias = g_perSlotSenderAlias[slotIndex];
            if (senderAlias && senderAlias->GetReference()) {
                logger::info("NPCLetterBeat: slot {} Sender alias filled; polling for "
                             "LetterRef fill",
                             slotIndex);
                SetSubPhase(ComposeSubPhase::PollingLetterRef);
            }
            // else: still empty; sub-phase counter continues to tick
        }

        void MainThreadCheckLetterRefFill()
        {
            int slotIndex = -1;
            {
                std::scoped_lock lock(g_sessionMutex);
                slotIndex = g_inFlightSlot;
            }
            if (slotIndex < 0) {
                SetSubPhase(ComposeSubPhase::Failed, "no_in_flight_slot");
                return;
            }
            auto* letterRefAlias = g_perSlotLetterRefAlias[slotIndex];
            if (letterRefAlias && letterRefAlias->GetReference()) {
                auto* ref = letterRefAlias->GetReference();
                logger::info("NPCLetterBeat: slot {} LetterRef filled (REFR=0x{:08X}); "
                             "COMPOSE succeeded, advancing to RUNNING",
                             slotIndex,
                             ref ? ref->GetFormID() : 0u);
                SetSubPhase(ComposeSubPhase::Succeeded);
            }
        }

        // RUNNING arm check — is the letter in the courier's container yet?
        std::atomic<bool> g_runningCheckInFlight{false};
        std::atomic<bool> g_runningCheckReady{false};
        std::atomic<bool> g_runningLetterInCourier{false};

        void MainThreadCheckCourier()
        {
            RE::FormID bookFormID = 0;
            {
                std::scoped_lock lock(g_sessionMutex);
                bookFormID = g_inFlightBookFormID;
            }
            const auto count = CourierUtils::GetCourierInventoryCount(bookFormID);
            g_runningLetterInCourier.store(count > 0, std::memory_order_release);
            g_runningCheckReady.store(true, std::memory_order_release);
            g_runningCheckInFlight.store(false, std::memory_order_release);
        }

        void MainThreadCleanup()
        {
            const bool success = g_cleanupWasSuccess.load(std::memory_order_acquire);

            int slotIndex = -1;
            RE::FormID senderFormID = 0;
            std::string reason;
            {
                std::scoped_lock lock(g_sessionMutex);
                slotIndex = g_inFlightSlot;
                senderFormID = g_dispatchedSenderFormID;
            }
            reason = g_subPhase.FailureReason();

            // Demote sender if we ever promoted them (any non-early-fail
            // path reached the promote step).
            if (senderFormID != 0) {
                auto* form = RE::TESForm::LookupByID(senderFormID);
                if (auto* sender = form ? form->As<RE::Actor>() : nullptr) {
                    DemoteSenderToCandidate(sender);
                }
            }

            if (slotIndex >= 0) {
                auto* quest = GetPerSlotQuest(static_cast<std::size_t>(slotIndex));
                if (success) {
                    // Letter reached courier: stamp per-beat cooldown,
                    // advance quest to Stage 20 so LetterPool's sinks
                    // pick up the rest of the lifecycle.
                    const double dispatchHours = EngineUtils::GetCurrentGameHours();
                    {
                        std::scoped_lock lock(g_cooldownMutex);
                        g_lastDispatchGameHours = dispatchHours;
                    }
                    logger::info("NPCLetterBeat: per-beat cooldown stamped at gameHours={:.2f}", dispatchHours);
                    if (quest) {
                        QuestUtils::VMDispatchQuestSetStage(quest, kStageInCourierContainer);
                    }
                } else {
                    // Roll the slot back. Route through Stage 60 (recycled
                    // by C++) if the quest actually made it that far,
                    // otherwise just AbortPending (already done in
                    // main-thread dispatch on early failures).
                    logger::warn("NPCLetterBeat: cleanup rolling back slot {} (reason='{}')", slotIndex, reason);
                    if (quest && quest->IsRunning()) {
                        QuestUtils::VMDispatchQuestSetStage(quest, kStageRecycledByCpp);
                    }
                    LetterPool::AbortPending(static_cast<std::size_t>(slotIndex));
                }
            } else if (!success) {
                logger::warn("NPCLetterBeat: cleanup with no allocated slot (reason='{}')", reason);
            }

            g_cleanupLatch.MarkComplete();
        }
    } // namespace

    // ---------------------------------------------------------------------
    // IBeat implementation
    // ---------------------------------------------------------------------

    std::string NPCLetterBeat::Name() const
    {
        return "npc_letter";
    }

    std::string NPCLetterBeat::Description() const
    {
        return "An NPC who knows the player character — chosen by you at "
               "action-select time from the letter sender candidates listed "
               "below — sends a personal letter via the vanilla courier "
               "system. Tone and polarity are driven by the generated "
               "content: warm thank-yous, mournful condolences, businesslike "
               "follow-ups, and urgent or menacing demands are all in scope, so "
               "this beat can serve either a raising or lowering direction "
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
               "or names a form outside the candidate list, the beat "
               "fails to start.\n"
               "  - `urgency_hint` (optional, string): `low` / `medium` / "
               "`high`. Defaults to `medium`. One input among several to "
               "the letter-writing prompt — not a hard directive.\n"
               "\n"
               "Do NOT include any other parameter fields — letter body, "
               "tone, mood, topic, and recipient are all decided by the "
               "beat's own internal letter-writing pipeline, which "
               "embodies the sender you chose. Extra fields will be "
               "silently ignored.";
    }

    BeatPolarity NPCLetterBeat::Polarity() const
    {
        return BeatPolarity::Either;
    }

    bool NPCLetterBeat::IsAvailable(const BeatContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("NPCLetterBeat::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        if (!CourierUtils::ResolveCourierQuest()) {
            return blocked("WICourier quest not resolved");
        }

        const int cooldownHours = Settings::Get().letterBeatCooldownGameHours;
        if (cooldownHours > 0) {
            double lastDispatch = 0.0;
            {
                std::scoped_lock lock(g_cooldownMutex);
                lastDispatch = g_lastDispatchGameHours;
            }
            if (lastDispatch > 0.0) {
                const double nowHours = EngineUtils::GetCurrentGameHours();
                const double elapsed = nowHours - lastDispatch;
                if (elapsed < static_cast<double>(cooldownHours)) {
                    if (debug) {
                        logger::debug("NPCLetterBeat::IsAvailable: blocked (per-beat "
                                      "cooldown: elapsed={:.2f}h < cooldown={}h)",
                                      elapsed,
                                      cooldownHours);
                    }
                    return false;
                }
            }
        }

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
                logger::debug("NPCLetterBeat::IsAvailable: blocked (only {} candidates, "
                              "need {})",
                              enrolled.size(),
                              minCandidates);
            }
            return false;
        }

        return true;
    }

    double NPCLetterBeat::RemainingCooldownGameHours() const
    {
        const int cooldownHours = Settings::Get().letterBeatCooldownGameHours;
        if (cooldownHours <= 0)
            return 0.0;
        double lastDispatch = 0.0;
        {
            std::scoped_lock lock(g_cooldownMutex);
            lastDispatch = g_lastDispatchGameHours;
        }
        if (lastDispatch <= 0.0)
            return 0.0;
        const double elapsed = EngineUtils::GetCurrentGameHours() - lastDispatch;
        const double remaining = static_cast<double>(cooldownHours) - elapsed;
        return remaining > 0.0 ? remaining : 0.0;
    }

    void NPCLetterBeat::OnStart(const BeatContext& /*ctx*/, const nlohmann::json& parameters)
    {
        // Parse sender_npc_form_id + urgency_hint. On sender parse
        // failure, seed the session straight into the Failed sub-phase
        // so the very first Tick transitions to CLEANUP with the reason
        // set. urgency_hint has no failure path (missing / bad input
        // defaults to Medium).
        std::string failureReason;
        const auto senderParsed = BeatParamHelpers::ParseSenderFormID(parameters, &failureReason);
        const auto urgency = BeatParamHelpers::ParseUrgencyHint(parameters);

        ResetSessionState();
        if (!senderParsed) {
            g_subPhase.Fail(ComposeSubPhase::Failed, std::move(failureReason));
        } else {
            std::scoped_lock lock(g_sessionMutex);
            g_paramSenderFormID = *senderParsed;
            g_paramUrgency = urgency;
        }
        logger::info("NPCLetterBeat::OnStart: sender=0x{:08X} urgency={}",
                     senderParsed.value_or(0),
                     urgency == BeatParamHelpers::UrgencyHint::Low    ? "low"
                     : urgency == BeatParamHelpers::UrgencyHint::High ? "high"
                                                                      : "medium");
    }

    TickResult NPCLetterBeat::Tick(TickMode mode, BeatState state)
    {
        // Freeze under any non-Normal gate. Neither the compose chain
        // nor the courier-container check should advance while paused /
        // in combat / mid-dialogue.
        if (mode != TickMode::Normal)
            return {};

        switch (state) {
        case BeatState::COMPOSE: {
            const ComposeSubPhase sub = g_subPhase.Get();
            switch (sub) {
            case ComposeSubPhase::Start: {
                SetSubPhase(ComposeSubPhase::ComposingLLM);
                AsyncDispatch::MarshalToMainThread(&MainThreadFireComposeLLM);
                return {};
            }
            case ComposeSubPhase::ComposingLLM: {
                return {}; // waiting for callback
            }
            case ComposeSubPhase::LLMResultReady: {
                SetSubPhase(ComposeSubPhase::DispatchRequested);
                AsyncDispatch::MarshalToMainThread(&MainThreadDispatchQuest);
                return {};
            }
            case ComposeSubPhase::DispatchRequested:
            case ComposeSubPhase::DispatchLaunched: {
                return {}; // waiting for main-thread flip to PollingSender
            }
            case ComposeSubPhase::PollingSender: {
                int ticks;
                {
                    std::scoped_lock lock(g_sessionMutex);
                    ticks = ++g_subPhaseTickCount;
                }
                if (ticks > kSubPhaseTimeoutTicks) {
                    g_subPhase.Fail(ComposeSubPhase::Failed, "sender_fill_timeout");
                    return {};
                }
                if ((ticks % kSubPhaseCheckEveryNTicks) == 0) {
                    AsyncDispatch::MarshalToMainThread(&MainThreadCheckSenderFill);
                }
                return {};
            }
            case ComposeSubPhase::PollingLetterRef: {
                int ticks;
                {
                    std::scoped_lock lock(g_sessionMutex);
                    ticks = ++g_subPhaseTickCount;
                }
                if (ticks > kSubPhaseTimeoutTicks) {
                    g_subPhase.Fail(ComposeSubPhase::Failed, "letter_ref_fill_timeout");
                    return {};
                }
                if ((ticks % kSubPhaseCheckEveryNTicks) == 0) {
                    AsyncDispatch::MarshalToMainThread(&MainThreadCheckLetterRefFill);
                }
                return {};
            }
            case ComposeSubPhase::Succeeded: {
                logger::info("NPCLetterBeat: COMPOSE succeeded; advancing to RUNNING");
                g_runningTickCount = 0;
                g_runningCheckInFlight.store(false, std::memory_order_release);
                g_runningCheckReady.store(false, std::memory_order_release);
                return {BeatState::RUNNING};
            }
            case ComposeSubPhase::Failed: {
                logger::warn("NPCLetterBeat: COMPOSE failed ({}); advancing to CLEANUP", g_subPhase.FailureReason());
                g_cleanupWasSuccess.store(false, std::memory_order_release);
                return {BeatState::CLEANUP};
            }
            }
            return {};
        }

        case BeatState::RUNNING: {
            // Consume any pending courier-check outcome.
            if (g_runningCheckReady.load(std::memory_order_acquire)) {
                const bool present = g_runningLetterInCourier.load(std::memory_order_acquire);
                g_runningCheckReady.store(false, std::memory_order_release);
                if (present) {
                    logger::info("NPCLetterBeat: RUNNING detected letter in courier "
                                 "container; advancing to CLEANUP (success)");
                    g_cleanupWasSuccess.store(true, std::memory_order_release);
                    return {BeatState::CLEANUP};
                }
            }

            ++g_runningTickCount;
            const int verifyDelayTicks = (Settings::Get().letterDispatchVerifyDelaySeconds * 1000)
                                         / std::max(1, Settings::Get().beatSystemPollIntervalMs);
            if (g_runningTickCount > verifyDelayTicks) {
                g_subPhase.Fail(ComposeSubPhase::Failed, "dispatch_verify_failed");
                logger::warn("NPCLetterBeat: RUNNING verify window elapsed ({} ticks) "
                             "with letter still missing from courier; advancing to "
                             "CLEANUP (failure)",
                             g_runningTickCount);
                g_cleanupWasSuccess.store(false, std::memory_order_release);
                return {BeatState::CLEANUP};
            }
            if ((g_runningTickCount % kRunningCheckEveryNTicks) == 0) {
                if (!g_runningCheckInFlight.exchange(true, std::memory_order_acq_rel)) {
                    AsyncDispatch::MarshalToMainThread(&MainThreadCheckCourier);
                }
            }
            return {};
        }

        case BeatState::CLEANUP: {
            if (g_cleanupLatch.Poll(&MainThreadCleanup)) {
                logger::info("NPCLetterBeat: CLEANUP done; returning to NOT_RUNNING");
                return {BeatState::NOT_RUNNING};
            }
            return {};
        }

        case BeatState::NOT_RUNNING:
        default:
            return {};
        }
    }

    void NPCLetterBeat::Abort()
    {
        logger::warn("NPCLetterBeat: Abort() invoked — running terminal cleanup");
        // Route the cleanup down the failure branch: any in-flight
        // per-slot quest is rolled back via kStageRecycledByCpp +
        // LetterPool::AbortPending, the sender is demoted from the
        // marker faction, and no per-beat cooldown is stamped. Already-
        // delivered letters (Read / Discarded slots) are unaffected —
        // MainThreadCleanup only touches the beat's current in-flight
        // slot.
        g_cleanupWasSuccess.store(false, std::memory_order_release);
        MainThreadCleanup();
        ResetSessionState();
    }

    // ---------------------------------------------------------------------
    // NPCLetterBeat_Init — kDataLoaded resolution
    // ---------------------------------------------------------------------

    namespace NPCLetterBeat_Init
    {
        void Initialize()
        {
            CourierUtils::ResolveCourierQuest();
            ResolvePerSlotQuests();
            ResolveSenderFaction();
        }
    } // namespace NPCLetterBeat_Init

    // ---------------------------------------------------------------------
    // NPCLetterBeat_QuestControl — external LetterPool call points
    // ---------------------------------------------------------------------

    namespace NPCLetterBeat_QuestControl
    {
        void AdvanceSlotStage(std::size_t slotIndex, std::uint32_t stage)
        {
            auto* quest = GetPerSlotQuest(slotIndex);
            if (!quest)
                return;
            QuestUtils::VMDispatchQuestSetStage(quest, stage);
        }

        void ShutdownSlotQuestSync(std::size_t slotIndex)
        {
            auto* quest = GetPerSlotQuest(slotIndex);
            if (!quest)
                return;
            const bool wasRunning = quest->IsRunning();
            quest->Stop();
            quest->Reset();
            logger::info("NPCLetterBeat: slot {} recycled by allocator "
                         "(quest=0x{:08X}, wasRunning={}, native Stop+Reset)",
                         slotIndex,
                         quest->GetFormID(),
                         wasRunning);
        }

        void DeleteLetterRef(std::size_t slotIndex)
        {
            if (slotIndex >= g_perSlotLetterRefAlias.size())
                return;
            auto* alias = g_perSlotLetterRefAlias[slotIndex];
            if (!alias)
                return;
            auto* ref = alias->GetReference();
            if (!ref)
                return;
            const auto refID = ref->GetFormID();
            ref->Disable();
            ref->SetDelete(true);
            logger::info("NPCLetterBeat: deleted LetterRef 0x{:08X} for slot {}", refID, slotIndex);
        }

        void ReleaseLetterFromCourier(std::size_t slotIndex)
        {
            if (slotIndex >= g_perSlotLetterRefAlias.size())
                return;
            auto* alias = g_perSlotLetterRefAlias[slotIndex];
            if (!alias)
                return;
            auto* ref = alias->GetReference();
            if (!ref)
                return;
            auto* courier = CourierUtils::ResolveCourierQuest();
            if (!courier)
                return;
            const bool giveToPlayer = false;
            const bool queued = QuestUtils::VMDispatchOnQuest(
                courier, "WICourierScript"sv, "removeRefFromContainer"sv, ref, giveToPlayer);
            if (queued) {
                logger::info("NPCLetterBeat: released LetterRef 0x{:08X} from WICourier "
                             "for slot {} (VM-dispatched)",
                             ref->GetFormID(),
                             slotIndex);
            } else {
                logger::warn("NPCLetterBeat: ReleaseLetterFromCourier VM dispatch failed "
                             "for slot {} (VM unavailable or handle failed)",
                             slotIndex);
            }
        }
    } // namespace NPCLetterBeat_QuestControl

    // ---------------------------------------------------------------------
    // NPCLetterBeat_Cooldowns — external LetterPool / LetterComposer callers
    // ---------------------------------------------------------------------

    namespace NPCLetterBeat_Cooldowns
    {
        void OnLetterDelivered(RE::FormID senderNpcFormID)
        {
            if (senderNpcFormID == 0)
                return;
            g_senderCooldowns.Stamp(senderNpcFormID);
            logger::info("NPCLetterBeat: per-sender cooldown stamp set for 0x{:08X}", senderNpcFormID);
        }

        bool IsSenderOnCooldown(RE::FormID senderNpcFormID)
        {
            return g_senderCooldowns.IsOnCooldown(senderNpcFormID, Settings::Get().letterSenderCooldownGameHours);
        }
    } // namespace NPCLetterBeat_Cooldowns

    // ---------------------------------------------------------------------
    // Cosave — 'NBLP' record, version 1.
    // Layout:
    //   double lastDispatchGameHours
    //   u32    senderStampCount
    //   [FormID(u32) + stamp(double)] * senderStampCount
    // ---------------------------------------------------------------------

    namespace NPCLetterBeat_Persistence
    {
        constexpr std::uint32_t kRecordVersion = 1;

        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc)
                return;
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
                logger::error("NPCLetterBeat::OnSave: OpenRecord failed");
                return;
            }
            double lastDispatch = 0.0;
            {
                std::scoped_lock lock(g_cooldownMutex);
                lastDispatch = g_lastDispatchGameHours;
            }
            intfc->WriteRecordData(lastDispatch);
            g_senderCooldowns.Serialize(intfc);
        }

        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
        {
            if (!intfc)
                return;
            if (version != kRecordVersion) {
                logger::warn("NPCLetterBeat::OnLoad: unknown version {} (length={}); "
                             "clearing cooldown state",
                             version,
                             length);
                OnRevert();
                return;
            }
            double lastDispatch = 0.0;
            if (intfc->ReadRecordData(lastDispatch) != sizeof(lastDispatch)) {
                logger::error("NPCLetterBeat::OnLoad: short read on lastDispatch; clearing");
                OnRevert();
                return;
            }
            if (!g_senderCooldowns.Deserialize(intfc)) {
                logger::error("NPCLetterBeat::OnLoad: sender-cooldown deserialize failed; "
                              "cleared");
                {
                    std::scoped_lock lock(g_cooldownMutex);
                    g_lastDispatchGameHours = 0.0;
                }
                return;
            }
            {
                std::scoped_lock lock(g_cooldownMutex);
                g_lastDispatchGameHours = lastDispatch;
            }
            logger::info("NPCLetterBeat::OnLoad: restored lastDispatchGameHours={:.2f}", lastDispatch);
        }

        void OnRevert()
        {
            {
                std::scoped_lock lock(g_cooldownMutex);
                g_lastDispatchGameHours = 0.0;
            }
            g_senderCooldowns.Clear();
        }
    } // namespace NPCLetterBeat_Persistence
} // namespace NarrativeEngine
