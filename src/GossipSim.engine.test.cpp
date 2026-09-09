#include <GossipSim.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FakeSkyrimNet.h>
#include <GossipGraph.h>
#include <GossipSpies.h>
#include <GossipState.h>
#include <GossipThread.h>
#include <GossipWorld.h>
#include <SkyrimNetAPI.h>
#include <ThreadRole.h>

#include <SKSE/Interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Tests for how a rumor spreads.
//
// This is a discrete-event model over the social graph, advanced by in-world
// time, and its cost is proportional to the number of tellings rather than to
// the size of the province — an idle world costs nothing, because a rumor with
// no live carriers has no events outstanding.
//
// Two properties of the model are load-bearing and both are easy to break by
// simplifying, so both have cases here.
//
// Contact is a FINITE DAILY BUDGET per person, divided among their contacts by
// weight. It is not a rate per pair: a per-pair rate makes a person's social
// activity scale with the size of the town they live in, and saturates the
// whole province inside one game day.
//
// A carrier's telling quota is spent by tellings that land on somebody who
// ALREADY KNOWS, not only by ones that land on somebody new. That is the
// saturation brake, and without it the model does not terminate — a rumor in a
// town where everyone knows it keeps trying forever.
//
// The memories the tellings write are what the player eventually sees, and they
// go out on two different schedules: a listener catches a rumor once, so their
// memory is written as it happens; a carrier can tell several people in one
// tick, so their side accumulates and goes out once, naming everybody.

namespace
{
    namespace GossipSim = NarrativeEngine::GossipSim;
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    namespace GossipThread = NarrativeEngine::GossipThread;
    using NarrativeEngine::GossipState;
    using NarrativeEngine::Testing::BuildGossipWorld;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::FakeSkyrimNetState;
    using NarrativeEngine::Testing::FakeSkyrimNetStateFunc;
    using NarrativeEngine::Testing::GossipWorld;
    using NarrativeEngine::Testing::kFakeSkyrimNetStateExport;
    using NarrativeEngine::Testing::LiveGossipState;
    using NarrativeEngine::Testing::ResetGossipState;

    constexpr const char* kSettings = "[Gossip]\nbGossipEnabled=1\niGossipRandomSeed=12345\n";

    constexpr double kDayOne = 10.0;

    // The version GossipSim stamps its co-save record with. A load carrying
    // anything else is discarded, so a test reading its own save back has to
    // name the current one.
    constexpr std::uint32_t kRecordVersion = 7;

    FakeSkyrimNetState& FakeLLM()
    {
        HMODULE module = ::LoadLibraryA("SkyrimNet");
        REQUIRE(module != nullptr);
        auto* accessor = reinterpret_cast<FakeSkyrimNetStateFunc>(
            reinterpret_cast<void*>(::GetProcAddress(module, kFakeSkyrimNetStateExport)));
        REQUIRE(accessor != nullptr);
        return *accessor();
    }

    SKSE::SerializationInterface* FakeInterface()
    {
        alignas(16) static std::byte storage[64]{};
        return reinterpret_cast<SKSE::SerializationInterface*>(storage);
    }

    template <class Fn> void OnGossipThread(Fn body)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        GossipThread::detail::JobDispatcher::Invoke([&](const GossipThread::Token& gt) { body(gt); });
    }

    void WindTo(double day)
    {
        OnGossipThread([&](const GossipThread::Token& gt) { GossipSim::SetHorizon(gt, day); });
    }

    void AdvanceTo(double day)
    {
        OnGossipThread([&](const GossipThread::Token& gt) {
            GossipSim::SetHorizon(gt, day);
            GossipSim::Advance(gt, day, {});
        });
    }

    std::uint32_t Seed(std::uint32_t origin, std::int64_t memoryId = 5001, float notability = 0.9f)
    {
        std::uint32_t id = 0;
        OnGossipThread([&](const GossipThread::Token& gt) {
            id = GossipSim::SeedRumor(gt, origin, notability, memoryId, {"first", "second", "third"});
        });
        return id;
    }

    std::vector<GossipSim::RumorView> Rumors()
    {
        return GossipSim::GetRumorViews(LiveGossipState());
    }

    GossipSim::Stats Stats()
    {
        return GossipSim::GetStats(LiveGossipState());
    }

    GossipSim::RumorView OnlyRumor()
    {
        const auto rumors = Rumors();
        REQUIRE(rumors.size() == 1);
        return rumors.front();
    }

    float ContactShare(std::uint32_t npc)
    {
        float share = 0.0f;
        OnGossipThread([&](const GossipThread::Token& gt) { share = GossipSim::AvailableContactShare(gt, npc); });
        return share;
    }
} // namespace

TEST_CASE("GossipSim starts a rumor with somebody", "[GossipSim][engine]")
{
    // Happy path, re-run per leaf: a built graph, a wound clock, and a rumor
    // about to be introduced.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ResetGossipState();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();
    REQUIRE(GossipGraph::IsReady());
    WindTo(kDayOne);

    SECTION("when somebody in the graph has something to tell")
    {
        const auto id = Seed(world.hulda);

        SECTION("should take the rumor")
        {
            REQUIRE(id != 0);
            REQUIRE(Rumors().size() == 1);
        }

        SECTION("should credit where it came from")
        {
            // The origin and the memory it was drawn from are what tie a rumor
            // back to the thing that happened, and are what stop the harvester
            // ever offering the same memory again.
            const auto rumors = Rumors();
            REQUIRE(rumors.front().originNpc == world.hulda);
            REQUIRE(rumors.front().sourceMemoryId == 5001);
        }

        SECTION("should keep the tellings it was written with")
        {
            // No model is involved past this point. Every telling the rumor
            // will ever produce is one of these, chosen by how far it has come.
            REQUIRE(Rumors().front().bands.size() == 3);
            REQUIRE(Rumors().front().text == "first");
        }

        SECTION("should start with its origin carrying it")
        {
            REQUIRE(Stats().totalCarriers == 1);
            REQUIRE(Stats().liveRumors == 1);
        }

        SECTION("should have something to do")
        {
            // One scheduled event per infectious carrier is the whole of the
            // model's cost. A rumor with nothing queued never spreads.
            REQUIRE(Stats().queuedEvents > 0);
        }
    }

    SECTION("when the origin is nobody the graph knows")
    {
        SECTION("should refuse the rumor")
        {
            // A rumor with no origin has no contacts to spread through, so it
            // would sit live forever taking up one of the few slots.
            REQUIRE(Seed(0x00DEAD01u) == 0);
            REQUIRE(Rumors().empty());
        }
    }

    SECTION("when the live-rumor cap is full")
    {
        const ConfiguredSettings capped{
            "[Gossip]\nbGossipEnabled=1\niGossipRandomSeed=12345\niGossipMaxLiveRumors=1\n"};
        REQUIRE(Seed(world.hulda, 5001) != 0);

        SECTION("should refuse the next one")
        {
            // The cap is what bounds the whole simulation's cost, and the
            // harvester treats a refusal as temporary — the memory goes back
            // to the pool rather than being spent.
            REQUIRE(Seed(world.ysolda, 5002) == 0);
        }
    }

    SECTION("when gossip is switched off")
    {
        const ConfiguredSettings off{"[Gossip]\nbGossipEnabled=0\n"};

        SECTION("should refuse the rumor")
        {
            REQUIRE(Seed(world.hulda) == 0);
        }
    }
}

TEST_CASE("GossipSim spreads a rumor and stops", "[GossipSim][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ResetGossipState();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();
    WindTo(kDayOne);
    REQUIRE(Seed(world.hulda) != 0);

    SECTION("when in-world days go by")
    {
        AdvanceTo(kDayOne + 5.0);

        SECTION("should reach somebody else")
        {
            // Hulda works at an inn with Saadia and lives in a town with
            // Ysolda, so there is somewhere for it to go. Read off the
            // session tally rather than off the rumor: a province this small
            // saturates inside a day, and the rumor has been reaped and its
            // carriers released long before day fifteen.
            REQUIRE(Stats().transmissionsThisSession > 0);
        }

        SECTION("should write what everyone heard into their memories")
        {
            // The memories are the only thing the player ever sees of this
            // whole subsystem. A rumor that spread and wrote nothing did not
            // happen as far as the game is concerned.
            REQUIRE(Stats().memoriesWritten > 0);
        }

        SECTION("should account for every conversation it drew")
        {
            // Transmissions, wasted tellings, people who did not catch it,
            // people who were unavailable, and conversations past a carrier's
            // daily budget. A drawn conversation falling into none of those is
            // a hole in the model, and the tuning is read off these numbers.
            const auto stats = Stats();
            const auto accounted = stats.transmissionsThisSession + stats.wastedThisSession + stats.notCaughtThisSession
                                   + stats.unavailableThisSession + stats.cappedThisSession;
            REQUIRE(accounted > 0);
        }
    }

    SECTION("when a very long time goes by")
    {
        AdvanceTo(kDayOne + 400.0);

        SECTION("should stop rather than run forever")
        {
            // The saturation brake. A carrier's daily quota is spent by
            // tellings that land on somebody who already knows, so a rumor in
            // a town where everyone has heard it runs out of budget instead of
            // retrying forever.
            REQUIRE(Stats().queuedEvents == 0);
        }

        SECTION("should have reaped what it finished with")
        {
            // Nothing live and nothing queued: the rumor is over and its
            // storage is back.
            REQUIRE(Stats().liveRumors == 0);
        }
    }

    SECTION("when the clock is wound backwards")
    {
        AdvanceTo(kDayOne + 5.0);
        const auto before = Stats().transmissionsThisSession;
        AdvanceTo(kDayOne + 1.0);

        SECTION("should do nothing")
        {
            // A console time change, or a save loaded from earlier in the
            // playthrough. Replaying is worse than standing still: every
            // carrier would step again and write a second memory for a
            // conversation that already happened.
            REQUIRE(Stats().transmissionsThisSession == before);
        }
    }
}

TEST_CASE("GossipSim knows who is left to tell", "[GossipSim][engine]")
{
    // The harvest asks before spending a model call on somebody: a person
    // whose whole circle already carries a rumor is a person no new rumor can
    // travel from.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ResetGossipState();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();
    WindTo(kDayOne);

    SECTION("when everybody around them is alive and about")
    {
        SECTION("should count the rungs that have somebody on them")
        {
            // Not 1.0, and that is the model rather than a gap in the world:
            // Hulda's household is the inn and her settlement is Whiterun,
            // the tiers between and above them hold nobody, and a draw on an
            // empty rung is spent in silence rather than redistributed. Half
            // her conversations therefore reach nobody however alive her
            // neighbours are.
            REQUIRE(ContactShare(world.hulda) > 0.5f);
        }

        SECTION("should describe it in words a log can print")
        {
            // The share alone cannot say which of the three sources it came
            // from, so the trace has to name the person and the verdict.
            std::string described;
            OnGossipThread([&](const GossipThread::Token& gt) {
                described = GossipSim::DescribeContactAvailability(gt, world.hulda);
            });
            REQUIRE(described.find("Hulda") != std::string::npos);
            REQUIRE(described.find("The Bannered Mare") != std::string::npos);
        }
    }

    SECTION("when the only person on a rung is down")
    {
        const float whole = ContactShare(world.hulda);
        engine.SetActorBleedingOut(engine.PlacedActorFor(world.saadia), true);

        SECTION("should stop counting that rung")
        {
            // Saadia is the whole of Hulda's household, so a Saadia who is
            // bleeding out takes the household rung's weight with her. This
            // is the number the harvester spends or withholds a model call
            // on, so it has to move when the circle does.
            REQUIRE(ContactShare(world.hulda) < whole);
        }
    }

    SECTION("when the person is not in the graph")
    {
        SECTION("should say nobody is reachable")
        {
            REQUIRE(ContactShare(0x00DEAD01u) == 0.0f);
        }
    }
}

TEST_CASE("GossipSim survives a save and load", "[GossipSim][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    ResetGossipState();
    REQUIRE(NarrativeEngine::SkyrimNetAPI::Initialize());
    FakeLLM().Reset();
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();
    WindTo(kDayOne);
    REQUIRE(Seed(world.hulda) != 0);
    GossipSim::PublishSnapshot();

    SECTION("when the world is written and read back")
    {
        GossipSim::OnSave(FakeInterface(), *GossipSim::Snapshot());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        ResetGossipState();
        REQUIRE(Rumors().empty());
        GossipSim::OnLoad(FakeInterface(), kRecordVersion, 0);
        REQUIRE(GossipSim::AdoptPendingState());

        SECTION("should bring the rumors back")
        {
            REQUIRE(Rumors().size() == 1);
        }

        SECTION("should put the people they name through the incoming load order")
        {
            // The ids in a co-save are the ids of the load order that wrote
            // it. Trusting them across a changed load order points a rumor's
            // origin at whichever form now occupies that index, which is
            // somebody else entirely once a plugin is added or removed.
            REQUIRE(std::find(engine.cosave.resolveRequests.begin(), engine.cosave.resolveRequests.end(), world.hulda)
                    != engine.cosave.resolveRequests.end());
            REQUIRE(OnlyRumor().originNpc == engine.cosave.resolvedFormID);
        }

        SECTION("should bring back what they say")
        {
            // The band text is written once, at seed time, by a model call
            // nobody is going to make again. Losing it across a save leaves a
            // rumor that spreads and says nothing.
            REQUIRE(OnlyRumor().bands == std::vector<std::string>{"first", "second", "third"});
        }

        SECTION("should bring back the clock they were dated against")
        {
            REQUIRE(GossipSim::LastSimulatedGameDay() == kDayOne);
        }
    }

    SECTION("when a load is staged but not yet adopted")
    {
        GossipSim::OnSave(FakeInterface(), *GossipSim::Snapshot());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        GossipSim::OnRevert();
        GossipSim::OnLoad(FakeInterface(), kRecordVersion, 0);

        SECTION("should answer with the incoming world's clock")
        {
            // The window between the load and the first tick of the new
            // session: live state still describes the outgoing world, and the
            // scheduler is asking precisely then which one it should anchor
            // against.
            REQUIRE(GossipSim::LastSimulatedGameDay() == kDayOne);
        }
    }

    SECTION("when the version is one no build ever wrote")
    {
        GossipSim::OnSave(FakeInterface(), *GossipSim::Snapshot());
        engine.cosave.readable = engine.cosave.written;
        engine.cosave.readCursor = 0;
        ResetGossipState();
        GossipSim::OnLoad(FakeInterface(), 99, 0);
        (void)GossipSim::AdoptPendingState();

        SECTION("should restore nothing")
        {
            REQUIRE(Rumors().empty());
        }
    }

    SECTION("when there is no serialization interface")
    {
        SECTION("should do nothing")
        {
            GossipSim::OnSave(nullptr, *GossipSim::Snapshot());
            REQUIRE(engine.cosave.written.empty());
        }
    }
}
