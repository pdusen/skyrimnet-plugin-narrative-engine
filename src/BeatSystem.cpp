#include <BeatSystem.h>

#include <BeatRegistry.h>
#include <EvaluationPipeline.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <logger.h>

#include <RE/Skyrim.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace NarrativeEngine::BeatSystem
{
    namespace
    {
        // ----- Top-level state (mutex-guarded) ---------------------------

        std::mutex   g_stateMutex;
        TopLevelState g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
        std::string   g_runningBeatName;
        std::uint32_t g_globalCooldownMs = 0;
        double        g_runningBeatStartedAt = 0.0;
        BeatState     g_runningBeatCurrentState = BeatState::NOT_RUNNING;

        // ----- Worker thread lifecycle ------------------------------------

        std::atomic<bool> g_stopRequested{false};
        std::thread       g_worker;
        bool              g_running = false;

        // Heartbeat cadence — one log line every kHeartbeatEveryNTicks
        // iterations. At 250ms cadence, 40 ticks = ~10s.
        constexpr int kHeartbeatEveryNTicks = 40;

        // ----- Gate reads (safe-off-thread bool reads on stable
        //       singleton pointers; see PHASE_06 doc) -------------------

        bool ReadGamePaused()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui != nullptr && ui->GameIsPaused();
        }

        bool ReadPlayerInCombat()
        {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            return pc != nullptr && pc->IsInCombat();
        }

        bool ReadPlayerInDialogue()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return false;
            return ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        TickMode ComputeTickMode()
        {
            const bool paused   = ReadGamePaused();
            const bool combat   = ReadPlayerInCombat();
            const bool dialogue = ReadPlayerInDialogue();
            if (paused)   return TickMode::Paused;
            if (combat)   return TickMode::Combat;
            if (dialogue) return TickMode::Dialogue;
            return TickMode::Normal;
        }

        const char* TickModeName(TickMode m)
        {
            switch (m) {
                case TickMode::Normal:   return "Normal";
                case TickMode::Paused:   return "Paused";
                case TickMode::Combat:   return "Combat";
                case TickMode::Dialogue: return "Dialogue";
            }
            return "?";
        }

        const char* TopLevelStateName(TopLevelState s)
        {
            switch (s) {
                case TopLevelState::NO_BEAT_RUNNING: return "NO_BEAT_RUNNING";
                case TopLevelState::BEAT_RUNNING:    return "BEAT_RUNNING";
            }
            return "?";
        }

        // ----- Master poll body -------------------------------------------

        void RunOneTick(std::uint32_t intervalMs)
        {
            const TickMode mode = ComputeTickMode();

            // Snapshot & mutate top-level state under the mutex.
            std::string runningName;
            BeatState   runningState = BeatState::NOT_RUNNING;
            TopLevelState topState;
            {
                std::scoped_lock lock(g_stateMutex);
                topState = g_topLevelState;
                if (topState == TopLevelState::NO_BEAT_RUNNING) {
                    // Only advance the cooldown counter under Normal mode
                    // — Paused / Combat / Dialogue freeze it, matching
                    // the wall-clock intent of "seconds of active
                    // gameplay since the last beat completed."
                    if (mode == TickMode::Normal) {
                        // Saturating add — one boot session ticking for
                        // 30+ days would overflow otherwise.
                        const std::uint64_t sum =
                            static_cast<std::uint64_t>(g_globalCooldownMs) +
                            static_cast<std::uint64_t>(intervalMs);
                        g_globalCooldownMs = (sum > UINT32_MAX)
                            ? UINT32_MAX
                            : static_cast<std::uint32_t>(sum);
                    }
                } else {
                    runningName  = g_runningBeatName;
                    runningState = g_runningBeatCurrentState;
                }
            }

            // BEAT_RUNNING dispatch — happens outside the mutex so beat
            // Tick can freely marshal to main thread without lock
            // inversion risks.
            if (topState == TopLevelState::BEAT_RUNNING) {
                IBeat* beat = BeatRegistry::Find(runningName);
                if (!beat) {
                    logger::warn(
                        "BeatSystem: BEAT_RUNNING with name '{}' but "
                        "registry has no match; forcing NO_BEAT_RUNNING",
                        runningName);
                    std::scoped_lock lock(g_stateMutex);
                    g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
                    g_runningBeatName.clear();
                    g_runningBeatCurrentState = BeatState::NOT_RUNNING;
                    g_globalCooldownMs = 0;
                    return;
                }

                TickResult result;
                try {
                    result = beat->Tick(mode, runningState);
                } catch (const std::exception& e) {
                    logger::error(
                        "BeatSystem: '{}' Tick threw: {}",
                        runningName, e.what());
                    result = {};
                } catch (...) {
                    logger::error(
                        "BeatSystem: '{}' Tick threw unknown exception",
                        runningName);
                    result = {};
                }

                if (result.transitionTo.has_value()) {
                    const BeatState nextState = *result.transitionTo;
                    std::scoped_lock lock(g_stateMutex);
                    // Guard against a stale write racing a StartBeat /
                    // Shutdown flip on another thread.
                    if (g_topLevelState == TopLevelState::BEAT_RUNNING &&
                        g_runningBeatName == runningName)
                    {
                        g_runningBeatCurrentState = nextState;
                        if (nextState == BeatState::NOT_RUNNING) {
                            logger::info(
                                "BeatSystem: '{}' returned to "
                                "NOT_RUNNING; releasing top-level slot",
                                runningName);
                            g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
                            g_runningBeatName.clear();
                            g_runningBeatCurrentState = BeatState::NOT_RUNNING;
                            g_globalCooldownMs = 0;
                        }
                    }
                }
            }
        }

        void WorkerLoop()
        {
            logger::info("BeatSystem: master poll worker thread started");
            const std::uint32_t intervalMs = static_cast<std::uint32_t>(
                std::max(1, Settings::Get().beatSystemPollIntervalMs));
            const auto sleepDuration = std::chrono::milliseconds(intervalMs);

            int tickCounter = 0;
            while (!g_stopRequested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(sleepDuration);
                if (g_stopRequested.load(std::memory_order_acquire)) break;

                try {
                    RunOneTick(intervalMs);
                } catch (const std::exception& e) {
                    logger::error("BeatSystem: tick threw: {}", e.what());
                } catch (...) {
                    logger::error("BeatSystem: tick threw unknown exception");
                }

                if (++tickCounter >= kHeartbeatEveryNTicks) {
                    tickCounter = 0;
                    if (Settings::Get().debugMode) {
                        std::string runningName;
                        BeatState runningState = BeatState::NOT_RUNNING;
                        TopLevelState topState;
                        std::uint32_t cooldown;
                        {
                            std::scoped_lock lock(g_stateMutex);
                            topState     = g_topLevelState;
                            runningName  = g_runningBeatName;
                            runningState = g_runningBeatCurrentState;
                            cooldown     = g_globalCooldownMs;
                        }
                        const TickMode mode = ComputeTickMode();
                        if (topState == TopLevelState::NO_BEAT_RUNNING) {
                            logger::debug(
                                "BeatSystem: heartbeat state={} mode={} cooldown={}ms",
                                TopLevelStateName(topState),
                                TickModeName(mode), cooldown);
                        } else {
                            logger::debug(
                                "BeatSystem: heartbeat state={} mode={} "
                                "beat='{}' beat_state={}",
                                TopLevelStateName(topState),
                                TickModeName(mode), runningName,
                                static_cast<int>(runningState));
                        }
                    }
                }
            }
            logger::info("BeatSystem: master poll worker thread stopped");
        }
    }

    void Initialize()
    {
        if (g_running) {
            return;
        }
        g_stopRequested.store(false, std::memory_order_release);
        g_running = true;
        g_worker  = std::thread(WorkerLoop);
    }

    void Shutdown()
    {
        if (!g_running) {
            return;
        }
        g_stopRequested.store(true, std::memory_order_release);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_running = false;
    }

    TopLevelState GetTopLevelState()
    {
        std::scoped_lock lock(g_stateMutex);
        return g_topLevelState;
    }

    std::string GetRunningBeatName()
    {
        std::scoped_lock lock(g_stateMutex);
        return g_runningBeatName;
    }

    std::uint32_t GetGlobalCooldownMs()
    {
        std::scoped_lock lock(g_stateMutex);
        return g_globalCooldownMs;
    }

    std::optional<InFlightInfo> GetInFlightInfo()
    {
        std::scoped_lock lock(g_stateMutex);
        if (g_topLevelState != TopLevelState::BEAT_RUNNING) {
            return std::nullopt;
        }
        InFlightInfo info;
        info.name                 = g_runningBeatName;
        info.startedAtRealSeconds = g_runningBeatStartedAt;
        info.state                = g_runningBeatCurrentState;
        return info;
    }

    // ---------------------------------------------------------------
    // Cosave layer
    // ---------------------------------------------------------------
    //
    // 'NBSY' record schema, version 1:
    //   u8    topLevelState        (NO_BEAT_RUNNING / BEAT_RUNNING)
    //   u32   nameLen              (length of runningBeatName in bytes)
    //   char* runningBeatName      (nameLen bytes, no null terminator)
    //   u32   globalCooldownMs
    //   u8    runningBeatCurrentState (BeatState)
    //   double runningBeatStartedAt (Unix-epoch seconds)
    //
    // The name and per-beat-state fields are meaningful only when
    // topLevelState == BEAT_RUNNING. They're always written to keep the
    // record layout fixed regardless of state, which simplifies the
    // parser.

    namespace
    {
        constexpr std::uint32_t kRecordVersion = 1;

        void WriteString(SKSE::SerializationInterface* intfc,
                         const std::string& s)
        {
            const auto len = static_cast<std::uint32_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0) intfc->WriteRecordData(s.data(), len);
        }

        bool ReadString(SKSE::SerializationInterface* intfc,
                        std::string& out)
        {
            std::uint32_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len)) return false;
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len)
                return false;
            return true;
        }
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) return;
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("BeatSystem::OnSave: OpenRecord failed");
            return;
        }

        TopLevelState topStateCopy;
        std::string   nameCopy;
        std::uint32_t cooldownCopy;
        BeatState     beatStateCopy;
        double        startedCopy;
        {
            std::scoped_lock lock(g_stateMutex);
            topStateCopy  = g_topLevelState;
            nameCopy      = g_runningBeatName;
            cooldownCopy  = g_globalCooldownMs;
            beatStateCopy = g_runningBeatCurrentState;
            startedCopy   = g_runningBeatStartedAt;
        }

        const auto topStateByte  = static_cast<std::uint8_t>(topStateCopy);
        const auto beatStateByte = static_cast<std::uint8_t>(beatStateCopy);
        intfc->WriteRecordData(topStateByte);
        WriteString(intfc, nameCopy);
        intfc->WriteRecordData(cooldownCopy);
        intfc->WriteRecordData(beatStateByte);
        intfc->WriteRecordData(startedCopy);
    }

    void OnLoad(SKSE::SerializationInterface* intfc,
                std::uint32_t version, std::uint32_t length)
    {
        if (!intfc) return;
        if (version != kRecordVersion) {
            logger::warn(
                "BeatSystem::OnLoad: unknown version {} (length={}); "
                "resetting to NO_BEAT_RUNNING",
                version, length);
            OnRevert();
            return;
        }

        std::uint8_t  topStateByte  = 0;
        std::string   nameLoaded;
        std::uint32_t cooldownLoaded = 0;
        std::uint8_t  beatStateByte  = 0;
        double        startedLoaded  = 0.0;

        if (intfc->ReadRecordData(topStateByte) != sizeof(topStateByte)) {
            logger::error("BeatSystem::OnLoad: short read on topLevelState");
            OnRevert();
            return;
        }
        if (!ReadString(intfc, nameLoaded)) {
            logger::error("BeatSystem::OnLoad: failed to read beat name");
            OnRevert();
            return;
        }
        if (intfc->ReadRecordData(cooldownLoaded) != sizeof(cooldownLoaded)) {
            logger::error("BeatSystem::OnLoad: short read on cooldown");
            OnRevert();
            return;
        }
        if (intfc->ReadRecordData(beatStateByte) != sizeof(beatStateByte)) {
            logger::error("BeatSystem::OnLoad: short read on beat state");
            OnRevert();
            return;
        }
        if (intfc->ReadRecordData(startedLoaded) != sizeof(startedLoaded)) {
            logger::error("BeatSystem::OnLoad: short read on startedAt");
            OnRevert();
            return;
        }

        auto topStateLoaded = static_cast<TopLevelState>(topStateByte);
        auto beatStateLoaded = static_cast<BeatState>(beatStateByte);

        // Defensive: if we come back into BEAT_RUNNING but the named
        // beat isn't in the current registry, drop the record and
        // return to idle. Handles a beat being removed between builds.
        if (topStateLoaded == TopLevelState::BEAT_RUNNING &&
            BeatRegistry::Find(nameLoaded) == nullptr)
        {
            logger::warn(
                "BeatSystem::OnLoad: BEAT_RUNNING with unknown beat "
                "'{}'; resetting to NO_BEAT_RUNNING",
                nameLoaded);
            OnRevert();
            return;
        }

        {
            std::scoped_lock lock(g_stateMutex);
            g_topLevelState           = topStateLoaded;
            g_runningBeatName         = std::move(nameLoaded);
            g_globalCooldownMs        = cooldownLoaded;
            g_runningBeatCurrentState = beatStateLoaded;
            g_runningBeatStartedAt    = startedLoaded;
        }
        logger::info(
            "BeatSystem::OnLoad: restored state={} beat='{}' "
            "cooldown={}ms beat_state={}",
            TopLevelStateName(topStateLoaded),
            g_runningBeatName, cooldownLoaded,
            static_cast<int>(beatStateLoaded));
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_stateMutex);
        g_topLevelState           = TopLevelState::NO_BEAT_RUNNING;
        g_runningBeatName.clear();
        g_globalCooldownMs        = 0;
        g_runningBeatStartedAt    = 0.0;
        g_runningBeatCurrentState = BeatState::NOT_RUNNING;
    }

    // -----------------------------------------------------------------
    // Director handshake — ConsiderBeat + StartBeat
    // -----------------------------------------------------------------
    //
    // ConsiderBeat is called on the main thread from
    // EvaluationPipeline::ProcessTick after the tension-eval LLM has
    // populated `rec`. Its job is:
    //
    //   1. Walk the top-level gates (already-running, cooldown, phase
    //      just-advanced, phase dwell).
    //   2. Build the candidate list from BeatRegistry filtered by
    //      IsAvailable + polarity + repetition window.
    //   3. If any survive, fire the beat-select LLM. On response,
    //      validate the chosen name and delegate to StartBeat.
    //   4. In either the "no candidates" or "gates block" branches,
    //      call ApplyDecision(rec) and onFinalized cleanly.
    //
    // For Steps 6–7 the candidate list is always empty (no beats
    // registered yet), so this implementation only exercises gate walk
    // + no-candidates skip. Steps 8–10 add beats to the registry, at
    // which point the LLM-round-trip branch becomes reachable; the
    // full LLM handshake will be built out then.

    namespace
    {
        void FinalizeWithoutBeat(DecisionLog::DecisionRecord rec,
                                 FinalizedCallback           onFinalized)
        {
            EvaluationPipeline::ApplyDecision(std::move(rec));
            if (onFinalized) onFinalized();
        }

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        int IdealDurationFor(PhaseTracker::Phase p)
        {
            const auto& cfg = Settings::Get();
            switch (p) {
                case PhaseTracker::Phase::Exposition:    return cfg.idealDurationExposition;
                case PhaseTracker::Phase::RisingAction:  return cfg.idealDurationRisingAction;
                case PhaseTracker::Phase::Climax:        return cfg.idealDurationClimax;
                case PhaseTracker::Phase::FallingAction: return cfg.idealDurationFallingAction;
                case PhaseTracker::Phase::Resolution:    return cfg.idealDurationResolution;
                default:                                  return 0;
            }
        }
    }

    void ConsiderBeat(Snapshot                    snapshot,
                      DecisionLog::DecisionRecord rec,
                      FinalizedCallback           onFinalized)
    {
        const bool debug = Settings::Get().debugMode;

        // Gate 1: a beat is already in flight.
        TopLevelState topState;
        std::uint32_t cooldownMs;
        std::string   inFlightName;
        {
            std::scoped_lock lock(g_stateMutex);
            topState     = g_topLevelState;
            cooldownMs   = g_globalCooldownMs;
            inFlightName = g_runningBeatName;
        }
        if (topState == TopLevelState::BEAT_RUNNING) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate in_flight blocked: "
                    "'{}' is still running", inFlightName);
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 2: tension eval already advanced the phase this tick.
        if (rec.advancedToPhase) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate just_advanced blocked: "
                    "phase advanced this tick");
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 3: global beat cooldown.
        const std::uint32_t cooldownThresholdMs =
            static_cast<std::uint32_t>(
                std::max(0, Settings::Get().beatCooldownSeconds) * 1000);
        if (cooldownMs < cooldownThresholdMs) {
            if (debug) {
                const std::uint32_t remaining =
                    cooldownThresholdMs - cooldownMs;
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate cooldown blocked: "
                    "{}ms of {}ms accumulated ({}ms remaining)",
                    cooldownMs, cooldownThresholdMs, remaining);
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 4: phase dwell time vs. ideal duration.
        const auto currentPhaseOpt =
            PhaseTracker::PhaseFromName(snapshot.currentPhase);
        if (!currentPhaseOpt) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate dwell blocked: "
                    "unknown phase '{}'", snapshot.currentPhase);
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }
        const int ideal = IdealDurationFor(*currentPhaseOpt);
        if (snapshot.timeInPhaseSeconds < static_cast<float>(ideal)) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate dwell blocked: "
                    "{:.1f}s / {}s ideal",
                    snapshot.timeInPhaseSeconds, ideal);
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 5: registered candidate exists. In Steps 6–7 the
        // registry is empty, so this always triggers. Steps 8–10 will
        // add beats, at which point the LLM round-trip below becomes
        // reachable.
        const auto direction = PhaseTracker::OutgoingDirection(*currentPhaseOpt);
        const auto desired =
            (direction == PhaseTracker::Direction::Raise)
                ? BeatPolarity::Raise
                : BeatPolarity::Lower;

        BeatContext ctx;
        // Beat-select LLM candidate build only reads name/description
        // + IsAvailable's gate reads; skipping the player pointer here
        // is fine because no beat's IsAvailable currently touches it.
        // A richer BuildBeatContextFromSnapshot helper will be added
        // alongside Step 8.
        ctx.playerInCombat    = false;
        ctx.playerInDialogue  = false;
        ctx.playerInInterior  = snapshot.player.cellIsInterior;
        ctx.locationName      = snapshot.player.locationName;
        ctx.cellName          = snapshot.player.cellName;
        ctx.desiredDirection  = direction;
        ctx.tensionDelta      = 0;

        auto candidates = BeatRegistry::AvailableMatching(ctx, desired);
        if (candidates.empty()) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate candidates blocked: "
                    "0 candidates after filtering "
                    "(no beats registered / all filtered)");
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // TODO PHASE-06 STEP 8+: fire the beat-select LLM here. For
        // now, the candidate path is unreachable because Steps 6–7
        // register no beats.
        logger::warn(
            "BeatSystem::ConsiderBeat: candidates non-empty but LLM "
            "handshake not implemented yet — skipping. This path "
            "activates in Step 8.");
        FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
    }

    void StartBeat(const std::string&    name,
                   const nlohmann::json& parameters)
    {
        IBeat* beat = BeatRegistry::Find(name);
        if (!beat) {
            logger::warn(
                "BeatSystem::StartBeat: unknown beat '{}'; no-op", name);
            return;
        }

        {
            std::scoped_lock lock(g_stateMutex);
            if (g_topLevelState == TopLevelState::BEAT_RUNNING) {
                logger::warn(
                    "BeatSystem::StartBeat: refused — beat '{}' is "
                    "already running", g_runningBeatName);
                return;
            }
            g_topLevelState           = TopLevelState::BEAT_RUNNING;
            g_runningBeatName         = name;
            g_runningBeatStartedAt    = NowUnixSeconds();
            g_runningBeatCurrentState = BeatState::COMPOSE;
            g_globalCooldownMs        = 0;
        }

        // Seed the beat's OnStart on the main thread — this call is
        // already main-thread-only per the contract.
        BeatContext ctx{};
        // Minimal context; beat is responsible for reading engine
        // state it needs (via marshal in Tick or main-thread reads
        // inside OnStart). Richer ctx build to come with Step 8's
        // full ConsiderBeat.
        try {
            beat->OnStart(ctx, parameters);
        } catch (const std::exception& e) {
            logger::error(
                "BeatSystem::StartBeat: '{}' OnStart threw: {}; "
                "reverting to NO_BEAT_RUNNING",
                name, e.what());
            OnRevert();
            return;
        } catch (...) {
            logger::error(
                "BeatSystem::StartBeat: '{}' OnStart threw unknown "
                "exception; reverting to NO_BEAT_RUNNING", name);
            OnRevert();
            return;
        }

        BeatRegistry::MarkDispatched(name);
        logger::info(
            "BeatSystem::StartBeat: '{}' started (BeatState=COMPOSE)",
            name);
    }
}
