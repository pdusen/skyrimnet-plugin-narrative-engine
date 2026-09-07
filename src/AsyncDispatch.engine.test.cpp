#include <AsyncDispatch.h>

#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

// Tests for the cadenced short-work plugin worker.
//
// Engine-free in substance — a mutex, a queue and a thread — but it includes
// <logger.h>, which is `SKSE::log`, so it cannot build in the core target and
// lands in the mocked one. It needed no engine stand-ins at all: the only
// non-standard thing it touches is spdlog, which the harness links for real.
//
// This is the only module so far whose behaviour is concurrent, so the tests
// are written to fail rather than hang. Every wait is bounded and asserted on,
// and nothing sleeps for a fixed period hoping the worker got there.
//
// The module's state is process-global and there is exactly one worker, so each
// case brackets itself with the RAII guard below rather than assuming what an
// earlier case left behind.

namespace
{
    using NarrativeEngine::CurrentThreadRole;
    using NarrativeEngine::ThreadRole;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace PluginThread = NarrativeEngine::PluginThread;

    // Long enough that a loaded machine still makes it, short enough that a
    // genuine deadlock fails the run instead of wedging it.
    constexpr auto kTimeout = std::chrono::seconds{5};

    struct RunningDispatcher
    {
        RunningDispatcher()
        {
            AsyncDispatch::Start();
        }
        ~RunningDispatcher()
        {
            AsyncDispatch::Stop();
        }
    };

    // Wait for a future, failing the test rather than blocking forever.
    template <class T> bool Arrived(std::future<T>& f)
    {
        return f.wait_for(kTimeout) == std::future_status::ready;
    }
} // namespace

TEST_CASE("AsyncDispatch::EnqueueWork", "[AsyncDispatch][engine]")
{
    SECTION("when the worker is running")
    {
        RunningDispatcher worker;

        SECTION("should run the work")
        {
            std::promise<int> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(7); });
            REQUIRE(Arrived(arrived));
            REQUIRE(arrived.get() == 7);
        }

        SECTION("should run it as the plugin thread")
        {
            // The role the whole token discipline is a compile-time mirror of.
            // Work arriving here from a SkyrimNet callback is Foreign until the
            // worker loop claims Plugin on its behalf.
            std::promise<ThreadRole> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(CurrentThreadRole()); });
            REQUIRE(Arrived(arrived));
            REQUIRE(arrived.get() == ThreadRole::Plugin);
        }

        SECTION("should run it somewhere other than the caller")
        {
            // EnqueueWork is the one plugin entry that takes no token, so its
            // callers are foreign threads that must not be blocked by the work.
            std::promise<std::thread::id> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(std::this_thread::get_id()); });
            REQUIRE(Arrived(arrived));
            REQUIRE(arrived.get() != std::this_thread::get_id());
        }

        SECTION("should run several pieces of work in the order they arrived")
        {
            std::atomic<int> order{0};
            std::promise<void> done;
            auto arrived = done.get_future();
            int first = 0;
            int second = 0;
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { first = ++order; });
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { second = ++order; });
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
            REQUIRE(first == 1);
            REQUIRE(second == 2);
        }
    }

    SECTION("when the work is null")
    {
        RunningDispatcher worker;

        SECTION("should ignore it and keep serving later work")
        {
            std::promise<void> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork(nullptr);
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
        }
    }

    SECTION("when the worker is not running")
    {
        AsyncDispatch::Stop();

        SECTION("should drop the work")
        {
            // Dropped rather than queued for a worker that may never start:
            // holding it would leak the closure and everything it captured for
            // the rest of the session.
            std::atomic<bool> ran{false};
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { ran = true; });
            AsyncDispatch::Start();
            std::promise<void> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
            AsyncDispatch::Stop();
            REQUIRE_FALSE(ran.load());
        }
    }
}

TEST_CASE("AsyncDispatch start and stop", "[AsyncDispatch][engine]")
{
    SECTION("when start is called twice")
    {
        AsyncDispatch::Start();
        AsyncDispatch::Start();

        SECTION("should still serve work with one worker")
        {
            // Idempotent by contract. A second worker on the same queue would
            // break the ordering guarantee above and double-run nothing
            // visible, so the damage would surface far from here.
            std::promise<void> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
        }

        AsyncDispatch::Stop();
    }

    SECTION("when stop is called without a start")
    {
        AsyncDispatch::Stop();

        SECTION("should do nothing")
        {
            AsyncDispatch::Stop();
            SUCCEED("stopping an idle dispatcher is a no-op");
        }
    }

    SECTION("when stop is called with work still queued")
    {
        AsyncDispatch::Start();

        SECTION("should drain the queue before returning")
        {
            // The loop only exits once the queue is empty, so Stop is a
            // flush-and-join rather than a cancel. Anything already accepted
            // has been promised to its caller.
            std::atomic<int> completed{0};
            for (int i = 0; i < 32; ++i) {
                AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { ++completed; });
            }
            AsyncDispatch::Stop();
            REQUIRE(completed.load() == 32);
        }
    }
}

TEST_CASE("AsyncDispatch worker survives a failing task", "[AsyncDispatch][engine]")
{
    RunningDispatcher worker;

    SECTION("when a task throws a standard exception")
    {
        SECTION("should keep serving later work")
        {
            // One bad task must not take the worker down with it: every
            // subsystem that reaches plugin-thread land shares this thread.
            std::promise<void> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([](const PluginThread::Token&) { throw std::runtime_error("boom"); });
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
        }
    }

    SECTION("when a task throws something that is not an exception")
    {
        SECTION("should keep serving later work")
        {
            std::promise<void> done;
            auto arrived = done.get_future();
            AsyncDispatch::EnqueueWork([](const PluginThread::Token&) { throw 42; });
            AsyncDispatch::EnqueueWork([&](const PluginThread::Token&) { done.set_value(); });
            REQUIRE(Arrived(arrived));
        }
    }
}
