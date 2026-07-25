#include <BeatWorkDispatch.h>

#include <logger.h>
#include <ThreadRole.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace NarrativeEngine::BeatWorkDispatch
{
    namespace
    {
        std::mutex g_mutex;
        std::condition_variable g_cv;
        std::deque<std::function<void(const PluginThread::Token&)>> g_queue;
        std::thread g_worker;
        bool g_running = false;
        bool g_shouldStop = false;

        void WorkerLoop()
        {
            ScopedThreadRole roleGuard(ThreadRole::Plugin);

            for (;;) {
                std::function<void(const PluginThread::Token&)> task;
                {
                    std::unique_lock lock(g_mutex);
                    g_cv.wait(lock, [] { return g_shouldStop || !g_queue.empty(); });
                    if (g_shouldStop && g_queue.empty()) {
                        return;
                    }
                    task = std::move(g_queue.front());
                    g_queue.pop_front();
                }
                // Swallow exceptions so a bad task can't kill the worker.
                try {
                    PluginThread::detail::JobDispatcher::Invoke(task);
                } catch (const std::exception& e) {
                    logger::error("BeatWorkDispatch worker: task threw exception: {}", e.what());
                } catch (...) {
                    logger::error("BeatWorkDispatch worker: task threw unknown exception");
                }
            }
        }
    } // namespace

    void Start()
    {
        std::unique_lock lock(g_mutex);
        if (g_running) {
            return;
        }
        g_shouldStop = false;
        g_running = true;
        g_worker = std::thread(WorkerLoop);
        logger::info("BeatWorkDispatch: worker thread started");
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
        logger::info("BeatWorkDispatch: worker thread stopped");
    }

    void EnqueueWork(std::function<void(const PluginThread::Token&)> work)
    {
        if (!work) {
            return;
        }
        {
            std::unique_lock lock(g_mutex);
            if (!g_running) {
                logger::warn("BeatWorkDispatch::EnqueueWork: worker not running; dropping task");
                return;
            }
            g_queue.push_back(std::move(work));
        }
        g_cv.notify_one();
    }
} // namespace NarrativeEngine::BeatWorkDispatch
