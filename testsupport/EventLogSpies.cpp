#include "EventLogSpies.h"

#include <EvaluationPipeline.h>
#include <FineRoads.h>
#include <PluginThread.h>

// See EventLogSpies.h for why these live beside the harness.

namespace NarrativeEngine::Testing
{
    void EventLogSpyState::Reset()
    {
        std::scoped_lock lock(mutex);
        fineRoadsPolls = 0;
        lastElapsed = 0.0;
        evaluations = 0;
        evaluationInFlight = false;
    }

    EventLogSpyState& EventLogSpies()
    {
        static auto* state = new EventLogSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::FineRoads
{
    void Poll(const PluginThread::Token&, double elapsedSeconds)
    {
        auto& spies = Testing::EventLogSpies();
        ++spies.fineRoadsPolls;
        // Recorded here because this is the last poll still stood in for. The
        // driver hands every collaborator the same figure, so one is enough to
        // say whether it is passing a real elapsed time or a zero.
        spies.lastElapsed = elapsedSeconds;
    }
} // namespace NarrativeEngine::FineRoads

namespace NarrativeEngine::EvaluationPipeline
{
    bool IsEvaluationInFlight()
    {
        return Testing::EventLogSpies().evaluationInFlight.load();
    }

    void BeginEvaluation(const PluginThread::Token&)
    {
        ++Testing::EventLogSpies().evaluations;
    }
} // namespace NarrativeEngine::EvaluationPipeline
