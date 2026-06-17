#include <Tick.h>

#include <AsyncDispatch.h>
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
        std::mutex              g_mutex;
        std::condition_variable g_cv;
        std::thread             g_thread;
        bool                    g_running    = false;
        bool                    g_shouldStop = false;

        void DriverLoop()
        {
            int tickCount = 0;

            while (true) {
                // Re-read the interval every iteration so settings reloads
                // (someday) take effect immediately. Clamped to >= 1s.
                const int intervalSec = std::max(1, Settings::Get().tickIntervalSeconds);

                {
                    std::unique_lock lock(g_mutex);
                    g_cv.wait_for(lock, std::chrono::seconds(intervalSec),
                                  [] { return g_shouldStop; });
                    if (g_shouldStop) {
                        return;
                    }
                }

                ++tickCount;
                if (Settings::Get().debugMode) {
                    logger::debug("Tick: firing #{}", tickCount);
                }

                // Single main-thread hop for the whole tick:
                //   (a) advance the phase tracker's real-time clock
                //       (no-ops while the game is paused).
                //   (b) kick off the evaluation pipeline. BeginEvaluation
                //       is no-op-safe — it CAS-guards against overlap.
                AsyncDispatch::MarshalToMainThread([] {
                    PhaseTracker::Tick();
                    EvaluationPipeline::BeginEvaluation();
                });
            }
        }
    }

    void Start()
    {
        std::unique_lock lock(g_mutex);
        if (g_running) {
            return;
        }
        g_shouldStop = false;
        g_running    = true;
        g_thread     = std::thread(DriverLoop);
        logger::info("Tick: driver thread started (interval={}s)", Settings::Get().tickIntervalSeconds);
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
