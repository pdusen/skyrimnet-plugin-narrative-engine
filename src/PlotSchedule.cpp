#include <PlotSchedule.h>

#include <cmath>

namespace NarrativeEngine::PlotSchedule
{
    Decision Advance(double lastFiredGameHours,
                     double nowGameHours,
                     double intervalGameHours,
                     std::size_t maxOutstanding,
                     std::size_t currentOutstanding)
    {
        Decision decision;
        decision.newLastFiredGameHours = lastFiredGameHours;

        // A misconfigured interval stalls the simulation rather than
        // dividing by zero or emitting a boundary per poll.
        if (!(intervalGameHours > 0.0)) {
            return decision;
        }

        // The clock moved backwards: the player loaded a save from
        // earlier than the one this schedule was following. Re-base onto
        // the new reading and enqueue nothing.
        //
        // Without this case the subtraction below goes negative, the
        // floor goes negative, and the loop either emits nothing forever
        // (because newLastFired stays in the future) or, with an
        // unsigned count, emits an astronomically large number of
        // stamps. It is the one case that cannot be provoked on purpose
        // in-game and the one most likely to be hit by accident.
        if (nowGameHours < lastFiredGameHours) {
            decision.newLastFiredGameHours = nowGameHours;
            decision.rebased = true;
            return decision;
        }

        const double elapsed = nowGameHours - lastFiredGameHours;
        const double boundariesReal = std::floor(elapsed / intervalGameHours);
        if (!(boundariesReal >= 1.0)) {
            return decision;
        }

        // Clamped before the cast: a save whose clock is wildly ahead of
        // the schedule (a console `set timescale`, a very long
        // wait) must not overflow the count on its way to being capped.
        constexpr double kAbsurdBoundaryCount = 1.0e9;
        const auto boundaries =
            static_cast<std::size_t>(boundariesReal > kAbsurdBoundaryCount ? kAbsurdBoundaryCount : boundariesReal);

        const std::size_t room = currentOutstanding >= maxOutstanding ? 0 : maxOutstanding - currentOutstanding;
        const std::size_t toEnqueue = boundaries < room ? boundaries : room;

        decision.stamps.reserve(toEnqueue);
        for (std::size_t i = 1; i <= toEnqueue; ++i) {
            decision.stamps.push_back(lastFiredGameHours + intervalGameHours * static_cast<double>(i));
        }

        // The schedule advances past EVERY boundary crossed, including
        // the ones whose work was skipped. Advancing only past the
        // enqueued ones would leave the backlog permanently owed: the
        // next poll would see the same overdue boundaries again and
        // enqueue another capped batch, forever.
        decision.newLastFiredGameHours = lastFiredGameHours + intervalGameHours * static_cast<double>(boundaries);
        decision.skipped = boundaries - toEnqueue;
        return decision;
    }
} // namespace NarrativeEngine::PlotSchedule
