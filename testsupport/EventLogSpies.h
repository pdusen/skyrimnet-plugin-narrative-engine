#pragma once

#include <EventLogUtil.h>

#include <atomic>
#include <mutex>
#include <vector>

// Stand-ins for the event logs and the evaluation pipeline that the tick driver
// polls, plus the record of what it polled them with.
//
// Beside the harness rather than in one test file because more than one test
// file needs them: Tick's, which checks that every source is polled and told
// how much time passed, and EventHistoryWriter's, which needs the drains to
// hand it entries. Defined twice they would be a duplicate symbol; defined in
// one of the two they would be a dependency between test files.
//
// Everything here goes away when the event logs get tests of their own and are
// compiled into the mocked-engine target for real. That is the same trade the
// gossip spies make, and it is why the two live side by side.
namespace NarrativeEngine::Testing
{
    struct EventLogSpyState
    {
        std::mutex mutex;

        // Poll counts, one per source the driver is supposed to reach.
        std::atomic<int> combatPolls{0};
        std::atomic<int> weatherPolls{0};
        std::atomic<int> travelPolls{0};
        std::atomic<int> fineRoadsPolls{0};

        // Elapsed seconds the last poll was told about. The logs accumulate
        // against this rather than sampling their own clocks, so a driver that
        // passed zero would freeze every one of them at once.
        std::atomic<double> lastElapsed{0.0};

        // The evaluation pipeline the driver fires into.
        std::atomic<int> evaluations{0};
        std::atomic<bool> evaluationInFlight{false};

        // What each log hands over on its next history drain, and how many
        // times it was asked. Consumed rather than copied, as the real drains
        // are.
        std::vector<EventLogUtil::HistoryEntry> combatQueue;
        std::vector<EventLogUtil::HistoryEntry> weatherQueue;
        std::vector<EventLogUtil::HistoryEntry> travelQueue;
        std::atomic<int> drainCalls{0};

        void Reset();
    };

    EventLogSpyState& EventLogSpies();
} // namespace NarrativeEngine::Testing
