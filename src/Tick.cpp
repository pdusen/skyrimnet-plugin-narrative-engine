#include <Tick.h>

#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <EvaluationPipeline.h>
#include <EventHistoryWriter.h>
#include <logger.h>
#include <PhaseTracker.h>
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

        // Main-thread accumulator state. Read and written only on the main
        // thread (via SKSE's task interface), so no synchronization needed
        // beyond the marshaling. Reset in Start() before the worker thread
        // begins polling.
        std::chrono::steady_clock::time_point g_lastSampleTime;
        double g_unpausedSecondsSinceLastTick = 0.0;
        int g_tickCount = 0;

        // How often to sample the pause state. 500ms is a comfortable
        // tradeoff: brisk enough that resuming after a long pause kicks the
        // next tick within half a second, slow enough that the marshaling
        // overhead is negligible (~2 main-thread tasks per second).
        constexpr std::chrono::milliseconds kPollInterval{500};

        // Runtime killswitch. When false, PollOnMainThread returns
        // immediately. Set from any thread via SetEnabled (dashboard
        // JS listener marshals from the PrismaUI worker thread).
        std::atomic<bool> g_enabled{true};

        // Main-thread poll: sample wall-clock elapsed since the last poll,
        // accumulate it only when the engine isn't paused, and fire the
        // tick when the unpaused accumulator crosses the configured
        // interval. All state lives in main-thread-only globals so no
        // locking is required here.
        void PollOnMainThread()
        {
            const auto now = std::chrono::steady_clock::now();
            const double elapsedSec = std::chrono::duration<double>(now - g_lastSampleTime).count();
            g_lastSampleTime = now;

            // The same pause predicate the rest of the engine uses for
            // "real time should not advance right now" (menus, console,
            // dialogue). Every poll below depends on real time
            // advancing, so bail early when paused. Don't credit the
            // accumulator with paused time either.
            const bool paused = []() {
                auto* ui = RE::UI::GetSingleton();
                return ui && ui->GameIsPaused();
            }();
            if (paused) {
                return;
            }

            // Housekeeping poll — CombatEventLog watches player
            // combat-state edges so the event log stays truthful even
            // when the Director's killswitch is engaged. Beat
            // completion detection lives inside each beat's Tick under
            // BeatSystem's master poll, not here.
            CombatEventLog::Poll();
            // WeatherEventLog samples Sky state on a pause-aware cadence
            // (default 30s of unpaused play between samples). We only
            // reach this line when the game is unpaused, so `elapsedSec`
            // — the wall-clock delta since the previous PollOnMainThread
            // cycle, capped at ~500ms during normal play — is genuine
            // unpaused elapsed time. WeatherEventLog accumulates it and
            // fires the sample once the accumulator crosses the interval.
            WeatherEventLog::Poll(elapsedSec);
            // TravelEventLog samples location + hold on every unpaused
            // Tick — no throttle, since cell-load transitions can be
            // transient and we want to catch them all. `elapsedSec` is
            // passed for signature symmetry with the Tick-driven-
            // accumulator pattern even though Travel has no cadenced
            // work today.
            TravelEventLog::Poll(elapsedSec);
            // Testing aid: rotating history log at
            // Data/../SKSE/NarrativeEngine_EventHistory.log. Same
            // Tick-driven accumulator pattern — the writer throttles
            // internally to iEventHistoryFlushIntervalSeconds.
            EventHistoryWriter::Poll(elapsedSec);

            // Killswitch — when the dashboard's debug toggle is off, we
            // consume the elapsed sample above (so re-enabling doesn't
            // credit disabled time) but skip the Director evaluation
            // cadence below.
            if (!g_enabled.load(std::memory_order_acquire)) {
                (void)elapsedSec;
                return;
            }
            g_unpausedSecondsSinceLastTick += elapsedSec;

            const double intervalSec = static_cast<double>(std::max(1, Settings::Get().tickIntervalSeconds));
            if (g_unpausedSecondsSinceLastTick < intervalSec) {
                return;
            }

            // Time to fire. Reset the accumulator. (Subtract rather than
            // zero so any overshoot rolls into the next interval — more
            // accurate over long runs than discarding the slack.)
            g_unpausedSecondsSinceLastTick -= intervalSec;
            ++g_tickCount;
            if (Settings::Get().debugMode) {
                logger::debug("Tick: firing #{}", g_tickCount);
            }

            // Same two main-thread calls the previous design made; we're
            // already on the main thread so no further marshaling needed.
            PhaseTracker::Tick();
            EvaluationPipeline::BeginEvaluation();
        }

        void DriverLoop()
        {
            // The driver thread is purely a polling clock — every kPollInterval
            // it marshals a sample onto the main thread, which does the real
            // pause-check + accumulator + tick-fire work.
            while (true) {
                {
                    std::unique_lock lock(g_mutex);
                    g_cv.wait_for(lock, kPollInterval, [] { return g_shouldStop; });
                    if (g_shouldStop) {
                        return;
                    }
                }
                AsyncDispatch::MarshalToMainThread([] { PollOnMainThread(); });
            }
        }
    } // namespace

    void Start()
    {
        std::unique_lock lock(g_mutex);
        if (g_running) {
            return;
        }
        // Reset the accumulator so a fresh interval starts from "now."
        // These are main-thread-only globals; Start() is called from the
        // main thread (kPostLoadGame / kNewGame), so writing here is safe.
        g_lastSampleTime = std::chrono::steady_clock::now();
        g_unpausedSecondsSinceLastTick = 0.0;
        g_tickCount = 0;

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
