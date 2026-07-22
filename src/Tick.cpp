#include <Tick.h>

#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <EngineUtils.h>
#include <EvaluationPipeline.h>
#include <EventHistoryWriter.h>
#include <logger.h>
#include <MainThread.h>
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
        // Driver-thread synchronization.
        std::mutex g_mutex;
        std::condition_variable g_cv;
        std::thread g_thread;
        bool g_running = false;
        bool g_shouldStop = false;

        // Plugin-thread accumulator state. Read and written only on the
        // plugin thread (AsyncDispatch's single worker), so no
        // synchronization needed beyond AsyncDispatch's queue mutex —
        // that mutex's acquire/release around the enqueue/dequeue pair
        // establishes the happens-before edge these plain-typed
        // globals depend on.
        //
        // Re-seeded on the first plugin-thread pass after each Start()
        // via `g_needsFirstTickInit` below, rather than seeded on the
        // main-thread caller of Start() — writing them from Start()
        // (main) and reading them from the plugin thread would need
        // additional synchronization we don't need to pay for.
        std::chrono::steady_clock::time_point g_lastSampleTime;
        double g_unpausedSecondsSinceLastTick = 0.0;
        int g_tickCount = 0;

        // Set true by Start(), cleared on the first PollOnPluginThread
        // pass afterward. Ensures the accumulator anchor is seeded on
        // the plugin thread rather than the main-thread caller of
        // Start(), so no cross-thread visibility guarantee is required
        // on the accumulator globals themselves. Access ordering is
        // safe via the chain: Start()'s mutex acquire → thread
        // creation → driver thread's AsyncDispatch enqueue → worker's
        // dequeue.
        bool g_needsFirstTickInit = true;

        // How often to sample the pause state. 500ms is a comfortable
        // tradeoff: brisk enough that resuming after a long pause kicks
        // the next tick within half a second, slow enough that the
        // enqueue overhead is negligible (~2 plugin-thread tasks per
        // second).
        constexpr std::chrono::milliseconds kPollInterval{500};

        // Runtime killswitch. When false, PollOnPluginThread returns
        // early after consuming the elapsed sample. Set from any
        // thread via SetEnabled (dashboard JS listener marshals from
        // the PrismaUI worker thread).
        std::atomic<bool> g_enabled{true};

        // Plugin-thread poll body — sample wall-clock elapsed since
        // the last poll, accumulate it only when the engine isn't
        // paused, and fire the tick when the unpaused accumulator
        // crosses the configured interval. All the accumulator state
        // lives in plugin-thread-only globals so no locking is
        // required here.
        //
        // The two pieces of work that genuinely need the main thread
        // (event-log polls and the PhaseTracker::Tick +
        // EvaluationPipeline::BeginEvaluation duo) marshal via
        // MainThread::FireAndForget — we don't need a return value
        // and don't want to block the plugin thread waiting for
        // them.
        void PollOnPluginThread(const PluginThread::Token& pt)
        {
            // First pass after each Start() — seed the accumulator
            // anchor on the plugin thread and skip the rest of the
            // body. The interval doesn't start counting until the
            // second pass.
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

            // Pause check. EngineUtils::IsGamePaused() without a token
            // is documented as "safe from any thread" — CommonLibSSE-NG
            // treats the UI singleton pointer + GameIsPaused bool read
            // as stable off-main, and BeatSystem's worker already
            // relies on the same guarantee. Doing this hop from the
            // plugin thread saves a MainThread::Run round trip on the
            // hot poll path.
            if (EngineUtils::IsGamePaused()) {
                return;
            }

            // Event-log polls now run on the plugin thread. Each
            // hops to main internally via MainThread::Run only for
            // its specific engine touches (Combat's player+bleedout
            // snapshot, Weather's sky read, Travel's location+hold+
            // party snapshot); EventHistoryWriter has no engine
            // touches at all and runs fully off main. This is the
            // audit-fix landing for findings 3 and 6.
            CombatEventLog::Poll(pt);
            WeatherEventLog::Poll(pt, elapsedSec);
            TravelEventLog::Poll(pt, elapsedSec);
            EventHistoryWriter::Poll(pt, elapsedSec);

            // Killswitch — when the dashboard's debug toggle is off,
            // we consume the elapsed sample above (so re-enabling
            // doesn't credit disabled time) but skip the Director
            // evaluation cadence below.
            if (!g_enabled.load(std::memory_order_acquire)) {
                return;
            }
            g_unpausedSecondsSinceLastTick += elapsedSec;

            const double intervalSec = static_cast<double>(std::max(1, Settings::Get().tickIntervalSeconds));
            if (g_unpausedSecondsSinceLastTick < intervalSec) {
                return;
            }

            // Time to fire. Reset the accumulator. (Subtract rather
            // than zero so any overshoot rolls into the next interval
            // — more accurate over long runs than discarding the
            // slack.)
            g_unpausedSecondsSinceLastTick -= intervalSec;
            ++g_tickCount;
            if (Settings::Get().debugMode) {
                logger::debug("Tick: firing #{}", g_tickCount);
            }

            // Both PhaseTracker::Tick and EvaluationPipeline::
            // BeginEvaluation now run on the plugin thread.
            // BeginEvaluation internally hops to main via
            // MainThread::Run for BuildSnapshot's engine reads (one
            // bundled hop per firing tick) and then does the prompt
            // build + LLM fire inline on the plugin thread. The Phase
            // D hand-off to BeatSystem::ConsiderBeat still marshals to
            // main from inside the LLM callback until the follow-on
            // BeatSystem migration lands.
            PhaseTracker::Tick(pt);
            EvaluationPipeline::BeginEvaluation(pt);
        }

        void DriverLoop()
        {
            // The driver thread is intentionally NOT marked as a
            // Plugin role thread. Its only responsibility is scheduling
            // — enqueue one tick body onto the plugin thread every
            // kPollInterval. All NarrativeEngine logic runs on the
            // plugin thread (AsyncDispatch's worker) via that enqueue
            // path.
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
        // Signal the plugin thread's first PollOnPluginThread pass to
        // re-seed the accumulator anchor. Writing this flag on the
        // main-thread Start() caller is safe: Start()'s mutex acquire
        // → std::thread creation → driver's AsyncDispatch enqueue →
        // worker's dequeue is a full happens-before chain.
        g_needsFirstTickInit = true;

        // Seed the runtime killswitch from Config before the worker
        // starts polling. tickEnabled is populated by Settings::Load's
        // cascade — plugin INI supplies the author default, MCM INI
        // overrides if the player has toggled it and rebooted.
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
