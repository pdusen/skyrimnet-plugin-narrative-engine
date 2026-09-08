#include "GossipSpies.h"

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
        stampedHorizons.clear();
        npcNames.clear();
        locationNames.clear();
        graphReadyQueries = 0;
        graphReady = true;
        sweepSucceeds = true;
        lastSimulatedGameDay = -1.0;
        cancelAfter.clear();
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

namespace NarrativeEngine::GossipGraph
{
    bool IsReady()
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        ++spies.graphReadyQueries;
        return spies.graphReady;
    }

    // Names the trace lines render. Answered from a table a test fills in, so a
    // line can be checked for the name a reader would actually see rather than
    // for a FormID.
    const std::string& NpcName(RE::FormID formID)
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        static const std::string empty;
        const auto it = spies.npcNames.find(formID);
        return it == spies.npcNames.end() ? empty : it->second;
    }

    const std::string& LocationName(RE::FormID formID)
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        static const std::string empty;
        const auto it = spies.locationNames.find(formID);
        return it == spies.locationNames.end() ? empty : it->second;
    }
} // namespace NarrativeEngine::GossipGraph

namespace NarrativeEngine::GossipSim
{
    bool AdoptPendingState()
    {
        Testing::GossipSpies().Record("adopt");
        return true;
    }

    void SetHorizon(const GossipThread::Token&, double asOfGameDay)
    {
        auto& spies = Testing::GossipSpies();
        spies.Record("horizon");
        std::scoped_lock lock(spies.mutex);
        spies.stampedHorizons.push_back(asOfGameDay);
    }

    void Advance(const GossipThread::Token&, double, const GossipDispatch::CancellationHandle&)
    {
        Testing::GossipSpies().Record("advance");
    }

    void PublishSnapshot()
    {
        Testing::GossipSpies().Record("publish");
    }

    double LastSimulatedGameDay()
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.lastSimulatedGameDay;
    }
} // namespace NarrativeEngine::GossipSim

namespace NarrativeEngine::GossipHarvest
{
    bool RunSweep(const GossipThread::Token&, double, const GossipDispatch::CancellationHandle& cancel)
    {
        auto& spies = Testing::GossipSpies();
        spies.Record("sweep");
        bool cancelHere = false;
        bool succeeds = true;
        {
            std::scoped_lock lock(spies.mutex);
            cancelHere = spies.cancelAfter == "sweep";
            succeeds = spies.sweepSucceeds;
        }
        // Lets a case put a cancellation between two steps, which is where the
        // real one arrives: a load cancels while a tick is mid-flight.
        if (cancelHere && cancel) {
            cancel->Cancel();
        }
        return succeeds;
    }
} // namespace NarrativeEngine::GossipHarvest
