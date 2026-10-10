#include <DashboardTimers.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

// Tests for the arithmetic behind every timing readout on the dashboard.
//
// Two distinctions carry the whole feature, and both are the kind that look
// fine on screen while being wrong:
//
//   Inactive is not zero. A cooldown configured off never blocks anything;
//   one that has just expired stopped blocking a moment ago. Rendering both
//   as "0s" answers "why has this beat not fired?" wrongly, and that is the
//   only question the panel exists to answer.
//
//   The clock travels with the number. Three of the four advance at
//   different rates -- wall-clock runs through everything, unpaused real
//   time stops when the game is paused, active-play real time also stops in
//   combat and in dialogue -- so a bare "120s" is not a duration a reader
//   can act on. The wire name is asserted here because the React side
//   switches on it, and a silent rename would drop the label rather than
//   fail anything.

namespace
{
    namespace Timers = NarrativeEngine::DashboardTimers;
    using Timers::Clock;
} // namespace

TEST_CASE("DashboardTimers::Remaining", "[DashboardTimers][engine]")
{
    SECTION("when part of the interval has elapsed")
    {
        const auto timer = Timers::Remaining(90.0, 30.0, Clock::UnpausedRealTime);

        SECTION("should report what is left")
        {
            REQUIRE(timer.active);
            REQUIRE(timer.remaining == 60.0);
        }

        SECTION("should carry the clock it was measured on")
        {
            REQUIRE(timer.clock == Clock::UnpausedRealTime);
        }
    }

    SECTION("when nothing has elapsed yet")
    {
        SECTION("should report the whole interval")
        {
            REQUIRE(Timers::Remaining(90.0, 0.0, Clock::UnpausedRealTime).remaining == 90.0);
        }
    }

    SECTION("when the deadline has already passed")
    {
        const auto timer = Timers::Remaining(90.0, 120.0, Clock::ActivePlayRealTime);

        SECTION("should clamp to zero rather than go negative")
        {
            // The accumulators overshoot routinely -- Tick subtracts the
            // interval rather than zeroing, and the beat cooldown keeps
            // counting up until a beat fires. A negative would render as
            // "-30s until the next evaluation".
            REQUIRE(timer.remaining == 0.0);
        }

        SECTION("should still be active")
        {
            // Expired, not disabled: the thing is scheduled and due. The
            // difference from the configured-off case below is the point.
            REQUIRE(timer.active);
        }
    }

    SECTION("when the interval is configured off")
    {
        const auto timer = Timers::Remaining(0.0, 500.0, Clock::GameTime);

        SECTION("should be inactive")
        {
            REQUIRE_FALSE(timer.active);
        }

        SECTION("should still name its clock, so the row can be labelled")
        {
            REQUIRE(timer.clock == Clock::GameTime);
        }
    }

    SECTION("when the interval is negative")
    {
        SECTION("should be inactive rather than inverted")
        {
            // An int setting read as -1 reaches here as a negative double.
            REQUIRE_FALSE(Timers::Remaining(-5.0, 0.0, Clock::WallClock).active);
        }
    }
}

TEST_CASE("DashboardTimers::FromRemaining", "[DashboardTimers][engine]")
{
    // For subsystems that own their own countdown and hand back a remainder
    // rather than an elapsed figure -- Tick, the gossip schedule, the visit
    // timeouts.
    SECTION("when the owning subsystem reports time left")
    {
        const auto timer = Timers::FromRemaining(true, 12.5, Clock::GameTime);

        SECTION("should pass it through")
        {
            REQUIRE(timer.active);
            REQUIRE(timer.remaining == 12.5);
            REQUIRE(timer.clock == Clock::GameTime);
        }
    }

    SECTION("when the owning subsystem reports a negative remainder")
    {
        SECTION("should clamp to zero")
        {
            REQUIRE(Timers::FromRemaining(true, -3.0, Clock::WallClock).remaining == 0.0);
        }
    }

    SECTION("when the thing is not running")
    {
        SECTION("should be inactive")
        {
            // A visit that is not in its approach phase has no approach
            // deadline; the row reads as absent, not as expired.
            REQUIRE_FALSE(Timers::FromRemaining(false, 45.0, Clock::ActivePlayRealTime).active);
        }
    }
}

TEST_CASE("DashboardTimers::Inactive", "[DashboardTimers][engine]")
{
    SECTION("should be inactive on the clock it names")
    {
        const auto timer = Timers::Inactive(Clock::GameTime);
        REQUIRE_FALSE(timer.active);
        REQUIRE(timer.remaining == 0.0);
        REQUIRE(timer.clock == Clock::GameTime);
    }
}

TEST_CASE("DashboardTimers::ClockName", "[DashboardTimers][engine]")
{
    SECTION("should give each clock the wire name the React side switches on")
    {
        // Pinned literally. A rename here type-checks on both sides and just
        // silently drops the unit label off every row.
        REQUIRE(std::string{Timers::ClockName(Clock::WallClock)} == "wall_clock");
        REQUIRE(std::string{Timers::ClockName(Clock::UnpausedRealTime)} == "unpaused_real");
        REQUIRE(std::string{Timers::ClockName(Clock::ActivePlayRealTime)} == "active_play_real");
        REQUIRE(std::string{Timers::ClockName(Clock::GameTime)} == "game_time");
    }

    SECTION("should give the four clocks four distinct names")
    {
        // Two clocks sharing a name would label a row with the wrong rate,
        // which is worse than leaving it off.
        const std::string names[] = {
            Timers::ClockName(Clock::WallClock),
            Timers::ClockName(Clock::UnpausedRealTime),
            Timers::ClockName(Clock::ActivePlayRealTime),
            Timers::ClockName(Clock::GameTime),
        };
        for (std::size_t i = 0; i < 4; ++i) {
            for (std::size_t j = i + 1; j < 4; ++j) {
                REQUIRE(names[i] != names[j]);
            }
        }
    }
}

TEST_CASE("DashboardTimers::ToJson", "[DashboardTimers][engine]")
{
    SECTION("when the timer is active")
    {
        const auto json = Timers::ToJson(Timers::FromRemaining(true, 42.0, Clock::UnpausedRealTime));

        SECTION("should carry the remainder and the clock")
        {
            REQUIRE(json.is_object());
            REQUIRE(json.value("remaining", 0.0) == 42.0);
            REQUIRE(json.value("clock", "") == "unpaused_real");
        }
    }

    SECTION("when the timer is inactive")
    {
        SECTION("should serialize as null, not as a zero")
        {
            // The React contract keys off null to render an em dash. An
            // object carrying 0.0 would render as "now", which is the
            // opposite of what an unconfigured cooldown means.
            REQUIRE(Timers::ToJson(Timers::Inactive(Clock::GameTime)).is_null());
        }
    }

    SECTION("when an expired timer is serialized")
    {
        SECTION("should be an object holding zero, not null")
        {
            // The other half of the same distinction, from the wire's side.
            const auto json = Timers::ToJson(Timers::Remaining(60.0, 90.0, Clock::WallClock));
            REQUIRE(json.is_object());
            REQUIRE(json.value("remaining", -1.0) == 0.0);
        }
    }
}
