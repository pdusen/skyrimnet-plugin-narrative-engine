#pragma once

#include <atomic>

// Stand-in for the dashboard, which is the last thing an evaluation's decision
// reaches and the only one still stood in for.
//
// Beside the harness rather than inside one test file because the evaluation's
// own tests and the tick driver's both need it. It is what is left of a much
// larger set: the event logs, the road graphs, the gossip scheduler and the
// beat system were all stood in for here once and are compiled in for real
// now, observed through their own effects instead of through a counter.
namespace NarrativeEngine::Testing
{
    struct DownstreamSpyState
    {
        // Times the dashboard was told the world had moved.
        std::atomic<int> dashboardPushes{0};

        void Reset();
    };

    DownstreamSpyState& DownstreamSpies();
} // namespace NarrativeEngine::Testing
