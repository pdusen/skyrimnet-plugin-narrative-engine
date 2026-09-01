#pragma once

#include <cstddef>

#include <PluginThread.h>

// PlotTick — the scheduler, and the single unit of work it schedules.
//
// Everything the plot simulation does in one beat of in-world time is
// ONE job, run start to finish on PlotDispatch's worker. In this step
// the job body only installs any pending loaded state, stamps the log
// and publishes; steps 5 and 6 fill in casting, dispatch, the progress
// race and adaptation.
//
// ---------------------------------------------------------------------
// The plugin thread does the cadence check and nothing else
//
// Poll() takes NO elapsed-seconds argument. This feature's cadence is
// in-game time and nothing else: it samples the game clock, asks
// PlotSchedule what that implies, and enqueues one stamped job per
// crossed boundary. There is no real-seconds accumulator to keep and no
// pause-awareness to get right — a paused game does not advance the game
// clock, so the comparison is simply never satisfied while paused.
//
// It follows that the driver's 500 ms poll rate is not a tuning
// parameter for this feature. It only bounds how promptly a crossed
// boundary is NOTICED; sleeping eight hours advances the clock in one
// jump and the poll that observes it enqueues every boundary that jump
// crossed.
//
// ---------------------------------------------------------------------
// Scheduled, stamped, never coalesced
//
// Passing 24 in-world hours with `T` crosses two 12-hour boundaries and
// TWO ticks run — not one that happens to have twice as much to do. Each
// job carries the game time it was SUPPOSED to fire at and reads the
// world as of that moment, which is what decouples when a tick runs from
// what it processes and lets the queue back up behind a slow LLM call
// without the simulation drifting.
//
// See docs/implementation/PHASE_14_FACTION_PLOTS.md step 4 and
// docs/design/FACTION_PLOTS.md Part 11.
namespace NarrativeEngine::PlotTick
{
    void Initialize();

    // kNewGame / kPostLoadGame. Re-bases the schedule onto the current
    // game clock so a load does not read as a colossal backlog.
    void OnSessionStart();

    // The plugin-thread cadence check, and the ONLY plot work that
    // happens on that thread. Microseconds, no locks on plot state, no
    // possibility of blocking.
    void Poll(const PluginThread::Token&);

    // Enqueue `count` ticks immediately, ignoring the game clock.
    //
    // Phase A's validation is entirely a question of watching many ticks
    // go by, and making that a button rather than an hour of waiting is
    // what keeps step 9 cheap enough to repeat after a tuning change.
    // Forced ticks go through the same queue and carry the same stamps
    // and cancellation handles as scheduled ones, so nothing observed
    // through them is an artefact of how they were triggered.
    //
    // Safe from any thread. Wired to dashboard bridge actions in step 8.
    void ForceTicks(std::size_t count);

    // How many ticks are queued or currently running.
    //
    // Kept here rather than read off PlotDispatch::OutstandingCount()
    // because the dashboard needs a value it can trust at PUSH time. A
    // job is retired by the dispatcher only after its body returns, so
    // the last tick of a burst still counts itself while it is asking
    // for the push that would re-enable the buttons -- and the compose
    // runs asynchronously, so which side of the retirement it lands on
    // is a race. This counter is decremented BEFORE that push is
    // issued, which makes the zero deterministic.
    //
    // Safe from any thread.
    [[nodiscard]] std::size_t OutstandingTicks();
} // namespace NarrativeEngine::PlotTick
