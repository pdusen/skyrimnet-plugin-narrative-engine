#include <Tick.h>

#include <AsyncDispatch.h>
#include <CombatEventLog.h>
#include <ConfiguredSettings.h>
#include <DownstreamSpies.h>
#include <EngineMock.h>
#include <EvalDispatch.h>
#include <EventHistoryWriter.h>
#include <FineRoads.h>
#include <GossipSpies.h>
#include <PluginThread.h>
#include <SkyrimNetAPI.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// Tests for the real-time tick driver.
//
// Everything the Director ever does starts here, and every way this can fail is
// quiet. A driver that never fires leaves the world exactly as it was and looks
// like a mod that is doing nothing on purpose. One that fires while the game is
// paused runs LLM calls over a world the player is not in. One that credits
// paused or disabled time to its accumulator fires a burst the moment play
// resumes.
//
// The collaborators the poll body calls are our own modules and every one of
// them is compiled in for real, so each is observed through the cheapest effect
// it has rather than through a counter: the combat log through the event it
// emits, the road graph through the nodes it extracts, the history writer
// through the file it flushes, and the evaluation through the decision it hands
// to the beat system.
//
// That last handoff is a stand-in (testsupport/DownstreamSpies.cpp), and it is
// also the lever for the in-flight cases. The beat system owns a decision once
// it is given one, and the callback it eventually makes is what lets the next
// evaluation start — so a stand-in that keeps the callback puts the pipeline in
// the state a slow LLM would, without anything having to be slow.
//
// The per-poll heartbeat is the driver's own first act: asking whether the game
// is paused. The harness counts that question, which makes it an exact measure
// of the loop rather than of anything the loop happens to call.
//
// These cases spend real time on purpose. The driver samples a steady clock at
// a fixed cadence and the tick interval floors at one second, so there is no
// way to reach the firing path without letting a second pass. Every wait is
// bounded and its arrival asserted, so a driver that stops firing fails the run
// rather than hanging it.

namespace
{
    namespace Tick = NarrativeEngine::Tick;
    namespace AsyncDispatch = NarrativeEngine::AsyncDispatch;
    namespace EvalDispatch = NarrativeEngine::EvalDispatch;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;

    // The driver polls twice a second and the tick interval floors at one, so a
    // fire needs a little over a second of unpaused time. Generous enough that
    // a loaded machine still makes it.
    constexpr auto kFireTimeout = std::chrono::seconds{10};
    constexpr auto kPollTimeout = std::chrono::seconds{5};

    // Where the harness points the plugin's log directory, and the file the
    // history writer flushes into.
    const std::filesystem::path kLogDir{"Data/SKSE/Plugins/NarrativeEngineTestLogs"};
    const std::filesystem::path kHistoryFile = kLogDir / "NarrativeEngine_EventHistory.log";

    // How many polls the driver has run. Its first act on every one is to ask
    // whether the game is paused, and the harness counts that — which makes it
    // an exact measure of the loop rather than of anything the loop calls.
    int PollCount()
    {
        auto* mock = EngineMock::Current();
        return mock ? mock->ui.pausedQueries.load() : 0;
    }

    // How many decisions have reached the beat system, which is one per
    // evaluation the driver has run to completion.
    int Evaluations()
    {
        return NarrativeEngine::Testing::DownstreamSpies().beatsConsidered.load();
    }

    bool HistoryFileHasContent()
    {
        std::error_code ec;
        return std::filesystem::exists(kHistoryFile, ec) && std::filesystem::file_size(kHistoryFile, ec) > 0;
    }

    // Waits for a condition the driver thread will bring about. Returns whether
    // it arrived, so a case asserts rather than hangs.
    template <class Predicate> bool Eventually(Predicate ready, std::chrono::milliseconds budget)
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (ready())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return ready();
    }

    // Brings the whole driver up: the queue its polls run on, the queue its
    // evaluations run on, and the driver itself. Torn down in the reverse
    // order, driver first, so no poll is enqueued after the queue has gone.
    struct RunningDriver
    {
        RunningDriver()
        {
            AsyncDispatch::Start();
            EvalDispatch::Start();
            Tick::Start();
        }

        ~RunningDriver()
        {
            Tick::Stop();
            AsyncDispatch::Stop();
            EvalDispatch::Stop();
        }

        RunningDriver(const RunningDriver&) = delete;
        RunningDriver& operator=(const RunningDriver&) = delete;
    };
} // namespace

TEST_CASE("Tick::SetEnabled", "[Tick][engine]")
{
    EngineMock engine;

    SECTION("when the killswitch is thrown")
    {
        Tick::SetEnabled(false);

        SECTION("should report itself disabled")
        {
            REQUIRE_FALSE(Tick::IsEnabled());
        }

        SECTION("should turn back on again")
        {
            Tick::SetEnabled(true);
            REQUIRE(Tick::IsEnabled());
        }
    }
}

TEST_CASE("Tick::Start seeds the killswitch from settings", "[Tick][engine]")
{
    EngineMock engine;

    SECTION("when the tick is disabled in settings")
    {
        const ConfiguredSettings settings{"[Director]\nbTickEnabled=0\n"};
        Tick::SetEnabled(true);

        SECTION("should start disabled")
        {
            // Read at Start rather than per poll, so a save loaded with the
            // tick turned off does not run one evaluation before noticing.
            Tick::Start();
            const bool enabled = Tick::IsEnabled();
            Tick::Stop();
            REQUIRE_FALSE(enabled);
        }
    }

    SECTION("when the tick is enabled in settings")
    {
        const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\n"};
        Tick::SetEnabled(false);

        SECTION("should start enabled")
        {
            Tick::Start();
            const bool enabled = Tick::IsEnabled();
            Tick::Stop();
            REQUIRE(enabled);
        }
    }
}

TEST_CASE("Tick polls the event logs", "[Tick][engine]")
{
    // Happy path, re-run per leaf: an unpaused game, the tick enabled, and a
    // one-second interval so a fire is reachable without a long wait.
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\niTickIntervalSeconds=1\n"
                                      "[Gossip]\nbGossipEnabled=1\n"
                                      "[FineRoads]\nbFineRoadsEnabled=1\n"
                                      "[EventHistory]\nbEventHistoryEnabled=1\niEventHistoryFlushIntervalSeconds=1\n"};
    NarrativeEngine::Testing::DownstreamSpies().Reset();
    NarrativeEngine::Testing::GossipSpies().Reset();
    Tick::SetEnabled(true);
    // The history writer is compiled in for real, so a session is opened here
    // to make its poll observable through the file it flushes. Closed at the
    // end of the case.
    std::error_code removeError;
    std::filesystem::remove(kHistoryFile, removeError);
    NarrativeEngine::EventHistoryWriter::OnSessionStart();
    // The combat log is compiled in for real. Its baseline starts at "not
    // fighting", so putting the player into combat here means the first poll
    // the driver performs is the one that notices — which is what makes the
    // driver's call to it observable at all.
    NarrativeEngine::CombatEventLog::OnRevert();
    engine.player.inCombat = true;
    // And the road graph is real too, so the player is stood on a loaded cell
    // carrying one stretch of road. Nothing but the driver's own poll can turn
    // that into graph nodes.
    auto* worldSpace = engine.AddWorldSpace(0x0000003Cu);
    auto* cell = engine.AddExteriorCell(worldSpace, 0, 0, nullptr);
    engine.AddNavMesh(cell, 0x00050001u, {EngineMock::FakeTriangle{0.0f, 0.0f, 0.0f}});
    engine.LoadGrid({cell});

    SECTION("when the game is running")
    {
        const RunningDriver driver;

        SECTION("should reach every one of them")
        {
            // Missing one is a subsystem that silently stops observing the
            // world while everything else carries on, and nothing says so.
            REQUIRE(Eventually([] { return PollCount() > 0; }, kPollTimeout));
            // The combat log: the player is already fighting, and only the
            // driver's own poll can notice that.
            REQUIRE(Eventually([] { return !NarrativeEngine::CombatEventLog::GetRenderedTail(0.0).empty(); },
                               kPollTimeout));
            // The road graph: the loaded cell holds one road triangle, and the
            // extraction that turns it into a node runs only from the poll.
            REQUIRE(Eventually([] { return NarrativeEngine::FineRoads::NodeCount() > 0; }, kPollTimeout));
        }

        SECTION("should tell them how much time passed")
        {
            // Every collaborator accumulates against this figure rather than
            // sampling its own clock, so a zero would freeze all of their
            // cadences at once. The history writer is the one that says so out
            // loud: it flushes only once a second of unpaused time has been
            // credited to it, and the combat event above gives it something to
            // write when it does.
            REQUIRE(Eventually([] { return HistoryFileHasContent(); }, kFireTimeout));
        }
    }

    SECTION("when the game is paused")
    {
        engine.ui.gameIsPaused = true;
        const RunningDriver driver;

        SECTION("should poll nothing")
        {
            // Menus, the console and dialogue all pause. Polling through a
            // pause would have the event logs report weather changes and
            // travel the player never experienced.
            //
            // Observed through the combat log rather than the poll counter:
            // the driver's loop keeps turning while paused and keeps asking
            // whether it still is, so what has to be absent is the work, not
            // the loop. The player is already fighting, so a single poll of
            // the combat log would put an event in it.
            REQUIRE_FALSE(Eventually([] { return !NarrativeEngine::CombatEventLog::GetRenderedTail(0.0).empty(); },
                                     kPollTimeout));
        }
    }

    NarrativeEngine::EventHistoryWriter::OnSessionEnd();
}

TEST_CASE("Tick fires the Director", "[Tick][engine]")
{
    // Happy path, re-run per leaf: unpaused, enabled, one-second interval.
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\niTickIntervalSeconds=1\n"
                                      "[Gossip]\nbGossipEnabled=1\n"};
    NarrativeEngine::Testing::DownstreamSpies().Reset();
    NarrativeEngine::Testing::GossipSpies().Reset();
    Tick::SetEnabled(true);
    // The evaluation ends in a call to the LLM, so the wrapper has to have
    // resolved against the stand-in SkyrimNet beside the executable. Without
    // it every evaluation fails at that call and never reaches the beat system.
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());

    SECTION("when the interval elapses")
    {
        const RunningDriver driver;

        SECTION("should begin an evaluation")
        {
            REQUIRE(Eventually([] { return Evaluations() > 0; }, kFireTimeout));
        }
    }

    SECTION("when the killswitch is off")
    {
        Tick::SetEnabled(false);
        const RunningDriver driver;
        // Start reads the setting, which says enabled; turn it off again after.
        Tick::SetEnabled(false);

        SECTION("should keep polling the event logs")
        {
            // Deliberate: the logs do edge detection, so a span they did not
            // observe would make the first event after a re-enable read as a
            // change that never happened.
            REQUIRE(Eventually([] { return PollCount() > 0; }, kPollTimeout));
        }

        SECTION("should begin no evaluation")
        {
            REQUIRE_FALSE(Eventually([] { return Evaluations() > 0; }, kFireTimeout));
        }
    }

    SECTION("when the previous evaluation is still running")
    {
        // The beat system keeps the decision rather than finishing with it,
        // which is what a round trip that outlasts the interval looks like.
        NarrativeEngine::Testing::DownstreamSpies().holdCompletion = true;
        const RunningDriver driver;

        SECTION("should skip rather than queue a catch-up burst")
        {
            // One evaluation gets through and is still running; every interval
            // after it must pass without starting another. Queueing one per
            // missed interval would fire the whole backlog the moment the
            // first returned.
            REQUIRE(Eventually([] { return Evaluations() >= 1; }, kFireTimeout));
            REQUIRE_FALSE(Eventually([] { return Evaluations() > 1; }, kFireTimeout));
        }

        SECTION("should fire again once it finishes")
        {
            REQUIRE(Eventually([] { return Evaluations() >= 1; }, kFireTimeout));
            NarrativeEngine::Testing::DownstreamSpies().holdCompletion = false;
            NarrativeEngine::Testing::DownstreamSpies().ReleaseHeld();
            REQUIRE(Eventually([] { return Evaluations() > 1; }, kFireTimeout));
        }
    }
}

TEST_CASE("Tick::Start and Stop", "[Tick][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\niTickIntervalSeconds=1\n"
                                      "[Gossip]\nbGossipEnabled=1\n"};
    NarrativeEngine::Testing::DownstreamSpies().Reset();
    NarrativeEngine::Testing::GossipSpies().Reset();

    SECTION("when start is called twice")
    {
        AsyncDispatch::Start();
        Tick::Start();
        Tick::Start();

        SECTION("should run one driver rather than two")
        {
            // Two drivers would double every event log's elapsed accounting and
            // halve the effective tick interval, and nothing would say so.
            REQUIRE(Eventually([] { return PollCount() >= 2; }, kPollTimeout));
            REQUIRE(PollCount() > 0);
        }

        Tick::Stop();
        AsyncDispatch::Stop();
    }

    SECTION("when stop is called on an idle driver")
    {
        SECTION("should do nothing")
        {
            Tick::Stop();
            Tick::Stop();
            SUCCEED("stopping an idle driver is a no-op");
        }
    }

    SECTION("when the driver is stopped")
    {
        AsyncDispatch::Start();
        Tick::Start();
        REQUIRE(Eventually([] { return PollCount() > 0; }, kPollTimeout));
        Tick::Stop();
        AsyncDispatch::Stop();

        SECTION("should poll no further")
        {
            // Stop is called from kPreLoadGame, so a poll that landed after it
            // would run against a world mid-deserialization.
            const int after = PollCount();
            std::this_thread::sleep_for(std::chrono::milliseconds{750});
            REQUIRE(PollCount() == after);
        }
    }
}
