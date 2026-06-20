#include <Tick.h>

#include <ActionDispatcher.h>
#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <EvaluationPipeline.h>
#include <PhaseTracker.h>
#include <Settings.h>
#include <logger.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace NarrativeEngine::Tick
{
    namespace
    {
        // Driver-thread synchronization.
        std::mutex              g_mutex;
        std::condition_variable g_cv;
        std::thread             g_thread;
        bool                    g_running    = false;
        bool                    g_shouldStop = false;

        // Main-thread accumulator state. Read and written only on the main
        // thread (via SKSE's task interface), so no synchronization needed
        // beyond the marshaling. Reset in Start() before the worker thread
        // begins polling.
        std::chrono::steady_clock::time_point g_lastSampleTime;
        double                                g_unpausedSecondsSinceLastTick = 0.0;
        int                                   g_tickCount                    = 0;

        // How often to sample the pause state. 500ms is a comfortable
        // tradeoff: brisk enough that resuming after a long pause kicks the
        // next tick within half a second, slow enough that the marshaling
        // overhead is negligible (~2 main-thread tasks per second).
        constexpr std::chrono::milliseconds kPollInterval{500};

        // Main-thread poll: sample wall-clock elapsed since the last poll,
        // accumulate it only when the engine isn't paused, and fire the
        // tick when the unpaused accumulator crosses the configured
        // interval. All state lives in main-thread-only globals so no
        // locking is required here.
        void PollOnMainThread()
        {
            const auto now = std::chrono::steady_clock::now();
            const double elapsedSec =
                std::chrono::duration<double>(now - g_lastSampleTime).count();
            g_lastSampleTime = now;

            // The same pause predicate the rest of the engine uses for
            // "real time should not advance right now" (menus, console,
            // dialogue). Don't credit the accumulator with paused time.
            const bool paused = []() {
                auto* ui = RE::UI::GetSingleton();
                return ui && ui->GameIsPaused();
            }();
            if (paused) {
                return;
            }
            g_unpausedSecondsSinceLastTick += elapsedSec;

            // Drive CombatEventLog's main-thread poll: detects player
            // combat-state flips (combat_start / combat_end) and bleedout
            // recoveries (regain_footing). Cheap — bool compare plus a
            // small map walk.
            CombatEventLog::Poll();

            // Drive ActionDispatcher's main-thread tick: currently just
            // the stale-lock check (auto-clear an in-flight action whose
            // completion ModEvent never arrived). Cheap — bool compare
            // plus a time delta.
            ActionDispatcher::OnTick();

            const double intervalSec =
                static_cast<double>(std::max(1, Settings::Get().tickIntervalSeconds));
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
    }

    void Start()
    {
        std::unique_lock lock(g_mutex);
        if (g_running) {
            return;
        }
        // Reset the accumulator so a fresh interval starts from "now."
        // These are main-thread-only globals; Start() is called from the
        // main thread (kPostLoadGame / kNewGame), so writing here is safe.
        g_lastSampleTime               = std::chrono::steady_clock::now();
        g_unpausedSecondsSinceLastTick = 0.0;
        g_tickCount                    = 0;

        g_shouldStop = false;
        g_running    = true;
        g_thread     = std::thread(DriverLoop);
        logger::info("Tick: driver thread started (interval={}s, paused-aware)",
                     Settings::Get().tickIntervalSeconds);
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
}
