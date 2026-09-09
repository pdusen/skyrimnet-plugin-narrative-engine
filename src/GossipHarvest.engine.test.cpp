#include <GossipHarvest.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <GossipClaims.h>
#include <GossipGraph.h>
#include <GossipSpies.h>
#include <GossipWorld.h>
#include <SkyrimNetAPI.h>
#include <ThreadRole.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Tests for turning memories into rumors.
//
// There is no way to ask SkyrimNet what happened lately. Its memory query is
// strictly per-actor, so a province-wide sweep would be hundreds of calls, most
// of them returning nothing. The design's answer is not to pick the most
// interesting people — that was the first attempt, and because "interesting" is
// measured by interaction with the player, the mill spent a fortnight reporting
// the player's own itinerary back at them while eight hundred other people were
// structurally incapable of ever seeding anything.
//
// So the population is split into buckets by a hash of each person's base
// FormID, and a sweep draws one bucket and examines everybody in it. Everyone
// gets a turn, just not at once, and a farmer in Rorikstead has the same
// prospect as the Arch-Mage. A bucket whose people hold nothing produces no
// rumor and the sweep does NOT scan on to a fuller one — doing that would
// re-concentrate the whole province onto the handful of people who have
// memories early in a playthrough, which is the exact pathology the buckets
// exist to remove.
//
// The other thing here is the feedback guard. Gossip writes memories; if those
// could seed, a rumor reaching twenty people would write forty memories and
// become forty rumors without bound. The guard is a tag on the query itself,
// re-checked on every row that comes back — not because the re-check is
// expected to fire, but because a row that trips it means the server-side
// filter did not hold, and the only symptom of that is a mill that will not
// stop.
//
// Almost every rejection below is nominally impossible for the same reason: the
// query already asked for none of it. They are covered anyway, because "the
// filter did not hold" is a real state and the difference between catching it
// and not is unbounded output.

namespace
{
    namespace GossipHarvest = NarrativeEngine::GossipHarvest;
    namespace GossipClaims = NarrativeEngine::GossipClaims;
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    namespace GossipThread = NarrativeEngine::GossipThread;
    namespace GossipDispatch = NarrativeEngine::GossipDispatch;
    namespace SkyrimNet = NarrativeEngine::SkyrimNetAPI;
    using NarrativeEngine::Testing::BuildGossipWorld;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::GossipSpies;
    using NarrativeEngine::Testing::GossipWorld;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;
    using NarrativeEngine::Testing::ResetGossipState;

    // One bucket, so every sweep draws the whole population and a case never
    // depends on which bucket the hash put somebody in.
    constexpr const char* kSettings = "[Gossip]\nbGossipEnabled=1\nbGossipHarvestEnabled=1\n"
                                      "iGossipHarvestBuckets=1\nfGossipMinMemoryImportance=0.45\n"
                                      "fGossipHarvestWindowDays=50\n";

    // The game day the sweeps below run as of, and the memory clock they are
    // measured against. Ten days in, so a memory can be older or newer.
    constexpr double kToday = 10.0;
    constexpr double kTodaySeconds = kToday * 86400.0;

    FakeSkyrimNetState& FakeLLM()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    template <std::size_t N> void SetJson(char (&dest)[N], const std::string& text)
    {
        std::size_t i = 0;
        for (; i + 1 < N && i < text.size(); ++i)
            dest[i] = text[i];
        dest[i] = '\0';
    }

    // One memory row, in the shape the endpoint actually returns — which is
    // not the shape its own header documents.
    nlohmann::json MemoryRow(std::int64_t id, const char* content, double importance = 0.8, double ageDays = 1.0)
    {
        nlohmann::json row;
        row["id"] = id;
        row["content"] = content;
        row["importance_score"] = importance;
        row["game_time"] = kTodaySeconds - ageDays * 86400.0;
        row["type"] = "EXPERIENCE";
        row["tags"] = nlohmann::json::array();
        row["related_event_ids"] = nlohmann::json::array({id * 10});
        return row;
    }

    void SetMemories(const nlohmann::json& rows)
    {
        SetJson(FakeLLM().queryMemoriesJson, rows.dump());
    }

    template <class Fn> void OnGossipThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        GossipThread::detail::JobDispatcher::Invoke([&](const GossipThread::Token& gt) { body(gt); });
    }

    bool Sweep(const GossipDispatch::CancellationHandle& cancel = {})
    {
        bool swept = false;
        OnGossipThread([&](const GossipThread::Token& gt) { swept = GossipHarvest::RunSweep(gt, kToday, cancel); });
        return swept;
    }

    // What the sweep handed on to the content layer, which is the sweep's
    // whole output: the memories it thought were worth a rumor.
    std::size_t Offered()
    {
        auto& spies = GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return spies.seeded.size();
    }

    bool OfferedMemory(std::int64_t id)
    {
        auto& spies = GossipSpies();
        std::scoped_lock lock(spies.mutex);
        return std::any_of(
            spies.seeded.begin(), spies.seeded.end(), [&](const auto& seed) { return seed.sourceMemoryId == id; });
    }
} // namespace

TEST_CASE("GossipHarvest turns a memory into a rumor", "[GossipHarvest][engine]")
{
    // Happy path, re-run per leaf: a world with a graph, SkyrimNet up, and one
    // memory worth telling. Each case changes what comes back from the query.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    engine.calendar.daysPassed = static_cast<float>(kToday);
    GossipSpies().Reset();
    ResetGossipState();
    REQUIRE(SkyrimNet::Initialize());
    FakeLLM().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    REQUIRE(GossipGraph::IsReady());
    SetMemories(nlohmann::json::array({MemoryRow(101, "A College mage was caught after hours.")}));
    // The content layer answers the evaluation and the composition from one
    // response, so a verdict and its bands travel together.
    SetJson(FakeLLM().promptResponse, R"({"verdict":"seed","bands":["a","b","c"]})");

    SECTION("when somebody in the drawn bucket holds something worth telling")
    {
        const bool swept = Sweep();

        SECTION("should run the sweep")
        {
            REQUIRE(swept);
        }

        SECTION("should turn it into a rumor")
        {
            REQUIRE(Offered() == 1);
            REQUIRE(OfferedMemory(101));
        }
    }

    SECTION("when the memory is one gossip wrote itself")
    {
        auto rows = nlohmann::json::array({MemoryRow(101, "Somebody heard a rumor going round.")});
        rows[0]["tags"] = nlohmann::json::array({"ne_gossip"});
        SetMemories(rows);
        Sweep();

        SECTION("should refuse it")
        {
            // The feedback guard, and the only rejection here whose failure is
            // unbounded: gossip seeding from gossip is a mill that will not
            // stop. The query already excludes these, so a row arriving with
            // the tag means the server-side filter did not hold.
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory is too dull to repeat")
    {
        SetMemories(nlohmann::json::array({MemoryRow(101, "She swept the floor.", 0.1)}));
        Sweep();

        SECTION("should refuse it")
        {
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory is older than the window")
    {
        SetMemories(nlohmann::json::array({MemoryRow(101, "Something from last season.", 0.8, 400.0)}));
        Sweep();

        SECTION("should refuse it")
        {
            // Nobody starts a rumor about something a year old, and the window
            // is also what keeps a claim from outliving the memory that earned
            // it.
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory is dated after the sweep's horizon")
    {
        SetMemories(nlohmann::json::array({MemoryRow(101, "Something that has not happened yet.", 0.8, -5.0)}));
        Sweep();

        SECTION("should refuse it")
        {
            // A tick that runs late harvests the world as it stood when it was
            // due, so anything written since is not its to see.
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory carries no game time at all")
    {
        auto rows = nlohmann::json::array({MemoryRow(101, "Something undated.")});
        rows[0].erase("game_time");
        SetMemories(rows);
        Sweep();

        SECTION("should refuse it")
        {
            // Guarded explicitly rather than left to a default of zero, which
            // would read as "written at the very start of the game" and look
            // merely old.
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory is somebody's diary entry")
    {
        SetMemories(nlohmann::json::array({MemoryRow(101, "Diary Entry: today I went to the market.")}));
        Sweep();

        SECTION("should refuse it")
        {
            // SkyrimNet folds diaries into the same table, typed like any
            // other experience, so the content prefix is the only thing that
            // tells them apart. A rumor sourced from one is a rumor about
            // somebody's journal.
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory has nothing written in it")
    {
        SetMemories(nlohmann::json::array({MemoryRow(101, "")}));
        Sweep();

        SECTION("should refuse it")
        {
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the memory is already claimed")
    {
        OnGossipThread([](const GossipThread::Token& gt) { GossipClaims::Claim(gt, 101, {}, kToday); });
        Sweep();

        SECTION("should refuse it")
        {
            // Somebody already turned this into a rumor, or decided not to.
            REQUIRE(Offered() == 0);
        }
    }

    SECTION("when the query answers with nothing usable")
    {
        SetJson(FakeLLM().queryMemoriesJson, "not json at all");
        Sweep();

        SECTION("should offer nothing")
        {
            REQUIRE(Offered() == 0);
        }
    }
}

TEST_CASE("GossipHarvest refuses to sweep a world it cannot read", "[GossipHarvest][engine]")
{
    // Each of these leaves the boundary owed rather than consuming it, which is
    // the scheduler's cue to try the same tick again.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    engine.calendar.daysPassed = static_cast<float>(kToday);
    GossipSpies().Reset();
    ResetGossipState();
    REQUIRE(SkyrimNet::Initialize());
    FakeLLM().Reset();

    SECTION("when the memory system is not up")
    {
        BuildGossipWorld(engine);
        GossipGraph::Initialize();
        FakeLLM().memorySystemReady = false;

        SECTION("should report the sweep did not run")
        {
            REQUIRE_FALSE(Sweep());
        }
    }

    SECTION("when SkyrimNet is too old for the filtered query")
    {
        BuildGossipWorld(engine);
        GossipGraph::Initialize();
        FakeLLM().version = 1;

        SECTION("should report the sweep did not run")
        {
            // On an older build the exclusion the feedback guard depends on
            // does not exist, and every query answers empty — a world where
            // nothing memorable ever happens, indistinguishable in the trace
            // from a quiet save. Refusing says which it is.
            REQUIRE_FALSE(Sweep());
        }
    }
}

TEST_CASE("GossipHarvest refuses to sweep without a graph", "[GossipHarvest][engine]")
{
    // Its own case with no world in it: the graph is built once per process
    // and cannot be taken down, so the window before it exists is only
    // reachable by never building one.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    engine.calendar.daysPassed = static_cast<float>(kToday);
    GossipSpies().Reset();
    ResetGossipState();
    REQUIRE(SkyrimNet::Initialize());
    FakeLLM().Reset();

    SECTION("when the graph has not been built")
    {
        SECTION("should report the sweep did not run")
        {
            REQUIRE_FALSE(GossipGraph::IsReady());
            REQUIRE_FALSE(Sweep());
        }
    }
}

TEST_CASE("GossipHarvest stops when the world is replaced", "[GossipHarvest][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    engine.calendar.daysPassed = static_cast<float>(kToday);
    GossipSpies().Reset();
    ResetGossipState();
    REQUIRE(SkyrimNet::Initialize());
    FakeLLM().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    SetMemories(nlohmann::json::array({MemoryRow(101, "A College mage was caught after hours.")}));
    SetJson(FakeLLM().promptResponse, R"({"verdict":"seed","bands":["a","b","c"]})");

    SECTION("when the tick was cancelled before it ran")
    {
        auto cancel = std::make_shared<GossipDispatch::CancellationToken>();
        cancel->Cancel();
        Sweep(cancel);

        SECTION("should seed nothing into a world that has gone")
        {
            // A load cancels the tick that was in flight. Anything seeded past
            // that point goes into the incoming save, whose own state has no
            // rumor to match it.
            REQUIRE(Offered() == 0);
        }
    }
}
