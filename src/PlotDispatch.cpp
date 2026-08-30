#include <PlotDispatch.h>

#include <logger.h>
#include <ThreadRole.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace NarrativeEngine::PlotDispatch
{
    namespace
    {
        // One entry per enqueued job. The handle is null for
        // non-cancellable work, which keeps the queue a single type
        // rather than two parallel ones.
        struct Job
        {
            std::function<void(const PlotThread::Token&, const CancellationHandle&)> work;
            CancellationHandle handle;
        };

        std::mutex g_mutex;
        std::condition_variable g_cv;
        std::deque<Job> g_queue;
        std::thread g_worker;
        bool g_running = false;
        bool g_shouldStop = false;

        // Handles for work that is queued OR currently running. A
        // running job's handle stays here until it returns, which is
        // what lets CancelAll reach the job that is actually doing the
        // damage rather than only the ones waiting behind it.
        std::vector<CancellationHandle> g_outstanding;

        void ForgetLocked(const CancellationHandle& handle)
        {
            if (!handle) {
                return;
            }
            const auto it = std::find(g_outstanding.begin(), g_outstanding.end(), handle);
            if (it != g_outstanding.end()) {
                g_outstanding.erase(it);
            }
        }

        void WorkerLoop()
        {
            ScopedThreadRole roleGuard(ThreadRole::Plugin);

            for (;;) {
                Job job;
                {
                    std::unique_lock lock(g_mutex);
                    g_cv.wait(lock, [] { return g_shouldStop || !g_queue.empty(); });
                    if (g_shouldStop && g_queue.empty()) {
                        return;
                    }
                    job = std::move(g_queue.front());
                    g_queue.pop_front();
                }

                // A job cancelled while it sat in the queue does nothing
                // at all. This is the cheap half of cancellation and the
                // one that matters after a load, where every queued tick
                // belongs to a world that has just been replaced.
                if (job.handle && job.handle->IsCancelled()) {
                    std::unique_lock lock(g_mutex);
                    ForgetLocked(job.handle);
                    continue;
                }

                try {
                    PlotThread::detail::JobDispatcher::Invoke(
                        [&job](const PlotThread::Token& pt) { job.work(pt, job.handle); });
                } catch (const std::exception& e) {
                    logger::error("PlotDispatch worker: task threw exception: {}", e.what());
                } catch (...) {
                    logger::error("PlotDispatch worker: task threw unknown exception");
                }

                // Released on the EXIT path rather than the success
                // path. A job that threw, or that returned early because
                // it was cancelled, must still stop counting against the
                // outstanding cap — otherwise one hung LLM call
                // permanently suppresses every later tick.
                {
                    std::unique_lock lock(g_mutex);
                    ForgetLocked(job.handle);
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
        logger::info("PlotDispatch: worker thread started");
    }

    void Stop()
    {
        // Cancel BEFORE signalling the stop, so a job already blocked in
        // an LLM call sees the flag at its next checkpoint rather than
        // running to completion while shutdown waits on the join.
        CancelAll();
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
        g_outstanding.clear();
        logger::info("PlotDispatch: worker thread stopped");
    }

    void EnqueueWork(std::function<void(const PlotThread::Token&)> work)
    {
        if (!work) {
            return;
        }
        {
            std::unique_lock lock(g_mutex);
            if (!g_running) {
                logger::warn("PlotDispatch::EnqueueWork: worker not running; dropping task");
                return;
            }
            g_queue.push_back(Job{
                [fn = std::move(work)](const PlotThread::Token& pt, const CancellationHandle&) { fn(pt); }, nullptr});
        }
        g_cv.notify_one();
    }

    CancellationHandle EnqueueCancellableWork(
        std::function<void(const PlotThread::Token&, const CancellationHandle&)> work)
    {
        if (!work) {
            return nullptr;
        }
        auto handle = std::make_shared<CancellationToken>();
        {
            std::unique_lock lock(g_mutex);
            if (!g_running) {
                logger::warn("PlotDispatch::EnqueueCancellableWork: worker not running; dropping task");
                return nullptr;
            }
            g_outstanding.push_back(handle);
            g_queue.push_back(Job{std::move(work), handle});
        }
        g_cv.notify_one();
        return handle;
    }

    void CancelAll()
    {
        std::vector<CancellationHandle> snapshot;
        {
            std::unique_lock lock(g_mutex);
            snapshot = g_outstanding;
        }
        // Flagged outside the lock: a running job may be polling its own
        // handle from the worker thread, and there is no reason to make
        // the two contend.
        for (const auto& handle : snapshot) {
            if (handle) {
                handle->Cancel();
            }
        }
        if (!snapshot.empty()) {
            logger::info("PlotDispatch: cancelled {} outstanding job(s)", snapshot.size());
        }
    }

    std::size_t OutstandingCount()
    {
        std::unique_lock lock(g_mutex);
        return g_outstanding.size();
    }
} // namespace NarrativeEngine::PlotDispatch
