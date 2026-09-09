#include <GossipTick.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipDispatch.h>
#include <GossipGraph.h>
#include <GossipLog.h>
#include <GossipSim.h>
#include <GossipSpies.h>
#include <GossipThread.h>
#include <GossipWorld.h>
#include <SkyrimNetAPI.h>

#include <FakeSkyrimNet.h>

#include <ThreadRole.h>
#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Tests for the gossip scheduler.
//
// The scheduler's contract is unusually specific and every part of it exists
// because the obvious implementation got something wrong. It does not ask "is
// gossip busy"; it asks how many scheduled in-world times have gone by that it
// has not enqueued yet, and enqueues one stamped job per boundary. Each job
// carries the game day it was supposed to fire at, which is what lets the queue
// back up behind a slow LLM call without the simulation drifting.
//
// Three of the cases below cover decisions the header records as having been
// wrong before. Blanking the schedule at load restarted the whole interval on
// every load, so a player whose sessions were shorter than one interval never
// saw a single tick however many in-game days they played. Preserving it
// instead would owe a day-3 schedule against a day-200 world. And resuming a
// long-abandoned save must owe the boundary once rather than once per missed
// interval, because re-harvesting one memory corpus N times finds the same
// memories N times.
//
// Every collaborator a tick calls is compiled in for real, so the tick is
// observed through what it leaves behind rather than through a record of the
// calls it made: the simulation's own clock says which moment a tick was
// stamped for, the harvest's own counter says whether it swept, and the
// published snapshot says what a reader would see.
//
// That costs one thing worth naming. A cancellation arriving BETWEEN two steps
// of a tick can no longer be staged from here — there is no seam left to raise
// one from. The checkpoints are reached where they can be: the harvest's own
// tests hand it a cancelled handle directly, and GossipDispatch's cover the
// cancellation machinery itself.

namespace
{
    namespace GossipTick = NarrativeEngine::GossipTick;
    namespace GossipDispatch = NarrativeEngine::GossipDispatch;
    namespace GossipThread = NarrativeEngine::GossipThread;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    namespace GossipSim = NarrativeEngine::GossipSim;
    using NarrativeEngine::Testing::BuildGossipWorld;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::GossipSpies;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;
    using NarrativeEngine::Testing::LiveGossipState;
    using NarrativeEngine::Testing::ResetGossipState;

    // How many sweeps the harvest has run this session. The sweep is real now,
    // and this is its own counter — kept in the simulation's state so that the
    // dashboard reads a sweep count and a rumor count from one image.
    std::size_t Sweeps()
    {
        return LiveGossipState().harvest.sweeps;
    }

    // The game day the LIVE simulation was last wound to. The tick sets this
    // before sweeping, so it is the horizon the tick was scheduled for.
    // Deliberately not LastSimulatedGameDay, which reads the published image —
    // the difference between the two is exactly what says whether a publish
    // happened.
    double SimulatedDay()
    {
        return LiveGossipState().simGameDay;
    }

    // Whether a snapshot has been published for readers at all. The tick
    // publishes once, at the end.
    bool Published()
    {
        return GossipSim::Snapshot() != nullptr;
    }

    // The clock the published snapshot carries, which is what a reader sees.
    double PublishedDay()
    {
        const auto snapshot = GossipSim::Snapshot();
        return snapshot ? snapshot->simGameDay : -1.0;
    }

    // Wind the simulation's clock, which is what a loaded save arrives with
    // and what the scheduler anchors its first interval against.
    void SetSimulatedDay(double day)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        GossipThread::detail::JobDispatcher::Invoke(
            [&](const GossipThread::Token& gt) { GossipSim::SetHorizon(gt, day); });
        GossipSim::PublishSnapshot();
    }

    // The stand-in SkyrimNet beside the executable, whose memory system the
    // harvest refuses to sweep without.
    FakeSkyrimNetState& FakeSkyrimNet()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    constexpr auto kTimeout = std::chrono::seconds{5};

    // The scheduler's own cadence check runs on the plugin thread.
    void PollWith(double unpausedElapsedSeconds)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        PluginThread::detail::JobDispatcher::Invoke(
            [&](const PluginThread::Token& pt) { GossipTick::Poll(pt, unpausedElapsedSeconds); });
    }

    // Waits until every tick enqueued so far has run. The dispatcher is FIFO
    // with one worker, so a marker job reaching the front proves it.
    void DrainTicks()
    {
        std::promise<void> marker;
        auto reached = marker.get_future();
        GossipDispatch::EnqueueWork([&](const GossipThread::Token&) { marker.set_value(); });
        REQUIRE(reached.wait_for(kTimeout) == std::future_status::ready);
    }

    // Puts the game clock at a given cumulative day. EventLogUtil derives game
    // seconds from the calendar's days-passed, which is what the scheduler
    // samples.
    void SetGameDay(EngineMock& engine, double day)
    {
        engine.calendar.daysPassed = static_cast<float>(day);
    }

    struct RunningGossip
    {
        RunningGossip()
        {
            GossipDispatch::Start();
        }
        ~RunningGossip()
        {
            GossipDispatch::Stop();
        }

        RunningGossip(const RunningGossip&) = delete;
        RunningGossip& operator=(const RunningGossip&) = delete;
    };
} // namespace

TEST_CASE("GossipTick waits for a graph to exist", "[GossipTick][engine]")
{
    // Deliberately its own case with no world in it. The graph is built once
    // at kDataLoaded and there is no way to take one down, so the window
    // before it exists can only be reached by never building one.
    EngineMock engine;
    const ConfiguredSettings settings{
        "[Gossip]\nbGossipEnabled=1\niGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=1.0\n"};
    const RunningGossip gossip;
    GossipSpies().Reset();
    SetGameDay(engine, 10.0);
    GossipTick::OnSessionStart();

    SECTION("when the graph is not ready yet")
    {
        SECTION("should wait rather than sweep an empty world")
        {
            // The graph is built after a load, and sweeping before it is ready
            // would qualify no actors and spend the boundary anyway.
            REQUIRE_FALSE(GossipGraph::IsReady());
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 0);
        }
    }
}

TEST_CASE("GossipTick::Poll refuses to schedule", "[GossipTick][engine]")
{
    // Happy path, re-run per leaf: gossip on, graph ready, a one-second check
    // cadence and a one-hour in-world interval. Each case removes one piece.
    EngineMock engine;
    const ConfiguredSettings settings{
        "[Gossip]\nbGossipEnabled=1\niGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=1.0\n"};
    const RunningGossip gossip;
    GossipSpies().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    // The harvest refuses to sweep without SkyrimNet's memory system and its
    // filtered query, so the wrapper is resolved against the stand-in beside
    // the executable before any tick runs.
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeSkyrimNet().Reset();
    SetGameDay(engine, 10.0);
    GossipTick::OnSessionStart();

    SECTION("when gossip is turned off")
    {
        const ConfiguredSettings off{
            "[Gossip]\nbGossipEnabled=0\niGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=1.0\n"};

        SECTION("should not even sample the clock")
        {
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 0);
        }
    }

    SECTION("when too little real time has passed")
    {
        SECTION("should not sample the clock yet")
        {
            // The cadence check is the cheap gate in front of everything else,
            // so it has to hold before any game-clock work happens.
            PollWith(0.25);
            DrainTicks();
            REQUIRE(Sweeps() == 0);
        }
    }
}

TEST_CASE("GossipTick::Poll anchors a new schedule", "[GossipTick][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{
        "[Gossip]\nbGossipEnabled=1\niGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=24.0\n"};
    const RunningGossip gossip;
    GossipSpies().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    // The harvest refuses to sweep without SkyrimNet's memory system and its
    // filtered query, so the wrapper is resolved against the stand-in beside
    // the executable before any tick runs.
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeSkyrimNet().Reset();

    SECTION("when this world has never run a tick")
    {
        ResetGossipState();
        SetGameDay(engine, 10.0);
        GossipTick::OnSessionStart();

        SECTION("should wait a full interval rather than sweep immediately")
        {
            // Firing on the first check would sweep a world the player has not
            // touched this session, dating rumors from a moment nothing
            // happened at.
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 0);
        }

        SECTION("should fire once the interval has passed in world")
        {
            PollWith(60.0);
            SetGameDay(engine, 11.5);
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 1);
        }
    }

    SECTION("when the saved world last ticked an interval ago")
    {
        // The case the "blank it on load" version got wrong. The schedule is
        // re-derived from the persisted simulation clock, so in-world time that
        // passed while gossip was not running still counts.
        ResetGossipState();
        SetSimulatedDay(9.0);
        SetGameDay(engine, 10.5);
        GossipTick::OnSessionStart();

        SECTION("should fire on the first check rather than restart the interval")
        {
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 1);
        }
    }

    SECTION("when the saved world has been abandoned for a long time")
    {
        // Twenty intervals owed. Replaying each would re-harvest one memory
        // corpus twenty times and find the same memories twenty times; the
        // carrier steps in between are still drained, because Advance processes
        // each queued event at its own due time rather than at the horizon.
        ResetGossipState();
        SetSimulatedDay(10.0);
        SetGameDay(engine, 30.0);
        GossipTick::OnSessionStart();

        SECTION("should owe the boundary once rather than once per missed interval")
        {
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 1);
        }

        SECTION("should stamp the tick for the present")
        {
            PollWith(60.0);
            DrainTicks();
            REQUIRE(SimulatedDay() >= 30.0);
        }
    }

    SECTION("when the loaded world's clock is behind the anchor")
    {
        // An older save, or a console time change. Nothing is owed against a
        // schedule belonging to a world further along than this one.
        ResetGossipState();
        SetSimulatedDay(50.0);
        SetGameDay(engine, 10.0);
        GossipTick::OnSessionStart();

        SECTION("should start a fresh interval instead")
        {
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() == 0);
        }
    }
}

TEST_CASE("GossipTick::Poll enqueues one job per crossed boundary", "[GossipTick][engine]")
{
    // One-hour intervals so several boundaries fit inside a short jump.
    EngineMock engine;
    const ConfiguredSettings settings{
        "[Gossip]\nbGossipEnabled=1\niGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=1.0\n"};
    const RunningGossip gossip;
    GossipSpies().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    // The harvest refuses to sweep without SkyrimNet's memory system and its
    // filtered query, so the wrapper is resolved against the stand-in beside
    // the executable before any tick runs.
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeSkyrimNet().Reset();
    ResetGossipState();
    SetGameDay(engine, 10.0);
    GossipTick::OnSessionStart();
    // Seed the schedule: the first check with no anchor sets the next due time
    // one interval out and enqueues nothing.
    PollWith(60.0);

    SECTION("when several boundaries have gone by")
    {
        // Three hours later, with a one-hour interval.
        SetGameDay(engine, 10.0 + 3.0 / 24.0);
        PollWith(60.0);
        DrainTicks();

        SECTION("should run one tick per boundary")
        {
            // Not one tick that has three times as much to do. Each carries its
            // own stamp and reads the world as of that moment.
            REQUIRE(Sweeps() == 3);
        }

        SECTION("should stamp each for its own scheduled moment")
        {
            // The last of the three winds the clock to the third boundary, not
            // to the moment the run happened to reach. A tick stamped `now`
            // instead of its scheduled time would date every rumor it seeds to
            // the same instant, collapsing three hours of history into one —
            // and would leave the clock reading the present rather than the
            // boundary it was due for.
            const double thirdBoundary = 10.0 + 3.0 / 24.0;
            REQUIRE(SimulatedDay() < thirdBoundary + 1e-6);
            REQUIRE(SimulatedDay() > 10.0);
        }
    }

    SECTION("when no boundary has gone by")
    {
        SetGameDay(engine, 10.0 + 0.5 / 24.0);
        PollWith(60.0);
        DrainTicks();

        SECTION("should enqueue nothing")
        {
            REQUIRE(Sweeps() == 0);
        }
    }

    SECTION("when the clock jumps backwards further than drift explains")
    {
        SetGameDay(engine, 2.0);
        PollWith(60.0);
        DrainTicks();

        SECTION("should re-base rather than owe nothing forever")
        {
            // A load of an older save. Left alone the schedule would sit a
            // hundred intervals in the future and gossip would never tick
            // again for the rest of the session.
            REQUIRE(Sweeps() == 0);
            SetGameDay(engine, 2.0 + 2.0 / 24.0);
            PollWith(60.0);
            DrainTicks();
            REQUIRE(Sweeps() >= 1);
        }
    }
}

TEST_CASE("GossipTick::Poll caps the backlog", "[GossipTick][engine]")
{
    EngineMock engine;
    // The trace is switched on here and nowhere else in this file: the
    // dropped-tick report is the one thing the scheduler says out loud, and
    // reading it back off the real file is the only way to check it was said.
    const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=1\nbGossipLogEnabled=1\n"
                                      "iGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=1.0\n"};
    const RunningGossip gossip;
    GossipSpies().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    // The harvest refuses to sweep without SkyrimNet's memory system and its
    // filtered query, so the wrapper is resolved against the stand-in beside
    // the executable before any tick runs.
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeSkyrimNet().Reset();
    NarrativeEngine::Testing::ClearGossipTrace();
    NarrativeEngine::GossipLog::OnSessionStart();
    ResetGossipState();
    SetGameDay(engine, 10.0);
    GossipTick::OnSessionStart();
    PollWith(60.0);

    SECTION("when a time jump crosses far more boundaries than the cap allows")
    {
        // Twenty hours at a one-hour interval.
        SetGameDay(engine, 10.0 + 20.0 / 24.0);
        PollWith(60.0);
        DrainTicks();

        SECTION("should run no more than the cap")
        {
            // A console time jump must not be able to queue a year of
            // simulation, and there is no value in harvesting the same corpus
            // twenty times in a row.
            REQUIRE(Sweeps() <= GossipTick::kMaxOutstandingTicks);
        }

        SECTION("should say how many it dropped")
        {
            // Dropped ticks are deliberate, which makes them exactly the kind
            // of thing that has to be reported: silence here reads identically
            // to a scheduler that stopped working. Read back off the real trace
            // file, which is what a player would attach to a bug report.
            const auto lines = NarrativeEngine::Testing::GossipTraceLines();
            const bool reported = std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
                return line.find("dropped") != std::string::npos;
            });
            REQUIRE(reported);
        }
    }
}

TEST_CASE("GossipTick runs a tick in order", "[GossipTick][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{
        "[Gossip]\nbGossipEnabled=1\niGossipTickIntervalSeconds=1\nfGossipHarvestIntervalGameHours=1.0\n"};
    const RunningGossip gossip;
    GossipSpies().Reset();
    BuildGossipWorld(engine);
    GossipGraph::Initialize();
    // The harvest refuses to sweep without SkyrimNet's memory system and its
    // filtered query, so the wrapper is resolved against the stand-in beside
    // the executable before any tick runs.
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeSkyrimNet().Reset();
    ResetGossipState();
    SetSimulatedDay(9.0);
    SetGameDay(engine, 10.0);
    GossipTick::OnSessionStart();

    SECTION("when a tick runs to completion")
    {
        PollWith(60.0);
        DrainTicks();

        SECTION("should wind the simulation to the tick's own moment")
        {
            // Set before the harvest rather than after it. A rumor seeded
            // during this tick dates itself from the simulation clock, so
            // stamping afterwards would date every one of them a whole
            // interval in the past.
            REQUIRE(SimulatedDay() > 9.0);
            REQUIRE(Sweeps() == 1);
        }

        SECTION("should publish what it finished with")
        {
            // A snapshot taken mid-drain shows some carriers stepped and some
            // not, with transmission counts that do not match the carrier set
            // they came from. One publish, at the end, from the finished
            // image.
            REQUIRE(Published());
            REQUIRE(PublishedDay() == SimulatedDay());
        }
    }

    SECTION("when the sweep cannot run")
    {
        // SkyrimNet's memory system is not up, which is what the window after
        // a load looks like. The tick still advances and publishes: the
        // carrier steps already queued are due whether or not new rumors were
        // seeded this time round.
        FakeSkyrimNet().memorySystemReady = false;

        SECTION("should still advance and publish")
        {
            PollWith(60.0);
            DrainTicks();
            REQUIRE(SimulatedDay() > 9.0);
            REQUIRE(PublishedDay() == SimulatedDay());
        }
    }
}
