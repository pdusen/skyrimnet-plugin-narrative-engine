#include "EvaluationSpies.h"

#include <EvaluationPipeline.h>
#include <PluginThread.h>

// See EvaluationSpies.h for why this lives beside the harness.

namespace NarrativeEngine::Testing
{
    void EvaluationSpyState::Reset()
    {
        evaluations = 0;
        inFlight = false;
    }

    EvaluationSpyState& EvaluationSpies()
    {
        static auto* state = new EvaluationSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::EvaluationPipeline
{
    bool IsEvaluationInFlight()
    {
        return Testing::EvaluationSpies().inFlight.load();
    }

    void BeginEvaluation(const PluginThread::Token&)
    {
        ++Testing::EvaluationSpies().evaluations;
    }
} // namespace NarrativeEngine::EvaluationPipeline
