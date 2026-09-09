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
        contactShare = 1.0f;
        contactAvailability = "everybody";
        lastSimulatedGameDay = -1.0;
        cancelAfter.clear();
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

    GossipState& LiveGossipState()
    {
        return *LiveStorage();
    }

    GossipState& StagedGossipState()
    {
        return *StagedStorage();
    }

    void ResetGossipState()
    {
        *LiveStorage() = GossipState{};
        *StagedStorage() = GossipState{};
    }
} // namespace NarrativeEngine::Testing

namespace NarrativeEngine::GossipSim
{
    float AvailableContactShare(const GossipThread::Token&, RE::FormID)
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.contactShare;
    }

    std::string DescribeContactAvailability(const GossipThread::Token&, RE::FormID)
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.contactAvailability;
    }

    std::uint32_t SeedRumor(const GossipThread::Token&,
                            RE::FormID originNpc,
                            float notability,
                            std::int64_t sourceMemoryId,
                            std::vector<std::string> bands)
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        spies.seeded.push_back(
            Testing::GossipSpyState::Seeded{originNpc, notability, sourceMemoryId, std::move(bands)});
        return spies.nextRumorID;
    }

    std::vector<RumorView> GetRumorViews(const GossipState&)
    {
        auto& spies = Testing::GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.circulating;
    }

    GossipState& MutableState(const GossipThread::Token&)
    {
        // The harvest's very first act, and the only thing it does before any
        // of its readiness gates. Recording it is how the scheduler's tests
        // see that a sweep started at all, now that the sweep itself is real.
        Testing::GossipSpies().Record("sweep");

        return Testing::LiveGossipState();
    }

    GossipState& PendingState()
    {
        return Testing::StagedGossipState();
    }

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

    void Advance(const GossipThread::Token&, double, const GossipDispatch::CancellationHandle& cancel)
    {
        auto& spies = Testing::GossipSpies();
        spies.Record("advance");
        bool cancelHere = false;
        {
            std::scoped_lock lock(spies.mutex);
            cancelHere = spies.cancelAfter == "advance";
        }
        // Lets a case put a cancellation between two steps of a tick, which is
        // where a real one arrives: a load lands while the tick is mid-flight.
        // This step is the one that can do it because it is the only one the
        // scheduler hands the handle to.
        if (cancelHere && cancel) {
            cancel->Cancel();
        }
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
