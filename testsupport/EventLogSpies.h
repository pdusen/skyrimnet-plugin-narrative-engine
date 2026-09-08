#pragma once

#include <atomic>
#include <mutex>
#include <vector>

// Stand-ins for the two collaborators the tick driver polls that are not yet
// compiled into the mocked-engine target, plus the record of what they were
// polled with.
//
// Beside the harness rather than in one test file because more than one needs
// them. The three event logs used to be here too and are now real; what is left
// goes the same way when FineRoads and EvaluationPipeline get tests of their
// own.
namespace NarrativeEngine::Testing
{
    struct EventLogSpyState
    {
        std::mutex mutex;

        // Poll counts, one per source the driver is supposed to reach.
        std::atomic<int> fineRoadsPolls{0};

        // Elapsed seconds the last poll was told about. The collaborators
        // accumulate against this rather than sampling their own clocks, so a
        // driver that passed zero would freeze every one of them at once.
        std::atomic<double> lastElapsed{0.0};

        // The evaluation pipeline the driver fires into.
        std::atomic<int> evaluations{0};
        std::atomic<bool> evaluationInFlight{false};

        void Reset();
    };

    EventLogSpyState& EventLogSpies();
} // namespace NarrativeEngine::Testing
