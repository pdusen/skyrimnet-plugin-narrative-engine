#pragma once

// Wall-clock tick driver for the Director.
//
// Runs a dedicated `std::thread` that sleeps for
// `Settings::tickIntervalSeconds`, wakes, and marshals
// `PhaseTracker::Tick(dt)` (and, from Step 9 onward,
// `EvaluationPipeline::BeginEvaluation()`) onto the main thread via the
// AsyncDispatch helper. Real-time, not game-time: pauses inside the engine
// (menus, console, dialogue) are handled by PhaseTracker itself, which
// drops the dt when `GameIsPaused()` is true.
namespace NarrativeEngine::Tick
{
    // Start the tick driver thread. Idempotent. Call from kPostLoadGame
    // and kNewGame (i.e. only when there's a real game in progress).
    void Start();

    // Signal the tick thread to exit and join it. Idempotent. Call from
    // kPreLoadGame so an in-flight tick can't fire during deserialization.
    void Stop();

    // Runtime killswitch for the tick's main-thread poll. When disabled,
    // PollOnMainThread returns immediately without accumulating time,
    // polling CombatEventLog, driving the ActionDispatcher tick, or firing
    // PhaseTracker/EvaluationPipeline. Used by the dashboard debug toggle
    // to suspend all timed Director behavior while diagnosing specific
    // subsystems in isolation. Defaults to true. Thread-safe.
    void SetEnabled(bool enabled);
    bool IsEnabled();
}
