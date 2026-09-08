#include "EventLogSpies.h"

#include <CombatEventLog.h>
#include <EvaluationPipeline.h>
#include <FineRoads.h>
#include <PluginThread.h>
#include <TravelEventLog.h>
#include <WeatherEventLog.h>

#include <utility>

// See EventLogSpies.h for why these live beside the harness.

namespace NarrativeEngine::Testing
{
    void EventLogSpyState::Reset()
    {
        std::scoped_lock lock(mutex);
        combatPolls = 0;
        weatherPolls = 0;
        travelPolls = 0;
        fineRoadsPolls = 0;
        lastElapsed = 0.0;
        evaluations = 0;
        evaluationInFlight = false;
        combatQueue.clear();
        weatherQueue.clear();
        travelQueue.clear();
        drainCalls = 0;
    }

    EventLogSpyState& EventLogSpies()
    {
        static auto* state = new EventLogSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::CombatEventLog
{
    void Poll(const PluginThread::Token&)
    {
        ++Testing::EventLogSpies().combatPolls;
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        auto& spies = Testing::EventLogSpies();
        ++spies.drainCalls;
        std::scoped_lock lock(spies.mutex);
        return std::exchange(spies.combatQueue, {});
    }
} // namespace NarrativeEngine::CombatEventLog

namespace NarrativeEngine::WeatherEventLog
{
    void Poll(const PluginThread::Token&, double elapsedSeconds)
    {
        auto& spies = Testing::EventLogSpies();
        ++spies.weatherPolls;
        spies.lastElapsed = elapsedSeconds;
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        auto& spies = Testing::EventLogSpies();
        std::scoped_lock lock(spies.mutex);
        return std::exchange(spies.weatherQueue, {});
    }
} // namespace NarrativeEngine::WeatherEventLog

namespace NarrativeEngine::TravelEventLog
{
    void Poll(const PluginThread::Token&, double)
    {
        ++Testing::EventLogSpies().travelPolls;
    }

    std::vector<EventLogUtil::HistoryEntry> DrainHistoryTail()
    {
        auto& spies = Testing::EventLogSpies();
        std::scoped_lock lock(spies.mutex);
        return std::exchange(spies.travelQueue, {});
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
