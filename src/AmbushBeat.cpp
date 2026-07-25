#include <AmbushBeat.h>

#include <EngineUtils.h>
#include <JsonUtils.h>
#include <LocationKeywords.h>
#include <logger.h>
#include <MainThread.h>
#include <Settings.h>

#include <RE/Skyrim.h>

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <string>

namespace NarrativeEngine
{
    namespace
    {
        constexpr const char* kQuestEditorID = "_ne_BanditAmbushQuest";
        constexpr std::uint32_t kRecordVersion = 1;

        // 20 ticks = 5s at the 250ms poll cadence.
        constexpr int kCompletionCheckEveryNTicks = 20;

        // Persistent (cosave-backed).
        std::mutex g_mutex;
        double g_lastCompletionGameHours = 0.0;

        // Session-only; reset by OnStart / OnRevert.
        int g_ticksSinceLastCompletionCheck = 0;
        bool g_composeOk = false;

        std::mutex g_sessionMutex;
        std::string g_failureReason;
        int g_resolvedBanditCount = 0;
        int g_resolvedSpawnDistance = 0;

        RE::TESQuest* LookupAmbushQuest()
        {
            auto* form = RE::TESForm::LookupByEditorID(kQuestEditorID);
            return form ? form->As<RE::TESQuest>() : nullptr;
        }

        void ResetSessionState()
        {
            g_ticksSinceLastCompletionCheck = 0;
            g_composeOk = false;
            std::scoped_lock lock(g_sessionMutex);
            g_failureReason.clear();
        }

        // Returns nullopt on success, else a failure_reason string.
        std::optional<std::string> StartQuest(const PluginThread::Token& pt, int banditCount, int spawnDistance)
        {
            return MainThread::Run(pt, [banditCount, spawnDistance](const MainThread::Token&) {
                auto* quest = LookupAmbushQuest();
                if (!quest) {
                    logger::warn("AmbushBeat: quest '{}' not found by EditorID", kQuestEditorID);
                    return std::optional<std::string>{"quest_not_found"};
                }

                logger::info("AmbushBeat: starting '{}' (banditCount={} spawnDistance={})",
                             kQuestEditorID,
                             banditCount,
                             spawnDistance);

                bool engineResult = false;
                const bool callOk = quest->EnsureQuestStarted(engineResult, true);
                if (!callOk || !engineResult) {
                    logger::warn(
                        "AmbushBeat: EnsureQuestStarted failed (callOk={} engineResult={})", callOk, engineResult);
                    return std::optional<std::string>{"ensure_quest_started_failed"};
                }
                return std::optional<std::string>{};
            });
        }

        // A missing quest counts as completed so we don't spin.
        bool CheckCompletion()
        {
            auto* quest = LookupAmbushQuest();
            return quest ? quest->IsCompleted() : true;
        }

        void Cleanup(const MainThread::Token&)
        {
            auto* quest = LookupAmbushQuest();
            if (quest) {
                logger::info("AmbushBeat: cleanup — Stop/Reset/SetEnabled(false) on '{}'", kQuestEditorID);
                quest->Stop();
                quest->Reset();
                quest->SetEnabled(false);
            }
        }

        void Cleanup(const PluginThread::Token& pt)
        {
            MainThread::Run(pt, [](const MainThread::Token& mt) { Cleanup(mt); });
        }
    } // namespace

    std::string AmbushBeat::Name() const
    {
        return "ambush";
    }

    std::string AmbushBeat::Description() const
    {
        return "A small group of leveled bandits (up to six) materializes at nearby "
               "world markers, jogs toward the player ignoring intervening NPCs, and "
               "engages in vanilla combat at close range. Best fit when the player is "
               "wandering open wilderness or a road with no obvious threat and the "
               "story has gone quiet — the ambush is high-visibility, clearly an "
               "intervention, and resolves in a single fight rather than escalating "
               "an existing situation. Not appropriate when the player is already in "
               "combat, in a settled area, or anywhere a fresh bandit attack would "
               "read as nonsensical (e.g. inside a city or inn).";
    }

    BeatPolarity AmbushBeat::Polarity() const
    {
        return BeatPolarity::Raise;
    }

    bool AmbushBeat::IsAvailable(const BeatContext& ctx) const
    {
        const bool debug = Settings::Get().debugMode;
        const auto blocked = [debug](const char* reason) {
            if (debug) {
                logger::debug("AmbushBeat::IsAvailable: blocked ({})", reason);
            }
            return false;
        };

        if (ctx.playerInInterior)
            return blocked("playerInInterior");

        if (ctx.player) {
            if (LocationKeywords::IsSafe(ctx.player->GetCurrentLocation())) {
                return blocked("LocationKeywords::IsSafe");
            }
        }

        auto* quest = LookupAmbushQuest();
        if (!quest)
            return blocked("quest not found by EditorID");
        if (quest->IsCompleted())
            return blocked("quest IsCompleted");
        if (quest->GetCurrentStageID() > 0)
            return blocked("quest stage > 0 (in flight)");

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
                        logger::debug("AmbushBeat::IsAvailable: blocked (per-beat cooldown: "
                                      "elapsed={:.2f}h < cooldown={}h)",
                                      elapsed,
                                      cooldownHours);
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
        if (cooldownHours <= 0)
            return 0.0;
        double lastCompletion = 0.0;
        {
            std::scoped_lock lock(g_mutex);
            lastCompletion = g_lastCompletionGameHours;
        }
        if (lastCompletion <= 0.0)
            return 0.0;
        const double elapsed = EngineUtils::GetCurrentGameHours() - lastCompletion;
        const double remaining = static_cast<double>(cooldownHours) - elapsed;
        return remaining > 0.0 ? remaining : 0.0;
    }

    void AmbushBeat::OnStart(const BeatContext& /*ctx*/, const nlohmann::json& parameters)
    {
        const auto& cfg = Settings::Get();
        const int banditCount = JsonUtils::ClampParameterInt(parameters,
                                                             "bandit_count",
                                                             cfg.ambushDefaultBanditCount,
                                                             cfg.ambushMinBanditCount,
                                                             cfg.ambushMaxBanditCount);
        const int spawnDistance = JsonUtils::ClampParameterInt(parameters,
                                                               "spawn_distance_units",
                                                               cfg.ambushDefaultSpawnDistanceUnits,
                                                               cfg.ambushMinSpawnDistanceUnits,
                                                               cfg.ambushMaxSpawnDistanceUnits);

        ResetSessionState();
        {
            std::scoped_lock lock(g_sessionMutex);
            g_resolvedBanditCount = banditCount;
            g_resolvedSpawnDistance = spawnDistance;
        }
        logger::info("AmbushBeat::OnStart: resolved banditCount={} spawnDistance={}", banditCount, spawnDistance);
    }

    TickResult AmbushBeat::Tick(const PluginThread::Token& pt, TickMode mode, BeatState state)
    {
        // Freeze under any non-Normal gate.
        if (mode != TickMode::Normal)
            return {};

        switch (state) {
        case BeatState::COMPOSE: {
            int banditCount = 0;
            int spawnDistance = 0;
            {
                std::scoped_lock lock(g_sessionMutex);
                banditCount = g_resolvedBanditCount;
                spawnDistance = g_resolvedSpawnDistance;
            }
            const auto failure = StartQuest(pt, banditCount, spawnDistance);
            if (failure) {
                {
                    std::scoped_lock lock(g_sessionMutex);
                    g_failureReason = *failure;
                }
                logger::warn("AmbushBeat: COMPOSE failed ({}); advancing to CLEANUP", *failure);
                g_composeOk = false;
                return {BeatState::CLEANUP};
            }
            logger::info("AmbushBeat: COMPOSE succeeded; advancing to RUNNING");
            g_composeOk = true;
            g_ticksSinceLastCompletionCheck = 0;
            return {BeatState::RUNNING};
        }

        case BeatState::RUNNING: {
            if (++g_ticksSinceLastCompletionCheck < kCompletionCheckEveryNTicks) {
                return {};
            }
            g_ticksSinceLastCompletionCheck = 0;
            if (CheckCompletion()) {
                logger::info("AmbushBeat: RUNNING detected completion; advancing to CLEANUP");
                return {BeatState::CLEANUP};
            }
            return {};
        }

        case BeatState::CLEANUP: {
            Cleanup(pt);
            // Cooldown stamp only on real completion, not compose failure.
            if (g_composeOk) {
                const double now = EngineUtils::GetCurrentGameHours();
                {
                    std::scoped_lock lock(g_mutex);
                    g_lastCompletionGameHours = now;
                }
                logger::info("AmbushBeat: per-beat cooldown stamped at gameHours={:.2f}", now);
            }
            logger::info("AmbushBeat: CLEANUP done; returning to NOT_RUNNING");
            return {BeatState::NOT_RUNNING};
        }

        case BeatState::NOT_RUNNING:
        default:
            return {};
        }
    }

    void AmbushBeat::Abort(const MainThread::Token& mt)
    {
        logger::warn("AmbushBeat: Abort() invoked — running terminal cleanup");
        // Run the failure branch — no cooldown stamp.
        g_composeOk = false;
        Cleanup(mt);
        ResetSessionState();
    }

    // Cosave 'NBAM' v1: double lastCompletionGameHours.
    namespace AmbushBeat_Persistence
    {
        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc)
                return;
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

        void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
        {
            if (!intfc)
                return;
            if (version != kRecordVersion) {
                logger::warn("AmbushBeat::OnLoad: unknown version {} (length={}); clearing", version, length);
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
            logger::info("AmbushBeat::OnLoad: restored lastCompletionGameHours={:.2f}", stampLoaded);
        }

        void OnRevert()
        {
            std::scoped_lock lock(g_mutex);
            g_lastCompletionGameHours = 0.0;
        }
    } // namespace AmbushBeat_Persistence
} // namespace NarrativeEngine
