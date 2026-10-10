#include <ThreadRole.h>

#include <catch2/catch_test_macros.hpp>

#include <thread>

// Unit tests for the thread-role marker.
//
// Pure by construction: a thread_local enum plus an RAII object that pushes and
// pops it. No engine, no PCH, nothing to mock — but also nothing trivial, since
// what the module promises is scoping and thread-locality rather than a return
// value, and both are properties you can only observe by nesting scopes and by
// looking from a second thread.

namespace
{
    using NarrativeEngine::CurrentThreadRole;
    using NarrativeEngine::ScopedThreadRole;
    using NarrativeEngine::ThreadRole;

    // Run `fn` on a brand-new thread and hand back what it saw. The second
    // thread is the only way to observe thread-locality at all: from one
    // thread, a thread_local and a plain global behave identically.
    //
    // `observed` starts as Main rather than Foreign on purpose. Seeded with the
    // answer a test is hoping for, a lambda that never ran would look like a
    // pass.
    template <class Fn> ThreadRole RoleSeenOnAnotherThread(Fn fn)
    {
        ThreadRole observed = ThreadRole::Main;
        std::thread worker([&] { observed = fn(); });
        worker.join();
        return observed;
    }
} // namespace

TEST_CASE("ScopedThreadRole claim and restore", "[ThreadRole]")
{
    // Little to hoist here on purpose: each case claims its own role, and a
    // role claimed at this level is one half the cases would immediately have
    // to undo. What is shared is only which role stands for "some role" —
    // named so a case that accidentally asserted the default would read wrong.
    constexpr ThreadRole kClaimed = ThreadRole::Plugin;

    SECTION("when no role has been claimed")
    {
        SECTION("should report Foreign")
        {
            // Also the check that every other case in this file cleaned up
            // after itself: the scopes are RAII, so a leaked role would show up
            // here as the test suite drifting away from the default.
            REQUIRE(CurrentThreadRole() == ThreadRole::Foreign);
        }
    }

    SECTION("when a role is claimed for a scope")
    {
        SECTION("should report the claimed role inside the scope")
        {
            const ScopedThreadRole scope{kClaimed};
            REQUIRE(CurrentThreadRole() == kClaimed);
        }

        SECTION("should restore the previous role when the scope ends")
        {
            {
                const ScopedThreadRole scope{kClaimed};
                REQUIRE(CurrentThreadRole() == kClaimed);
            }
            REQUIRE(CurrentThreadRole() == ThreadRole::Foreign);
        }
    }

    SECTION("when the other role is claimed")
    {
        SECTION("should report that role too")
        {
            const ScopedThreadRole scope{ThreadRole::Main};
            REQUIRE(CurrentThreadRole() == ThreadRole::Main);
        }
    }

    SECTION("when the role claimed is the one already in effect")
    {
        const ScopedThreadRole outer{kClaimed};

        SECTION("should report that role")
        {
            const ScopedThreadRole inner{kClaimed};
            REQUIRE(CurrentThreadRole() == kClaimed);
        }

        SECTION("should still restore it when the scope ends")
        {
            // The case that separates "restore what was there" from "reset to
            // the default". Both implementations agree everywhere else; here an
            // implementation that reset would drop back to Foreign and strand
            // the outer scope's claim.
            {
                const ScopedThreadRole inner{kClaimed};
            }
            REQUIRE(CurrentThreadRole() == kClaimed);
        }
    }
}

TEST_CASE("ScopedThreadRole nesting", "[ThreadRole]")
{
    // The scopes cannot be hoisted: the last case asserts what is left AFTER
    // both have been destroyed, so each case has to own its own block and let
    // it close where the case needs it to.

    SECTION("when an inner scope claims a different role")
    {
        SECTION("should report the innermost role")
        {
            const ScopedThreadRole outer{ThreadRole::Main};
            const ScopedThreadRole inner{ThreadRole::Plugin};
            REQUIRE(CurrentThreadRole() == ThreadRole::Plugin);
        }

        SECTION("should restore the outer role when the inner scope ends")
        {
            // The load-bearing one. Main-thread work marshalled out of a plugin
            // worker nests exactly like this, and a restore-to-default would
            // silently demote the worker to Foreign for the rest of its life.
            const ScopedThreadRole outer{ThreadRole::Main};
            {
                const ScopedThreadRole inner{ThreadRole::Plugin};
            }
            REQUIRE(CurrentThreadRole() == ThreadRole::Main);
        }

        SECTION("should restore Foreign once both scopes have ended")
        {
            {
                const ScopedThreadRole outer{ThreadRole::Main};
                {
                    const ScopedThreadRole inner{ThreadRole::Plugin};
                }
            }
            REQUIRE(CurrentThreadRole() == ThreadRole::Foreign);
        }
    }
}

TEST_CASE("ThreadRole is per thread", "[ThreadRole]")
{
    // Happy path for both cases below, re-run per leaf: this thread has claimed
    // a role. Without it the isolation being tested would be invisible — a
    // second thread reading Foreign proves nothing if this thread is Foreign
    // too.
    const ScopedThreadRole thisThread{ThreadRole::Plugin};

    SECTION("when another thread has claimed no role")
    {
        SECTION("should read Foreign there even while this thread is claimed")
        {
            // The documented contract for every thread NarrativeEngine has not
            // claimed: Skyrim's event-dispatch pool and SkyrimNet's callback
            // threads arrive here and must not inherit a role from whoever
            // happened to spawn them.
            const ThreadRole other = RoleSeenOnAnotherThread([] { return CurrentThreadRole(); });
            REQUIRE(other == ThreadRole::Foreign);
        }
    }

    SECTION("when another thread claims a role")
    {
        SECTION("should leave this thread's role untouched")
        {
            const ThreadRole other = RoleSeenOnAnotherThread([] {
                const ScopedThreadRole claimed{ThreadRole::Main};
                return CurrentThreadRole();
            });
            REQUIRE(other == ThreadRole::Main);
            REQUIRE(CurrentThreadRole() == ThreadRole::Plugin);
        }
    }
}
