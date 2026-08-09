#include <Tick.h>

#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <EngineUtils.h>
#include <EvalDispatch.h>
#include <EvaluationPipeline.h>
#include <EventHistoryWriter.h>
#include <FineRoads.h>
#include <GossipHarvest.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <logger.h>
#include <PhaseTracker.h>
#include <PluginThread.h>
#include <Settings.h>
#include <TravelEventLog.h>
#include <WeatherEventLog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace NarrativeEngine::Tick
{
    namespace
    {
        std::mutex g_mutex;
        std::condition_variable g_cv;
        std::thread g_thread;
        bool g_running = false;
        bool g_shouldStop = false;

        // Plugin-thread-only; AsyncDispatch's queue mutex provides the
        // happens-before edge across successive tick jobs.
        std::chrono::steady_clock::time_point g_lastSampleTime;
        double g_unpausedSecondsSinceLastTick = 0.0;
        int g_tickCount = 0;

        // First plugin-thread pass after Start() seeds g_lastSampleTime
        // on the plugin thread so we don't need cross-thread visibility
        // on the accumulator globals.
        bool g_needsFirstTickInit = true;

        constexpr std::chrono::milliseconds kPollInterval{500};

        std::atomic<bool> g_enabled{true};

        // Accumulate wall-clock elapsed while unpaused; fire the tick
        // when the accumulator crosses tickIntervalSeconds.
        void PollOnPluginThread(const PluginThread::Token& pt)
        {
            if (g_needsFirstTickInit) {
                g_lastSampleTime = std::chrono::steady_clock::now();
                g_unpausedSecondsSinceLastTick = 0.0;
                g_tickCount = 0;
                g_needsFirstTickInit = false;
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const double elapsedSec = std::chrono::duration<double>(now - g_lastSampleTime).count();
            g_lastSampleTime = now;

            if (EngineUtils::IsGamePaused()) {
                return;
            }

            CombatEventLog::Poll(pt);
            WeatherEventLog::Poll(pt, elapsedSec);
            TravelEventLog::Poll(pt, elapsedSec);
            EventHistoryWriter::Poll(pt, elapsedSec);
            FineRoads::Poll(pt, elapsedSec);
            // A load or a revert stages its state rather than writing
            // live, so adopt before any gossip work reads it. Harvest
            // runs first, so this has to sit ahead of it — a sweep
            // against the outgoing world would claim memories the
            // incoming one has no rumors for.
            //
            // No-op when nothing is staged. Milestone 3 step 5 folds
            // this into the tick job.
            GossipSim::AdoptPendingState();
            GossipHarvest::Poll(pt, elapsedSec);
            GossipSim::Poll(pt, elapsedSec);
            GossipLog::Poll(pt, elapsedSec);

            // Consume the elapsed sample above so a subsequent
            // re-enable doesn't credit disabled time.
            if (!g_enabled.load(std::memory_order_acquire)) {
                return;
            }
            g_unpausedSecondsSinceLastTick += elapsedSec;

            const double intervalSec = static_cast<double>(std::max(1, Settings::Get().tickIntervalSeconds));
            if (g_unpausedSecondsSinceLastTick < intervalSec) {
                return;
            }

            // Skip if the previous tick's LLM is still running so we
            // don't queue a catch-up burst.
            if (EvaluationPipeline::IsEvaluationInFlight()) {
                if (Settings::Get().debugMode) {
                    logger::debug("Tick: skipping fire — previous evaluation still in flight");
                }
                return;
            }

            // Subtract rather than zero so any overshoot rolls into
            // the next interval.
            g_unpausedSecondsSinceLastTick -= intervalSec;
            ++g_tickCount;
            if (Settings::Get().debugMode) {
                logger::debug("Tick: firing #{}", g_tickCount);
            }

            // BeginEvaluation blocks on the LLM for seconds; run it on
            // EvalDispatch so the cadenced poll here keeps ticking.
            PhaseTracker::Tick(pt);
            EvalDispatch::EnqueueWork(
                [](const PluginThread::Token& evalPt) { EvaluationPipeline::BeginEvaluation(evalPt); });
        }

        // Driver thread is NOT a plugin-role thread — schedule-only.
        void DriverLoop()
        {
            while (true) {
                {
                    std::unique_lock lock(g_mutex);
                    g_cv.wait_for(lock, kPollInterval, [] { return g_shouldStop; });
                    if (g_shouldStop) {
                        return;
                    }
                }
                AsyncDispatch::EnqueueWork([](const PluginThread::Token& pt) { PollOnPluginThread(pt); });
            }
        }
    } // namespace

    void Start()
    {
        std::unique_lock lock(g_mutex);
        if (g_running) {
            return;
        }
        g_needsFirstTickInit = true;
        g_enabled.store(Settings::Get().tickEnabled, std::memory_order_release);

        g_shouldStop = false;
        g_running = true;
        g_thread = std::thread(DriverLoop);
        logger::info("Tick: driver thread started (interval={}s, paused-aware, enabled={})",
                     Settings::Get().tickIntervalSeconds,
                     Settings::Get().tickEnabled);
    }

    void SetEnabled(bool enabled)
    {
        const bool prev = g_enabled.exchange(enabled, std::memory_order_acq_rel);
        if (prev != enabled) {
            logger::info("Tick: killswitch -> {}", enabled ? "enabled" : "disabled");
        }
    }

    bool IsEnabled()
    {
        return g_enabled.load(std::memory_order_acquire);
    }

    void Stop()
    {
        {
            std::unique_lock lock(g_mutex);
            if (!g_running) {
                return;
            }
            g_shouldStop = true;
        }
        g_cv.notify_all();
        if (g_thread.joinable()) {
            g_thread.join();
        }
        std::unique_lock lock(g_mutex);
        g_running = false;
        logger::info("Tick: driver thread stopped");
    }
} // namespace NarrativeEngine::Tick
