#include <MainThread.h>

#include <EngineMock.h>
#include <PluginThread.h>
#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

// Mocked-engine tests for the main-thread marshalling helpers.
//
// The module hands work to SKSE's task queue, so it is engine-coupled with no
// pure core. `FireAndForget` lives in the .cpp; `Run<T>` is a template in the
// header and is instantiated into this translation unit, so both are covered
// here.
//
// The mocked task interface runs a queued task inline, standing in for the main
// thread picking it up next frame. That is not a shortcut: `Run` blocks on a
// future only the task completes, so a harness that merely recorded the task
// would deadlock. `engine.tasks.runImmediately` turns the inline run off for
// the fire-and-forget cases that want to observe a task before it runs.
//
// A plugin token cannot be constructed directly — that is the point of the
// threading model — so these go through `PluginThread::detail::JobDispatcher`,
// the same public, header-only entry point the real dispatchers use.

namespace
{
    using NarrativeEngine::CurrentThreadRole;
    using NarrativeEngine::ThreadRole;
    using NarrativeEngine::Testing::EngineMock;
    namespace MainThread = NarrativeEngine::MainThread;
    namespace PluginThread = NarrativeEngine::PluginThread;

    void ThrowRuntimeError()
    {
        throw std::runtime_error("boom");
    }

    // Throwing through a volatile function pointer, so the compiler cannot
    // prove the call never returns. A lambda that provably always throws makes
    // the `return` after `future.get()` inside MainThread::Run unreachable,
    // which /W4 /WX rejects — a warning this test would be creating rather than
    // finding, since production never instantiates Run with such a body.
    void (*volatile Boom)() = &ThrowRuntimeError;

    // Stand in for a real plugin worker: claim the Plugin role, then run
    // `body` with a genuine token in hand.
    //
    // Both halves matter. JobDispatcher::Invoke hands out the token but does
    // NOT set the thread role — the worker loops do that separately with their
    // own ScopedThreadRole — so a helper that only took the token would leave
    // the thread reading as Foreign and misrepresent every caller of Run.
    template <class Fn> void AsPluginThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) { body(pt); });
    }
} // namespace

TEST_CASE("MainThread::FireAndForget", "[MainThread][engine]")
{
    // Happy path, re-run per leaf: a task interface that accepts work and runs
    // it inline. Each case overrides one of those.
    EngineMock engine;
    int ran = 0;

    SECTION("when the task interface is available")
    {
        AsPluginThread([&](const PluginThread::Token& pt) {
            MainThread::FireAndForget(pt, [&](const MainThread::Token&) { ++ran; });
        });

        SECTION("should queue exactly one task")
        {
            REQUIRE(engine.tasks.queued == 1);
        }

        SECTION("should run the caller's work")
        {
            REQUIRE(ran == 1);
        }
    }

    SECTION("when the work runs")
    {
        SECTION("should present itself as the main thread")
        {
            // The role marker is what every downstream runtime assertion reads.
            // Without the guard the work would run as Plugin and any main-only
            // check inside it would be wrong about where it is.
            ThreadRole observed = ThreadRole::Foreign;
            AsPluginThread([&](const PluginThread::Token& pt) {
                MainThread::FireAndForget(pt, [&](const MainThread::Token&) { observed = CurrentThreadRole(); });
            });
            REQUIRE(observed == ThreadRole::Main);
        }

        SECTION("should restore the previous role afterwards")
        {
            AsPluginThread([&](const PluginThread::Token& pt) {
                MainThread::FireAndForget(pt, [](const MainThread::Token&) {});
                REQUIRE(CurrentThreadRole() == ThreadRole::Plugin);
            });
        }
    }

    SECTION("when the work throws")
    {
        SECTION("should swallow a standard exception")
        {
            // Fire-and-forget has nobody to report to, so an escaping exception
            // would tear down whichever engine thread ran the task.
            AsPluginThread([&](const PluginThread::Token& pt) {
                MainThread::FireAndForget(pt, [](const MainThread::Token&) { throw std::runtime_error("boom"); });
            });
            SUCCEED("the exception did not escape the task");
        }

        SECTION("should swallow a non-standard exception")
        {
            AsPluginThread([&](const PluginThread::Token& pt) {
                MainThread::FireAndForget(pt, [](const MainThread::Token&) { throw 42; });
            });
            SUCCEED("the exception did not escape the task");
        }
    }

    SECTION("when no work was supplied")
    {
        SECTION("should queue nothing")
        {
            AsPluginThread([&](const PluginThread::Token& pt) { MainThread::FireAndForget(pt, nullptr); });
            REQUIRE(engine.tasks.queued == 0);
        }
    }

    SECTION("when the task interface is unavailable")
    {
        engine.tasks.interfacePresent = false;

        SECTION("should drop the work rather than crash")
        {
            AsPluginThread([&](const PluginThread::Token& pt) {
                MainThread::FireAndForget(pt, [&](const MainThread::Token&) { ++ran; });
            });
            REQUIRE(ran == 0);
        }
    }

    SECTION("when the queue holds work instead of running it")
    {
        engine.tasks.runImmediately = false;

        SECTION("should not have run the work yet")
        {
            AsPluginThread([&](const PluginThread::Token& pt) {
                MainThread::FireAndForget(pt, [&](const MainThread::Token&) { ++ran; });
            });
            REQUIRE(engine.tasks.queued == 1);
            REQUIRE(ran == 0);
        }
    }
}

TEST_CASE("MainThread::Run", "[MainThread][engine]")
{
    EngineMock engine;

    SECTION("when the work returns a value")
    {
        SECTION("should hand the value back to the caller")
        {
            int result = 0;
            AsPluginThread([&](const PluginThread::Token& pt) {
                result = MainThread::Run(pt, [](const MainThread::Token&) { return 42; });
            });
            REQUIRE(result == 42);
        }

        SECTION("should run it as the main thread")
        {
            ThreadRole observed = ThreadRole::Foreign;
            AsPluginThread([&](const PluginThread::Token& pt) {
                observed = MainThread::Run(pt, [](const MainThread::Token&) { return CurrentThreadRole(); });
            });
            REQUIRE(observed == ThreadRole::Main);
        }
    }

    SECTION("when the work returns nothing")
    {
        SECTION("should still run it")
        {
            int ran = 0;
            AsPluginThread(
                [&](const PluginThread::Token& pt) { MainThread::Run(pt, [&](const MainThread::Token&) { ++ran; }); });
            REQUIRE(ran == 1);
        }
    }

    SECTION("when the work throws")
    {
        SECTION("should re-throw to the caller")
        {
            // The difference from FireAndForget, and the reason Run exists: a
            // blocking marshal behaves like a synchronous call, exceptions
            // included, so the caller can handle a failure it caused.
            bool caught = false;
            AsPluginThread([&](const PluginThread::Token& pt) {
                try {
                    MainThread::Run(pt, [](const MainThread::Token&) -> int {
                        Boom();
                        return 0;
                    });
                } catch (const std::runtime_error&) {
                    caught = true;
                }
            });
            REQUIRE(caught);
        }

        SECTION("should re-throw from a void call too")
        {
            bool caught = false;
            AsPluginThread([&](const PluginThread::Token& pt) {
                try {
                    MainThread::Run(pt, [](const MainThread::Token&) { Boom(); });
                } catch (const std::runtime_error&) {
                    caught = true;
                }
            });
            REQUIRE(caught);
        }
    }

    SECTION("when the task interface is unavailable")
    {
        engine.tasks.interfacePresent = false;

        SECTION("should return a default-constructed result")
        {
            // No main thread to marshal onto, so the call cannot block forever
            // waiting for one. The caller gets a zero it has to be ready for.
            int result = 99;
            AsPluginThread([&](const PluginThread::Token& pt) {
                result = MainThread::Run(pt, [](const MainThread::Token&) { return 42; });
            });
            REQUIRE(result == 0);
        }

        SECTION("should not run the work")
        {
            int ran = 0;
            AsPluginThread(
                [&](const PluginThread::Token& pt) { MainThread::Run(pt, [&](const MainThread::Token&) { ++ran; }); });
            REQUIRE(ran == 0);
        }
    }
}
