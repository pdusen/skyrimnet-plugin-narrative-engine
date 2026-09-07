#include "DispatcherContract.h"

#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

// The shared plugin-worker contract. See DispatcherContract.h for why it is
// written once rather than three times.
//
// These are the only concurrent tests in the suite, so they are written to fail
// rather than hang: every wait is a bounded future whose arrival is asserted,
// and nothing sleeps for a fixed period hoping the worker got there.
//
// Each dispatcher's state is process-global with exactly one worker, so every
// case brackets itself rather than assuming what an earlier one left behind.

namespace NarrativeEngine::Testing
{
    namespace
    {
        using NarrativeEngine::CurrentThreadRole;
        using NarrativeEngine::ThreadRole;

        // Long enough that a loaded machine still makes it, short enough that a
        // genuine deadlock fails the run instead of wedging it.
        constexpr auto kTimeout = std::chrono::seconds{5};

        template <class T> bool Arrived(std::future<T>& f)
        {
            return f.wait_for(kTimeout) == std::future_status::ready;
        }
    } // namespace

    void CheckDispatcherContract(const DispatcherOps& ops)
    {
        // A running worker is the happy path for most of what follows, but not
        // all of it, so it is claimed per context rather than hoisted here.
        struct Running
        {
            explicit Running(const DispatcherOps& o) : ops(o)
            {
                ops.start();
            }
            ~Running()
            {
                ops.stop();
            }
            const DispatcherOps& ops;
        };

        SECTION("when the worker is running")
        {
            Running worker{ops};

            SECTION("should run the work")
            {
                std::promise<int> done;
                auto arrived = done.get_future();
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(7); });
                REQUIRE(Arrived(arrived));
                REQUIRE(arrived.get() == 7);
            }

            SECTION("should run it as the plugin thread")
            {
                // The role the token discipline mirrors at compile time. Work
                // arriving from a SkyrimNet callback is Foreign until the
                // worker loop claims Plugin on its behalf.
                std::promise<ThreadRole> done;
                auto arrived = done.get_future();
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(CurrentThreadRole()); });
                REQUIRE(Arrived(arrived));
                REQUIRE(arrived.get() == ThreadRole::Plugin);
            }

            SECTION("should run it somewhere other than the caller")
            {
                // These queues exist to keep work off the calling thread — a
                // foreign engine thread, or a poll that must not be delayed.
                std::promise<std::thread::id> done;
                auto arrived = done.get_future();
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(std::this_thread::get_id()); });
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
                ops.enqueue([&](const PluginThread::Token&) { first = ++order; });
                ops.enqueue([&](const PluginThread::Token&) { second = ++order; });
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(); });
                REQUIRE(Arrived(arrived));
                REQUIRE(first == 1);
                REQUIRE(second == 2);
            }

            SECTION("should ignore empty work and keep serving what follows")
            {
                std::promise<void> done;
                auto arrived = done.get_future();
                ops.enqueue(nullptr);
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(); });
                REQUIRE(Arrived(arrived));
            }
        }

        SECTION("when the worker is not running")
        {
            ops.stop();

            SECTION("should drop the work rather than hold it")
            {
                // Holding it for a worker that may never start would leak the
                // closure and everything it captured for the rest of the
                // session, and run it at a moment nobody expected.
                std::atomic<bool> ran{false};
                ops.enqueue([&](const PluginThread::Token&) { ran = true; });

                ops.start();
                std::promise<void> done;
                auto arrived = done.get_future();
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(); });
                REQUIRE(Arrived(arrived));
                ops.stop();

                REQUIRE_FALSE(ran.load());
            }
        }

        SECTION("when start is called twice")
        {
            ops.start();
            ops.start();

            SECTION("should still serve work with one worker")
            {
                // Idempotent by contract. A second worker on the same queue
                // would break the ordering above, and the damage would surface
                // far from the second Start that caused it.
                std::promise<void> done;
                auto arrived = done.get_future();
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(); });
                REQUIRE(Arrived(arrived));
            }

            ops.stop();
        }

        SECTION("when stop is called on an idle worker")
        {
            ops.stop();

            SECTION("should do nothing")
            {
                ops.stop();
                SUCCEED("stopping an idle dispatcher is a no-op");
            }
        }

        SECTION("when stop is called with work still queued")
        {
            ops.start();

            SECTION("should drain the queue before returning")
            {
                // The loop exits only once the queue is empty, so Stop is a
                // flush-and-join rather than a cancel: anything already
                // accepted has been promised to whoever queued it.
                std::atomic<int> completed{0};
                for (int i = 0; i < 32; ++i) {
                    ops.enqueue([&](const PluginThread::Token&) { ++completed; });
                }
                ops.stop();
                REQUIRE(completed.load() == 32);
            }
        }

        SECTION("when a task throws")
        {
            Running worker{ops};

            SECTION("should keep serving after a standard exception")
            {
                // One bad task must not take the worker down: every subsystem
                // that reaches plugin-thread land shares this one thread.
                std::promise<void> done;
                auto arrived = done.get_future();
                ops.enqueue([](const PluginThread::Token&) { throw std::runtime_error("boom"); });
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(); });
                REQUIRE(Arrived(arrived));
            }

            SECTION("should keep serving after a non-exception throw")
            {
                std::promise<void> done;
                auto arrived = done.get_future();
                ops.enqueue([](const PluginThread::Token&) { throw 42; });
                ops.enqueue([&](const PluginThread::Token&) { done.set_value(); });
                REQUIRE(Arrived(arrived));
            }
        }
    }
} // namespace NarrativeEngine::Testing
