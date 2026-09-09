#include <RoadRoute.h>

#include <ConfiguredSettings.h>
#include <EngineMock.h>
#include <FineRoads.h>
#include <MainThread.h>
#include <PluginThread.h>
#include <ThreadRole.h>
#include <TravelGraph.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// Tests for the query layer over the two road graphs.
//
// One graph is a coarse skeleton of the whole province, the other a
// high-resolution graph of whatever cells are loaded, and neither answers the
// question anyone actually has: where can this NPC walk, and where do they hand
// off to the network beyond?
//
// The interesting part is how the handoff is chosen. The fine graph's boundary
// is its frontier nodes, and any of them could be it. Picking the nearest one
// is wrong: a branch that leaves coverage after one step can come out in open
// country a long way from any road, while a branch five steps away comes out
// beside one. So the choice is made on the whole trip instead — the walked
// distance to the frontier, plus the gap from there onto the network, plus the
// rest of the way on the coarse graph — and the road below is laid out so those
// two rules disagree about which branch to take.
//
// The rest is degradation. There is usually no fine graph at all (indoors, or
// on a save just loaded), often no frontier reachable from where the player
// stands (the end of a spur), and sometimes a frontier that joins a stretch of
// road with no route onward. None of those may fail the query; each drops to
// the next-best answer, because a route that stops being offered is an NPC that
// stops travelling and nothing that says why.
//
// ONE COARSE GRAPH PER PROCESS. TravelGraph::Initialize is one-shot with no
// reset, so the skeleton below is built once per test case and every section
// shares it. The fine graph is poll-driven and each section is free to rebuild
// it, which is where the variation lives.

namespace
{
    namespace RoadRoute = NarrativeEngine::RoadRoute;
    namespace FineRoads = NarrativeEngine::FineRoads;
    namespace TravelGraph = NarrativeEngine::TravelGraph;
    namespace PluginThread = NarrativeEngine::PluginThread;
    using NarrativeEngine::Testing::ConfiguredSettings;
    using NarrativeEngine::Testing::EngineMock;
    using Triangle = EngineMock::FakeTriangle;

    constexpr const char* kSettings = "[FineRoads]\nbFineRoadsEnabled=1\niFineRoadsBackstopSeconds=1\n"
                                      "bFineRoadsDebugBitmap=0\n"
                                      "[TravelGraph]\nbTravelGraphEnabled=1\nbTravelGraphDebugBitmap=0\n";

    // Harness records: nothing here is looked up against the game's own data.
    constexpr std::uint32_t kMainland = 0x00B10001u;
    constexpr std::uint32_t kIsland = 0x00B10002u;
    constexpr std::uint32_t kNavi = 0x00B10010u;
    constexpr std::uint32_t kFineMesh = 0x00B11001u;
    constexpr std::uint32_t kUnloadedMesh = 0x00B1FFFFu;

    constexpr float kCellUnits = 4096.0f;

    float CellCentre(int cell)
    {
        return (static_cast<float>(cell) + 0.5f) * kCellUnits;
    }

    // The fine graph, as the player would see it standing at the junction.
    //
    //   stub — junction — overland
    //             |
    //           bend
    //             |
    //           climb — roadside
    //
    // Three branches leave the loaded region. `overland` is one step away and
    // comes out in open country; `roadside` is three and comes out beside the
    // coarse road; `stub` comes out beside a piece of network that goes
    // nowhere. The distances are geometric, so what a branch costs to walk is
    // exactly how far apart these points are.
    struct FinePlaces
    {
        static constexpr float junctionX = 40000.0f;
        static constexpr float junctionY = 40000.0f;
        static constexpr float overlandX = 45000.0f;
        static constexpr float overlandY = 40000.0f;
        static constexpr float bendX = 45000.0f;
        static constexpr float bendY = 45000.0f;
        static constexpr float climbX = 50000.0f;
        static constexpr float climbY = 48000.0f;
        static constexpr float roadsideX = 59392.0f;
        static constexpr float roadsideY = 50000.0f;
        static constexpr float stubX = 23000.0f;
        static constexpr float stubY = 23000.0f;
    };

    Triangle Road(float x, float y)
    {
        Triangle t;
        t.x = x;
        t.y = y;
        return t;
    }

    Triangle Frontier(float x, float y)
    {
        auto t = Road(x, y);
        // A portal into a navmesh nobody has loaded is what makes a node the
        // edge of coverage.
        t.portalMesh[2] = kUnloadedMesh;
        t.portalTriangle[2] = 0;
        return t;
    }

    // Lays the fine road out on one navmesh in `cell`. Triangle indices are the
    // node order below, and the edges name each other by index.
    void LayFineRoad(EngineMock& engine, RE::TESObjectCELL* cell)
    {
        auto junction = Road(FinePlaces::junctionX, FinePlaces::junctionY);
        junction.neighbor[0] = 1; // overland
        junction.neighbor[1] = 2; // bend, towards the roadside frontier
        junction.neighbor[2] = 5; // stub

        auto overland = Frontier(FinePlaces::overlandX, FinePlaces::overlandY);
        overland.neighbor[0] = 0;

        auto bend = Road(FinePlaces::bendX, FinePlaces::bendY);
        bend.neighbor[0] = 3;
        auto climb = Road(FinePlaces::climbX, FinePlaces::climbY);
        climb.neighbor[0] = 4;
        auto roadside = Frontier(FinePlaces::roadsideX, FinePlaces::roadsideY);
        roadside.neighbor[0] = 3;

        auto stub = Frontier(FinePlaces::stubX, FinePlaces::stubY);
        stub.neighbor[0] = 0;

        engine.AddNavMesh(cell, kFineMesh, {junction, overland, bend, climb, roadside, stub});
    }

    // The coarse skeleton: one long road running east, well north of the fine
    // graph, and an isolated pair of nodes off on their own that no route to
    // the destination ever passes through.
    //
    // Positions come out at cell centres, because no navmesh here has a
    // resident form for the layout to be measured against — which is also the
    // ordinary case in the game, where most of the province is not loaded.
    struct CoarsePlaces
    {
        static constexpr int roadFirstCell = 9;
        static constexpr int roadLastCell = 21;
        static constexpr int roadCellY = 12;
        // Well away from the road and from everything the fine graph covers,
        // so the only frontier that ever joins it is the one meant to.
        static constexpr int strandedCellX = 5;
        static constexpr int strandedCellY = 5;
    };

    void LayCoarseRoad(EngineMock& engine)
    {
        auto* navi = engine.AddNavMeshInfoMap(kNavi);
        std::uint32_t nextForm = 0x00B12000u;

        std::vector<const RE::BSNavmeshInfo*> road;
        for (int cell = CoarsePlaces::roadFirstCell; cell <= CoarsePlaces::roadLastCell; ++cell) {
            road.push_back(engine.AddNavmeshInfo(navi,
                                                 nextForm++,
                                                 kMainland,
                                                 static_cast<std::int16_t>(cell),
                                                 static_cast<std::int16_t>(CoarsePlaces::roadCellY),
                                                 0.0f,
                                                 0.0f,
                                                 0.0f));
        }
        engine.AddPreferredPath(navi, road);

        // The stranded pair, right beside where the fine stub leaves coverage
        // and connected to nothing else.
        const auto* strandedWest = engine.AddNavmeshInfo(navi,
                                                         nextForm++,
                                                         kMainland,
                                                         static_cast<std::int16_t>(CoarsePlaces::strandedCellX),
                                                         static_cast<std::int16_t>(CoarsePlaces::strandedCellY),
                                                         0.0f,
                                                         0.0f,
                                                         0.0f);
        const auto* strandedEast = engine.AddNavmeshInfo(navi,
                                                         nextForm++,
                                                         kMainland,
                                                         static_cast<std::int16_t>(CoarsePlaces::strandedCellX + 1),
                                                         static_cast<std::int16_t>(CoarsePlaces::strandedCellY),
                                                         0.0f,
                                                         0.0f,
                                                         0.0f);
        engine.AddPreferredPath(navi, {strandedWest, strandedEast});
    }

    void PollFineRoads()
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        // A large elapsed, so the rescan schedule is never the reason a case
        // sees the grid it just set up.
        PluginThread::detail::JobDispatcher::Invoke([](const PluginThread::Token& pt) { FineRoads::Poll(pt, 1000.0); });
    }

    bool PathVisits(const std::vector<RE::NiPoint3>& path, float x, float y)
    {
        for (const auto& p : path) {
            if (p.x == x && p.y == y)
                return true;
        }
        return false;
    }

    RE::NiPoint3 At(float x, float y)
    {
        return RE::NiPoint3{x, y, 0.0f};
    }

    // ResolveOrigin reads engine state and so takes a main-thread token. The
    // only way to hold one is to be handed it, which is what this does.
    RoadRoute::Origin ResolveFor(RE::TESObjectREFR* ref)
    {
        const NarrativeEngine::ScopedThreadRole role{NarrativeEngine::ThreadRole::Plugin};
        RoadRoute::Origin origin;
        PluginThread::detail::JobDispatcher::Invoke([&](const PluginThread::Token& pt) {
            origin = NarrativeEngine::MainThread::Run(
                pt, [ref](const NarrativeEngine::MainThread::Token& mt) { return RoadRoute::ResolveOrigin(mt, ref); });
        });
        return origin;
    }

    // Far east along the coarse road, and a long way from any fine node.
    RE::NiPoint3 FarEast()
    {
        return At(CellCentre(CoarsePlaces::roadLastCell), CellCentre(CoarsePlaces::roadCellY));
    }
} // namespace

TEST_CASE("RoadRoute hands off at the frontier that gets there soonest", "[RoadRoute][engine]")
{
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    LayCoarseRoad(engine);
    TravelGraph::Initialize();
    REQUIRE(TravelGraph::NodeCount() > 0);

    auto* space = engine.AddWorldSpace(kMainland);
    auto* cell = engine.AddExteriorCell(space, 9, 9, nullptr);
    LayFineRoad(engine, cell);
    engine.LoadGrid({cell});
    PollFineRoads();
    REQUIRE(FineRoads::NodeCount() == 6);

    SECTION("when the destination lies beyond the loaded region")
    {
        const auto plan = RoadRoute::Route(kMainland, At(FinePlaces::junctionX, FinePlaces::junctionY), FarEast());

        SECTION("should plan a route")
        {
            REQUIRE(plan.valid);
        }

        SECTION("should walk to the frontier with the cheapest whole journey")
        {
            // The overland branch leaves coverage after a single step and is by
            // far the cheapest frontier to reach. It comes out three cells from
            // the nearest road, and every one of those units has to be walked
            // too. The roadside branch is four times as long on foot and still
            // the shorter journey.
            REQUIRE(PathVisits(plan.finePath, FinePlaces::roadsideX, FinePlaces::roadsideY));
            REQUIRE_FALSE(PathVisits(plan.finePath, FinePlaces::overlandX, FinePlaces::overlandY));
        }

        SECTION("should start the walk where the traveller is standing")
        {
            REQUIRE_FALSE(plan.finePath.empty());
            REQUIRE(plan.finePath.front().x == FinePlaces::junctionX);
            REQUIRE(plan.finePath.front().y == FinePlaces::junctionY);
        }

        SECTION("should walk the road rather than cut across it")
        {
            // Junction, then both nodes between it and the frontier. A path
            // that skipped the middle would be a straight line across whatever
            // lies between them, which is the one thing a road graph exists to
            // avoid.
            REQUIRE(plan.finePath.size() == 4);
            REQUIRE(PathVisits(plan.finePath, FinePlaces::bendX, FinePlaces::bendY));
            REQUIRE(PathVisits(plan.finePath, FinePlaces::climbX, FinePlaces::climbY));
        }

        SECTION("should ignore a frontier that joins a road going nowhere")
        {
            // The stub comes out right on top of a piece of network, closer to
            // one than either other frontier gets — but that piece has no route
            // to the destination, so taking it would walk the NPC to a dead end
            // and stop. (The explicit guard against an unreachable entry is
            // belt and braces: an unreachable remainder is already infinite, so
            // the score alone rules the stub out. The guard says so plainly and
            // keeps the arithmetic from having to.)
            REQUIRE_FALSE(PathVisits(plan.finePath, FinePlaces::stubX, FinePlaces::stubY));
        }

        SECTION("should carry on along the coarse road afterwards")
        {
            REQUIRE_FALSE(plan.coarsePath.empty());
        }

        SECTION("should not claim the destination is within reach on foot")
        {
            REQUIRE_FALSE(plan.destinationWithinFine);
        }

        SECTION("should price the whole journey, not just the walk")
        {
            // The cost is what ranks one plan against another, so it has to
            // include the part beyond the handoff — otherwise every plan that
            // hands off early looks cheap.
            REQUIRE(plan.estimatedCost > 20000.0f);
        }
    }

    SECTION("when the destination is inside the loaded region")
    {
        const auto plan =
            RoadRoute::Route(kMainland, At(FinePlaces::junctionX, FinePlaces::junctionY), At(60000.0f, 51000.0f));

        SECTION("should say the whole trip is walkable")
        {
            // Handing off for a journey that never leaves the loaded region
            // would route the NPC out to the edge of coverage and back.
            REQUIRE(plan.valid);
            REQUIRE(plan.destinationWithinFine);
        }

        SECTION("should walk all the way there")
        {
            REQUIRE(PathVisits(plan.finePath, FinePlaces::roadsideX, FinePlaces::roadsideY));
        }

        SECTION("should hand off to nothing")
        {
            REQUIRE(plan.coarsePath.empty());
        }
    }

    SECTION("when the fine graph covers a different worldspace")
    {
        // Which is what the moment after a load-door transition looks like:
        // the graph still holds the cells around where the player was.
        auto* island = engine.AddWorldSpace(kIsland);
        auto* islandCell = engine.AddExteriorCell(island, 9, 9, nullptr);
        LayFineRoad(engine, islandCell);
        engine.LoadGrid({islandCell});
        PollFineRoads();
        const auto plan = RoadRoute::Route(kMainland, At(FinePlaces::junctionX, FinePlaces::junctionY), FarEast());

        SECTION("should plan on the coarse graph alone")
        {
            // Coordinates mean different things in different worldspaces, so
            // fine nodes from another one are not near anything.
            REQUIRE(plan.valid);
            REQUIRE(plan.finePath.empty());
            REQUIRE_FALSE(plan.coarsePath.empty());
        }
    }

    SECTION("when nothing loaded carries any road")
    {
        auto* bare = engine.AddExteriorCell(space, 9, 10, nullptr);
        engine.LoadGrid({bare});
        PollFineRoads();
        REQUIRE(FineRoads::NodeCount() == 0);
        const auto plan = RoadRoute::Route(kMainland, At(FinePlaces::junctionX, FinePlaces::junctionY), FarEast());

        SECTION("should plan on the coarse graph alone")
        {
            REQUIRE(plan.valid);
            REQUIRE(plan.finePath.empty());
            REQUIRE_FALSE(plan.coarsePath.empty());
        }
    }

    SECTION("when no fine branch leaves the loaded region")
    {
        // The player at the end of a spur, with road around them that goes
        // nowhere. Walking it would take the NPC into a dead end.
        auto* enclosed = engine.AddExteriorCell(space, 9, 11, nullptr);
        auto here = Road(FinePlaces::junctionX, FinePlaces::junctionY);
        here.neighbor[0] = 1;
        auto there = Road(FinePlaces::overlandX, FinePlaces::overlandY);
        there.neighbor[0] = 0;
        engine.AddNavMesh(enclosed, kFineMesh, {here, there});
        engine.LoadGrid({enclosed});
        PollFineRoads();
        REQUIRE(FineRoads::NodeCount() == 2);
        const auto plan = RoadRoute::Route(kMainland, At(FinePlaces::junctionX, FinePlaces::junctionY), FarEast());

        SECTION("should fall back to the coarse graph")
        {
            REQUIRE(plan.valid);
            REQUIRE(plan.finePath.empty());
            REQUIRE_FALSE(plan.coarsePath.empty());
        }
    }

    SECTION("when the worldspace has no coarse road in it")
    {
        const auto plan = RoadRoute::Route(kIsland, At(FinePlaces::junctionX, FinePlaces::junctionY), FarEast());

        SECTION("should refuse to plan")
        {
            // Distinct from a plan with an empty path: there is no answer here
            // at all, and a caller that treated one as the other would send an
            // NPC to the origin of the worldspace.
            REQUIRE_FALSE(plan.valid);
        }
    }

    SECTION("when the two ends sit on unconnected roads")
    {
        // With nothing loaded there is no fine graph to snap the start onto,
        // so the whole question falls to the coarse skeleton — which is the
        // only place two ends can turn out to be unreachable from each other.
        auto* bare = engine.AddExteriorCell(space, 9, 13, nullptr);
        engine.LoadGrid({bare});
        PollFineRoads();
        const auto plan = RoadRoute::Route(
            kMainland, At(CellCentre(CoarsePlaces::strandedCellX), CellCentre(CoarsePlaces::strandedCellY)), FarEast());

        SECTION("should refuse to plan")
        {
            // The skeleton is deliberately not one connected network. Handing
            // back a route between two of its islands would cross whatever
            // separates them.
            REQUIRE_FALSE(plan.valid);
        }
    }
}

TEST_CASE("RoadRoute places a reference on the road network", "[RoadRoute][engine]")
{
    // Every route starts from a reference, and half of them start indoors,
    // where there is no fine graph and the interior has no place on the coarse
    // one. Following a door out and using where it comes out is what makes an
    // NPC leaving a building read correctly — they appear at the door, not at
    // the middle of the town.
    EngineMock engine;
    const ConfiguredSettings settings{kSettings};
    auto* space = engine.AddWorldSpace(kMainland);
    auto* outside = engine.AddExteriorCell(space, 9, 9, nullptr);
    auto* inn = engine.AddExteriorCell(space, 9, 10, nullptr);
    auto* cellar = engine.AddExteriorCell(space, 9, 11, nullptr);
    engine.SetCellInterior(inn, true);
    engine.SetCellInterior(cellar, true);

    constexpr float kDoorstepX = 41000.0f;
    constexpr float kDoorstepY = 41000.0f;
    constexpr float kMarkerX = 30000.0f;

    SECTION("when the reference is outdoors")
    {
        auto* ref = engine.AddReference(outside, 0x00B13001u, At(40000.0f, 40000.0f));
        const auto origin = ResolveFor(ref);

        SECTION("should use where it is standing")
        {
            REQUIRE(origin.valid);
            REQUIRE(origin.position.x == 40000.0f);
        }

        SECTION("should name the worldspace it is in")
        {
            REQUIRE(origin.worldSpace == kMainland);
        }

        SECTION("should not claim it came out of a door")
        {
            REQUIRE_FALSE(origin.viaLoadDoor);
        }
    }

    SECTION("when the reference is indoors and the building has a way out")
    {
        engine.AddLoadDoor(inn, 0x00B13010u, outside, At(kDoorstepX, kDoorstepY));
        auto* ref = engine.AddReference(inn, 0x00B13002u, At(1000.0f, 1000.0f));
        const auto origin = ResolveFor(ref);

        SECTION("should use where the door comes out")
        {
            // Interior coordinates are local to the cell and mean nothing
            // outside it, so using the reference's own position would put the
            // origin next to the middle of the worldspace.
            REQUIRE(origin.valid);
            REQUIRE(origin.position.x == kDoorstepX);
            REQUIRE(origin.position.y == kDoorstepY);
        }

        SECTION("should say the position came from a door")
        {
            // The caller uses this to know the position is where the occupant
            // will emerge rather than where they currently are.
            REQUIRE(origin.viaLoadDoor);
        }

        SECTION("should take the worldspace from the far side")
        {
            REQUIRE(origin.worldSpace == kMainland);
        }
    }

    SECTION("when the only door leads to another room")
    {
        // A cellar hatch is not a way outside, and taking it would resolve the
        // origin to a second set of interior coordinates.
        engine.AddLoadDoor(inn, 0x00B13020u, cellar, At(5.0f, 5.0f));
        auto* ref = engine.AddReference(inn, 0x00B13003u, At(1000.0f, 1000.0f));

        SECTION("and there is a way outside further along")
        {
            engine.AddLoadDoor(inn, 0x00B13030u, outside, At(kDoorstepX, kDoorstepY));
            const auto origin = ResolveFor(ref);

            SECTION("should keep looking until it finds it")
            {
                REQUIRE(origin.valid);
                REQUIRE(origin.position.x == kDoorstepX);
            }
        }

        SECTION("and the location has a map marker")
        {
            auto* location = engine.AddLocation(0x00B13100u, "The Inn", {});
            engine.SetLocationMarker(location, outside, At(kMarkerX, kMarkerX));
            engine.world.playerHasLocation = true;
            engine.world.playerLocationOverride = location;
            const auto origin = ResolveFor(ref);

            SECTION("should fall back to the marker")
            {
                // Markers sit some way from the actual entrance — outside the
                // walls, for a town — so this is a worse answer than a door.
                // It is a much better one than none, which would leave the NPC
                // with nowhere to travel from.
                REQUIRE(origin.valid);
                REQUIRE(origin.position.x == kMarkerX);
                REQUIRE(origin.viaLoadDoor);
            }
        }
    }

    SECTION("when the door has no other side")
    {
        // Which happens: an unlinked door survives in a save.
        engine.AddUnlinkedDoor(inn, 0x00B13040u);
        auto* ref = engine.AddReference(inn, 0x00B13004u, At(1000.0f, 1000.0f));
        engine.world.playerHasLocation = false;
        engine.world.playerLocationOverride = nullptr;
        const auto origin = ResolveFor(ref);

        SECTION("should give up rather than guess")
        {
            REQUIRE_FALSE(origin.valid);
        }
    }

    SECTION("when the interior has neither a way out nor a marker")
    {
        auto* ref = engine.AddReference(cellar, 0x00B13005u, At(1000.0f, 1000.0f));
        engine.world.playerHasLocation = false;
        engine.world.playerLocationOverride = nullptr;
        const auto origin = ResolveFor(ref);

        SECTION("should give up")
        {
            // Some scripted interiors genuinely have no exit. An origin
            // invented for one would put a traveller at the worldspace origin,
            // which is out at sea.
            REQUIRE_FALSE(origin.valid);
        }
    }

    SECTION("when the cell belongs to no worldspace")
    {
        // Written through the cell directly, because it is not a state the
        // harness can build: every fabricated exterior cell belongs to the
        // worldspace it was added to. The engine has cells that do not.
        auto* orphan = engine.AddExteriorCell(space, 9, 12, nullptr);
        orphan->GetRuntimeData().worldSpace = nullptr;
        auto* ref = engine.AddReference(orphan, 0x00B13006u, At(40000.0f, 40000.0f));
        const auto origin = ResolveFor(ref);

        SECTION("should give up")
        {
            REQUIRE_FALSE(origin.valid);
        }
    }

    SECTION("when there is no reference at all")
    {
        SECTION("should give up")
        {
            REQUIRE_FALSE(ResolveFor(nullptr).valid);
        }
    }

    SECTION("when the reference is in no cell")
    {
        auto* ref = engine.AddReference(nullptr, 0x00B13007u, At(40000.0f, 40000.0f));
        const auto origin = ResolveFor(ref);

        SECTION("should give up")
        {
            // An unloaded reference has no parent cell, and its stored
            // position is wherever it was last put down.
            REQUIRE_FALSE(origin.valid);
        }
    }
}
