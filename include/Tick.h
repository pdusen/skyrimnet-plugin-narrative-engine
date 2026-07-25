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
} // namespace NarrativeEngine::Tick
