#pragma once

// Real-time tick driver for the Director. Paused-game intervals are
// dropped by the poll body itself (via GameIsPaused()).
namespace NarrativeEngine::Tick
{
    // Idempotent. Call from kPostLoadGame and kNewGame.
    void Start();

    // Idempotent. Call from kPreLoadGame to keep an in-flight tick from
    // firing during deserialization.
    void Stop();

    // Suspends PhaseTracker + EvaluationPipeline firings; event-log
    // polls keep running so their edge-detection stays truthful across
    // the disabled span. Defaults to true. Thread-safe.
    void SetEnabled(bool enabled);
    bool IsEnabled();

    // Unpaused real-time seconds until the accumulator next crosses
    // `iTickIntervalSeconds` and an evaluation fires. Thread-safe.
    //
    // Unpaused, so it does not run down while the game is paused --
    // which includes the whole time the dashboard that displays it is
    // on screen. It is a reading as of the last poll, not a countdown.
    //
    // Returns the full interval before the first poll of a session, and
    // 0 when the accumulator has already crossed (the next poll fires,
    // unless an evaluation is still in flight).
    double SecondsUntilNextTick();
} // namespace NarrativeEngine::Tick
