#include "DownstreamSpies.h"

#include <DashboardUIManager.h>

// See DownstreamSpies.h for why this lives beside the harness.

namespace NarrativeEngine::Testing
{
    void DownstreamSpyState::Reset()
    {
        dashboardPushes = 0;
    }

    DownstreamSpyState& DownstreamSpies()
    {
        static auto* state = new DownstreamSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::DashboardUIManager
{
    void PushFullState()
    {
        ++Testing::DownstreamSpies().dashboardPushes;
    }
} // namespace NarrativeEngine::DashboardUIManager
