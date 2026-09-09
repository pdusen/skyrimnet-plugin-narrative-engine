#include "DownstreamSpies.h"

#include <BeatSystem.h>
#include <DashboardUIManager.h>
#include <DecisionLog.h>
#include <PluginThread.h>
#include <Snapshot.h>

#include <utility>

// See DownstreamSpies.h for why these live beside the harness.

namespace NarrativeEngine::Testing
{
    void DownstreamSpyState::Reset()
    {
        // Anything still held is finished first. The flag that says an
        // evaluation is in flight lives in the pipeline and outlives any one
        // test, so a reset that dropped a held callback would leave every
        // later evaluation in the process skipping forever.
        holdCompletion = false;
        ReleaseHeld();

        std::scoped_lock lock(mutex);
        beatsConsidered = 0;
        dashboardPushes = 0;
        lastAction.clear();
        lastReasoning.clear();
        holdCompletion = false;
        held_ = nullptr;
    }

    void DownstreamSpyState::ReleaseHeld()
    {
        std::function<void()> release;
        {
            std::scoped_lock lock(mutex);
            release = std::move(held_);
            held_ = nullptr;
        }
        if (release) {
            release();
        }
    }

    void DownstreamSpyState::RecordHandoff(std::string action, std::string reasoning, std::function<void()> onComplete)
    {
        bool runNow = true;
        {
            std::scoped_lock lock(mutex);
            lastAction = std::move(action);
            lastReasoning = std::move(reasoning);
            if (holdCompletion.load()) {
                held_ = std::move(onComplete);
                runNow = false;
            }
        }
        ++beatsConsidered;
        if (runNow && onComplete) {
            onComplete();
        }
    }

    DownstreamSpyState& DownstreamSpies()
    {
        static auto* state = new DownstreamSpyState();
        return *state;
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::BeatSystem
{
    void ConsiderBeat(const PluginThread::Token&,
                      Snapshot,
                      DecisionLog::DecisionRecord record,
                      std::function<void()> onComplete)
    {
        Testing::DownstreamSpies().RecordHandoff(record.beatSelected, record.narrativeNote, std::move(onComplete));
    }
} // namespace NarrativeEngine::BeatSystem

namespace NarrativeEngine::DashboardUIManager
{
    void PushFullState()
    {
        ++Testing::DownstreamSpies().dashboardPushes;
    }
} // namespace NarrativeEngine::DashboardUIManager
