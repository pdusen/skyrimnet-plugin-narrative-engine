#include "EventLogSpies.h"

#include <EvaluationPipeline.h>
#include <FineRoads.h>
#include <PluginThread.h>
#include <TravelEventLog.h>

#include <utility>

// See EventLogSpies.h for why these live beside the harness.

namespace NarrativeEngine::Testing
{
    void EventLogSpyState::Reset()
    {
        std::scoped_lock lock(mutex);
        combatPolls = 0;
        travelPolls = 0;
        fineRoadsPolls = 0;
        lastElapsed = 0.0;
        evaluations = 0;
        evaluationInFlight = false;
        combatQueue.clear();
        travelQueue.clear();
        drainCalls = 0;
        combatPhaseAdvances = 0;
        travelPhaseAdvances = 0;
    }

    EventLogSpyState& EventLogSpies()
    {
        static auto* state = new EventLogSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::TravelEventLog
{
    void Poll(const PluginThread::Token&, double elapsedSeconds)
    {
        auto& spies = Testing::EventLogSpies();
        ++spies.travelPolls;
        spies.lastElapsed = elapsedSeconds;
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        auto& spies = Testing::EventLogSpies();
        // Counted here because this is the last drain still stood in for, and
        // the history writer calls every drain in one flush — so one counter
        // is enough to say the writer's poll was reached.
        ++spies.drainCalls;
        std::scoped_lock lock(spies.mutex);
        return std::exchange(spies.travelQueue, {});
    }

    void OnPhaseAdvanced()
    {
        ++Testing::EventLogSpies().travelPhaseAdvances;
    }
} // namespace NarrativeEngine::TravelEventLog

namespace NarrativeEngine::FineRoads
{
    void Poll(const PluginThread::Token&, double)
    {
        ++Testing::EventLogSpies().fineRoadsPolls;
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
