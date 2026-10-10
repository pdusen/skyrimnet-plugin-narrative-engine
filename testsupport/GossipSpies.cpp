#include "GossipSpies.h"

#include <GossipClaims.h>
#include <GossipSim.h>
#include <GossipThread.h>
#include <ThreadRole.h>

#include <GossipDispatch.h>
#include <GossipGraph.h>
#include <GossipHarvest.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <GossipThread.h>

#include <filesystem>
#include <fstream>
#include <system_error>

// See GossipSpies.h for why these live beside the harness rather than in a
// test file.

namespace NarrativeEngine::Testing
{
    void GossipSpyState::Record(std::string step)
    {
        std::scoped_lock lock(mutex);
        calls.push_back(std::move(step));
    }

    std::vector<std::string> GossipSpyState::Calls()
    {
        std::scoped_lock lock(mutex);
        return calls;
    }

    std::size_t GossipSpyState::CountOf(std::string_view step)
    {
        std::scoped_lock lock(mutex);
        std::size_t n = 0;
        for (const auto& c : calls) {
            if (c == step)
                ++n;
        }
        return n;
    }

    void GossipSpyState::Reset()
    {
        std::scoped_lock lock(mutex);
        calls.clear();
        contactShare = 1.0f;
        contactAvailability = "everybody";
        circulating.clear();
        seeded.clear();
        nextRumorID = 1;
    }

    GossipSpyState& GossipSpies()
    {
        static GossipSpyState state;
        return state;
    }

    namespace
    {
        // Mirrors GossipLog's own naming. Duplicated rather than shared on
        // purpose: a test that read the path from the module under test would
        // still pass if the module wrote somewhere nobody looks.
        std::filesystem::path TracePath()
        {
            return std::filesystem::path{"Data/SKSE/Plugins/NarrativeEngineTestLogs"} / "NarrativeEngine_Gossip.log";
        }
    } // namespace

    std::vector<std::string> GossipTraceLines()
    {
        std::vector<std::string> lines;
        std::ifstream in{TracePath()};
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    void ClearGossipTrace()
    {
        GossipLog::OnSessionEnd();
        std::error_code ec;
        for (int slot = 0; slot <= 5; ++slot) {
            auto path = TracePath();
            if (slot > 0) {
                path.replace_filename("NarrativeEngine_Gossip." + std::to_string(slot) + ".log");
            }
            std::filesystem::remove(path, ec);
        }
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::Testing
{
    namespace
    {
        // Leaked, like every other static holding a CommonLibSSE-adjacent
        // container: see the note on the form tables in RelocationMocks.cpp.
        GossipState*& LiveStorage()
        {
            static auto* state = new GossipState();
            return state;
        }

        GossipState*& StagedStorage()
        {
            static auto* state = new GossipState();
            return state;
        }
    } // namespace

    // Now that the simulation is compiled in for real, its own state is the
    // only state. These stay as the harness's name for it so that every test
    // file reaching for "the gossip world" keeps working, and so there is one
    // place to change if that ever stops being true.
    GossipState& LiveGossipState()
    {
        GossipState* live = nullptr;
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        GossipThread::detail::JobDispatcher::Invoke(
            [&](const GossipThread::Token& gt) { live = &GossipSim::MutableState(gt); });
        return *live;
    }

    GossipState& StagedGossipState()
    {
        return GossipSim::PendingState();
    }

    void ResetGossipState()
    {
        // What loading a save into a running session does, in the order the
        // session does it: the two modules each clear their own portion of the
        // staging area, and the next tick adopts the empty result over the
        // live image. A revert alone would leave the outgoing world live,
        // which is correct in the game — the adopt is the second half.
        GossipSim::OnRevert();
        GossipClaims::OnRevert();
        (void)GossipSim::AdoptPendingState();
    }
} // namespace NarrativeEngine::Testing
