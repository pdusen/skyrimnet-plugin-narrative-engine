#include <GossipLog.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipGraph.h>
#include <GossipSpies.h>
#include <GossipWorld.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Tests for the gossip trace file.
//
// This file is the only record of what the gossip simulation did. Nothing else
// reports a rumour that failed to spread, a sweep that examined a hundred
// memories and kept none, or a carrier that retired one telling short — and the
// header is explicit that the interesting case is a sweep finding nothing,
// which is only diagnosable if the near-misses are named. A trace that silently
// stops being written turns every gossip bug report into guesswork.
//
// The module is exercised for real rather than mocked: it opens a genuine file
// under the directory EngineMock reports as SKSE's log directory, and every
// case reads back what a player would attach to a bug report. That is also what
// makes the rotation checkable at all — five slots shifting on each session
// start is a filesystem behaviour, not a formatting one.
//
// The two name lookups it renders come from GossipGraph, which is stood in for
// out of the graph, which is built here from the shared harness world so a line
// can be checked for the name a reader would actually see.

namespace
{
    namespace GossipLog = NarrativeEngine::GossipLog;
    using NarrativeEngine::Testing::ClearGossipTrace;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    using NarrativeEngine::Testing::BuildGossipWorld;
    using NarrativeEngine::Testing::GossipSpies;
    using NarrativeEngine::Testing::GossipTraceLines;

    // Everyone and everywhere the lines below name. These are the shared
    // harness world's own FormIDs, because the names a line renders now come
    // out of a real graph built over that world rather than from a table.
    constexpr std::uint32_t kYsolda = 0x0001A6A0u;
    constexpr std::uint32_t kCarlotta = 0x0001A6A1u; // Valga, in the harness world
    constexpr std::uint32_t kWhiterun = 0x00013163u;
    constexpr std::uint32_t kFalkreath = 0x00018A56u;

    const std::filesystem::path kLogDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};

    // The first RECORD line containing `needle`, or an empty string. Comment
    // lines are skipped because the file's own legend names every record type,
    // so a search for "SEED" would otherwise always find the legend.
    std::string LineContaining(std::string_view needle)
    {
        const auto lines = GossipTraceLines();
        const auto it = std::find_if(lines.begin(), lines.end(), [&](const std::string& line) {
            return !line.starts_with("#") && line.find(needle) != std::string::npos;
        });
        return it == lines.end() ? std::string{} : *it;
    }

    // The same search including the legend, for the header's own content.
    bool AnyLineContains(std::string_view needle)
    {
        const auto lines = GossipTraceLines();
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

    // Brings the trace up with names resolvable, and closes it again so the
    // file is flushed and readable. Every case that writes uses this.
    struct OpenTrace
    {
        OpenTrace()
        {
            ClearGossipTrace();
            GossipSpies().Reset();
            GossipLog::OnSessionStart();
        }

        ~OpenTrace()
        {
            GossipLog::OnSessionEnd();
        }

        OpenTrace(const OpenTrace&) = delete;
        OpenTrace& operator=(const OpenTrace&) = delete;
    };

    std::size_t RotatedFileCount()
    {
        std::size_t n = 0;
        std::error_code ec;
        for (int slot = 1; slot <= 5; ++slot) {
            const auto path = kLogDir / ("NarrativeEngine_Gossip." + std::to_string(slot) + ".log");
            if (std::filesystem::exists(path, ec))
                ++n;
        }
        return n;
    }
} // namespace

TEST_CASE("GossipLog::OnSessionStart", "[GossipLog][engine]")
{
    // Happy path, re-run per leaf: gossip on and the trace switched on with it.
    EngineMock engine;
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=1\n"};

    SECTION("when the trace is enabled")
    {
        const OpenTrace trace;

        SECTION("should open a file")
        {
            REQUIRE(GossipLog::IsActive());
        }

        SECTION("should write a legend a reader can follow")
        {
            // The file is read by whoever is diagnosing a gossip report, and
            // the line shapes are not self-explanatory. The header is what
            // makes the rest of it readable.
            GossipLog::OnSessionEnd();
            REQUIRE(AnyLineContains("NarrativeEngine gossip trace"));
            REQUIRE(AnyLineContains("BURNOUT"));
        }
    }

    SECTION("when the trace is turned off")
    {
        const ConfiguredSettings off{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=0\n"};
        ClearGossipTrace();
        GossipLog::OnSessionStart();

        SECTION("should open nothing")
        {
            REQUIRE_FALSE(GossipLog::IsActive());
        }

        SECTION("should swallow every line rather than crash")
        {
            // Every emitter goes through one guarded write, so a disabled trace
            // has to be safe rather than merely unused: the simulation calls
            // these on every telling whether anyone is reading or not.
            GossipLog::Note("nothing should reach the disk");
            GossipLog::Seed(1, 0.5f, kYsolda, kWhiterun, 42);
            REQUIRE(GossipTraceLines().empty());
        }
    }

    SECTION("when gossip itself is turned off")
    {
        const ConfiguredSettings off{"[Gossip]\nbGossipEnabled=0\nbGossipLogEnabled=1\n"};
        ClearGossipTrace();
        GossipLog::OnSessionStart();

        SECTION("should open nothing")
        {
            // A trace of a simulation that is not running would be an empty
            // file that looks like a broken one.
            REQUIRE_FALSE(GossipLog::IsActive());
        }
    }
}

TEST_CASE("GossipLog rotates its files", "[GossipLog][engine]")
{
    EngineMock engine;
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=1\n"};
    ClearGossipTrace();

    SECTION("when a second session starts")
    {
        GossipLog::OnSessionStart();
        GossipLog::Note("first session");
        GossipLog::OnSessionEnd();
        GossipLog::OnSessionStart();

        SECTION("should keep the previous session's file")
        {
            // The report almost always arrives after the player has restarted,
            // so the session that actually went wrong is the previous one.
            GossipLog::OnSessionEnd();
            REQUIRE(RotatedFileCount() >= 1);
        }

        SECTION("should start the new one empty")
        {
            GossipLog::Note("second session");
            GossipLog::OnSessionEnd();
            REQUIRE(LineContaining("first session").empty());
            REQUIRE_FALSE(LineContaining("second session").empty());
        }
    }

    SECTION("when more sessions start than there are slots")
    {
        SECTION("should keep no more than the slot count")
        {
            // Bounded on purpose: this file grows a line per examined memory,
            // and an unbounded set of them would fill a player's disk.
            for (int i = 0; i < 8; ++i) {
                GossipLog::OnSessionStart();
                GossipLog::Note("session");
                GossipLog::OnSessionEnd();
            }
            REQUIRE(RotatedFileCount() <= 5);
        }
    }
}

TEST_CASE("GossipLog renders each line shape", "[GossipLog][engine]")
{
    // Happy path, re-run per leaf: an open trace with both names resolvable.
    EngineMock engine;
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=1\n"};
    const OpenTrace trace;

    SECTION("when a rumour is seeded")
    {
        GossipLog::Seed(7, 0.62f, kYsolda, kWhiterun, 4242);
        GossipLog::OnSessionEnd();
        const auto line = LineContaining("SEED");

        SECTION("should name the origin and the settlement")
        {
            REQUIRE(line.find("Ysolda") != std::string::npos);
            REQUIRE(line.find("Whiterun") != std::string::npos);
        }

        SECTION("should carry the source memory id")
        {
            // The one field that ties a rumour back to the memory it came from.
            // Without it a rumour cannot be traced to its cause at all.
            REQUIRE(line.find("4242") != std::string::npos);
        }

        SECTION("should stamp the line with the in-game time")
        {
            // Absolute in-game time, first on the line, because the whole file
            // is read against the player's account of when something happened.
            REQUIRE(line.starts_with("[4E "));
        }
    }

    SECTION("when a telling crosses a hold boundary")
    {
        GossipLog::Tell(7, 2, 0.62f, kYsolda, kCarlotta, "acquaintance", kWhiterun, kWhiterun, kFalkreath);
        GossipLog::OnSessionEnd();
        const auto line = LineContaining("TELL");

        SECTION("should flag the crossing inline")
        {
            // A rumour leaving its hold is the event the whole simulation is
            // for, and it is otherwise indistinguishable from any other
            // telling in a file of thousands.
            REQUIRE(line.find("XHOLD") != std::string::npos);
            REQUIRE(line.find("Whiterun->Falkreath") != std::string::npos);
        }
    }

    SECTION("when a telling stays inside one hold")
    {
        GossipLog::Tell(7, 2, 0.62f, kYsolda, kCarlotta, "acquaintance", kWhiterun, kWhiterun, kWhiterun);
        GossipLog::OnSessionEnd();

        SECTION("should not flag a crossing")
        {
            REQUIRE(LineContaining("XHOLD").empty());
        }
    }

    SECTION("when a telling is wasted")
    {
        GossipLog::Wasted(7, kYsolda, kCarlotta, 3);
        GossipLog::OnSessionEnd();

        SECTION("should say the quota was still spent")
        {
            // A wasted telling consumes a conversation, so it is the difference
            // between a rumour that ran out of people and one that ran out of
            // turns.
            const auto line = LineContaining("WASTED");
            REQUIRE(line.find("already knows") != std::string::npos);
            REQUIRE(line.find("convs_left=3") != std::string::npos);
        }
    }

    SECTION("when a rumour burns out")
    {
        GossipLog::BurnoutStats stats;
        stats.reach = 12;
        stats.depth = 4;
        stats.holds = 2;
        stats.settlements = 3;
        stats.days = 6.5;
        stats.conversations = 40;
        stats.transmissions = 12;
        stats.wasted = 9;
        GossipLog::Burnout(7, stats);
        GossipLog::OnSessionEnd();

        SECTION("should account for every conversation the rumour spent")
        {
            // The summary is the only place the whole life of a rumour appears
            // at once, and the breakdown is what distinguishes "nobody cared"
            // from "everybody already knew".
            const auto line = LineContaining("BURNOUT");
            REQUIRE(line.find("reach=12") != std::string::npos);
            REQUIRE(line.find("12 told") != std::string::npos);
            REQUIRE(line.find("9 knew") != std::string::npos);
        }
    }

    SECTION("when a sweep reports its harvest")
    {
        GossipLog::HarvestStats stats;
        stats.bucket = 2;
        stats.bucketCount = 5;
        stats.bucketPopulation = 30;
        stats.memoriesExamined = 100;
        stats.candidates = 4;
        stats.sentForGeneration = 1;
        stats.rejectedDiary = 40;
        stats.rejectedUnfiltered = 7;
        GossipLog::Harvest(stats);
        GossipLog::OnSessionEnd();
        const auto line = LineContaining("HARVEST");

        SECTION("should say why the rejected memories were rejected")
        {
            // A sweep that found nothing is the interesting case, and it is
            // only diagnosable if the rejections are broken out by reason.
            REQUIRE(line.find("40 diary") != std::string::npos);
            REQUIRE(line.find("7 unfiltered") != std::string::npos);
        }

        SECTION("should say which bucket it drew")
        {
            REQUIRE(line.find("bucket=2/5") != std::string::npos);
        }
    }

    SECTION("when a memory is examined")
    {
        GossipLog::Memory(4242, kYsolda, 0.81f, "candidate");
        GossipLog::OnSessionEnd();

        SECTION("should name the owner and the verdict")
        {
            const auto line = LineContaining("MEMORY");
            REQUIRE(line.find("Ysolda") != std::string::npos);
            REQUIRE(line.find("candidate") != std::string::npos);
        }
    }

    SECTION("when a claim changes hands")
    {
        GossipLog::Claim(4242, "claimed", 3);
        GossipLog::OnSessionEnd();

        SECTION("should report the outstanding count")
        {
            // The ledger leaking is the failure this line exists to catch: a
            // claim that is never released silences that memory for good.
            const auto line = LineContaining("CLAIM");
            REQUIRE(line.find("outstanding=3") != std::string::npos);
        }
    }
}

TEST_CASE("GossipLog names what it can", "[GossipLog][engine]")
{
    EngineMock engine;
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=1\n"};
    const OpenTrace trace;

    SECTION("when a form has no cached name")
    {
        SECTION("should fall back to its form id")
        {
            // A silently anonymous line would be unusable: the reader has no
            // other way to tell which NPC a rumour reached.
            GossipLog::Seed(7, 0.5f, 0x000ABCDEu, kWhiterun, 1);
            GossipLog::OnSessionEnd();
            REQUIRE(LineContaining("0x000ABCDE").empty() == false);
        }
    }

    SECTION("when a line has no settlement")
    {
        SECTION("should render a placeholder rather than a zero id")
        {
            GossipLog::Seed(7, 0.5f, kYsolda, 0, 1);
            GossipLog::OnSessionEnd();
            const auto line = LineContaining("SEED");
            REQUIRE(line.find("@-") != std::string::npos);
        }
    }
}

TEST_CASE("GossipLog::OnSessionEnd", "[GossipLog][engine]")
{
    EngineMock engine;
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=1\n"};

    SECTION("when a session ends")
    {
        ClearGossipTrace();
        GossipSpies().Reset();
        GossipLog::OnSessionStart();
        GossipLog::Note("one");
        GossipLog::Note("two");
        GossipLog::OnSessionEnd();

        SECTION("should record how many lines it wrote")
        {
            // A truncated file is the common failure — the game exited without
            // flushing — and the count is how a reader tells a short trace from
            // a truncated one.
            REQUIRE(AnyLineContains("2 lines"));
        }

        SECTION("should close the file")
        {
            REQUIRE_FALSE(GossipLog::IsActive());
        }

        SECTION("should be safe to call twice")
        {
            GossipLog::OnSessionEnd();
            SUCCEED("ending an already-ended session is a no-op");
        }
    }
}
