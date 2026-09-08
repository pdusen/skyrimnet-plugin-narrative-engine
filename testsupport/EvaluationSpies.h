#pragma once

#include <atomic>

// Stand-in for the evaluation pipeline the tick driver fires into, and the
// record of how often it fired.
//
// Beside the harness rather than inside one test file because the driver's own
// tests and anything else that drives a tick both need it. It is what is left
// of a larger set: the event logs, the gossip scheduler and the road graph were
// all stood in for here once and are now compiled in for real, observed through
// their own effects instead of through a counter.
namespace NarrativeEngine::Testing
{
    struct EvaluationSpyState
    {
        // How many evaluations the driver has begun.
        std::atomic<int> evaluations{0};

        // Whether one is still running. The driver skips rather than queues
        // while this is set, which is the behaviour that keeps a slow LLM round
        // trip from producing a burst of catch-up evaluations.
        std::atomic<bool> inFlight{false};

        void Reset();
    };

    EvaluationSpyState& EvaluationSpies();
} // namespace NarrativeEngine::Testing
