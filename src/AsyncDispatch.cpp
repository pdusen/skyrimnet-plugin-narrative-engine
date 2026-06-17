#include <AsyncDispatch.h>

#include <logger.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace NarrativeEngine::AsyncDispatch
{
    namespace
    {
        std::mutex                        g_mutex;
        std::condition_variable           g_cv;
        std::deque<std::function<void()>> g_queue;
        std::thread                       g_worker;
        bool                              g_running    = false;
        bool                              g_shouldStop = false;

        void WorkerLoop()
        {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(g_mutex);
                    g_cv.wait(lock, [] { return g_shouldStop || !g_queue.empty(); });
                    if (g_shouldStop && g_queue.empty()) {
                        return;
                    }
                    task = std::move(g_queue.front());
                    g_queue.pop_front();
                }
                // Swallow exceptions so a single bad task can't kill the worker.
                try {
                    task();
                } catch (const std::exception& e) {
                    logger::error("AsyncDispatch worker: task threw exception: {}", e.what());
                } catch (...) {
                    logger::error("AsyncDispatch worker: task threw unknown exception");
                }
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
        g_worker     = std::thread(WorkerLoop);
        logger::info("AsyncDispatch: worker thread started");
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
        if (g_worker.joinable()) {
            g_worker.join();
        }
        std::unique_lock lock(g_mutex);
        g_running = false;
        logger::info("AsyncDispatch: worker thread stopped");
    }

    void EnqueueWork(std::function<void()> work)
    {
        if (!work) {
            return;
        }
        {
            std::unique_lock lock(g_mutex);
            if (!g_running) {
                logger::warn("AsyncDispatch::EnqueueWork: worker not running; dropping task");
                return;
            }
            g_queue.push_back(std::move(work));
        }
        g_cv.notify_one();
    }

    void MarshalToMainThread(std::function<void()> work)
    {
        if (!work) {
            return;
        }
        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::error("AsyncDispatch::MarshalToMainThread: SKSE task interface unavailable; dropping task");
            return;
        }
        taskInterface->AddTask(std::move(work));
    }
}
