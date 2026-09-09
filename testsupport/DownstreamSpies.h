#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// Stand-ins for the two things the Director's evaluation hands its decision on
// to, and the record of what they were handed.
//
// Beside the harness rather than inside one test file because the evaluation's
// own tests and the tick driver's both need them. They are what is left of a
// larger set: the event logs, the road graphs and the gossip scheduler were all
// stood in for here once and are compiled in for real now, observed through
// their own effects instead of through a counter.
//
// The completion callback is the interesting one. The beat system owns the
// decision once it is handed over, and calling that callback is what tells the
// evaluation it may start another — so holding it is how a test says "the
// previous evaluation is still running", without having to stall an LLM.
namespace NarrativeEngine::Testing
{
    struct DownstreamSpyState
    {
        std::mutex mutex;

        // Decisions handed to the beat system.
        std::atomic<int> beatsConsidered{0};

        // Times the dashboard was told the world had moved.
        std::atomic<int> dashboardPushes{0};

        // What the last decision said. The evaluation fills these in from the
        // LLM's answer, or from the snapshot when the answer is unusable.
        std::string lastAction;
        std::string lastReasoning;

        // When set, the completion callback is kept rather than run, which
        // leaves the evaluation in flight until a test releases it.
        std::atomic<bool> holdCompletion{false};

        // Run and forget whatever was held. Does nothing when nothing is.
        void ReleaseHeld();

        // Finishes anything still held first: the in-flight flag belongs to
        // the pipeline and outlives any one case.
        void Reset();

        // Called by the stand-in; not by tests.
        void RecordHandoff(std::string action, std::string reasoning, std::function<void()> onComplete);

    private:
        std::function<void()> held_;
    };

    DownstreamSpyState& DownstreamSpies();
} // namespace NarrativeEngine::Testing
