#include <GossipDispatch.h>

#include <DispatcherContract.h>
#include <GossipThread.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <utility>

// GossipDispatch is one of four plugin worker queues built to the same shape,
// so the queue, the worker loop and the start/stop handshake are checked
// against the shared contract in testsupport/DispatcherContract.h rather than
// against a copy of it.
//
// Unlike the other three it also carries cancellation, and that half is checked
// here because it belongs to this dispatcher alone. It is worth checking
// carefully. A gossip carrier-step writes memories into SkyrimNet's vector
// database, which lives outside our co-save and is NOT rolled back by loading
// an earlier save. A job that keeps running past a load therefore keeps writing
// memories about a rumour the loaded world has no record of — so cancellation
// has to stop the writes rather than merely disown them afterwards, and a job
// that is cancelled while queued must never start at all.
//
// These are the only concurrent tests in the suite besides the contract itself,
// so they are written to fail rather than hang: every wait is a bounded future
// whose arrival is asserted.

namespace
{
    namespace GossipDispatch = NarrativeEngine::GossipDispatch;
    namespace GossipThread = NarrativeEngine::GossipThread;
    using GossipDispatch::CancellationHandle;

    // Long enough that a loaded machine still makes it, short enough that a
    // genuine deadlock fails the run instead of wedging it.
    constexpr auto kTimeout = std::chrono::seconds{5};

    template <class T> bool Arrived(std::future<T>& f)
    {
        return f.wait_for(kTimeout) == std::future_status::ready;
    }

    // Waits until every job enqueued before this call has been released. The
    // queue is FIFO with a single worker, so a marker job reaching the front
    // proves the ones ahead of it are done — which is what makes an
    // OutstandingCount assertion meaningful without stopping the worker first.
    // Stop() clears the outstanding list outright, so checking after a Stop
    // would pass no matter what the worker did.
    void DrainQueue()
    {
        std::promise<void> marker;
        auto reached = marker.get_future();
        GossipDispatch::EnqueueWork([&](const GossipThread::Token&) { marker.set_value(); });
        REQUIRE(Arrived(reached));
    }

    // Brackets a running worker. The dispatcher's state is process-global, so
    // every case claims and releases it rather than assuming what the last one
    // left behind.
    struct Running
    {
        Running()
        {
            GossipDispatch::Start();
        }
        ~Running()
        {
            GossipDispatch::Stop();
        }

        Running(const Running&) = delete;
        Running& operator=(const Running&) = delete;
    };

    // Occupies the single worker so that the next job enqueued is still sitting
    // in the queue when the test looks at it.
    //
    // Releasing from a destructor rather than at the end of the case is the
    // whole point. A failing REQUIRE throws, and a blocker left unreleased
    // would hold the worker forever, so the Stop in ~Running would never
    // return -- turning one failed assertion into a suite that hangs and
    // reports nothing at all. The wait inside the job is bounded for the same
    // reason.
    struct WorkerBlocker
    {
        WorkerBlocker()
        {
            auto* self = this;
            GossipDispatch::EnqueueWork([self](const GossipThread::Token&) {
                self->entered.set_value();
                (void)self->releaseFuture.wait_for(kTimeout);
            });
            REQUIRE(Arrived(running));
        }

        ~WorkerBlocker()
        {
            if (!released_) {
                release.set_value();
            }
        }

        void Release()
        {
            release.set_value();
            released_ = true;
        }

        WorkerBlocker(const WorkerBlocker&) = delete;
        WorkerBlocker& operator=(const WorkerBlocker&) = delete;

        std::promise<void> entered;
        std::future<void> running = entered.get_future();
        std::promise<void> release;
        std::shared_future<void> releaseFuture = release.get_future().share();

    private:
        bool released_ = false;
    };
} // namespace

TEST_CASE("GossipDispatch worker contract", "[GossipDispatch][engine]")
{
    NarrativeEngine::Testing::CheckDispatcherContract({
        [] { GossipDispatch::Start(); },
        [] { GossipDispatch::Stop(); },
        [](std::function<void()> work) {
            GossipDispatch::EnqueueWork([w = std::move(work)](const GossipThread::Token&) { w(); });
        },
        [] { GossipDispatch::EnqueueWork(nullptr); },
    });
}

TEST_CASE("GossipDispatch::EnqueueCancellableWork", "[GossipDispatch][engine]")
{
    // Happy path, re-run per leaf: a running worker and one cancellable job.
    const Running worker;

    SECTION("when the job runs to completion")
    {
        std::promise<bool> done;
        auto arrived = done.get_future();
        auto handle = GossipDispatch::EnqueueCancellableWork(
            [&](const GossipThread::Token&, const CancellationHandle& h) { done.set_value(h->IsCancelled()); });

        SECTION("should hand the job its own handle")
        {
            // The job polls this at every operation boundary, so it has to be
            // the same token CancelAll reaches — not a copy of its value.
            REQUIRE(handle != nullptr);
            REQUIRE(Arrived(arrived));
            REQUIRE_FALSE(arrived.get());
        }

        SECTION("should stop counting it once it returns")
        {
            REQUIRE(Arrived(arrived));
            DrainQueue();
            REQUIRE(GossipDispatch::OutstandingCount() == 0);
        }
    }

    SECTION("when the work is empty")
    {
        SECTION("should refuse it rather than register a handle")
        {
            REQUIRE(GossipDispatch::EnqueueCancellableWork(nullptr) == nullptr);
        }
    }

    SECTION("when the worker is not running")
    {
        GossipDispatch::Stop();

        SECTION("should refuse the job")
        {
            // No handle means the caller cannot be left holding one for a job
            // that will never run and never be released.
            REQUIRE(GossipDispatch::EnqueueCancellableWork([](const GossipThread::Token&, const CancellationHandle&) {})
                    == nullptr);
            REQUIRE(GossipDispatch::OutstandingCount() == 0);
        }
    }
}

TEST_CASE("GossipDispatch::CancelAll", "[GossipDispatch][engine]")
{
    const Running worker;

    SECTION("when a job is cancelled while it waits in the queue")
    {
        // A blocker holds the worker so the job behind it is still queued when
        // the cancellation lands. This is the case that matters after a load:
        // every queued tick belongs to a world that has just been replaced.
        WorkerBlocker blocker;

        std::atomic<bool> ran{false};
        (void)GossipDispatch::EnqueueCancellableWork(
            [&](const GossipThread::Token&, const CancellationHandle&) { ran = true; });

        GossipDispatch::CancelAll();
        blocker.Release();
        DrainQueue();

        SECTION("should never run it at all")
        {
            // Not "runs and discards its results". The job's first act is a
            // write into a database our co-save does not own, so the only safe
            // outcome is that it never begins.
            REQUIRE_FALSE(ran.load());
        }

        SECTION("should stop counting it")
        {
            REQUIRE(GossipDispatch::OutstandingCount() == 0);
        }
    }

    SECTION("when a job is cancelled while it is running")
    {
        std::promise<void> entered;
        auto running = entered.get_future();
        std::promise<bool> observed;
        auto sawIt = observed.get_future();
        (void)GossipDispatch::EnqueueCancellableWork([&](const GossipThread::Token&, const CancellationHandle& h) {
            entered.set_value();
            // Poll the way a real carrier-step does, at what would be an
            // operation boundary between two memory writes.
            for (int i = 0; i < 500 && !h->IsCancelled(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            observed.set_value(h->IsCancelled());
        });
        REQUIRE(Arrived(running));

        SECTION("should reach the job that is actually doing the damage")
        {
            // A running job's handle stays registered until it returns, which
            // is what lets CancelAll reach it rather than only the jobs waiting
            // behind it.
            GossipDispatch::CancelAll();
            REQUIRE(Arrived(sawIt));
            REQUIRE(sawIt.get());
        }
    }

    SECTION("when nothing is outstanding")
    {
        SECTION("should do nothing")
        {
            GossipDispatch::CancelAll();
            REQUIRE(GossipDispatch::OutstandingCount() == 0);
        }
    }

    SECTION("when work was enqueued without a handle")
    {
        SECTION("should still run it")
        {
            // Uncancellable work is for jobs short enough that abandoning them
            // would cost more than finishing them, so a blanket CancelAll must
            // not reach them.
            std::promise<void> done;
            auto arrived = done.get_future();
            GossipDispatch::CancelAll();
            GossipDispatch::EnqueueWork([&](const GossipThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
        }
    }
}

TEST_CASE("GossipDispatch::OutstandingCount", "[GossipDispatch][engine]")
{
    const Running worker;

    SECTION("when jobs are queued behind a blocker")
    {
        WorkerBlocker blocker;

        SECTION("should count them")
        {
            // The scheduler caps the backlog on this number, so a count that
            // stayed at zero would let gossip work pile up without limit.
            (void)GossipDispatch::EnqueueCancellableWork([](const GossipThread::Token&, const CancellationHandle&) {});
            (void)GossipDispatch::EnqueueCancellableWork([](const GossipThread::Token&, const CancellationHandle&) {});
            REQUIRE(GossipDispatch::OutstandingCount() == 2);
        }

        SECTION("should not count work with no handle")
        {
            // Uncancellable work is deliberately invisible here: it cannot be
            // abandoned, so counting it against a cap nothing can relieve would
            // stall the scheduler for good.
            GossipDispatch::EnqueueWork([](const GossipThread::Token&) {});
            REQUIRE(GossipDispatch::OutstandingCount() == 0);
        }
    }

    SECTION("when a job throws")
    {
        SECTION("should still stop counting it")
        {
            // Released on the exit path rather than the success path: one hung
            // or throwing job that kept its slot would suppress every later
            // tick for the rest of the session.
            std::promise<void> threw;
            auto arrived = threw.get_future();
            (void)GossipDispatch::EnqueueCancellableWork([&](const GossipThread::Token&, const CancellationHandle&) {
                threw.set_value();
                throw std::runtime_error("boom");
            });
            REQUIRE(Arrived(arrived));
            DrainQueue();
            REQUIRE(GossipDispatch::OutstandingCount() == 0);
        }
    }
}
