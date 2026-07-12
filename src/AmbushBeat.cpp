#include <AmbushBeat.h>

#include <AsyncDispatch.h>
#include <BeatUtils.h>
#include <EngineUtils.h>
#include <JsonUtils.h>
#include <LocationKeywords.h>
#include <Settings.h>
#include <logger.h>

#include <RE/Skyrim.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>

namespace NarrativeEngine
{
    namespace
    {
        constexpr const char* kQuestEditorID = "_ne_BanditAmbushQuest";
        constexpr std::uint32_t kRecordVersion = 1;

        // How often (in poll ticks) to check the quest's IsCompleted()
        // bit while RUNNING. Every 20 ticks = 5s at the 250ms poll
        // cadence — fast enough to feel responsive, slow enough to
        // keep marshal traffic negligible.
        constexpr int kCompletionCheckEveryNTicks = 20;

        // -------------------------------------------------------------
        // Persistent state (cosave-backed)
        // -------------------------------------------------------------

        std::mutex g_mutex;
        double     g_lastCompletionGameHours = 0.0;

        // -------------------------------------------------------------
        // Session-only state (not persisted; reset by OnStart /
        // OnRevert). Tick reads them from the worker thread; marshaled
        // main-thread tasks write them. Atomics for the boolean
        // handoffs; a small helper mutex for strings.
        // -------------------------------------------------------------

        std::atomic<bool> g_composeTaskFired{false};
        std::atomic<bool> g_composeOutcomeReady{false};
        std::atomic<bool> g_composeSucceeded{false};

        int               g_ticksSinceLastCompletionCheck = 0;
        std::atomic<bool> g_completionTaskInFlight{false};
        std::atomic<bool> g_completionOutcomeReady{false};
        std::atomic<bool> g_completionDetected{false};

        BeatUtils::CleanupLatch g_cleanupLatch;

        std::mutex        g_sessionMutex;
        std::string       g_failureReason;
        int               g_resolvedBanditCount   = 0;
        int               g_resolvedSpawnDistance = 0;

        // -------------------------------------------------------------
        // Helpers
        // -------------------------------------------------------------

        RE::TESQuest* LookupAmbushQuest()
        {
            auto* form = RE::TESForm::LookupByEditorID(kQuestEditorID);
            return form ? form->As<RE::TESQuest>() : nullptr;
        }

        void ResetSessionState()
        {
            g_composeTaskFired.store(false, std::memory_order_release);
            g_composeOutcomeReady.store(false, std::memory_order_release);
            g_composeSucceeded.store(false, std::memory_order_release);

            g_ticksSinceLastCompletionCheck = 0;
            g_completionTaskInFlight.store(false, std::memory_order_release);
            g_completionOutcomeReady.store(false, std::memory_order_release);
            g_completionDetected.store(false, std::memory_order_release);

            g_cleanupLatch.Reset();

            std::scoped_lock lock(g_sessionMutex);
            g_failureReason.clear();
        }

        // -------------------------------------------------------------
        // Main-thread tasks (marshaled from Tick)
        // -------------------------------------------------------------

        void MainThreadStartQuest()
        {
            auto* quest = LookupAmbushQuest();
            if (!quest) {
                logger::warn(
                    "AmbushBeat: quest '{}' not found by EditorID",
                    kQuestEditorID);
                {
                    std::scoped_lock lock(g_sessionMutex);
                    g_failureReason = "quest_not_found";
                }
                g_composeSucceeded.store(false, std::memory_order_release);
                g_composeOutcomeReady.store(true, std::memory_order_release);
                return;
            }

            int banditCount   = 0;
            int spawnDistance = 0;
            {
                std::scoped_lock lock(g_sessionMutex);
                banditCount   = g_resolvedBanditCount;
                spawnDistance = g_resolvedSpawnDistance;
            }

            logger::info(
                "AmbushBeat: starting '{}' (banditCount={} spawnDistance={})",
                kQuestEditorID, banditCount, spawnDistance);

            bool       engineResult = false;
            const bool callOk       = quest->EnsureQuestStarted(engineResult, true);
            const bool ok           = callOk && engineResult;
            if (!ok) {
                logger::warn(
                    "AmbushBeat: EnsureQuestStarted failed (callOk={} engineResult={})",
                    callOk, engineResult);
                std::scoped_lock lock(g_sessionMutex);
                g_failureReason = "ensure_quest_started_failed";
            }
            g_composeSucceeded.store(ok, std::memory_order_release);
            g_composeOutcomeReady.store(true, std::memory_order_release);
        }

        void MainThreadCheckCompletion()
        {
            auto* quest = LookupAmbushQuest();
            if (!quest) {
                // ESP gone mid-flight — treat as completed so we
                // proceed to cleanup rather than spin.
                g_completionDetected.store(true, std::memory_order_release);
            } else {
                g_completionDetected.store(
                    quest->IsCompleted(),
                    std::memory_order_release);
            }
            g_completionOutcomeReady.store(true, std::memory_order_release);
            g_completionTaskInFlight.store(false, std::memory_order_release);
        }

        void MainThreadCleanup()
        {
            auto* quest = LookupAmbushQuest();
            if (quest) {
                logger::info(
                    "AmbushBeat: cleanup — Stop/Reset/SetEnabled(false) on '{}'",
                    kQuestEditorID);
                quest->Stop();
                quest->Reset();
                quest->SetEnabled(false);
            }
            // Stamp the per-beat cooldown only on a real completion,
            // not a compose failure. The distinction: compose failure
            // leaves g_composeSucceeded=false; a completed encounter
            // has it true.
            if (g_composeSucceeded.load(std::memory_order_acquire)) {
                const double now = EngineUtils::GetCurrentGameHours();
                {
                    std::scoped_lock lock(g_mutex);
                    g_lastCompletionGameHours = now;
                }
                logger::info(
                    "AmbushBeat: per-beat cooldown stamped at gameHours={:.2f}",
                    now);
            }
            g_cleanupLatch.MarkComplete();
        }
    }

    std::string AmbushBeat::Name() const { return "ambush"; }

    std::string AmbushBeat::Description() const
    {
        return
            "A small group of leveled bandits (up to six) materializes at nearby "
            "world markers, jogs toward the player ignoring intervening NPCs, and "
            "engages in vanilla combat at close range. Best fit when the player is "
            "wandering open wilderness or a road with no obvious threat and the "
            "story has gone quiet — the ambush is high-visibility, clearly an "
            "intervention, and resolves in a single fight rather than escalating "
            "an existing situation. Not appropriate when the player is already in "
            "combat, in a settled area, or anywhere a fresh bandit attack would "
            "read as nonsensical (e.g. inside a city or inn).";
    }

    BeatPolarity AmbushBeat::Polarity() const { return BeatPolarity::Raise; }

    bool AmbushBeat::IsAvailable(const BeatContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("AmbushBeat::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        if (ctx.playerInInterior) return blocked("playerInInterior");

        if (ctx.player) {
            if (LocationKeywords::IsSafe(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsSafe");
            }
        }

        auto* quest = LookupAmbushQuest();
        if (!quest)                          return blocked("quest not found by EditorID");
        if (quest->IsCompleted())            return blocked("quest IsCompleted");
        if (quest->GetCurrentStageID() > 0)  return blocked("quest stage > 0 (in flight)");

        const int cooldownHours = Settings::Get().ambushPerBeatCooldownGameHours;
        if (cooldownHours > 0) {
            double lastCompletion = 0.0;
            {
                std::scoped_lock lock(g_mutex);
                lastCompletion = g_lastCompletionGameHours;
            }
            if (lastCompletion > 0.0) {
                const double elapsed = EngineUtils::GetCurrentGameHours() - lastCompletion;
                if (elapsed < static_cast<double>(cooldownHours)) {
                    if (debug) {
                        logger::debug(
                            "AmbushBeat::IsAvailable: blocked (per-beat cooldown: "
                            "elapsed={:.2f}h < cooldown={}h)",
                            elapsed, cooldownHours);
                    }
                    return false;
                }
            }
        }

        return true;
    }

    double AmbushBeat::RemainingCooldownGameHours() const
    {
        const int cooldownHours = Settings::Get().ambushPerBeatCooldownGameHours;
        if (cooldownHours <= 0) return 0.0;
        double lastCompletion = 0.0;
        {
            std::scoped_lock lock(g_mutex);
            lastCompletion = g_lastCompletionGameHours;
        }
        if (lastCompletion <= 0.0) return 0.0;
        const double elapsed   = EngineUtils::GetCurrentGameHours() - lastCompletion;
        const double remaining = static_cast<double>(cooldownHours) - elapsed;
        return remaining > 0.0 ? remaining : 0.0;
    }

    void AmbushBeat::OnStart(const BeatContext& /*ctx*/,
                             const nlohmann::json& parameters)
    {
        const auto& cfg = Settings::Get();
        const int banditCount = JsonUtils::ClampParameterInt(
            parameters, "bandit_count",
            cfg.ambushDefaultBanditCount,
            cfg.ambushMinBanditCount, cfg.ambushMaxBanditCount);
        const int spawnDistance = JsonUtils::ClampParameterInt(
            parameters, "spawn_distance_units",
            cfg.ambushDefaultSpawnDistanceUnits,
            cfg.ambushMinSpawnDistanceUnits, cfg.ambushMaxSpawnDistanceUnits);

        ResetSessionState();
        {
            std::scoped_lock lock(g_sessionMutex);
            g_resolvedBanditCount   = banditCount;
            g_resolvedSpawnDistance = spawnDistance;
        }
        logger::info(
            "AmbushBeat::OnStart: resolved banditCount={} spawnDistance={}",
            banditCount, spawnDistance);
    }

    TickResult AmbushBeat::Tick(TickMode mode, BeatState state)
    {
        // Freeze under any non-Normal gate.
        if (mode != TickMode::Normal) return {};

        switch (state) {
            case BeatState::COMPOSE: {
                if (g_composeOutcomeReady.load(std::memory_order_acquire)) {
                    const bool ok = g_composeSucceeded.load(std::memory_order_acquire);
                    if (ok) {
                        logger::info(
                            "AmbushBeat: COMPOSE succeeded; advancing to RUNNING");
                        g_ticksSinceLastCompletionCheck = 0;
                        return {BeatState::RUNNING};
                    } else {
                        std::string reason;
                        {
                            std::scoped_lock lock(g_sessionMutex);
                            reason = g_failureReason;
                        }
                        logger::warn(
                            "AmbushBeat: COMPOSE failed ({}); advancing to CLEANUP",
                            reason);
                        return {BeatState::CLEANUP};
                    }
                }
                if (!g_composeTaskFired.exchange(true, std::memory_order_acq_rel)) {
                    AsyncDispatch::MarshalToMainThread(&MainThreadStartQuest);
                }
                return {};
            }

            case BeatState::RUNNING: {
                if (g_completionOutcomeReady.load(std::memory_order_acquire)) {
                    // Consume the outcome regardless — next check will
                    // fire a new task if this one said not-yet.
                    const bool done =
                        g_completionDetected.load(std::memory_order_acquire);
                    g_completionOutcomeReady.store(false, std::memory_order_release);
                    if (done) {
                        logger::info(
                            "AmbushBeat: RUNNING detected completion; advancing to CLEANUP");
                        return {BeatState::CLEANUP};
                    }
                }
                if (++g_ticksSinceLastCompletionCheck >= kCompletionCheckEveryNTicks) {
                    g_ticksSinceLastCompletionCheck = 0;
                    if (!g_completionTaskInFlight.exchange(true, std::memory_order_acq_rel)) {
                        AsyncDispatch::MarshalToMainThread(&MainThreadCheckCompletion);
                    }
                }
                return {};
            }

            case BeatState::CLEANUP: {
                if (g_cleanupLatch.Poll(&MainThreadCleanup)) {
                    logger::info(
                        "AmbushBeat: CLEANUP done; returning to NOT_RUNNING");
                    return {BeatState::NOT_RUNNING};
                }
                return {};
            }

            case BeatState::NOT_RUNNING:
            default:
                return {};
        }
    }

    // -----------------------------------------------------------------
    // Cosave — 'NBAM' record, version 1.
    // Layout: double lastCompletionGameHours
    // -----------------------------------------------------------------

    namespace AmbushBeat_Persistence
    {
        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) return;
            if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
                logger::error("AmbushBeat::OnSave: OpenRecord failed");
                return;
            }
            double stampCopy = 0.0;
            {
                std::scoped_lock lock(g_mutex);
                stampCopy = g_lastCompletionGameHours;
            }
            intfc->WriteRecordData(stampCopy);
        }

        void OnLoad(SKSE::SerializationInterface* intfc,
                    std::uint32_t version, std::uint32_t length)
        {
            if (!intfc) return;
            if (version != kRecordVersion) {
                logger::warn(
                    "AmbushBeat::OnLoad: unknown version {} (length={}); clearing",
                    version, length);
                OnRevert();
                return;
            }
            double stampLoaded = 0.0;
            if (intfc->ReadRecordData(stampLoaded) != sizeof(stampLoaded)) {
                logger::error("AmbushBeat::OnLoad: short read on completion stamp");
                OnRevert();
                return;
            }
            {
                std::scoped_lock lock(g_mutex);
                g_lastCompletionGameHours = stampLoaded;
            }
            logger::info(
                "AmbushBeat::OnLoad: restored lastCompletionGameHours={:.2f}",
                stampLoaded);
        }

        void OnRevert()
        {
            std::scoped_lock lock(g_mutex);
            g_lastCompletionGameHours = 0.0;
        }
    }
}
