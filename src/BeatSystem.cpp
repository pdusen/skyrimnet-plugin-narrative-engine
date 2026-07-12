#include <BeatSystem.h>

#include <AlphaCanon.h>
#include <AsyncDispatch.h>
#include <BeatRegistry.h>
#include <CombatEventLog.h>
#include <DecisionLog.h>
#include <EngineUtils.h>
#include <EvaluationPipeline.h>
#include <LLMTextSanitizer.h>
#include <LetterComposer.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <SkyrimNetAPI.h>
#include <SkyrimNetEvents.h>
#include <VisitComposer.h>
#include <logger.h>

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

        std::mutex   g_stateMutex;
        TopLevelState g_topLevelState = TopLevelState::NO_BEAT_RUNNING;
        std::string   g_runningBeatName;
        std::uint32_t g_globalCooldownMs = 0;
        double        g_runningBeatStartedAt = 0.0;
        BeatState     g_runningBeatCurrentState = BeatState::NOT_RUNNING;

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
            double      dispatchedAt = 0.0;  // Unix-epoch seconds
        };
        std::mutex                 g_recentMutex;
        std::deque<RecentlyFired>  g_recentlyFired;
        constexpr std::size_t      kRecentlyFiredCap = 32;

        // ----- Worker thread lifecycle ------------------------------------

        std::atomic<bool> g_stopRequested{false};
        std::thread       g_worker;
        bool              g_running = false;

        // Heartbeat cadence — one log line every kHeartbeatEveryNTicks
        // iterations. At 250ms cadence, 40 ticks = ~10s.
        constexpr int kHeartbeatEveryNTicks = 40;

        // Gate-derived TickMode. The three underlying reads live in
        // EngineUtils so other subsystems (e.g. beats' own Tick logic
        // that wants a paused-check without going through BeatSystem)
        // can share the same "safe off-thread bool read on a stable
        // singleton pointer" guarantee. Precedence: Paused > Combat
        // > Dialogue > Normal — a paused game is never also reported
        // as Combat or Dialogue.
        TickMode ComputeTickMode()
        {
            const bool paused   = EngineUtils::IsGamePaused();
            const bool combat   = EngineUtils::IsPlayerInCombat();
            const bool dialogue = EngineUtils::IsPlayerInDialogue();
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
                                "BeatSystem: heartbeat state={} mode={} cooldown={:.1f}s",
                                TopLevelStateName(topState),
                                TickModeName(mode), cooldown / 1000.0);
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
            "cooldown={:.1f}s beat_state={}",
            TopLevelStateName(topStateLoaded),
            g_runningBeatName, cooldownLoaded / 1000.0,
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

        int ComputeTensionDelta(PhaseTracker::Phase phase,
                                std::uint32_t       currentTension)
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
        BeatContext BuildBeatContextFromSnapshot(
            const Snapshot&         snapshot,
            PhaseTracker::Direction direction    = PhaseTracker::Direction::Raise,
            int                     tensionDelta = 0)
        {
            BeatContext ctx;
            ctx.player = RE::PlayerCharacter::GetSingleton();
            if (ctx.player) {
                ctx.playerInCombat = ctx.player->IsInCombat();
            }
            ctx.playerInInterior = snapshot.player.cellIsInterior;
            ctx.locationName     = snapshot.player.locationName;
            ctx.cellName         = snapshot.player.cellName;
            if (auto* ui = RE::UI::GetSingleton()) {
                ctx.playerInDialogue = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            }
            ctx.desiredDirection = direction;
            ctx.tensionDelta     = tensionDelta;
            return ctx;
        }

        // Globally-disqualifying world state. Returns nullptr on
        // all-clear, or a short literal naming the gate that blocked.
        // Runs twice per firing tick — once pre-LLM to save the round
        // trip when we know we'd bail, once post-LLM in case the
        // situation changed during the round trip.
        const char* CheckGlobalBeatPreconditions(const BeatContext& ctx)
        {
            if (ctx.playerInCombat)                  return "playerInCombat";
            if (ctx.playerInDialogue)                return "playerInDialogue";
            if (AlphaCanon::IsInScriptedScene())     return "scriptedScene";
            if (AlphaCanon::IsInDoNotDisturbCell())  return "doNotDisturbCell";
            return nullptr;
        }

        // Caller holds g_recentMutex.
        void TrimRecentlyFiredLocked(double now)
        {
            const double window = static_cast<double>(
                Settings::Get().beatRepetitionWindowSeconds);
            while (!g_recentlyFired.empty() &&
                   (now - g_recentlyFired.front().dispatchedAt) > window) {
                g_recentlyFired.pop_front();
            }
            while (g_recentlyFired.size() > kRecentlyFiredCap) {
                g_recentlyFired.pop_front();
            }
        }

        // Caller holds g_recentMutex.
        bool WasFiredRecentlyLocked(const std::string& name, double now)
        {
            const double window = static_cast<double>(
                Settings::Get().beatRepetitionWindowSeconds);
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
            const Snapshot&                                     snapshot,
            const std::vector<IBeat*>&                          candidates,
            PhaseTracker::Direction                             direction,
            int                                                 tensionDelta,
            const std::vector<LetterComposer::SenderCandidate>& letterSenderCandidates,
            const std::vector<VisitComposer::SenderCandidate>&  visitSenderCandidates)
        {
            nlohmann::json ctx;
            ctx["desired_direction"] =
                (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
            ctx["tension_delta"] = tensionDelta;

            const auto serializedLetterCandidates =
                letterSenderCandidates.empty()
                    ? nlohmann::json::array()
                    : LetterComposer::SerializeSenderCandidates(letterSenderCandidates);
            const auto serializedVisitCandidates =
                visitSenderCandidates.empty()
                    ? nlohmann::json::array()
                    : VisitComposer::SerializeSenderCandidates(visitSenderCandidates);

            nlohmann::json candArr = nlohmann::json::array();
            for (auto* b : candidates) {
                if (!b) continue;
                nlohmann::json cj = {
                    {"name",        b->Name()},
                    {"description", b->Description()},
                    {"letter_sender_candidates",
                        (b->Name() == "npc_letter")
                            ? serializedLetterCandidates
                            : nlohmann::json::array()},
                    {"visit_sender_candidates",
                        (b->Name() == "npc_visit")
                            ? serializedVisitCandidates
                            : nlohmann::json::array()},
                };
                candArr.push_back(std::move(cj));
            }
            ctx["candidates"] = std::move(candArr);

            ctx["player_context"] = {
                {"location_name",    snapshot.player.locationName},
                {"cell_name",        snapshot.player.cellName},
                {"cell_is_interior", snapshot.player.cellIsInterior},
            };

            {
                auto parsed = nlohmann::json::parse(
                    snapshot.skyrimNetEventsJSON, nullptr, false);
                nlohmann::json skyrimSide = nlohmann::json::array();
                if (parsed.is_array()) {
                    const double cutoff = snapshot.phaseEnteredAtRealTime;
                    nlohmann::json filtered = nlohmann::json::array();
                    for (auto& evt : parsed) {
                        if (!evt.is_object()) continue;
                        if (cutoff > 0.0) {
                            const double et = evt.value("localTime", 0.0);
                            if (et < cutoff) continue;
                        }
                        filtered.push_back(std::move(evt));
                    }
                    std::reverse(filtered.begin(), filtered.end());
                    SkyrimNetEvents::FormatEventsText(
                        filtered, snapshot.player.gameTimeSeconds);
                    skyrimSide = std::move(filtered);
                }
                auto merged = SkyrimNetEvents::BuildMergedTimeline(
                    std::move(skyrimSide),
                    CombatEventLog::GetRenderedTail(snapshot.player.gameTimeSeconds),
                    snapshot.player.gameTimeSeconds);

                if (merged.is_array() && merged.size() > kBeatSelectEventTailSize) {
                    const std::size_t skipFront =
                        merged.size() - kBeatSelectEventTailSize;
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
        void FinalizeWithFailure(DecisionLog::DecisionRecord rec,
                                 const std::string&          reason,
                                 FinalizedCallback           onFinalized)
        {
            rec.beatSelected = "(failed: " + reason + ")";
            {
                std::scoped_lock lock(g_stateMutex);
                g_globalCooldownMs = 0;
            }
            logger::warn("BeatSystem: beat-select failed: {}", reason);
            EvaluationPipeline::ApplyDecision(rec);
            if (onFinalized) onFinalized();
        }

        // Forward declaration — StartBeat's definition lives at the
        // bottom of this file.
        void StartBeatInternal(const std::string&    name,
                               const BeatContext&    ctx,
                               const nlohmann::json& parameters);

        // Called on the main thread when the LLM response has been
        // parsed and is structurally valid. Validates the chosen name
        // against the candidate set, re-checks global preconditions,
        // populates the record, and hands off to StartBeatInternal.
        void FinalizeWithLLMResponse(Snapshot                     snapshot,
                                     DecisionLog::DecisionRecord  rec,
                                     std::vector<std::string>     candidateNames,
                                     std::string                  chosenBeat,
                                     nlohmann::json               parameters,
                                     std::string                  narrativeNote,
                                     std::string                  parameterJustification,
                                     PhaseTracker::Direction      direction,
                                     int                          tensionDelta,
                                     FinalizedCallback            onFinalized)
        {
            const bool isValidCandidate =
                std::find(candidateNames.begin(), candidateNames.end(), chosenBeat)
                != candidateNames.end();
            if (!isValidCandidate) {
                FinalizeWithFailure(
                    std::move(rec),
                    "LLM returned unknown beat '" + chosenBeat + "'",
                    std::move(onFinalized));
                return;
            }

            IBeat* beat = BeatRegistry::Find(chosenBeat);
            if (!beat) {
                FinalizeWithFailure(
                    std::move(rec),
                    "LLM-chosen beat '" + chosenBeat +
                        "' missing from registry",
                    std::move(onFinalized));
                return;
            }

            const BeatContext ctx =
                BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta);

            if (const char* blockedBy = CheckGlobalBeatPreconditions(ctx)) {
                if (Settings::Get().debugMode) {
                    logger::debug(
                        "BeatSystem: global preconditions changed during "
                        "LLM round trip (blocked: {}); dropping chosen "
                        "beat '{}'", blockedBy, chosenBeat);
                }
                FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
                return;
            }

            // Populate the record. beatSelected is tentatively the beat
            // name; a StartBeat failure downstream doesn't roll this
            // back — the beat *was* chosen, just failed to fire.
            rec.beatSelected       = chosenBeat;
            rec.beatParametersJSON = parameters.dump();
            if (!narrativeNote.empty()) {
                rec.narrativeNote = std::move(narrativeNote);
            }

            // Inject parameter_justification into params for the
            // compose step to consume as sender-motivation seed.
            if (!parameterJustification.empty()) {
                parameters["parameter_justification"] =
                    std::move(parameterJustification);
            }

            StartBeatInternal(chosenBeat, ctx, parameters);
            PushRecentlyFired(chosenBeat);
            logger::info(
                "BeatSystem: beat '{}' selected (direction={}, "
                "tension_delta={})", chosenBeat,
                (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower",
                tensionDelta);

            EvaluationPipeline::ApplyDecision(rec);
            if (onFinalized) onFinalized();
        }

        // The shared body invoked by both ConsiderBeat's LLM finalizer
        // and ForceDispatchBeat. Sends the beat-select prompt with a
        // pre-built candidate list; on response, marshals to main and
        // hands off to FinalizeWithLLMResponse. Handles the !queued
        // failure path.
        void FireBeatSelectLLM(
            Snapshot                                            snapshot,
            DecisionLog::DecisionRecord                         rec,
            std::vector<IBeat*>                                 candidates,
            std::vector<LetterComposer::SenderCandidate>        letterSenderCandidates,
            std::vector<VisitComposer::SenderCandidate>         visitSenderCandidates,
            PhaseTracker::Direction                             direction,
            int                                                 tensionDelta,
            FinalizedCallback                                   onFinalized,
            const char*                                         logPrefix)
        {
            const std::string promptCtx =
                BuildBeatSelectPromptContext(
                    snapshot, candidates, direction, tensionDelta,
                    letterSenderCandidates, visitSenderCandidates);
            if (Settings::Get().debugMode) {
                logger::debug("BeatSystem: {}beat-select prompt context: {}",
                              logPrefix, promptCtx);
            }

            std::vector<std::string> candidateNames;
            candidateNames.reserve(candidates.size());
            for (auto* b : candidates) {
                if (b) candidateNames.push_back(b->Name());
            }

            DecisionLog::DecisionRecord recBackup       = rec;
            FinalizedCallback           finalizedBackup = onFinalized;

            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                "narrative_engine_action_select", "narrative_engine_director", promptCtx,
                [snapshot       = std::move(snapshot),
                 rec            = std::move(rec),
                 candidateNames = std::move(candidateNames),
                 onFinalized    = std::move(onFinalized),
                 direction,
                 tensionDelta]
                (std::string response, bool success) mutable
                {
                    if (!success) {
                        AsyncDispatch::MarshalToMainThread(
                            [rec = std::move(rec),
                             onFinalized = std::move(onFinalized),
                             response = std::move(response)]() mutable {
                                FinalizeWithFailure(
                                    std::move(rec),
                                    std::string("LLM error: ") + response,
                                    std::move(onFinalized));
                            });
                        return;
                    }

                    if (Settings::Get().debugMode) {
                        logger::debug(
                            "BeatSystem: beat-select LLM callback: body={}B",
                            response.size());
                        if (!response.empty()) {
                            logger::debug(
                                "BeatSystem: beat-select LLM response: {}",
                                response);
                        }
                    }

                    const std::string body =
                        EvaluationPipeline::StripMarkdownFences(response);
                    auto parsed = nlohmann::json::parse(body, nullptr, false);

                    if (parsed.is_discarded() || !parsed.is_object()) {
                        AsyncDispatch::MarshalToMainThread(
                            [rec = std::move(rec),
                             onFinalized = std::move(onFinalized),
                             response = std::move(response)]() mutable {
                                FinalizeWithFailure(
                                    std::move(rec),
                                    std::string("LLM response was not a "
                                                "JSON object: ") + response,
                                    std::move(onFinalized));
                            });
                        return;
                    }

                    std::string chosenBeat;
                    if (auto it = parsed.find("action");
                        it != parsed.end() && it->is_string())
                    {
                        chosenBeat = it->get<std::string>();
                    }
                    nlohmann::json parameters = nlohmann::json::object();
                    if (auto it = parsed.find("parameters");
                        it != parsed.end() && it->is_object())
                    {
                        parameters = *it;
                    }
                    std::string narrativeNote;
                    if (auto it = parsed.find("narrative_note");
                        it != parsed.end() && it->is_string())
                    {
                        narrativeNote =
                            LLMTextSanitizer::Sanitize(it->get<std::string>());
                        if (narrativeNote.size() > 200) {
                            narrativeNote.resize(200);
                        }
                    }
                    std::string parameterJustification;
                    if (auto it = parsed.find("parameter_justification");
                        it != parsed.end() && it->is_string())
                    {
                        parameterJustification =
                            LLMTextSanitizer::Sanitize(it->get<std::string>());
                        if (parameterJustification.size() > 400) {
                            parameterJustification.resize(400);
                        }
                    }

                    AsyncDispatch::MarshalToMainThread(
                        [snapshot       = std::move(snapshot),
                         rec            = std::move(rec),
                         candidateNames = std::move(candidateNames),
                         chosenBeat     = std::move(chosenBeat),
                         parameters     = std::move(parameters),
                         narrativeNote  = std::move(narrativeNote),
                         parameterJustification =
                             std::move(parameterJustification),
                         direction,
                         tensionDelta,
                         onFinalized    = std::move(onFinalized)]() mutable {
                            FinalizeWithLLMResponse(
                                std::move(snapshot), std::move(rec),
                                std::move(candidateNames),
                                std::move(chosenBeat),
                                std::move(parameters),
                                std::move(narrativeNote),
                                std::move(parameterJustification),
                                direction, tensionDelta,
                                std::move(onFinalized));
                        });
                });

            if (!queued) {
                logger::warn(
                    "BeatSystem: {}SendCustomPromptToLLM returned false; "
                    "treating as failure", logPrefix);
                FinalizeWithFailure(
                    std::move(recBackup),
                    "SendCustomPromptToLLM returned false (queue full?)",
                    std::move(finalizedBackup));
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
                    "{:.1f}s of {:.1f}s accumulated ({:.1f}s remaining)",
                    cooldownMs / 1000.0, cooldownThresholdMs / 1000.0,
                    remaining / 1000.0);
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

        // Compute direction + tension delta up-front — both flow into
        // BeatContext (Either-polarity beats consume them) and into the
        // beat-select prompt.
        const auto direction = PhaseTracker::OutgoingDirection(*currentPhaseOpt);
        const int  tensionDelta =
            ComputeTensionDelta(*currentPhaseOpt, rec.tensionScore);

        // Gate 5: global beat preconditions (combat / dialogue /
        // scripted scene / DND cell). Checked before the LLM round trip
        // so we don't waste it.
        const BeatContext ctx =
            BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta);
        if (const char* blockedBy = CheckGlobalBeatPreconditions(ctx)) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate "
                    "global_preconditions blocked: {}", blockedBy);
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 6: at least one candidate survives IsAvailable + recency.
        const BeatPolarity desired =
            (direction == PhaseTracker::Direction::Raise)
                ? BeatPolarity::Raise
                : BeatPolarity::Lower;
        auto candidates = BeatRegistry::AvailableMatching(ctx, desired);

        if (!candidates.empty()) {
            const double now = NowUnixSeconds();
            std::scoped_lock lock(g_recentMutex);
            TrimRecentlyFiredLocked(now);
            candidates.erase(
                std::remove_if(candidates.begin(), candidates.end(),
                               [now](IBeat* b) {
                                   return WasFiredRecentlyLocked(b->Name(), now);
                               }),
                candidates.end());
        }

        if (candidates.empty()) {
            if (debug) {
                logger::debug(
                    "BeatSystem::ConsiderBeat: gate candidates blocked: "
                    "0 candidates after filtering");
            }
            FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
            return;
        }

        // Gate 7: collect per-beat sender candidate lists for the two
        // beats that need them at select time. If a pool falls under
        // its min-candidate threshold, drop that beat from the list
        // rather than starve the LLM.
        std::vector<LetterComposer::SenderCandidate> letterSenderCandidates;
        std::vector<VisitComposer::SenderCandidate>  visitSenderCandidates;
        const auto isLetterBeat = [](IBeat* b) {
            return b && b->Name() == "npc_letter";
        };
        const auto isVisitBeat = [](IBeat* b) {
            return b && b->Name() == "npc_visit";
        };
        const bool npcLetterPresent =
            std::any_of(candidates.begin(), candidates.end(), isLetterBeat);
        const bool npcVisitPresent =
            std::any_of(candidates.begin(), candidates.end(), isVisitBeat);
        if (npcLetterPresent) {
            letterSenderCandidates = LetterComposer::CollectSenderCandidates();
            const int minSenders = Settings::Get().letterMinSenderCandidates;
            if (static_cast<int>(letterSenderCandidates.size()) < minSenders) {
                if (debug) {
                    logger::debug(
                        "BeatSystem::ConsiderBeat: dropping npc_letter "
                        "from candidates ({} viable senders < min {})",
                        letterSenderCandidates.size(), minSenders);
                }
                candidates.erase(
                    std::remove_if(candidates.begin(), candidates.end(), isLetterBeat),
                    candidates.end());
                letterSenderCandidates.clear();
                if (candidates.empty()) {
                    FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
                    return;
                }
            }
        }
        if (npcVisitPresent) {
            visitSenderCandidates = VisitComposer::CollectSenderCandidates();
            const int minSenders = Settings::Get().visitMinSenderCandidates;
            if (static_cast<int>(visitSenderCandidates.size()) < minSenders) {
                if (debug) {
                    logger::debug(
                        "BeatSystem::ConsiderBeat: dropping npc_visit "
                        "from candidates ({} viable senders < min {})",
                        visitSenderCandidates.size(), minSenders);
                }
                candidates.erase(
                    std::remove_if(candidates.begin(), candidates.end(), isVisitBeat),
                    candidates.end());
                visitSenderCandidates.clear();
                if (candidates.empty()) {
                    FinalizeWithoutBeat(std::move(rec), std::move(onFinalized));
                    return;
                }
            }
        }

        // All gates passed — fire the beat-select LLM call.
        const char* dirName =
            (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
        logger::info(
            "BeatSystem: firing beat-select (direction={}, "
            "tension_delta={}, candidates={}, "
            "letter_sender_candidates={}, visit_sender_candidates={}, "
            "dwell={:.1f}/{}s)",
            dirName, tensionDelta, candidates.size(),
            letterSenderCandidates.size(), visitSenderCandidates.size(),
            snapshot.timeInPhaseSeconds, ideal);

        FireBeatSelectLLM(
            std::move(snapshot), std::move(rec), std::move(candidates),
            std::move(letterSenderCandidates),
            std::move(visitSenderCandidates),
            direction, tensionDelta, std::move(onFinalized), "");
    }

    namespace
    {
        // Shared body for StartBeat / FinalizeWithLLMResponse. Assumes
        // main thread and a valid `beat` in the registry; performs the
        // top-level flip, calls OnStart, and stamps
        // BeatRegistry::MarkDispatched. On OnStart throw, reverts the
        // top-level state so the system doesn't get stuck in
        // BEAT_RUNNING with a beat that never ran.
        void StartBeatInternal(const std::string&    name,
                               const BeatContext&    ctx,
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

    void StartBeat(const std::string&    name,
                   const nlohmann::json& parameters)
    {
        // Build a live BeatContext from a fresh snapshot so callers
        // that don't (e.g. cosave restore paths, direct startups) still
        // land the beat with real world state visible to OnStart.
        Snapshot snapshot = EvaluationPipeline::BuildSnapshot();
        const auto currentPhaseOpt =
            PhaseTracker::PhaseFromName(snapshot.currentPhase);
        const auto currentPhase =
            currentPhaseOpt.value_or(PhaseTracker::Phase::Exposition);
        const auto direction = PhaseTracker::OutgoingDirection(currentPhase);
        const BeatContext ctx =
            BuildBeatContextFromSnapshot(snapshot, direction, 0);
        StartBeatInternal(name, ctx, parameters);
    }

    void ForceDispatchBeat(std::string_view name)
    {
        // Single-flight lock is the one gate we never bypass.
        {
            std::scoped_lock lock(g_stateMutex);
            if (g_topLevelState == TopLevelState::BEAT_RUNNING) {
                logger::warn(
                    "BeatSystem: force-dispatch refused ('{}' is in "
                    "flight, requested='{}')",
                    g_runningBeatName, std::string{name});
                return;
            }
        }

        IBeat* beat = BeatRegistry::Find(name);
        if (!beat) {
            logger::warn(
                "BeatSystem: force-dispatch refused (unknown beat '{}')",
                std::string{name});
            return;
        }

        logger::info(
            "BeatSystem: force-dispatch entry for '{}'", std::string{name});

        Snapshot snapshot = EvaluationPipeline::BuildSnapshot();
        const auto currentPhaseOpt =
            PhaseTracker::PhaseFromName(snapshot.currentPhase);
        const auto currentPhase =
            currentPhaseOpt.value_or(PhaseTracker::Phase::Exposition);
        const auto direction = PhaseTracker::OutgoingDirection(currentPhase);

        DecisionLog::DecisionRecord rec;
        rec.realTimeSec              = NowUnixSeconds();
        rec.gameDaysPassed           = snapshot.player.gameTimeSeconds > 0.0
                                           ? static_cast<float>(snapshot.player.gameTimeSeconds / 86400.0)
                                           : 0.0f;
        rec.tensionScore             = DecisionLog::LatestTensionScore().value_or(0);
        rec.currentPhase             = currentPhase;
        rec.alphaCanonActiveSignals  = 0;
        rec.narrativeNote            = "forced dispatch via debug UI";

        const int tensionDelta = ComputeTensionDelta(currentPhase, rec.tensionScore);

        // Bypassed: global cooldown, phase-dwell, anti-repetition,
        // beat's own IsAvailable, and letter/visit min-sender gate.
        // Respected: global preconditions.
        const BeatContext ctx =
            BuildBeatContextFromSnapshot(snapshot, direction, tensionDelta);
        if (const char* blockedBy = CheckGlobalBeatPreconditions(ctx)) {
            logger::warn(
                "BeatSystem: force-dispatch refused ('{}' blocked by "
                "global precondition: {})",
                std::string{name}, blockedBy);
            return;
        }

        // One-element candidate list. Still collect sender candidates
        // for letter/visit so the LLM can render its selection prompt;
        // an empty pool doesn't fail the force-dispatch — OnStart's
        // failure detail message is what surfaces the problem.
        std::vector<IBeat*> candidates{ beat };
        std::vector<LetterComposer::SenderCandidate> letterSenderCandidates;
        std::vector<VisitComposer::SenderCandidate>  visitSenderCandidates;
        if (beat->Name() == "npc_letter") {
            letterSenderCandidates = LetterComposer::CollectSenderCandidates();
        } else if (beat->Name() == "npc_visit") {
            visitSenderCandidates = VisitComposer::CollectSenderCandidates();
        }

        const char* dirName =
            (direction == PhaseTracker::Direction::Raise) ? "raise" : "lower";
        logger::info(
            "BeatSystem: firing force-dispatch beat-select (beat='{}', "
            "direction={}, tension_delta={}, letter_sender_candidates={}, "
            "visit_sender_candidates={})",
            std::string{name}, dirName, tensionDelta,
            letterSenderCandidates.size(), visitSenderCandidates.size());

        // Empty finalizer — force-dispatch runs outside the normal
        // evaluation loop, so there's no g_inFlight guard to release.
        FireBeatSelectLLM(
            std::move(snapshot), std::move(rec), std::move(candidates),
            std::move(letterSenderCandidates),
            std::move(visitSenderCandidates),
            direction, tensionDelta, /*onFinalized=*/nullptr,
            "force-dispatch ");
    }
}
