#include <BeatSystem.h>

#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <BeatRegistry.h>
#include <CombatEventLog.h>
#include <DecisionLog.h>
#include <EngineUtils.h>
#include <EvaluationPipeline.h>
#include <LetterComposer.h>
#include <LLMTextSanitizer.h>
#include <logger.h>
#include <MainThread.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <ThreadRole.h>
#include <TravelEventLog.h>
#include <VisitComposer.h>
#include <WeatherEventLog.h>

#include <RE/Skyrim.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace NarrativeEngine::BeatSystem
{
    namespace
    {
        // ----- Top-level state (mutex-guarded) ---------------------------

        std::mutex g_stateMutex;
        TopLevelState g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
        std::string g_runningBeatName;
        std::uint32_t g_globalCooldownMs = 0;
        double g_runningBeatStartedAt = 0.0;
        BeatState g_runningBeatCurrentState = BeatState::NOT_RUNNING;

        // ----- Anti-repetition ring (session-only) -----------------------
        //
        // When a beat fires, its name + real-time is pushed to
        // g_recentlyFired. ConsiderBeat's candidate filter drops any
        // name still inside Settings::Get().beatRepetitionWindowSeconds.
        // Not persisted — the window is short enough (default 300s) that
        // a reload wipes it without visible consequence.
        struct RecentlyFired
        {
            std::string name;
            double dispatchedAt = 0.0; // Unix-epoch seconds
        };
        std::mutex g_recentMutex;
        std::deque<RecentlyFired> g_recentlyFired;
        constexpr std::size_t kRecentlyFiredCap = 32;

        // ----- Worker thread lifecycle ------------------------------------

        std::atomic<bool> g_stopRequested{false};
        std::thread g_worker;
        bool g_running = false;

        // Gate-derived TickMode. The three underlying reads live in
        // EngineUtils so other subsystems (e.g. beats' own Tick logic
        // that wants a paused-check without going through BeatSystem)
        // can share the same "safe off-thread bool read on a stable
        // singleton pointer" guarantee. Precedence: Paused > Combat
        // > Dialogue > Normal — a paused game is never also reported
        // as Combat or Dialogue.
        TickMode ComputeTickMode()
        {
            const bool paused = EngineUtils::IsGamePaused();
            const bool combat = EngineUtils::IsPlayerInCombat();
            const bool dialogue = EngineUtils::IsPlayerInDialogue();
            if (paused)
                return TickMode::Paused;
            if (combat)
                return TickMode::Combat;
            if (dialogue)
                return TickMode::Dialogue;
            return TickMode::Normal;
        }

        const char* TickModeName(TickMode m)
        {
            switch (m) {
            case TickMode::Normal:
                return "Normal";
            case TickMode::Paused:
                return "Paused";
            case TickMode::Combat:
                return "Combat";
            case TickMode::Dialogue:
                return "Dialogue";
            }
            return "?";
        }

        const char* TopLevelStateName(TopLevelState s)
        {
            switch (s) {
            case TopLevelState::NO_BEAT_RUNNING:
                return "NO_BEAT_RUNNING";
            case TopLevelState::BEAT_RUNNING:
                return "BEAT_RUNNING";
            }
            return "?";
        }

        // ----- Master poll body -------------------------------------------

        void RunOneTick(std::uint32_t intervalMs)
        {
            const TickMode mode = ComputeTickMode();

            // Snapshot & mutate top-level state under the mutex.
            std::string runningName;
            BeatState runningState = BeatState::NOT_RUNNING;
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
                            static_cast<std::uint64_t>(g_globalCooldownMs) + static_cast<std::uint64_t>(intervalMs);
                        g_globalCooldownMs = (sum > UINT32_MAX) ? UINT32_MAX : static_cast<std::uint32_t>(sum);
                    }
                } else {
                    runningName = g_runningBeatName;
                    runningState = g_runningBeatCurrentState;
                }
            }

            // BEAT_RUNNING dispatch — happens outside the mutex so beat
            // Tick can freely marshal to main thread without lock
            // inversion risks.
            if (topState == TopLevelState::BEAT_RUNNING) {
                IBeat* beat = BeatRegistry::Find(runningName);
                if (!beat) {
                    logger::warn("BeatSystem: BEAT_RUNNING with name '{}' but "
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
                    logger::error("BeatSystem: '{}' Tick threw: {}", runningName, e.what());
                    result = {};
                } catch (...) {
                    logger::error("BeatSystem: '{}' Tick threw unknown exception", runningName);
                    result = {};
                }

                if (result.transitionTo.has_value()) {
                    const BeatState nextState = *result.transitionTo;
                    std::scoped_lock lock(g_stateMutex);
                    // Guard against a stale write racing a StartBeat /
                    // Shutdown flip on another thread.
                    if (g_topLevelState == TopLevelState::BEAT_RUNNING && g_runningBeatName == runningName) {
                        g_runningBeatCurrentState = nextState;
                        if (nextState == BeatState::NOT_RUNNING) {
                            logger::info("BeatSystem: '{}' returned to "
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
            // Declare this thread as Plugin for its entire lifetime.
            // Runtime belt-and-braces beneath the compile-time token
            // barrier; useful for observability and for the assertion
            // in MainThread::Run.
            ScopedThreadRole roleGuard(ThreadRole::Plugin);
            logger::info("BeatSystem: master poll worker thread started (role installed: Plugin)");
            const std::uint32_t intervalMs =
                static_cast<std::uint32_t>(std::max(1, Settings::Get().beatSystemPollIntervalMs));
            const auto sleepDuration = std::chrono::milliseconds(intervalMs);

            bool hasPrev = false;
            TopLevelState prevTopState = TopLevelState::NO_BEAT_RUNNING;
            TickMode prevMode = TickMode::Normal;
            std::string prevRunningName;
            BeatState prevRunningState = BeatState::NOT_RUNNING;
            std::uint32_t prevCooldown = 0;

            while (!g_stopRequested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(sleepDuration);
                if (g_stopRequested.load(std::memory_order_acquire))
                    break;

                try {
                    RunOneTick(intervalMs);
                } catch (const std::exception& e) {
                    logger::error("BeatSystem: tick threw: {}", e.what());
                } catch (...) {
                    logger::error("BeatSystem: tick threw unknown exception");
                }

                if (Settings::Get().debugMode) {
                    std::string runningName;
                    BeatState runningState = BeatState::NOT_RUNNING;
                    TopLevelState topState;
                    std::uint32_t cooldown;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        topState = g_topLevelState;
                        runningName = g_runningBeatName;
                        runningState = g_runningBeatCurrentState;
                        cooldown = g_globalCooldownMs;
                    }
                    const TickMode mode = ComputeTickMode();

                    // Cooldown counts up by intervalMs each Normal-mode
                    // tick and is frozen otherwise; those two shapes are
                    // the routine "boring" change and don't warrant a
                    // log. Anything else (reset to 0, cosave load jump,
                    // saturation clamp) is a discontinuity worth
                    // recording.
                    const bool cooldownRoutine = cooldown == prevCooldown || cooldown == prevCooldown + intervalMs;

                    const bool changed = !hasPrev || topState != prevTopState || mode != prevMode
                                         || runningName != prevRunningName || runningState != prevRunningState
                                         || !cooldownRoutine;

                    if (changed) {
                        if (topState == TopLevelState::NO_BEAT_RUNNING) {
                            logger::debug("BeatSystem: heartbeat state={} mode={} cooldown={:.1f}s",
                                          TopLevelStateName(topState),
                                          TickModeName(mode),
                                          cooldown / 1000.0);
                        } else {
                            logger::debug("BeatSystem: heartbeat state={} mode={} "
                                          "beat='{}' beat_state={}",
                                          TopLevelStateName(topState),
                                          TickModeName(mode),
                                          runningName,
                                          static_cast<int>(runningState));
                        }
                    }

                    hasPrev = true;
                    prevTopState = topState;
                    prevMode = mode;
                    prevRunningName = runningName;
                    prevRunningState = runningState;
                    prevCooldown = cooldown;
                }
            }
            logger::info("BeatSystem: master poll worker thread stopped");
        }
    } // namespace

    void Initialize()
    {
        if (g_running) {
            return;
        }
        g_stopRequested.store(false, std::memory_order_release);
        g_running = true;
        g_worker = std::thread(WorkerLoop);
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
        info.name = g_runningBeatName;
        info.startedAtRealSeconds = g_runningBeatStartedAt;
        info.state = g_runningBeatCurrentState;
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

        void WriteString(SKSE::SerializationInterface* intfc, const std::string& s)
        {
            const auto len = static_cast<std::uint32_t>(s.size());
            intfc->WriteRecordData(len);
            if (len > 0)
                intfc->WriteRecordData(s.data(), len);
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint32_t len = 0;
            if (intfc->ReadRecordData(len) != sizeof(len))
                return false;
            out.resize(len);
            if (len > 0 && intfc->ReadRecordData(out.data(), len) != len)
                return false;
            return true;
        }
    } // namespace

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        if (!intfc)
            return;
        if (!intfc->OpenRecord(kRecordTypeId, kRecordVersion)) {
            logger::error("BeatSystem::OnSave: OpenRecord failed");
            return;
        }

        TopLevelState topStateCopy;
        std::string nameCopy;
        std::uint32_t cooldownCopy;
        BeatState beatStateCopy;
        double startedCopy;
        {
            std::scoped_lock lock(g_stateMutex);
            topStateCopy = g_topLevelState;
            nameCopy = g_runningBeatName;
            cooldownCopy = g_globalCooldownMs;
            beatStateCopy = g_runningBeatCurrentState;
            startedCopy = g_runningBeatStartedAt;
        }

        const auto topStateByte = static_cast<std::uint8_t>(topStateCopy);
        const auto beatStateByte = static_cast<std::uint8_t>(beatStateCopy);
        intfc->WriteRecordData(topStateByte);
        WriteString(intfc, nameCopy);
        intfc->WriteRecordData(cooldownCopy);
        intfc->WriteRecordData(beatStateByte);
        intfc->WriteRecordData(startedCopy);
    }

    void OnLoad(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
    {
        if (!intfc)
            return;
        if (version != kRecordVersion) {
            logger::warn("BeatSystem::OnLoad: unknown version {} (length={}); "
                         "resetting to NO_BEAT_RUNNING",
                         version,
                         length);
            OnRevert();
            return;
        }

        std::uint8_t topStateByte = 0;
        std::string nameLoaded;
        std::uint32_t cooldownLoaded = 0;
        std::uint8_t beatStateByte = 0;
        double startedLoaded = 0.0;

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
        if (topStateLoaded == TopLevelState::BEAT_RUNNING && BeatRegistry::Find(nameLoaded) == nullptr) {
            logger::warn("BeatSystem::OnLoad: BEAT_RUNNING with unknown beat "
                         "'{}'; resetting to NO_BEAT_RUNNING",
                         nameLoaded);
            OnRevert();
            return;
        }

        {
            std::scoped_lock lock(g_stateMutex);
            g_topLevelState = topStateLoaded;
            g_runningBeatName = std::move(nameLoaded);
            g_globalCooldownMs = cooldownLoaded;
            g_runningBeatCurrentState = beatStateLoaded;
            g_runningBeatStartedAt = startedLoaded;
        }
        logger::info("BeatSystem::OnLoad: restored state={} beat='{}' "
                     "cooldown={:.1f}s beat_state={}",
                     TopLevelStateName(topStateLoaded),
                     g_runningBeatName,
                     cooldownLoaded / 1000.0,
                     static_cast<int>(beatStateLoaded));
    }

    void OnRevert()
    {
        std::scoped_lock lock(g_stateMutex);
        g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
        g_runningBeatName.clear();
        g_globalCooldownMs = 0;
        g_runningBeatStartedAt = 0.0;
        g_runningBeatCurrentState = BeatState::NOT_RUNNING;
    }

    // -----------------------------------------------------------------
    // Director handshake — ConsiderBeat + StartBeat
    // -----------------------------------------------------------------
    //
    // ConsiderBeat is called on the plugin thread from
    // EvaluationPipeline::BeginEvaluation after the tension-eval LLM
    // has populated `rec`. Its job is:
    //
    //   1. Walk the top-level gates (already-running, cooldown, phase
    //      just-advanced, phase dwell) — pure state-mutex reads and
    //      pure computation, plugin-thread-safe.
    //   2. Bundle the engine-touching work (BuildBeatContext,
    //      precondition probes, per-beat IsAvailable, sender-candidate
    //      collection with min-threshold pruning) into a single
    //      MainThread::Run hop.
    //   3. Recency filter — plugin thread, brief g_recentMutex acquire.
    //   4. If any candidates survive, fire the beat-select LLM via
    //      the synchronous SkyrimNetAPI wrapper (plugin thread blocks
    //      for the round-trip). Parse the response on the plugin
    //      thread; marshal to main only for the finalize step
    //      (StartBeat calls beat->OnStart which is main-thread by
    //      IBeat contract; ApplyDecision pushes DashboardUI state).
    //   5. In the "no candidates" or "gates block" branches, marshal a
    //      MainThread::FireAndForget of FinalizeWithoutBeat, which
    //      calls ApplyDecision(rec) and onFinalized cleanly.
    //
    // For Steps 6–7 the candidate list is always empty (no beats
    // registered yet), so this implementation only exercises gate walk
    // + no-candidates skip. Steps 8–10 add beats to the registry, at
    // which point the LLM-round-trip branch becomes reachable; the
    // full LLM handshake will be built out then.

    namespace
    {
        // Plugin thread. ApplyDecision is a plugin-thread body (mutex-
        // guarded log append + PhaseTracker::AdvanceTo + a
        // PushFullState that internally schedules its compose off
        // main) — nothing here needs a main-thread hop.
        void FinalizeWithoutBeat(const PluginThread::Token& pt,
                                 DecisionLog::DecisionRecord rec,
                                 FinalizedCallback onFinalized)
        {
            EvaluationPipeline::ApplyDecision(pt, rec);
            if (onFinalized)
                onFinalized();
        }

        double NowUnixSeconds()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        int IdealDurationFor(PhaseTracker::Phase p)
        {
            const auto& cfg = Settings::Get();
            switch (p) {
            case PhaseTracker::Phase::Exposition:
                return cfg.idealDurationExposition;
            case PhaseTracker::Phase::RisingAction:
                return cfg.idealDurationRisingAction;
            case PhaseTracker::Phase::Climax:
                return cfg.idealDurationClimax;
            case PhaseTracker::Phase::FallingAction:
                return cfg.idealDurationFallingAction;
            case PhaseTracker::Phase::Resolution:
                return cfg.idealDurationResolution;
            default:
                return 0;
            }
        }

        int ComputeTensionDelta(PhaseTracker::Phase phase, std::uint32_t currentTension)
        {
            const auto& cfg = Settings::Get();
            const int current = static_cast<int>(currentTension);
            switch (phase) {
            case PhaseTracker::Phase::Exposition:
                return std::max(0, cfg.advanceThresholdExposition - current);
            case PhaseTracker::Phase::RisingAction:
                return std::max(0, cfg.advanceThresholdRisingAction - current);
            case PhaseTracker::Phase::Climax:
                return std::max(0, current - cfg.advanceThresholdClimax);
            case PhaseTracker::Phase::FallingAction:
                return std::max(0, current - cfg.advanceThresholdFallingAction);
            case PhaseTracker::Phase::Resolution:
                return std::max(0, cfg.advanceThresholdResolution - current);
            default:
                return 0;
            }
        }

        // BeatContext builder — main thread; reads live engine state so
        // an LLM round-trip that took seconds resolves against current
        // conditions, not the stale snapshot.
        BeatContext BuildBeatContextFromSnapshot(const Snapshot& snapshot,
                                                 PhaseTracker::Direction direction = PhaseTracker::Direction::Raise,
                                                 int tensionDelta = 0)
        {
            BeatContext ctx;
            ctx.player = RE::PlayerCharacter::GetSingleton();
            if (ctx.player) {
                ctx.playerInCombat = ctx.player->IsInCombat();
            }
            ctx.playerInInterior = snapshot.player.cellIsInterior;
            ctx.locationName = snapshot.player.locationName;
            ctx.cellName = snapshot.player.cellName;
            if (auto* ui = RE::UI::GetSingleton()) {
                ctx.playerInDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            }
            ctx.desiredDirection = direction;
            ctx.tensionDelta = tensionDelta;
            return ctx;
        }

        // Globally-disqualifying world state. Returns nullptr on
        // all-clear, or a short literal naming the gate that blocked.
        // Runs twice per firing tick — once pre-LLM to save the round
        // trip when we know we'd bail, once post-LLM in case the
        // situation changed during the round trip.
        const char* CheckGlobalBeatPreconditions(const BeatContext& ctx)
        {
            if (ctx.playerInCombat)
                return "playerInCombat";
            if (ctx.playerInDialogue)
                return "playerInDialogue";
            if (AlphaCanon::IsInScriptedScene())
                return "scriptedScene";
            if (AlphaCanon::IsInDoNotDisturbCell())
                return "doNotDisturbCell";
            return nullptr;
        }

        // Caller holds g_recentMutex.
        void TrimRecentlyFiredLocked(double now)
        {
            const double window = static_cast<double>(Settings::Get().beatRepetitionWindowSeconds);
            while (!g_recentlyFired.empty() && (now - g_recentlyFired.front().dispatchedAt) > window) {
                g_recentlyFired.pop_front();
            }
            while (g_recentlyFired.size() > kRecentlyFiredCap) {
                g_recentlyFired.pop_front();
            }
        }

        // Caller holds g_recentMutex.
        bool WasFiredRecentlyLocked(const std::string& name, double now)
        {
            const double window = static_cast<double>(Settings::Get().beatRepetitionWindowSeconds);
            for (const auto& r : g_recentlyFired) {
                if (r.name == name && (now - r.dispatchedAt) <= window) {
                    return true;
                }
            }
            return false;
        }

        void PushRecentlyFired(const std::string& name)
        {
            const double now = NowUnixSeconds();
            std::scoped_lock lock(g_recentMutex);
            g_recentlyFired.push_back({name, now});
            TrimRecentlyFiredLocked(now);
        }

        // ---------- prompt context ----------
        //
        // The SkyrimNet-side prompt template is still named
        // `narrative_engine_action_select` — the file lives in
        // statics/SKSE/Plugins/SkyrimNet/prompts/ and hasn't been
        // renamed to `_beat_select` yet. Renaming there would only
        // require a file rename plus one string-constant flip on the
        // SkyrimNetAPI::SendCustomPromptToLLM call below.
        constexpr std::size_t kBeatSelectEventTailSize = 10;

        std::string BuildBeatSelectPromptContext(
            const Snapshot& snapshot,
            const std::vector<IBeat*>& candidates,
            PhaseTracker::Direction direction,
            int tensionDelta,
            const std::vector<LetterComposer::SenderCandidate>& letterSenderCandidates,
            const std::vector<VisitComposer::SenderCandidate>& visitSenderCandidates)
        {
            nlohmann::json ctx;
            ctx["desired_direction"] = (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
            ctx["tension_delta"] = tensionDelta;

            const auto serializedLetterCandidates =
                letterSenderCandidates.empty() ? nlohmann::json::array()
                                               : LetterComposer::SerializeSenderCandidates(letterSenderCandidates);
            const auto serializedVisitCandidates =
                visitSenderCandidates.empty() ? nlohmann::json::array()
                                              : VisitComposer::SerializeSenderCandidates(visitSenderCandidates);

            nlohmann::json candArr = nlohmann::json::array();
            for (auto* b : candidates) {
                if (!b)
                    continue;
                nlohmann::json cj = {
                    {"name", b->Name()},
                    {"description", b->Description()},
                    {"letter_sender_candidates",
                     (b->Name() == "npc_letter") ? serializedLetterCandidates : nlohmann::json::array()},
                    {"visit_sender_candidates",
                     (b->Name() == "npc_visit") ? serializedVisitCandidates : nlohmann::json::array()},
                };
                candArr.push_back(std::move(cj));
            }
            ctx["candidates"] = std::move(candArr);

            ctx["player_context"] = {
                {"location_name", snapshot.player.locationName},
                {"cell_name", snapshot.player.cellName},
                {"cell_is_interior", snapshot.player.cellIsInterior},
            };

            {
                auto parsed = nlohmann::json::parse(snapshot.skyrimNetEventsJSON, nullptr, false);
                nlohmann::json skyrimSide = nlohmann::json::array();
                if (parsed.is_array()) {
                    const double cutoff = snapshot.phaseEnteredAtRealTime;
                    nlohmann::json filtered = nlohmann::json::array();
                    for (auto& evt : parsed) {
                        if (!evt.is_object())
                            continue;
                        if (cutoff > 0.0) {
                            const double et = evt.value("localTime", 0.0);
                            if (et < cutoff)
                                continue;
                        }
                        filtered.push_back(std::move(evt));
                    }
                    std::reverse(filtered.begin(), filtered.end());
                    SkyrimNetEvents::FormatEventsText(filtered, snapshot.player.gameTimeSeconds);
                    skyrimSide = std::move(filtered);
                }
                auto merged = SkyrimNetEvents::BuildMergedTimeline(
                    std::move(skyrimSide),
                    CombatEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    WeatherEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    TravelEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    snapshot.player.gameTimeSeconds);

                if (merged.is_array() && merged.size() > kBeatSelectEventTailSize) {
                    const std::size_t skipFront = merged.size() - kBeatSelectEventTailSize;
                    merged.erase(merged.begin(), merged.begin() + skipFront);
                }
                ctx["recent_events"] = std::move(merged);
            }

            return ctx.dump();
        }

        // ---------- finalize paths ----------
        //
        // Called on the main thread after the LLM round trip has failed
        // (network error, malformed response, etc.). Stamps a failure
        // marker on the record and applies the same "global cooldown
        // gate" a successful completion would — "we tried to fire, so
        // wait the normal cooldown before trying again." In the counter
        // model that means leaving g_globalCooldownMs at zero (it
        // already is — the gate walk ran while NO_BEAT_RUNNING) and
        // NOT touching the top-level state, so the counter climbs back
        // up cleanly.
        // Plugin thread. Called from FireBeatSelectLLM's failure
        // branches after the sync SkyrimNet round-trip returned an
        // error or malformed body. The state-mutex acquire is safe
        // from any thread; ApplyDecision is plugin-thread.
        void FinalizeWithFailure(const PluginThread::Token& pt,
                                 DecisionLog::DecisionRecord rec,
                                 const std::string& reason,
                                 FinalizedCallback onFinalized)
        {
            rec.beatSelected = "(failed: " + reason + ")";
            {
                std::scoped_lock lock(g_stateMutex);
                g_globalCooldownMs = 0;
            }
            logger::warn("BeatSystem: beat-select failed: {}", reason);
            EvaluationPipeline::ApplyDecision(pt, rec);
            if (onFinalized)
                onFinalized();
        }

        // Forward declaration — StartBeat's definition lives at the
        // bottom of this file.
        void StartBeatInternal(const PluginThread::Token&,
                               const std::string& name,
                               const BeatContext& ctx,
                               const nlohmann::json& parameters);

        // Plugin thread. Called from FireBeatSelectLLM after a
        // structurally-valid LLM response. Runs the whole finalize
        // chain off main except for the single MainThread::Run hop
        // that re-checks preconditions and calls StartBeatInternal
        // (which must be main because beat->OnStart is main-thread
        // per IBeat contract).
        void FinalizeWithLLMResponse(const PluginThread::Token& pt,
                                     Snapshot snapshot,
                                     DecisionLog::DecisionRecord rec,
                                     std::vector<std::string> candidateNames,
                                     std::string chosenBeat,
                                     nlohmann::json parameters,
                                     std::string narrativeNote,
                                     std::string parameterJustification,
                                     PhaseTracker::Direction direction,
                                     int tensionDelta,
                                     FinalizedCallback onFinalized)
        {
            // Registry lookup + validation — thread-safe (BeatRegistry
            // is mutex-guarded internally).
            const bool isValidCandidate =
                std::find(candidateNames.begin(), candidateNames.end(), chosenBeat) != candidateNames.end();
            if (!isValidCandidate) {
                FinalizeWithFailure(
                    pt, std::move(rec), "LLM returned unknown beat '" + chosenBeat + "'", std::move(onFinalized));
                return;
            }

            IBeat* beat = BeatRegistry::Find(chosenBeat);
            if (!beat) {
                FinalizeWithFailure(pt,
                                    std::move(rec),
                                    "LLM-chosen beat '" + chosenBeat + "' missing from registry",
                                    std::move(onFinalized));
                return;
            }

            // Populate the record on the plugin thread. beatSelected is
            // tentatively the beat name; a StartBeat failure downstream
            // doesn't roll this back — the beat *was* chosen, just
            // failed to fire.
            rec.beatSelected = chosenBeat;
            rec.beatParametersJSON = parameters.dump();
            if (!narrativeNote.empty()) {
                rec.narrativeNote = std::move(narrativeNote);
            }

            // Inject parameter_justification into params for the
            // compose step to consume as sender-motivation seed.
            if (!parameterJustification.empty()) {
                parameters["parameter_justification"] = std::move(parameterJustification);
            }

            // Re-check global preconditions off-main — they may have
            // changed during the LLM round-trip. Uses the same off-
            // main-safe reads BuildBeatSelectPrep uses.
            const BeatContext ctx = BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta);
            if (const char* blockedBy = CheckGlobalBeatPreconditions(ctx)) {
                if (Settings::Get().debugMode) {
                    logger::debug("BeatSystem: global preconditions changed during "
                                  "LLM round trip (blocked: {}); dropping chosen "
                                  "beat '{}'",
                                  blockedBy,
                                  chosenBeat);
                }
                FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
                return;
            }

            // Runs on the plugin thread. StartBeatInternal's state
            // flip is mutex-guarded and beat->OnStart is documented
            // as plugin-thread-safe (see IBeat::OnStart).
            StartBeatInternal(pt, chosenBeat, ctx, parameters);

            PushRecentlyFired(chosenBeat);
            logger::info("BeatSystem: beat '{}' selected (direction={}, "
                         "tension_delta={})",
                         chosenBeat,
                         (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower",
                         tensionDelta);

            EvaluationPipeline::ApplyDecision(pt, rec);
            if (onFinalized)
                onFinalized();
        }

        // The shared body invoked by both ConsiderBeat and
        // ForceDispatchBeat. Runs entirely on the plugin thread:
        // prompt build, sync LLM round-trip, JSON parse, and the
        // finalize step (including StartBeatInternal + beat->OnStart,
        // both plugin-thread-safe per their contracts) all live
        // off-main.
        void FireBeatSelectLLM(const PluginThread::Token& pt,
                               Snapshot snapshot,
                               DecisionLog::DecisionRecord rec,
                               std::vector<IBeat*> candidates,
                               std::vector<LetterComposer::SenderCandidate> letterSenderCandidates,
                               std::vector<VisitComposer::SenderCandidate> visitSenderCandidates,
                               PhaseTracker::Direction direction,
                               int tensionDelta,
                               FinalizedCallback onFinalized,
                               const char* logPrefix)
        {
            const std::string promptCtx = BuildBeatSelectPromptContext(
                snapshot, candidates, direction, tensionDelta, letterSenderCandidates, visitSenderCandidates);
            if (Settings::Get().debugMode) {
                logger::debug("BeatSystem: {}beat-select prompt context: {}", logPrefix, promptCtx);
            }

            std::vector<std::string> candidateNames;
            candidateNames.reserve(candidates.size());
            for (auto* b : candidates) {
                if (b)
                    candidateNames.push_back(b->Name());
            }

            // Sync LLM round-trip. Blocks the plugin thread. See
            // SkyrimNetAPI.h for the rationale — the caller is
            // single-flighted (EvaluationPipeline::g_inFlight for
            // ConsiderBeat, top-level BEAT_RUNNING slot for
            // ForceDispatchBeat) so no plugin-thread work is idle-
            // starving during the wait.
            const auto result = SkyrimNetAPI::SendCustomPromptToLLM(
                pt, "narrative_engine_action_select", "narrative_engine_director", promptCtx);

            if (!result.ok) {
                logger::warn("BeatSystem: {}beat-select LLM call failed: {}", logPrefix, result.response);
                FinalizeWithFailure(
                    pt, std::move(rec), std::string("LLM error: ") + result.response, std::move(onFinalized));
                return;
            }

            if (Settings::Get().debugMode) {
                logger::debug("BeatSystem: beat-select LLM callback: body={}B", result.response.size());
                if (!result.response.empty()) {
                    logger::debug("BeatSystem: beat-select LLM response: {}", result.response);
                }
            }

            const std::string body = EvaluationPipeline::StripMarkdownFences(result.response);
            auto parsed = nlohmann::json::parse(body, nullptr, false);

            if (parsed.is_discarded() || !parsed.is_object()) {
                FinalizeWithFailure(pt,
                                    std::move(rec),
                                    std::string("LLM response was not a JSON object: ") + result.response,
                                    std::move(onFinalized));
                return;
            }

            std::string chosenBeat;
            if (auto it = parsed.find("action"); it != parsed.end() && it->is_string()) {
                chosenBeat = it->get<std::string>();
            }
            nlohmann::json parameters = nlohmann::json::object();
            if (auto it = parsed.find("parameters"); it != parsed.end() && it->is_object()) {
                parameters = *it;
            }
            std::string narrativeNote;
            if (auto it = parsed.find("narrative_note"); it != parsed.end() && it->is_string()) {
                narrativeNote = LLMTextSanitizer::Sanitize(it->get<std::string>());
                if (narrativeNote.size() > 200) {
                    narrativeNote.resize(200);
                }
            }
            std::string parameterJustification;
            if (auto it = parsed.find("parameter_justification"); it != parsed.end() && it->is_string()) {
                parameterJustification = LLMTextSanitizer::Sanitize(it->get<std::string>());
                if (parameterJustification.size() > 400) {
                    parameterJustification.resize(400);
                }
            }

            FinalizeWithLLMResponse(pt,
                                    std::move(snapshot),
                                    std::move(rec),
                                    std::move(candidateNames),
                                    std::move(chosenBeat),
                                    std::move(parameters),
                                    std::move(narrativeNote),
                                    std::move(parameterJustification),
                                    direction,
                                    tensionDelta,
                                    std::move(onFinalized));
        }
    } // namespace

    namespace
    {
        // Bundled result of the main-thread engine reads ConsiderBeat
        // needs: BeatContext build, precondition probe, IsAvailable
        // gathering, and sender-candidate collection with min-threshold
        // pruning. Everything the LLM prompt needs to know about the
        // world lands here in one MainThread::Run hop, so the plugin
        // thread never has to make repeated round trips.
        //
        // Result codes:
        //   * blockedReason non-null → precondition failed; skip cleanly
        //   * candidates.empty() (and blockedReason null) → no viable
        //     beats after IsAvailable + composer prune
        //   * otherwise → fire the LLM with the returned lists
        struct BeatSelectPrep
        {
            const char* blockedReason = nullptr;
            BeatContext ctx;
            std::vector<IBeat*> candidates;
            std::vector<LetterComposer::SenderCandidate> letterSenderCandidates;
            std::vector<VisitComposer::SenderCandidate> visitSenderCandidates;
        };

        // Runs entirely on the plugin thread. Every engine read
        // here is either (a) a stable singleton pointer + plain
        // bool/pointer load that the codebase already accepts off-
        // main (see docs/MAIN_THREAD_STUTTER_AUDIT.md and the
        // BeatSystem worker's own gate reads via EngineUtils), or
        // (b) a SkyrimNet DLL call that's documented thread-safe,
        // or (c) an alias / extra-list walk that carries its own
        // BSReadLockGuard precisely to permit off-main reading.
        // Nothing here mutates engine state.
        //
        // Concretely:
        //   * BuildBeatContextFromSnapshot: PlayerCharacter::IsInCombat,
        //     UI::IsMenuOpen — matches EngineUtils::IsPlayerInCombat /
        //     IsPlayerInDialogue (both untagged, safe-from-any-thread).
        //   * CheckGlobalBeatPreconditions → AlphaCanon::IsInScriptedScene
        //     / IsInDoNotDisturbCell — stable-singleton pointer walks
        //     + bool/string loads.
        //   * BeatRegistry::AvailableMatching → per-beat IsAvailable —
        //     the two implementations that exist (NPCLetterBeat,
        //     NPCVisitBeat) do SkyrimNet DLL calls + alias-walk
        //     filtering, both off-main-safe per the audit.
        //   * LetterComposer / VisitComposer CollectSenderCandidates —
        //     same shape as the IsAvailable path (SenderCandidatePool
        //     build).
        BeatSelectPrep BuildBeatSelectPrep(const PluginThread::Token&,
                                           const Snapshot& snapshot,
                                           PhaseTracker::Direction direction,
                                           int tensionDelta,
                                           bool debug)
        {
            BeatSelectPrep prep;
            prep.ctx = BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta);

            if (const char* blockedBy = CheckGlobalBeatPreconditions(prep.ctx)) {
                prep.blockedReason = blockedBy;
                return prep;
            }

            const BeatPolarity desired =
                (direction == PhaseTracker::Direction::Raise) ? BeatPolarity::Raise : BeatPolarity::Lower;
            prep.candidates = BeatRegistry::AvailableMatching(prep.ctx, desired);

            // Composer collection + min-threshold prune. Both
            // collectors iterate over live actor pools via the same
            // off-main-safe reads the per-beat IsAvailable path uses
            // (SkyrimNetAPI::GetActorEngagement + alias walk). A pool
            // falling under its threshold drops that beat from the
            // candidate list rather than starving the LLM with an
            // unviable sender pool.
            const auto isLetterBeat = [](IBeat* b) { return b && b->Name() == "npc_letter"; };
            const auto isVisitBeat = [](IBeat* b) { return b && b->Name() == "npc_visit"; };
            const bool npcLetterPresent = std::any_of(prep.candidates.begin(), prep.candidates.end(), isLetterBeat);
            const bool npcVisitPresent = std::any_of(prep.candidates.begin(), prep.candidates.end(), isVisitBeat);
            if (npcLetterPresent) {
                prep.letterSenderCandidates = LetterComposer::CollectSenderCandidates();
                const int minSenders = Settings::Get().letterMinSenderCandidates;
                if (static_cast<int>(prep.letterSenderCandidates.size()) < minSenders) {
                    if (debug) {
                        logger::debug("BeatSystem::ConsiderBeat: dropping npc_letter "
                                      "from candidates ({} viable senders < min {})",
                                      prep.letterSenderCandidates.size(),
                                      minSenders);
                    }
                    prep.candidates.erase(std::remove_if(prep.candidates.begin(), prep.candidates.end(), isLetterBeat),
                                          prep.candidates.end());
                    prep.letterSenderCandidates.clear();
                }
            }
            if (npcVisitPresent) {
                prep.visitSenderCandidates = VisitComposer::CollectSenderCandidates();
                const int minSenders = Settings::Get().visitMinSenderCandidates;
                if (static_cast<int>(prep.visitSenderCandidates.size()) < minSenders) {
                    if (debug) {
                        logger::debug("BeatSystem::ConsiderBeat: dropping npc_visit "
                                      "from candidates ({} viable senders < min {})",
                                      prep.visitSenderCandidates.size(),
                                      minSenders);
                    }
                    prep.candidates.erase(std::remove_if(prep.candidates.begin(), prep.candidates.end(), isVisitBeat),
                                          prep.candidates.end());
                    prep.visitSenderCandidates.clear();
                }
            }

            return prep;
        }
    } // namespace

    void ConsiderBeat(const PluginThread::Token& pt,
                      Snapshot snapshot,
                      DecisionLog::DecisionRecord rec,
                      FinalizedCallback onFinalized)
    {
        const bool debug = Settings::Get().debugMode;

        // Gate 1: a beat is already in flight.
        TopLevelState topState;
        std::uint32_t cooldownMs;
        std::string inFlightName;
        {
            std::scoped_lock lock(g_stateMutex);
            topState = g_topLevelState;
            cooldownMs = g_globalCooldownMs;
            inFlightName = g_runningBeatName;
        }
        if (topState == TopLevelState::BEAT_RUNNING) {
            if (debug) {
                logger::debug("BeatSystem::ConsiderBeat: gate in_flight blocked: "
                              "'{}' is still running",
                              inFlightName);
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 2: tension eval already advanced the phase this tick.
        if (rec.advancedToPhase) {
            if (debug) {
                logger::debug("BeatSystem::ConsiderBeat: gate just_advanced blocked: "
                              "phase advanced this tick");
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 3: global beat cooldown.
        const std::uint32_t cooldownThresholdMs =
            static_cast<std::uint32_t>(std::max(0, Settings::Get().beatCooldownSeconds) * 1000);
        if (cooldownMs < cooldownThresholdMs) {
            if (debug) {
                const std::uint32_t remaining = cooldownThresholdMs - cooldownMs;
                logger::debug("BeatSystem::ConsiderBeat: gate cooldown blocked: "
                              "{:.1f}s of {:.1f}s accumulated ({:.1f}s remaining)",
                              cooldownMs / 1000.0,
                              cooldownThresholdMs / 1000.0,
                              remaining / 1000.0);
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 4: phase dwell time vs. ideal duration.
        const auto currentPhaseOpt = PhaseTracker::PhaseFromName(snapshot.currentPhase);
        if (!currentPhaseOpt) {
            if (debug) {
                logger::debug("BeatSystem::ConsiderBeat: gate dwell blocked: "
                              "unknown phase '{}'",
                              snapshot.currentPhase);
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }
        const int ideal = IdealDurationFor(*currentPhaseOpt);
        if (snapshot.timeInPhaseSeconds < static_cast<float>(ideal)) {
            if (debug) {
                logger::debug("BeatSystem::ConsiderBeat: gate dwell blocked: "
                              "{:.1f}s / {}s ideal",
                              snapshot.timeInPhaseSeconds,
                              ideal);
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }

        // Compute direction + tension delta up-front — both flow into
        // BeatContext (Either-polarity beats consume them) and into the
        // beat-select prompt.
        const auto direction = PhaseTracker::OutgoingDirection(*currentPhaseOpt);
        const int tensionDelta = ComputeTensionDelta(*currentPhaseOpt, rec.tensionScore);

        // Build BeatContext, check global preconditions, gather
        // IsAvailable candidates, collect letter/visit sender pools,
        // prune under min-thresholds. All on the plugin thread —
        // every engine read on this path is off-main-safe per the
        // helper's contract. See BuildBeatSelectPrep above.
        auto prep = BuildBeatSelectPrep(pt, snapshot, direction, tensionDelta, debug);

        if (prep.blockedReason) {
            if (debug) {
                logger::debug("BeatSystem::ConsiderBeat: gate "
                              "global_preconditions blocked: {}",
                              prep.blockedReason);
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }

        // Recency filter — runs on plugin thread, briefly acquires the
        // session-only g_recentMutex.
        if (!prep.candidates.empty()) {
            const double now = NowUnixSeconds();
            std::scoped_lock lock(g_recentMutex);
            TrimRecentlyFiredLocked(now);
            prep.candidates.erase(std::remove_if(prep.candidates.begin(),
                                                 prep.candidates.end(),
                                                 [now](IBeat* b) { return WasFiredRecentlyLocked(b->Name(), now); }),
                                  prep.candidates.end());
        }

        if (prep.candidates.empty()) {
            if (debug) {
                logger::debug("BeatSystem::ConsiderBeat: gate candidates blocked: "
                              "0 candidates after filtering");
            }
            FinalizeWithoutBeat(pt, std::move(rec), std::move(onFinalized));
            return;
        }

        // All gates passed — fire the beat-select LLM call.
        const char* dirName = (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
        logger::info("BeatSystem: firing beat-select (direction={}, "
                     "tension_delta={}, candidates={}, "
                     "letter_sender_candidates={}, visit_sender_candidates={}, "
                     "dwell={:.1f}/{}s)",
                     dirName,
                     tensionDelta,
                     prep.candidates.size(),
                     prep.letterSenderCandidates.size(),
                     prep.visitSenderCandidates.size(),
                     snapshot.timeInPhaseSeconds,
                     ideal);

        FireBeatSelectLLM(pt,
                          std::move(snapshot),
                          std::move(rec),
                          std::move(prep.candidates),
                          std::move(prep.letterSenderCandidates),
                          std::move(prep.visitSenderCandidates),
                          direction,
                          tensionDelta,
                          std::move(onFinalized),
                          "");
    }

    namespace
    {
        // Shared body for StartBeat / FinalizeWithLLMResponse. Runs on
        // the plugin thread: BeatRegistry::Find and the g_stateMutex
        // flip are mutex-guarded; the beat->OnStart implementations
        // that exist today (AmbushBeat, NPCLetterBeat, NPCVisitBeat)
        // do only param parse + session-state reset behind their own
        // mutexes / atomics — no engine reads or mutations. See
        // IBeat::OnStart's contract.
        //
        // On OnStart throw, reverts the top-level state so the system
        // doesn't get stuck in BEAT_RUNNING with a beat that never ran.
        void StartBeatInternal(const PluginThread::Token&,
                               const std::string& name,
                               const BeatContext& ctx,
                               const nlohmann::json& parameters)
        {
            IBeat* beat = BeatRegistry::Find(name);
            if (!beat) {
                logger::warn("BeatSystem::StartBeat: unknown beat '{}'; no-op", name);
                return;
            }

            {
                std::scoped_lock lock(g_stateMutex);
                if (g_topLevelState == TopLevelState::BEAT_RUNNING) {
                    logger::warn("BeatSystem::StartBeat: refused — beat '{}' is "
                                 "already running",
                                 g_runningBeatName);
                    return;
                }
                g_topLevelState = TopLevelState::BEAT_RUNNING;
                g_runningBeatName = name;
                g_runningBeatStartedAt = NowUnixSeconds();
                g_runningBeatCurrentState = BeatState::COMPOSE;
                g_globalCooldownMs = 0;
            }

            try {
                beat->OnStart(ctx, parameters);
            } catch (const std::exception& e) {
                logger::error("BeatSystem::StartBeat: '{}' OnStart threw: {}; "
                              "reverting to NO_BEAT_RUNNING",
                              name,
                              e.what());
                OnRevert();
                return;
            } catch (...) {
                logger::error("BeatSystem::StartBeat: '{}' OnStart threw unknown "
                              "exception; reverting to NO_BEAT_RUNNING",
                              name);
                OnRevert();
                return;
            }

            BeatRegistry::MarkDispatched(name);
            logger::info("BeatSystem::StartBeat: '{}' started (BeatState=COMPOSE)", name);
        }
    } // namespace

    void StartBeat(const PluginThread::Token& pt, const std::string& name, const nlohmann::json& parameters)
    {
        // Build a live BeatContext from a fresh snapshot so callers
        // that don't provide one (cosave restore paths, direct
        // startups) still land the beat with real world state visible
        // to OnStart. BuildSnapshot(pt) runs on the plugin thread with
        // a single bundled MainThread::Run for its engine reads.
        // BuildBeatContextFromSnapshot itself is off-main-safe (see
        // BuildBeatSelectPrep's contract in this file).
        Snapshot snapshot = EvaluationPipeline::BuildSnapshot(pt);
        const auto currentPhaseOpt = PhaseTracker::PhaseFromName(snapshot.currentPhase);
        const auto currentPhase = currentPhaseOpt.value_or(PhaseTracker::Phase::Exposition);
        const auto direction = PhaseTracker::OutgoingDirection(currentPhase);
        const BeatContext ctx = BuildBeatContextFromSnapshot(snapshot, direction, 0);
        StartBeatInternal(pt, name, ctx, parameters);
    }

    void ForceDispatchBeat(std::string_view name)
    {
        // Single-flight lock is the one gate we never bypass.
        {
            std::scoped_lock lock(g_stateMutex);
            if (g_topLevelState == TopLevelState::BEAT_RUNNING) {
                logger::warn("BeatSystem: force-dispatch refused ('{}' is in "
                             "flight, requested='{}')",
                             g_runningBeatName,
                             std::string{name});
                return;
            }
        }

        IBeat* beat = BeatRegistry::Find(name);
        if (!beat) {
            logger::warn("BeatSystem: force-dispatch refused (unknown beat '{}')", std::string{name});
            return;
        }

        logger::info("BeatSystem: force-dispatch entry for '{}'", std::string{name});

        Snapshot snapshot = EvaluationPipeline::BuildSnapshot();
        const auto currentPhaseOpt = PhaseTracker::PhaseFromName(snapshot.currentPhase);
        const auto currentPhase = currentPhaseOpt.value_or(PhaseTracker::Phase::Exposition);
        const auto direction = PhaseTracker::OutgoingDirection(currentPhase);

        DecisionLog::DecisionRecord rec;
        rec.realTimeSec = NowUnixSeconds();
        rec.gameDaysPassed = snapshot.player.gameTimeSeconds > 0.0
                                 ? static_cast<float>(snapshot.player.gameTimeSeconds / 86400.0)
                                 : 0.0f;
        rec.tensionScore = DecisionLog::LatestTensionScore().value_or(0);
        rec.currentPhase = currentPhase;
        rec.alphaCanonActiveSignals = 0;
        rec.narrativeNote = "forced dispatch via debug UI";

        const int tensionDelta = ComputeTensionDelta(currentPhase, rec.tensionScore);

        // Bypassed: global cooldown, phase-dwell, anti-repetition,
        // beat's own IsAvailable, and letter/visit min-sender gate.
        // Respected: global preconditions.
        const BeatContext ctx = BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta);
        if (const char* blockedBy = CheckGlobalBeatPreconditions(ctx)) {
            logger::warn("BeatSystem: force-dispatch refused ('{}' blocked by "
                         "global precondition: {})",
                         std::string{name},
                         blockedBy);
            return;
        }

        // One-element candidate list. Still collect sender candidates
        // for letter/visit so the LLM can render its selection prompt;
        // an empty pool doesn't fail the force-dispatch — OnStart's
        // failure detail message is what surfaces the problem.
        std::vector<IBeat*> candidates{beat};
        std::vector<LetterComposer::SenderCandidate> letterSenderCandidates;
        std::vector<VisitComposer::SenderCandidate> visitSenderCandidates;
        if (beat->Name() == "npc_letter") {
            letterSenderCandidates = LetterComposer::CollectSenderCandidates();
        } else if (beat->Name() == "npc_visit") {
            visitSenderCandidates = VisitComposer::CollectSenderCandidates();
        }

        const char* dirName = (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
        logger::info("BeatSystem: firing force-dispatch beat-select (beat='{}', "
                     "direction={}, tension_delta={}, letter_sender_candidates={}, "
                     "visit_sender_candidates={})",
                     std::string{name},
                     dirName,
                     tensionDelta,
                     letterSenderCandidates.size(),
                     visitSenderCandidates.size());

        // Empty finalizer — force-dispatch runs outside the normal
        // evaluation loop, so there's no g_inFlight guard to release.
        //
        // Hop onto the plugin thread for the LLM round-trip.
        // ForceDispatchBeat's front-half runs on main today (dashboard
        // -> MarshalToMainThread -> here), so we enqueue the LLM firing
        // + finalize chain instead of calling FireBeatSelectLLM
        // directly. This mirrors the ConsiderBeat migration: the LLM
        // wait is off-main; only StartBeat's OnStart hop returns to
        // main via MainThread::FireAndForget inside FireBeatSelectLLM.
        AsyncDispatch::EnqueueWork([snapshot = std::move(snapshot),
                                    rec = std::move(rec),
                                    candidates = std::move(candidates),
                                    letterSenderCandidates = std::move(letterSenderCandidates),
                                    visitSenderCandidates = std::move(visitSenderCandidates),
                                    direction,
                                    tensionDelta](const PluginThread::Token& pt) mutable {
            FireBeatSelectLLM(pt,
                              std::move(snapshot),
                              std::move(rec),
                              std::move(candidates),
                              std::move(letterSenderCandidates),
                              std::move(visitSenderCandidates),
                              direction,
                              tensionDelta,
                              /*onFinalized=*/nullptr,
                              "force-dispatch ");
        });
    }
    bool AbortRunningBeat()
    {
        std::string runningName;
        {
            std::scoped_lock lock(g_stateMutex);
            if (g_topLevelState != TopLevelState::BEAT_RUNNING) {
                logger::info("BeatSystem::AbortRunningBeat: no beat in flight; no-op");
                return false;
            }
            runningName = g_runningBeatName;
        }

        IBeat* beat = BeatRegistry::Find(runningName);
        if (beat) {
            logger::warn("BeatSystem::AbortRunningBeat: aborting '{}'", runningName);
            try {
                beat->Abort();
            } catch (const std::exception& e) {
                logger::error("BeatSystem::AbortRunningBeat: '{}' Abort threw: {}; "
                              "continuing to force NO_BEAT_RUNNING",
                              runningName,
                              e.what());
            } catch (...) {
                logger::error("BeatSystem::AbortRunningBeat: '{}' Abort threw unknown "
                              "exception; continuing to force NO_BEAT_RUNNING",
                              runningName);
            }
        } else {
            logger::warn("BeatSystem::AbortRunningBeat: BEAT_RUNNING with unknown beat "
                         "'{}'; forcing NO_BEAT_RUNNING without calling Abort",
                         runningName);
        }

        // Force top-level state back to idle regardless of Abort
        // outcome — the worker Tick reads state under the same mutex
        // before dispatching to the beat, so this cleanly halts any
        // further Tick calls into the aborted beat.
        {
            std::scoped_lock lock(g_stateMutex);
            g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
            g_runningBeatName.clear();
            g_runningBeatCurrentState = BeatState::NOT_RUNNING;
            g_runningBeatStartedAt = 0.0;
            g_globalCooldownMs = 0;
        }
        return true;
    }
} // namespace NarrativeEngine::BeatSystem
