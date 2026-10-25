#include <PlotPopulation.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipGraph.h>
#include <GossipWorld.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

// Tests for who holds an office. The records list everybody the Civil War
// may seat -- the sitting jarl and the one who would replace her alike, at
// rank -1 -- so the running game, which ranks whoever actually rules, has
// the last word once a game is loaded.

namespace
{
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    namespace PlotCasting = NarrativeEngine::PlotCasting;
    namespace PlotPopulation = NarrativeEngine::PlotPopulation;
    using NarrativeEngine::Testing::BuildGossipWorld;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::GossipWorld;

    constexpr const char* kSettings = "[Gossip]\nbGossipEnabled=1\n";
    constexpr std::uint32_t kJobJarlFaction = 0x00050920;

    PlotCasting::Office OfficeOf(std::uint32_t npc)
    {
        const auto* member = PlotPopulation::Get().Find(npc);
        REQUIRE(member != nullptr);
        return member->office;
    }
} // namespace

TEST_CASE("PlotPopulation seats whoever the running game ranks", "[PlotPopulation][engine]")
{
    // One run, in order: Build reads the records once per process, and each
    // load reads the running game again from what they list.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const GossipWorld world = BuildGossipWorld(engine);
    // Both listed among the jarls at -1, as vanilla lists Laila and Maven.
    auto* jarls = engine.AddFaction(kJobJarlFaction, "JobJarlFaction");
    engine.JoinFaction(RE::TESForm::LookupByID<RE::TESNPC>(world.hulda), jarls, -1);
    engine.JoinFaction(RE::TESForm::LookupByID<RE::TESNPC>(world.ysolda), jarls, -1);
    GossipGraph::Initialize();
    PlotPopulation::Build();

    // Before a game is loaded: everybody the records list.
    REQUIRE(OfficeOf(world.hulda) == PlotCasting::Office::Jarl);
    REQUIRE(OfficeOf(world.ysolda) == PlotCasting::Office::Jarl);

    // A game that ranks one of them seats only that one.
    engine.SetFactionRank(engine.PlacedActorFor(world.hulda), jarls, 0);
    PlotPopulation::OnSessionStart();
    REQUIRE(OfficeOf(world.hulda) == PlotCasting::Office::Jarl);
    REQUIRE(OfficeOf(world.ysolda) == PlotCasting::Office::None);

    // A later game that ranks the other seats her instead, read afresh
    // rather than from the session before.
    engine.SetFactionRank(engine.PlacedActorFor(world.hulda), jarls, -1);
    engine.SetFactionRank(engine.PlacedActorFor(world.ysolda), jarls, 0);
    PlotPopulation::OnSessionStart();
    REQUIRE(OfficeOf(world.hulda) == PlotCasting::Office::None);
    REQUIRE(OfficeOf(world.ysolda) == PlotCasting::Office::Jarl);

    // A game that ranks nobody as a jarl keeps what the records list rather
    // than leave Skyrim with no jarl at all.
    engine.SetFactionRank(engine.PlacedActorFor(world.ysolda), jarls, -1);
    PlotPopulation::OnSessionStart();
    REQUIRE(OfficeOf(world.hulda) == PlotCasting::Office::Jarl);
    REQUIRE(OfficeOf(world.ysolda) == PlotCasting::Office::Jarl);
}

TEST_CASE("PlotPopulation::IsAlive", "[PlotPopulation][engine]")
{
    // Happy path, re-run per leaf: everybody in the gossip world is alive,
    // enabled and solid. Casting asks this of every mastermind, agent and
    // memory keeper, so each case below is somebody who must not be cast.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();

    SECTION("when somebody is alive and well")
    {
        SECTION("should say so")
        {
            REQUIRE(PlotPopulation::IsAlive(world.hulda));
        }
    }

    SECTION("when a script has made somebody a ghost")
    {
        // A ghost record never joins the population, so this is the live
        // reference turned spectral after it was built.
        engine.SetActorGhost(engine.PlacedActorFor(world.hulda), true);

        SECTION("should not count them among the living")
        {
            REQUIRE_FALSE(PlotPopulation::IsAlive(world.hulda));
        }

        SECTION("should leave everybody else alone")
        {
            REQUIRE(PlotPopulation::IsAlive(world.ysolda));
        }
    }

    SECTION("when somebody is not in the graph")
    {
        SECTION("should not count them")
        {
            REQUIRE_FALSE(PlotPopulation::IsAlive(0x00E91005u));
        }
    }
}
