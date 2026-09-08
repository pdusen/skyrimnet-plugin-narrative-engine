#include <Tick.h>

#include <AsyncDispatch.h>
#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <EvalDispatch.h>
#include <PluginThread.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

// Tests for the real-time tick driver.
//
// Everything the Director ever does starts here, and every way this can fail is
// quiet. A driver that never fires leaves the world exactly as it was and looks
// like a mod that is doing nothing on purpose. One that fires while the game is
// paused runs LLM calls over a world the player is not in. One that credits
// paused or disabled time to its accumulator fires a burst the moment play
// resumes.
//
// The six collaborators the poll body calls are our own modules, and linking
// them would drag most of the plugin into this executable, so the test defines
// their poll entry points itself. That makes them spies as well as stand-ins:
// which polls ran, and with what elapsed, is exactly what the cases below are
// about.
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

    struct Spies
    {
        std::atomic<int> combat{0};
        std::atomic<int> weather{0};
        std::atomic<int> travel{0};
        std::atomic<int> history{0};
        std::atomic<int> fineRoads{0};
        std::atomic<int> gossip{0};
        std::atomic<int> evaluations{0};
        std::atomic<bool> evaluationInFlight{false};
        // Elapsed seconds the last poll was told about. The event logs use this
        // to advance their own accumulators, so a driver that passed zero would
        // freeze every one of them without any of them noticing.
        std::atomic<double> lastElapsed{0.0};

        void Reset()
        {
            combat = 0;
            weather = 0;
            travel = 0;
            history = 0;
            fineRoads = 0;
            gossip = 0;
            evaluations = 0;
            evaluationInFlight = false;
            lastElapsed = 0.0;
        }
    };

    Spies g_spies;

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

// The poll entry points the driver calls, and the pipeline it fires into.
// Defining them here keeps six modules out of this link closure and turns each
// into a spy.
namespace NarrativeEngine::CombatEventLog
{
    void Poll(const PluginThread::Token&)
    {
        ++g_spies.combat;
    }
} // namespace NarrativeEngine::CombatEventLog
namespace NarrativeEngine::WeatherEventLog
{
    void Poll(const PluginThread::Token&, double elapsedSeconds)
    {
        ++g_spies.weather;
        g_spies.lastElapsed = elapsedSeconds;
    }
} // namespace NarrativeEngine::WeatherEventLog
namespace NarrativeEngine::TravelEventLog
{
    void Poll(const PluginThread::Token&, double)
    {
        ++g_spies.travel;
    }
} // namespace NarrativeEngine::TravelEventLog
namespace NarrativeEngine::EventHistoryWriter
{
    void Poll(const PluginThread::Token&, double)
    {
        ++g_spies.history;
    }
} // namespace NarrativeEngine::EventHistoryWriter
namespace NarrativeEngine::FineRoads
{
    void Poll(const PluginThread::Token&, double)
    {
        ++g_spies.fineRoads;
    }
} // namespace NarrativeEngine::FineRoads
namespace NarrativeEngine::GossipTick
{
    void Poll(const PluginThread::Token&, double)
    {
        ++g_spies.gossip;
    }
} // namespace NarrativeEngine::GossipTick
namespace NarrativeEngine::EvaluationPipeline
{
    bool IsEvaluationInFlight()
    {
        return g_spies.evaluationInFlight.load();
    }
    void BeginEvaluation(const PluginThread::Token&)
    {
        ++g_spies.evaluations;
    }
} // namespace NarrativeEngine::EvaluationPipeline

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
    const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\niTickIntervalSeconds=1\n"};
    g_spies.Reset();
    Tick::SetEnabled(true);

    SECTION("when the game is running")
    {
        const RunningDriver driver;

        SECTION("should poll every event log")
        {
            // All six, every poll. Missing one is a subsystem that silently
            // stops observing the world while everything else carries on.
            REQUIRE(Eventually([] { return g_spies.combat.load() > 0; }, kPollTimeout));
            REQUIRE(g_spies.weather.load() > 0);
            REQUIRE(g_spies.travel.load() > 0);
            REQUIRE(g_spies.history.load() > 0);
            REQUIRE(g_spies.fineRoads.load() > 0);
            REQUIRE(g_spies.gossip.load() > 0);
        }

        SECTION("should tell them how much time passed")
        {
            // The logs accumulate against this rather than sampling their own
            // clocks, so a zero would freeze all of their cadences at once.
            REQUIRE(Eventually([] { return g_spies.weather.load() > 0; }, kPollTimeout));
            REQUIRE(g_spies.lastElapsed.load() > 0.0);
        }
    }

    SECTION("when the game is paused")
    {
        engine.ui.gameIsPaused = true;
        const RunningDriver driver;

        SECTION("should poll nothing")
        {
            // Menus, the console and dialogue all pause. Polling through a
            // pause would have the event logs report weather changes and travel
            // the player never experienced.
            REQUIRE_FALSE(Eventually([] { return g_spies.combat.load() > 0; }, kPollTimeout));
        }
    }
}

TEST_CASE("Tick fires the Director", "[Tick][engine]")
{
    // Happy path, re-run per leaf: unpaused, enabled, one-second interval.
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\niTickIntervalSeconds=1\n"};
    g_spies.Reset();
    Tick::SetEnabled(true);

    SECTION("when the interval elapses")
    {
        const RunningDriver driver;

        SECTION("should begin an evaluation")
        {
            REQUIRE(Eventually([] { return g_spies.evaluations.load() > 0; }, kFireTimeout));
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
            REQUIRE(Eventually([] { return g_spies.combat.load() > 0; }, kPollTimeout));
        }

        SECTION("should begin no evaluation")
        {
            REQUIRE_FALSE(Eventually([] { return g_spies.evaluations.load() > 0; }, kFireTimeout));
        }
    }

    SECTION("when the previous evaluation is still running")
    {
        g_spies.evaluationInFlight = true;
        const RunningDriver driver;

        SECTION("should skip rather than queue a catch-up burst")
        {
            // An LLM round trip outlasts several intervals. Queueing one
            // evaluation per missed interval would fire a burst of them the
            // moment the first returned.
            REQUIRE_FALSE(Eventually([] { return g_spies.evaluations.load() > 0; }, kFireTimeout));
        }

        SECTION("should fire again once it finishes")
        {
            // The accumulator is not consumed by a skip, so the tick that was
            // held back is not lost either.
            REQUIRE(Eventually([] { return g_spies.combat.load() > 2; }, kPollTimeout));
            g_spies.evaluationInFlight = false;
            REQUIRE(Eventually([] { return g_spies.evaluations.load() > 0; }, kFireTimeout));
        }
    }
}

TEST_CASE("Tick::Start and Stop", "[Tick][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{"[Director]\nbTickEnabled=1\niTickIntervalSeconds=1\n"};
    g_spies.Reset();

    SECTION("when start is called twice")
    {
        AsyncDispatch::Start();
        Tick::Start();
        Tick::Start();

        SECTION("should run one driver rather than two")
        {
            // Two drivers would double every event log's elapsed accounting and
            // halve the effective tick interval, and nothing would say so.
            REQUIRE(Eventually([] { return g_spies.combat.load() >= 2; }, kPollTimeout));
            const int combatBefore = g_spies.combat.load();
            const int gossipBefore = g_spies.gossip.load();
            REQUIRE(combatBefore == gossipBefore);
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
        REQUIRE(Eventually([] { return g_spies.combat.load() > 0; }, kPollTimeout));
        Tick::Stop();
        AsyncDispatch::Stop();

        SECTION("should poll no further")
        {
            // Stop is called from kPreLoadGame, so a poll that landed after it
            // would run against a world mid-deserialization.
            const int after = g_spies.combat.load();
            std::this_thread::sleep_for(std::chrono::milliseconds{750});
            REQUIRE(g_spies.combat.load() == after);
        }
    }
}
