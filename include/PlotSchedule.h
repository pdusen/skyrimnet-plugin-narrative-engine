#pragma once

#include <cstddef>
#include <vector>

// PlotSchedule — when plot ticks fire, as a pure function.
//
// This is the whole of the cadence decision, expressed with no engine
// pointer, no wall clock and no global state: hand it where the schedule
// last fired, what the game clock reads now, and the two settings, and
// it returns the stamps to enqueue.
//
// It is a free function taking plain numbers for one reason: it is the
// only way this step can be verified at all. "Sleep 24 in-world hours
// and count the ticks" needs a running game, several minutes, and a
// human — and it cannot provoke the case that actually bites, which is
// the clock moving BACKWARDS when a player loads an earlier save. A
// table-driven probe settles every case in milliseconds.
//
// Two properties the caller depends on:
//
//   * TICKS ARE NEVER COALESCED. Crossing two boundaries produces two
//     stamps, not one tick with twice as much to do. Each carries the
//     game time it was SUPPOSED to fire at and the job reasons as of
//     that moment, which is what decouples when a tick runs from what it
//     processes and lets the queue back up behind a slow LLM call
//     without the simulation drifting.
//
//   * THE BACKLOG IS BOUNDED, NOT THE TICK. Past the outstanding cap the
//     schedule advances WITHOUT the work being done, so a console time
//     jump cannot queue a year of simulation. Skipped boundaries are
//     reported rather than swallowed.
//
// See docs/implementation/PHASE_14_FACTION_PLOTS.md step 4.
namespace NarrativeEngine::PlotSchedule
{
    struct Decision
    {
        // Game-hour values, ascending. One job per entry, each reasoning
        // as of its own stamp.
        std::vector<double> stamps;

        // Where the schedule now stands. Always advances past every
        // boundary crossed, including ones whose work was skipped.
        double newLastFiredGameHours = 0.0;

        // The clock moved backwards — a save loaded from an earlier
        // point. The schedule is re-based onto the new reading rather
        // than producing a negative backlog, and nothing is enqueued.
        bool rebased = false;

        // Boundaries that were crossed but not enqueued, because the
        // outstanding cap was already reached. Non-zero is worth a log
        // line: it means the simulation is deliberately skipping time.
        std::size_t skipped = 0;
    };

    // `currentOutstanding` is how many jobs are already queued or
    // running (PlotDispatch::OutstandingCount). It is a parameter rather
    // than a call so this function stays pure.
    //
    // A non-positive interval yields nothing: a misconfigured INI should
    // stall the simulation, not divide by zero or spin.
    [[nodiscard]] Decision Advance(double lastFiredGameHours,
                                   double nowGameHours,
                                   double intervalGameHours,
                                   std::size_t maxOutstanding,
                                   std::size_t currentOutstanding);
} // namespace NarrativeEngine::PlotSchedule
