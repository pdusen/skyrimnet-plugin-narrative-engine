#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

// DashboardTimers — "when does the next thing happen, and how long is
// this cooldown" for every tab of the dashboard, in the unit that
// actually governs each one.
//
// ---------------------------------------------------------------------
// There are four clocks, and they are not interchangeable
// ---------------------------------------------------------------------
//
//   WallClock          system_clock. Runs through everything: pause,
//                      combat, dialogue, menus. Only the anti-repetition
//                      ring and the letter pool's delivery timeout are
//                      on it.
//
//   UnpausedRealTime   The `unpausedElapsedSeconds` accumulators in
//                      Tick.cpp, which bail on IsGamePaused(). The
//                      Director tick, the minimum phase duration and
//                      the gossip schedule check.
//
//   ActivePlayRealTime Advances only under TickMode::Normal, so it is
//                      frozen in combat and in dialogue as well as on
//                      pause -- strictly slower than UnpausedRealTime.
//                      The global beat cooldown, the visit approach and
//                      return-home deadlines, and the conclusion poll's
//                      silence gate.
//
//   GameTime           RE::Calendar. Per-beat and per-sender cooldowns,
//                      the gossip tick, the poll's max-interval gate.
//                      Carried in HOURS, where the three real-time
//                      clocks are carried in seconds.
//
// Rendering all four as bare seconds would be worse than showing
// nothing: "120" against the beat cooldown and "120" against the
// repetition window describe durations that elapse at different rates,
// and the difference is exactly what a user staring at a beat that will
// not fire needs to know.
//
// ---------------------------------------------------------------------
// These are readings, not countdowns
// ---------------------------------------------------------------------
//
// DashboardUIManager::ToggleVisibility shows the view with
// PrismaUI_API::Focus(..., pauseGame=true), and PushFullState returns
// early unless the view is visible. So the dashboard is only ever on
// screen with the game paused, and three of the four clocks above are
// frozen for precisely as long as anyone is looking at them. There is
// nothing to animate and no reason for a cadenced push; the values are
// correct as of the moment the panel was opened, and the UI says so.
//
// Threading: Collect() reads live subsystem state through their own
// thread-safe accessors and touches RE::Calendar via
// EngineUtils::GetCurrentGameHours. DashboardUIManager calls it from
// the plugin thread, where the rest of the state compose already runs.
namespace NarrativeEngine::DashboardTimers
{
    enum class Clock : std::uint8_t
    {
        WallClock,
        UnpausedRealTime,
        ActivePlayRealTime,
        GameTime,
    };

    // Wire name for `clock`, as the React side switches on it.
    const char* ClockName(Clock clock);

    // One readout. `active` false means there is nothing to wait for --
    // the cooldown is configured off, or nothing is scheduled -- and
    // `remaining` is then meaningless and must not be rendered as
    // "0 left". The two cases read very differently to a user trying to
    // work out why a beat has not fired.
    //
    // `remaining` is seconds for the three real-time clocks and HOURS
    // for Clock::GameTime.
    struct Timer
    {
        bool active = false;
        double remaining = 0.0;
        Clock clock = Clock::WallClock;
    };

    // The arithmetic every readout shares, in one place so it is tested
    // once: inactive when the interval is not configured, clamped at
    // zero when the deadline has already passed, never negative.
    Timer Remaining(double configuredInterval, double elapsed, Clock clock);

    // Same, for a deadline already expressed as a remainder by the
    // owning subsystem. `enabled` false yields an inactive timer.
    Timer FromRemaining(bool enabled, double remaining, Clock clock);

    // An inactive timer on `clock` -- "nothing scheduled", rendered as
    // such rather than as zero.
    Timer Inactive(Clock clock);

    // `{active, remaining, clock}`, or `null` when inactive, so the
    // React side can render "--" without repeating the active check at
    // every call site.
    nlohmann::json ToJson(const Timer& timer);

    // Every timer the dashboard shows, gathered from the live
    // subsystems, as the `timers` object of the state payload. Shaped
    // per tab to match the React contract in dashboard/src/types.ts.
    nlohmann::json Collect();
} // namespace NarrativeEngine::DashboardTimers
