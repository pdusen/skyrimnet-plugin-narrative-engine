#include <GossipGraph.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <GossipWorld.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Tests for the social graph a rumor travels along.
//
// The graph is reconstructed at runtime from the load order, and everything it
// knows comes from records that were authored for other purposes: which
// keywords a location carries, which locations are inside which, which unique
// NPCs a location's residence list names, and which factions their base records
// join. Nothing in the game says "these people talk to each other" — that is
// entirely inferred here, and getting the inference wrong is the difference
// between a rumor spreading through a town and a rumor going nowhere.
//
// Three layers, and each has a way of failing quietly.
//
// The TIER TREE classifies locations into household, settlement and hold by
// keyword. A location may satisfy more than one tier and the tiers collapse
// onto it, which is real: an Orc stronghold is both a settlement and a
// household. Miss a tier and everyone in it becomes unreachable at that range.
//
// RESIDENCE comes from each location's unique-NPC list, and the row's own
// finer-grained location wins over the location carrying it — a town's list
// names the houses inside it, and reading the town instead would put a whole
// city in one household.
//
// PERSONAL EDGES are the distance-blind channel, and the only one that reliably
// crosses a hold border. They compose from two sources rather than override:
// somebody you are related to badly and share a faction with cancels out and
// stays where geography put them.
//
// ONE GRAPH PER PROCESS. Initialize is one-shot with no reset, so each test
// case builds its world once above the sections and the sections ask questions
// of it. The suite runs through ctest, which gives each case its own process.

namespace
{
    namespace GossipGraph = NarrativeEngine::GossipGraph;
    using NarrativeEngine::Testing::BuildGossipWorld;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using NarrativeEngine::Testing::GossipWorld;

    constexpr const char* kSettings = "[Gossip]\nbGossipEnabled=1\n";

    bool IsParticipant(std::uint32_t npc)
    {
        const auto& all = GossipGraph::Participants();
        return std::find(all.begin(), all.end(), npc) != all.end();
    }

    bool Contains(const std::vector<RE::FormID>& members, std::uint32_t npc)
    {
        return std::find(members.begin(), members.end(), npc) != members.end();
    }

    bool HasEdgeTo(std::uint32_t from, std::uint32_t to)
    {
        const auto& edges = GossipGraph::PersonalEdges(from);
        return std::any_of(
            edges.begin(), edges.end(), [&](const GossipGraph::PersonalEdge& e) { return e.other == to; });
    }
} // namespace

TEST_CASE("GossipGraph places everyone it can", "[GossipGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();

    SECTION("when the load order has been read")
    {
        SECTION("should be ready")
        {
            REQUIRE(GossipGraph::IsReady());
        }

        SECTION("should take every unique person it found")
        {
            REQUIRE(GossipGraph::ParticipantCount() == 4);
            REQUIRE(IsParticipant(world.hulda));
            REQUIRE(IsParticipant(world.valga));
        }

        SECTION("should remember what they are called")
        {
            // Cached as strings at build time so the simulation and its log
            // never have to touch the engine to render a line — which is what
            // lets both run off the main thread.
            REQUIRE(GossipGraph::NpcName(world.ysolda) == "Ysolda");
            REQUIRE(GossipGraph::LocationName(world.whiterun) == "Whiterun");
        }
    }

    SECTION("when somebody's residence is looked up")
    {
        SECTION("should use the row's own finer location")
        {
            // Hulda's row sits on Whiterun and names the inn. Reading the
            // location carrying the row instead would put every resident of a
            // city in one household, and a rumor would cross the whole of
            // Whiterun in a single household step.
            const auto* hulda = GossipGraph::Find(world.hulda);
            REQUIRE(hulda != nullptr);
            REQUIRE(hulda->household == world.banneredMare);
        }

        SECTION("should walk up to the settlement and the hold")
        {
            // Neither is on the residence row. Both are found by walking the
            // location's parents until a location of that tier turns up.
            const auto* hulda = GossipGraph::Find(world.hulda);
            REQUIRE(hulda != nullptr);
            REQUIRE(hulda->settlement == world.whiterun);
            REQUIRE(hulda->hold == world.whiterunHold);
        }

        SECTION("should place somebody with no house in their town anyway")
        {
            // Ysolda's row names the town itself. She has no household, and
            // the settlement tier is what she is reachable through.
            const auto* ysolda = GossipGraph::Find(world.ysolda);
            REQUIRE(ysolda != nullptr);
            REQUIRE(ysolda->settlement == world.whiterun);
        }
    }

    SECTION("when a stranger is looked up")
    {
        SECTION("should answer with nothing")
        {
            REQUIRE(GossipGraph::Find(0x00DEAD01u) == nullptr);
        }
    }

    SECTION("when the members of a place are asked for")
    {
        SECTION("should list everyone in that house")
        {
            REQUIRE(Contains(GossipGraph::HouseholdMembers(world.banneredMare), world.hulda));
            REQUIRE(Contains(GossipGraph::HouseholdMembers(world.banneredMare), world.saadia));
        }

        SECTION("should list everyone in that town, house or not")
        {
            const auto& town = GossipGraph::SettlementMembers(world.whiterun);
            REQUIRE(Contains(town, world.hulda));
            REQUIRE(Contains(town, world.ysolda));
        }

        SECTION("should keep the two holds apart")
        {
            REQUIRE(Contains(GossipGraph::HoldMembers(world.whiterunHold), world.hulda));
            REQUIRE_FALSE(Contains(GossipGraph::HoldMembers(world.whiterunHold), world.valga));
            REQUIRE(Contains(GossipGraph::HoldMembers(world.falkreathHold), world.valga));
        }

        SECTION("should answer with nothing for a place nobody lives")
        {
            REQUIRE(GossipGraph::HouseholdMembers(0x00DEAD01u).empty());
        }
    }

    SECTION("when the harvest buckets are asked for")
    {
        SECTION("should put every participant in exactly one")
        {
            // A sweep works one bucket at a time, so anybody in none is never
            // harvested and anybody in two is harvested twice.
            std::size_t total = 0;
            for (std::uint32_t i = 0; i < GossipGraph::BucketCount(); ++i)
                total += GossipGraph::BucketMembers(i).size();
            REQUIRE(total == GossipGraph::ParticipantCount());
        }
    }

    SECTION("when the census is read")
    {
        SECTION("should count what it built")
        {
            const auto& census = GossipGraph::GetCensus();
            REQUIRE(census.participants == 4);
            REQUIRE(census.households == 2);
            REQUIRE(census.settlements == 2);
            REQUIRE(census.holds == 2);
        }
    }

    SECTION("when the build is asked for a second time")
    {
        GossipGraph::Initialize();

        SECTION("should leave the graph alone")
        {
            REQUIRE(GossipGraph::ParticipantCount() == 4);
        }
    }
}

TEST_CASE("GossipGraph joins people through what they belong to", "[GossipGraph][engine]")
{
    // The distance-blind channel, and the only one that reliably crosses a
    // hold border: measured on vanilla, most relationship edges are redundant
    // with living nearby, so the faction half is what actually carries a rumor
    // between provinces.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const GossipWorld world = BuildGossipWorld(engine);
    GossipGraph::Initialize();

    SECTION("when two people share a faction")
    {
        SECTION("should join them regardless of distance")
        {
            // Ysolda is in Whiterun and Valga a hold away.
            REQUIRE(HasEdgeTo(world.ysolda, world.valga));
            REQUIRE(HasEdgeTo(world.valga, world.ysolda));
        }

        SECTION("should record the faction so a line can name it")
        {
            const auto& edges = GossipGraph::PersonalEdges(world.ysolda);
            const auto it = std::find_if(
                edges.begin(), edges.end(), [&](const GossipGraph::PersonalEdge& e) { return e.other == world.valga; });
            REQUIRE(it != edges.end());
            REQUIRE(it->sharedFaction);
            REQUIRE(it->faction == world.companions);
        }
    }

    SECTION("when two people share nothing")
    {
        SECTION("should leave them unjoined")
        {
            // Hulda and Valga live a hold apart with no faction between them.
            // Their only route is the province channel, which is a lottery
            // rather than an edge.
            REQUIRE_FALSE(HasEdgeTo(world.hulda, world.valga));
        }
    }

    SECTION("when somebody is not in the graph")
    {
        SECTION("should answer with no edges")
        {
            REQUIRE(GossipGraph::PersonalEdges(0x00DEAD01u).empty());
        }
    }
}

TEST_CASE("GossipGraph refuses to build what it cannot", "[GossipGraph][engine]")
{
    // Each of these is a load order the module has to survive rather than a
    // state it can fix. An empty graph makes every accessor answer empty and
    // the simulation idle, which is the right failure.
    EngineMock engine;

    SECTION("when gossip is switched off")
    {
        const ConfiguredSettings settings{"[Gossip]\nbGossipEnabled=0\n"};
        BuildGossipWorld(engine);
        GossipGraph::Initialize();

        SECTION("should not build at all")
        {
            // The shipped default, so the cost of the whole subsystem to a
            // player who has not turned it on has to be nothing.
            REQUIRE_FALSE(GossipGraph::IsReady());
            REQUIRE(GossipGraph::ParticipantCount() == 0);
        }
    }
}

TEST_CASE("GossipGraph builds nothing without a load order", "[GossipGraph][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    BuildGossipWorld(engine);
    engine.world.dataHandlerPresent = false;
    GossipGraph::Initialize();

    SECTION("when the data handler is not up yet")
    {
        SECTION("should build nothing")
        {
            REQUIRE_FALSE(GossipGraph::IsReady());
        }
    }
}

TEST_CASE("GossipGraph builds nothing when no location says what it is", "[GossipGraph][engine]")
{
    // Locations carry their tier as keywords, and those keywords come from the
    // base game — an install missing them is an install the graph cannot read.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* nowhere = engine.AddLocation(0x00E90001u, "Somewhere", {}, "SomewhereLocation");
    auto* somebody = engine.AddNPC(0x00E90002u, "Somebody");
    somebody->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
    engine.AddResident(nowhere, somebody);
    GossipGraph::Initialize();

    SECTION("when nothing classifies into a tier")
    {
        SECTION("should build nothing")
        {
            REQUIRE_FALSE(GossipGraph::IsReady());
        }
    }
}

TEST_CASE("GossipGraph takes only people", "[GossipGraph][engine]")
{
    // Unique-NPC lists carry creatures and test records as well as people, and
    // a rumor told to a horse is a rumor that stops there.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    const GossipWorld world = BuildGossipWorld(engine);

    auto* town = RE::TESForm::LookupByID<RE::BGSLocation>(world.whiterun);
    REQUIRE(town != nullptr);

    // A horse: unique, named, resident, and not a person.
    auto* horse = engine.AddNPC(0x00E91001u, "Frost");
    horse->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
    engine.SetNPCRace(horse, engine.AddRace(0x00E91002u, "HorseRace", {"ActorTypeCreature"}));
    engine.AddResident(town, horse);

    // Somebody with no name at all, which is what a template record looks like.
    auto* nameless = engine.AddNPC(0x00E91003u);
    nameless->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
    engine.AddResident(town, nameless);

    // A record left in the file for testing, recognisable only by its editor
    // ID — which is exactly how the vanilla data marks them.
    auto* dummy = engine.AddNPC(0x00E91004u, "Placeholder");
    dummy->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
    engine.SetEditorIDOf(dummy, "TestDummyGuy");
    engine.AddResident(town, dummy);

    GossipGraph::Initialize();

    SECTION("when the residence lists hold more than people")
    {
        SECTION("should leave the creatures out")
        {
            REQUIRE_FALSE(IsParticipant(0x00E91001u));
        }

        SECTION("should leave the nameless out")
        {
            // A rumor about somebody with no name renders a sentence with a
            // hole in it.
            REQUIRE_FALSE(IsParticipant(0x00E91003u));
        }

        SECTION("should leave the test records out")
        {
            REQUIRE_FALSE(IsParticipant(0x00E91004u));
        }

        SECTION("should still take the people")
        {
            REQUIRE(IsParticipant(world.hulda));
        }
    }
}
